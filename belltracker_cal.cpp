// belltracker_cal.cpp — cal phase, auto bell/chime detection, YIN, NMF templates
#include "belltracker.h"
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <utility>

// ── debug instrumentation ─────────────────────────────────────────────────────
bool g_debug = false;

static void midi_to_name(int midi, char* out, size_t n);  // defined below

static const char* cal_state_name(CalSubState s) {
    switch (s) {
    case CalSubState::AWAITING_STRIKE:      return "AWAITING_STRIKE";
    case CalSubState::CAPTURING:            return "CAPTURING";
    case CalSubState::BELL_CONFIRM_PENDING: return "BELL_CONFIRM_PENDING";
    case CalSubState::BYPASS_LOADING:       return "BYPASS_LOADING";
    case CalSubState::BYPASS_ACK_PENDING:   return "BYPASS_ACK_PENDING";
    }
    return "?";
}

void BellTracker::set_cal_state(CalSubState s) {
    if (s != cal_state_)
        DBG("cal state %s -> %s\n", cal_state_name(cal_state_), cal_state_name(s));
    cal_state_ = s;
}

// ── constructor — start MIDI worker thread ───────────────────────────────────
BellTracker::BellTracker() {
    midi_worker_ = std::thread(&BellTracker::midi_worker_run, this);
}

// ── destructor — drain note-offs, poison worker ──────────────────────────────
BellTracker::~BellTracker() {
    // finalize WAV — must come before MIDI cleanup; independent of JACK state
    stop_recording();

    // send note-offs for any live perf notes
    if (state_ == AppState::PERF)
        for (int i = 0; i < n_bells_; ++i)
            if (bells_[i].note_state == NoteState::SOUNDING)
                midi_note_off(bells_[i].midi_note, bell_channel(i));

    // poison pill — shuts down the worker after it drains the queue
    post_midi({ -1, 0, 0, 0 });
    if (midi_worker_.joinable()) midi_worker_.join();

    if (seq_) snd_seq_close(seq_);
}

// ── MIDI worker thread ───────────────────────────────────────────────────────
//
// Runs entirely off the JACK RT thread, as an explicit two-state machine:
//
//   IDLE         → dequeue next command (blocks on the condvar when empty)
//                    duration_ms > 0  → send note-on, go to HOLDING_NOTE
//                    duration_ms == 0 → bare note-on/off (by velocity),
//                                       stay IDLE
//                    note == -1       → poison pill, exit
//   HOLDING_NOTE → sleep out the hold, send note-off, sleep out any trailing
//                    gap_ms, return to IDLE
//
void BellTracker::midi_worker_run() {
    MidiWorkerState wstate  = MidiWorkerState::IDLE;
    MidiCmd         holding{};

    while (true) {
        switch (wstate) {

        case MidiWorkerState::IDLE: {
            MidiCmd cmd;
            {
                std::unique_lock<std::mutex> lk(midi_mutex_);
                midi_cv_.wait(lk, [this]{ return !midi_queue_.empty(); });
                cmd = midi_queue_.front();
                midi_queue_.pop();
            }

            if (cmd.note < 0) return;  // poison pill

            if (cmd.program >= 0) {    // program change — no note handling
                midi_program_change(cmd.channel, cmd.program);
                break;
            }

            if (cmd.duration_ms > 0) {
                midi_note_on(cmd.note, cmd.channel, cmd.velocity);
                holding = cmd;
                wstate  = MidiWorkerState::HOLDING_NOTE;
            } else {
                if (cmd.velocity > 0)
                    midi_note_on(cmd.note, cmd.channel, cmd.velocity);
                else
                    midi_note_off(cmd.note, cmd.channel);
                // stays IDLE
            }
            break;
        }

        case MidiWorkerState::HOLDING_NOTE:
            std::this_thread::sleep_for(std::chrono::milliseconds(holding.duration_ms));
            midi_note_off(holding.note, holding.channel);
            if (holding.gap_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(holding.gap_ms));
            wstate = MidiWorkerState::IDLE;
            break;
        }
    }
}

// ── post_midi — enqueue from any thread (RT-safe: lock-free queue post) ─────
//
// std::mutex lock is held only briefly to push; the worker holds it only
// briefly to pop. In the RT callback this is technically a potential stall,
// but the critical section is a single queue push — in practice < 1 µs,
// well within the 5.3 ms JACK budget. A lock-free queue (e.g. boost::lockfree)
// could replace this if xruns appear under heavy load.
//
void BellTracker::post_midi(MidiCmd cmd) {
    {
        std::lock_guard<std::mutex> lk(midi_mutex_);
        midi_queue_.push(cmd);
    }
    midi_cv_.notify_one();
}

// ── post_ping — convenience: enqueue a note-on+hold+off(+gap) sequence ──────
void BellTracker::post_ping(int note, int ch, int vel, int dur_ms, int gap_ms) {
    post_midi({ note, ch, vel, dur_ms, gap_ms });
}

// ── notify_ready — headless "armed and listening" cue ───────────────────────
void BellTracker::notify_ready() {
    post_system_prompt(SystemPrompt::READY);
}

// ── post_system_prompt — headless status cues on the reserved system channel ─
void BellTracker::post_system_prompt(SystemPrompt p) {
    switch (p) {
        case SystemPrompt::READY:
            post_ping(PROMPT_NOTE_READY, SYSTEM_CHANNEL, PROMPT_VEL, PROMPT_DUR_MS);
            break;
        case SystemPrompt::CAL_BYPASS:
            post_ping(PROMPT_NOTE_BYPASS, SYSTEM_CHANNEL, PROMPT_VEL, PROMPT_DUR_MS);
            break;
        case SystemPrompt::CAL_BYPASS_FAILED:
            // triple rapid ping — deliberately reads as "error" by ear, distinct
            // from every single-ping success cue elsewhere in the system
            for (int i = 0; i < 3; ++i)
                post_ping(PROMPT_NOTE_BYPASS_FAIL, SYSTEM_CHANNEL, PROMPT_VEL,
                           PROMPT_FAIL_DUR_MS);
            break;
    }
}

// ── init ─────────────────────────────────────────────────────────────────────
bool BellTracker::init(const std::string& load_cal_path,
                        const std::string& save_cal_path,
                        int channel_override) {
    if (!open_midi()) return false;
    channel_override_ = channel_override;
    save_cal_path_ = save_cal_path;
    find_usb_drive();

    if (!load_cal_path.empty()) {
        if (load_cal_from_file(load_cal_path)) {
            for (int i = 0; i < n_bells_; ++i) {
                bells_[i].goertzel.init(bells_[i].freq_hz, GOERTZEL_WINDOW);
                if (channel_override_ >= 0)          // --channel beats cal file
                    bells_[i].channel = channel_override_;
            }
            send_program_changes();
            state_ = AppState::PERF;
            printf("belltracker: MIDI ready → UR22mkII DIN out\n");
            printf("belltracker: loaded %d bell(s) from %s\n",
                   n_bells_, load_cal_path.c_str());
            for (int i = 0; i < n_bells_; ++i) {
                const char* ts = (bells_[i].type == InstrumentType::BELL) ? "bell" : "chime";
                char nn[8]; midi_to_name(bells_[i].midi_note, nn, sizeof(nn));
                printf("  %2d: %.2f Hz  MIDI %3d (%-3s)  ch %d  [%s]\n",
                       i+1, bells_[i].freq_hz, bells_[i].midi_note, nn,
                       bell_channel(i), ts);
            }
            // audible cue — headless deployment, console isn't visible.
            // Sequential arpeggio, not a simultaneous chord — each queued
            // ping is processed serially by the MIDI worker, so this plays
            // bell 1, then bell 2, etc., 0.5s apart.
            for (int i = 0; i < n_bells_; ++i)
                post_ping(bells_[i].midi_note, bell_channel(i), 100,
                          ARPEGGIO_NOTE_MS, ARPEGGIO_GAP_MS);
            start_recording();
            printf("── Entering PERF mode (CAL skipped) ──\n\n");
            return true;
        }
        fprintf(stderr, "belltracker: failed to load %s — falling back to CAL\n",
                load_cal_path.c_str());
    }

    send_program_changes();
    printf("belltracker: MIDI ready → UR22mkII DIN out\n");
    printf("belltracker: CAL mode\n");
    printf("  Pre-onset ring : %d ms\n", CAL_PRE_RING_MS);
    printf("  Sustain capture: %d ms\n", CAL_CAPTURE_MS);
    printf("  Bell threshold : %.0f ms attack\n", ATTACK_BELL_MS);
    printf("  Strike each note once; repeat bell 1 to end cal.\n");
    printf("  Cal data will be saved to %s on completion.\n\n", save_cal_path_.c_str());
    return true;
}

// ── top-level process dispatch ───────────────────────────────────────────────
void BellTracker::process(const float* in, jack_nframes_t nframes) {
    switch (state_) {
    case AppState::CAL:  cal_process(in, nframes);  break;
    case AppState::PERF: perf_process(in, nframes); break;
    }
}

// ── cal_process ──────────────────────────────────────────────────────────────
//
// Per-sample dispatch over CalSubState. If a sample's handling flips the
// top-level state_ to PERF (full cal complete via finalize_cal(), or a
// successful bypass), the remaining samples in this same JACK buffer are
// handed directly to perf_process() rather than waiting for the next
// callback — no samples processed under a stale sub-state, none dropped.
//
void BellTracker::cal_process(const float* in, jack_nframes_t nframes) {
    const double jack_time_s =
        (double)jack_last_frame_time(jack_client_) / SAMPLE_RATE;

    for (jack_nframes_t i = 0; i < nframes; ++i) {
        float x = in[i];

        // pre-onset ring buffer — always running regardless of sub-state
        pre_ring_[pre_ring_pos_] = x;
        if (++pre_ring_pos_ >= CAL_PRE_RING_SAMP) {
            pre_ring_pos_  = 0;
            pre_ring_full_ = true;
        }

        // rolling RMS — always running regardless of sub-state, same reason
        // as the pre-ring buffer: must stay warm through CAPTURING so the
        // window is accurate the instant we're back in AWAITING_STRIKE,
        // not full of stale pre-capture samples
        rms_buf_[rms_pos_++ & 255] = x * x;
        float rms = 0.0f;
        for (int j = 0; j < 256; ++j) rms += rms_buf_[j];
        rms = sqrtf(rms / 256.0f);

        switch (cal_state_) {

        case CalSubState::AWAITING_STRIKE: {
            // Periodic "waiting for input" cue — only while nothing has been
            // struck yet this session. Stops permanently the instant the
            // first onset fires below, even if that strike turns out to be
            // the damped bypass gesture rather than a normal registration.
            if (!heard_first_input_ &&
                jack_time_s - last_ready_ping_s_ >= READY_PING_INTERVAL_S) {
                post_system_prompt(SystemPrompt::READY);
                last_ready_ping_s_ = jack_time_s;
            }

            if (g_debug && jack_time_s - dbg_wait_last_s_ >= DBG_WAIT_PERIOD_S) {
                DBG("waiting  rms=%.4f  thr=%.4f  bells=%d  armed=%d\n",
                    rms, ONSET_THRESHOLD, n_bells_, (int)cal_armed_);
                dbg_wait_last_s_ = jack_time_s;
            }

            if (rms > ONSET_THRESHOLD) {
                printf("  Onset (RMS %.4f)\n", rms);
                start_capture();
                bells_.back().cal_buf.push_back(x);
                onset_counter_     = 1;
                heard_first_input_ = true;
                set_cal_state(CalSubState::CAPTURING);
            }
            break;
        }

        case CalSubState::CAPTURING:
            bells_.back().cal_buf.push_back(x);
            if (++onset_counter_ >= CAL_CAPTURE_SAMP) {
                onset_counter_ = 0;
                // analyze_bell runs in the RT callback but does NOT block on
                // I/O: MIDI is posted to the worker queue and any file I/O
                // (cal save/load) is brief, synchronous, and the same
                // accepted exception already covering build_nmf_templates.
                // It may itself transition cal_state_ (to BELL_CONFIRM_PENDING
                // for a normal registration, or BYPASS_ACK_PENDING for the
                // damped-strike gesture) or state_ (via finalize_cal, on a
                // bell-1 repeat) — only fall back to AWAITING_STRIKE if it
                // left cal_state_ untouched.
                analyze_bell(bells_.back());
                if (cal_state_ == CalSubState::CAPTURING)
                    set_cal_state(CalSubState::AWAITING_STRIKE);
            }
            break;

        case CalSubState::BELL_CONFIRM_PENDING:
            // Same polling pattern as BYPASS_ACK_PENDING below: a stored
            // deadline checked each callback rather than a sleeping thread.
            // No new capture starts until this bell's confirmation has
            // played — calibrating a large set takes longer wall-clock time
            // as a result (>2s per bell: 1s delay + 3×0.5s playback), but
            // keeps cal_state_/bells_/n_bells_ single-writer with no locking.
            if (jack_time_s >= confirm_deadline_s_) {
                for (int r = 0; r < BELL_CONFIRM_REPEATS; ++r)
                    post_ping(confirm_note_, confirm_channel_, 80,
                              ARPEGGIO_NOTE_MS, ARPEGGIO_GAP_MS);

                bool do_finalize       = confirm_then_finalize_;
                confirm_then_finalize_ = false;
                set_cal_state(CalSubState::AWAITING_STRIKE);  // harmless if
                                                              // finalize_cal
                                                              // flips state_
                                                              // to PERF below
                if (do_finalize)
                    finalize_cal();
            }
            break;

        case CalSubState::BYPASS_LOADING:
            // Never actually observed here — entered and exited synchronously
            // within analyze_bell() inside the same sample's CAPTURING case
            // above. Listed explicitly so every CalSubState has a case.
            break;

        case CalSubState::BYPASS_ACK_PENDING:
            // RMS/pre-ring buffers above keep running so AWAITING_STRIKE is
            // warm the moment we return to it; we just don't act on onset
            // here — no new capture starts while waiting out the ack delay.
            // Just watch the clock.
            if (jack_time_s >= bypass_ack_deadline_s_) {
                post_system_prompt(SystemPrompt::CAL_BYPASS);
                if (bypass_loaded_) {
                    // sequential arpeggio, not simultaneous — see finalize_cal()
                    for (auto& nc : bypass_chord_notes_)
                        post_ping(nc.first, nc.second, 100,
                                  ARPEGGIO_NOTE_MS, ARPEGGIO_GAP_MS);
                    bypass_chord_notes_.clear();
                    start_recording();
                    state_ = AppState::PERF;   // top-level transition
                } else {
                    post_system_prompt(SystemPrompt::CAL_BYPASS_FAILED);
                }
                set_cal_state(CalSubState::AWAITING_STRIKE);  // resume normal cal
                                                              // if bypass failed;
                                                              // harmless if PERF
            }
            break;
        }

        if (state_ != AppState::CAL) {
            jack_nframes_t remaining = nframes - (i + 1);
            if (remaining > 0)
                perf_process(in + i + 1, remaining);
            return;
        }
    }
}

// ── start_capture ────────────────────────────────────────────────────────────
void BellTracker::start_capture() {
    BellData b;
    b.cal_buf.reserve(CAL_PRE_RING_SAMP + CAL_CAPTURE_SAMP);

    if (pre_ring_full_) {
        for (int i = pre_ring_pos_; i < CAL_PRE_RING_SAMP; ++i)
            b.cal_buf.push_back(pre_ring_[i]);
        for (int i = 0; i < pre_ring_pos_; ++i)
            b.cal_buf.push_back(pre_ring_[i]);
    } else {
        for (int i = 0; i < pre_ring_pos_; ++i)
            b.cal_buf.push_back(pre_ring_[i]);
    }

    if (channel_override_ >= 0)
        b.channel = channel_override_;
    bells_.push_back(std::move(b));
}

// ── measure_attack_ms ────────────────────────────────────────────────────────
float BellTracker::measure_attack_ms(const std::vector<float>& buf) {
    const int WIN = 256;
    int   len       = (int)buf.size();
    int   onset_idx = -1, peak_idx = 0;
    float peak_rms  = 0.0f, acc = 0.0f;

    for (int i = 0; i < len; ++i) {
        acc += buf[i] * buf[i];
        if (i >= WIN) acc -= buf[i - WIN] * buf[i - WIN];
        float rms = sqrtf(acc / std::min(i + 1, WIN));
        if (onset_idx < 0 && rms > ONSET_THRESHOLD) onset_idx = i;
        if (rms > peak_rms) { peak_rms = rms; peak_idx = i; }
    }

    if (onset_idx < 0) onset_idx = 0;
    return (float)std::max(0, peak_idx - onset_idx) / SAMPLE_RATE * 1000.0f;
}

// ── measure_decay_ms ─────────────────────────────────────────────────────────
// Time from peak RMS to the first point the envelope falls below
// DECAY_RELEASE_FRAC of that peak. A free-ringing bell stays loud for most
// of the 500ms capture; a palm-damped strike collapses within tens of ms.
// Returns the full remaining capture length if the envelope never releases
// within the window (i.e. "definitely not damped" — saturates instead of
// triggering a false bypass).
float BellTracker::measure_decay_ms(const std::vector<float>& buf) {
    const int WIN = 256;
    int   len      = (int)buf.size();
    int   peak_idx = 0;
    float peak_rms = 0.0f, acc = 0.0f;

    for (int i = 0; i < len; ++i) {
        acc += buf[i] * buf[i];
        if (i >= WIN) acc -= buf[i - WIN] * buf[i - WIN];
        float rms = sqrtf(acc / std::min(i + 1, WIN));
        if (rms > peak_rms) { peak_rms = rms; peak_idx = i; }
    }

    if (peak_rms < ONSET_THRESHOLD)
        return (float)len / SAMPLE_RATE * 1000.0f;   // no real strike — not damped

    const float release_thresh = peak_rms * DECAY_RELEASE_FRAC;

    acc = 0.0f;
    for (int i = 0; i <= peak_idx && i < len; ++i) {
        acc += buf[i] * buf[i];
        if (i >= WIN) acc -= buf[i - WIN] * buf[i - WIN];
    }
    for (int i = peak_idx; i < len; ++i) {
        if (i > peak_idx) {
            acc += buf[i] * buf[i];
            if (i >= WIN) acc -= buf[i - WIN] * buf[i - WIN];
        }
        float rms = sqrtf(acc / std::min(i + 1, WIN));
        if (rms < release_thresh)
            return (float)(i - peak_idx) / SAMPLE_RATE * 1000.0f;
    }
    return (float)(len - peak_idx) / SAMPLE_RATE * 1000.0f;  // never released
}

// ── analyze_bell — non-blocking: posts MIDI to worker, returns immediately ───
void BellTracker::analyze_bell(BellData& b) {
    int len = (int)b.cal_buf.size();
    if (len < 2048) {
        printf("  Capture too short (%d samples), ignoring\n", len);
        bells_.pop_back();
        return;
    }

    // ── damped-strike cal bypass ─────────────────────────────────────────
    // Only meaningful as the very first strike of a session (n_bells_==0,
    // nothing registered yet). A damped strike at any other point is just
    // a damped note, not a command — no special handling past here.
    if (n_bells_ == 0) {
        float decay_ms = measure_decay_ms(b.cal_buf);
        DBG("first-strike decay %.1f ms (damped threshold %.0f ms)\n",
            decay_ms, DAMPED_DECAY_MS);
        if (decay_ms < DAMPED_DECAY_MS) {
            printf("  Damped strike (decay %.0f ms) — bypassing CAL\n", decay_ms);
            bells_.pop_back();   // discard: this was a command, not bell 1

            set_cal_state(CalSubState::BYPASS_LOADING);   // transient, for clarity

            // All state decisions happen synchronously here on the RT
            // thread — same accepted exception as finalize_cal()/
            // build_nmf_templates: small, one-time, no I/O beyond a tiny
            // text file read. Outcome and chord notes are stashed in
            // member fields for BYPASS_ACK_PENDING to consume once the
            // ack delay (an ordinary timed state transition, not a thread)
            // elapses in cal_process().
            bypass_loaded_ = load_cal_from_file(save_cal_path_);
            bypass_chord_notes_.clear();
            if (bypass_loaded_) {
                for (int i = 0; i < n_bells_; ++i)
                    bells_[i].goertzel.init(bells_[i].freq_hz, GOERTZEL_WINDOW);
                printf("  Loaded %d bell(s) from %s\n",
                       n_bells_, save_cal_path_.c_str());
                bypass_chord_notes_.reserve(n_bells_);
                for (int i = 0; i < n_bells_; ++i)
                    bypass_chord_notes_.emplace_back(bells_[i].midi_note, bell_channel(i));
            } else {
                fprintf(stderr, "  No saved cal data at %s — continuing CAL\n",
                        save_cal_path_.c_str());
            }

            const double jack_time_s =
                (double)jack_last_frame_time(jack_client_) / SAMPLE_RATE;
            bypass_ack_deadline_s_ = jack_time_s + (BYPASS_ACK_DELAY_MS / 1000.0);
            set_cal_state(CalSubState::BYPASS_ACK_PENDING);
            return;
        }
    }

    // classify
    b.attack_ms = measure_attack_ms(b.cal_buf);
    b.type = (b.attack_ms < ATTACK_BELL_MS) ? InstrumentType::BELL
                                             : InstrumentType::CHIME;
    const char* ts = (b.type == InstrumentType::BELL) ? "BELL" : "CHIME";

    // YIN window selection
    const int YIN_WIN = 4096;
    const float* yin_ptr;
    int          yin_len;

    if (b.type == InstrumentType::BELL) {
        int pre = (int)b.cal_buf.size() - CAL_CAPTURE_SAMP;
        int skip = std::max(0, std::min(pre + (int)(0.030f * SAMPLE_RATE),
                                        len - YIN_WIN));
        yin_ptr = b.cal_buf.data() + skip;
        yin_len = std::min(YIN_WIN, len - skip);
    } else {
        int start = std::max(0, len / 2 - YIN_WIN / 2);
        yin_ptr = b.cal_buf.data() + start;
        yin_len = std::min(YIN_WIN, len - start);
    }

    float freq = yin_fundamental(yin_ptr, yin_len, SAMPLE_RATE);

    if (freq < 60.0f || freq > 8000.0f) {
        printf("  Frequency %.1f Hz out of range, ignoring\n", freq);
        bells_.pop_back();
        return;
    }

    float cents = freq_to_cents(freq);
    DBG("YIN %.2f Hz  %+.1f cents  attack %.1f ms  [%s]  len=%d\n",
        freq, cents, b.attack_ms, ts, len);
    if (cal_armed_)
        DBG("repeat check vs bell 1: |%.1f - %.1f| = %.1f cents (tol %.0f)\n",
            bells_[0].cents, cents, fabsf(bells_[0].cents - cents),
            REPEAT_CENTS_TOL);

    // repeat check — only bell 0 ends cal
    if (cal_armed_ && fabsf(bells_[0].cents - cents) < REPEAT_CENTS_TOL) {
        printf("  %.2f Hz — bell 1 repeated, ending cal\n", freq);
        bells_.pop_back();
        finalize_cal();   // posts arpeggio to worker, sets state = PERF, returns
        return;
    }

    // commit
    b.freq_hz   = freq;
    b.cents     = cents;
    b.midi_note = freq_to_midi(freq);
    n_bells_++;
    if (n_bells_ == 1) cal_armed_ = true;

    int ch = bell_channel(n_bells_ - 1);
    printf("  Bell %2d: %.2f Hz  MIDI %d  ch %d  [%s  attack %.1f ms]\n",
           n_bells_, b.freq_hz, b.midi_note, ch, ts, b.attack_ms);

    // Confirmation is deliberately delayed and repeated (3x at arpeggio
    // timing, after a 1s pause) — see BELL_CONFIRM_PENDING in cal_process().
    // If this bell hit MAX_BELLS, finalize_cal() runs only after that
    // confirmation has played, not immediately — the ringer still hears
    // their last bell confirmed before the cal-complete sequence starts.
    confirm_note_    = b.midi_note;
    confirm_channel_ = ch;
    const double jack_time_s =
        (double)jack_last_frame_time(jack_client_) / SAMPLE_RATE;
    confirm_deadline_s_ = jack_time_s + (BELL_CONFIRM_DELAY_MS / 1000.0);

    confirm_then_finalize_ = (n_bells_ >= MAX_BELLS);
    if (confirm_then_finalize_)
        printf("  Max bells reached, ending cal after confirmation\n");

    set_cal_state(CalSubState::BELL_CONFIRM_PENDING);
}

// ── YIN pitch estimator ──────────────────────────────────────────────────────
float BellTracker::yin_fundamental(const float* buf, int len, float sr) {
    const int tau_max = len / 2;
    std::vector<float> d(tau_max, 0.0f);

    for (int tau = 1; tau < tau_max; ++tau)
        for (int i = 0; i < tau_max; ++i) {
            float diff = buf[i] - buf[i + tau];
            d[tau] += diff * diff;
        }

    std::vector<float> cmnd(tau_max, 0.0f);
    cmnd[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau < tau_max; ++tau) {
        running += d[tau];
        cmnd[tau] = d[tau] * tau / running;
    }

    const float yin_thresh = 0.10f;
    int tau_est = -1;
    for (int tau = 2; tau < tau_max - 1; ++tau) {
        if (cmnd[tau] < yin_thresh &&
            cmnd[tau] < cmnd[tau-1] &&
            cmnd[tau] < cmnd[tau+1]) {
            tau_est = tau;
            break;
        }
    }
    if (tau_est < 0)
        tau_est = (int)(std::min_element(cmnd.begin()+2, cmnd.end()) - cmnd.begin());

    if (tau_est > 1 && tau_est < tau_max - 1) {
        float s0 = cmnd[tau_est-1], s1 = cmnd[tau_est], s2 = cmnd[tau_est+1];
        float denom = 2.0f * (2.0f*s1 - s2 - s0);
        if (fabsf(denom) > 1e-9f)
            return sr / (tau_est + (s2 - s0) / denom);
    }
    return sr / (float)tau_est;
}

// ── build_nmf_templates ──────────────────────────────────────────────────────
void BellTracker::build_nmf_templates() {
    printf("\n  Building NMF templates...\n");
    const int N = TEMPLATE_FFT_SIZE;
    std::vector<float> re(N), im(N);

    for (int i = 0; i < n_bells_; ++i) {
        std::fill(re.begin(), re.end(), 0.0f);
        std::fill(im.begin(), im.end(), 0.0f);
        int copy_len = std::min((int)bells_[i].cal_buf.size(), N);
        for (int s = 0; s < copy_len; ++s) re[s] = bells_[i].cal_buf[s];
        for (int s = 0; s < copy_len; ++s) {
            float w = 0.5f * (1.0f - cosf(2.0f * M_PI * s / (copy_len - 1)));
            re[s] *= w;
        }
        radix2_fft(re.data(), im.data(), N);

        bells_[i].nmf_template.resize(n_bells_, 0.0f);
        float tmpl_sum = 0.0f;
        for (int k = 0; k < n_bells_; ++k) {
            int bin = (int)roundf(bells_[k].freq_hz * N / SAMPLE_RATE);
            bin = std::max(0, std::min(N/2 - 1, bin));
            float mag2 = re[bin]*re[bin] + im[bin]*im[bin];
            bells_[i].nmf_template[k] = mag2;
            tmpl_sum += mag2;
        }
        if (tmpl_sum > 1e-12f)
            for (int k = 0; k < n_bells_; ++k)
                bells_[i].nmf_template[k] /= tmpl_sum;

        const char* ts = (bells_[i].type == InstrumentType::BELL) ? "bell" : "chime";
        printf("    [%5s] Bell %2d %.1f Hz  self=%.3f\n",
               ts, i+1, bells_[i].freq_hz, bells_[i].nmf_template[i]);

        bells_[i].cal_buf.clear();
        bells_[i].cal_buf.shrink_to_fit();
    }
}

// ── radix-2 FFT ─────────────────────────────────────────────────────────────
void BellTracker::radix2_fft(float* re, float* im, int n) {
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * M_PI / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int j = 0; j < len/2; ++j) {
                float ur = re[i+j], ui = im[i+j];
                float vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                float vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]       = ur+vr; im[i+j]       = ui+vi;
                re[i+j+len/2] = ur-vr; im[i+j+len/2] = ui-vi;
                float ncr = cr*wr - ci*wi;
                ci = cr*wi + ci*wr; cr = ncr;
            }
        }
    }
}

// ── finalize_cal ─────────────────────────────────────────────────────────────
//
// Called from the RT thread (via analyze_bell). Must not block on I/O.
// YIN + FFT are compute-only — no syscalls — but take ~44 ms on Pi 5,
// causing xruns at the moment cal ends. This is intentional and acceptable:
// it happens exactly once, after cal is complete, before perf begins.
// The cal-complete sequence masks any audio glitch from the outside.
//
void BellTracker::finalize_cal() {
    printf("\n── Cal complete: %d notes ──\n", n_bells_);
    build_nmf_templates();   // compute-only, no blocking

    int nb = 0, nc = 0;
    send_program_changes();
    for (int i = 0; i < n_bells_; ++i) {
        bells_[i].goertzel.init(bells_[i].freq_hz, GOERTZEL_WINDOW);
        const char* ts = (bells_[i].type == InstrumentType::BELL) ? "bell" : "chime";
        char nn[8]; midi_to_name(bells_[i].midi_note, nn, sizeof(nn));
        printf("  %2d: %.2f Hz  MIDI %3d (%-3s)  ch %d  [%s]\n",
               i+1, bells_[i].freq_hz, bells_[i].midi_note, nn, bell_channel(i), ts);
        (bells_[i].type == InstrumentType::BELL) ? ++nb : ++nc;
    }
    printf("  %d bell(s), %d chime(s)\n", nb, nc);

    // Post cal-complete sequence: each bell plays in turn (sequential
    // arpeggio), not all at once. Posting them back-to-back into the same
    // worker queue achieves this for free — the worker processes one
    // MidiCmd at a time (note-on, hold ARPEGGIO_NOTE_MS, note-off, silence
    // ARPEGGIO_GAP_MS) before dequeuing the next, so the notes come out
    // serialized at an even 0.5s onset-to-onset spacing.
    // Persist cal data so a future run can skip straight to PERF via
    // --load-cal. The snapshot copy (heap-allocating vector copy) happens
    // here on the RT thread — consistent with the exception already accepted
    // above for build_nmf_templates, since this is the same one-time window
    // masked by the cal-complete sequence that follows. The actual blocking
    // file write is handed to a detached thread so it can't extend that
    // window with disk-I/O latency. Small risk: if the process is killed
    // within ~1ms of cal ending, the write may not complete — negligible in
    // practice given the sequence that follows takes several seconds for
    // any real bell set.
    {
        std::vector<BellData> snapshot = bells_;
        int                   nb       = n_bells_;
        std::string           path     = save_cal_path_;
        std::thread([this, snapshot, nb, path]() mutable {
            save_cal_to_file(path, snapshot, nb);
        }).detach();
    }

    for (int i = 0; i < n_bells_; ++i)
        post_ping(bells_[i].midi_note, bell_channel(i), 100,
                  ARPEGGIO_NOTE_MS, ARPEGGIO_GAP_MS);

    start_recording();
    printf("── Entering PERF mode ──\n\n");
    state_ = AppState::PERF;   // flip state — returns immediately to JACK callback
}

// ── helpers ──────────────────────────────────────────────────────────────────
float BellTracker::freq_to_cents(float freq) {
    return 1200.0f * log2f(freq / 440.0f);
}

int BellTracker::freq_to_midi(float freq) {
    int note = (int)roundf(69.0f + 12.0f * log2f(freq / 440.0f));
    return std::max(0, std::min(127, note));
}

// ── ALSA sequencer MIDI ──────────────────────────────────────────────────────
bool BellTracker::open_midi() {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
        fprintf(stderr, "belltracker: cannot open ALSA sequencer\n");
        return false;
    }
    snd_seq_set_client_name(seq_, "belltracker");
    seq_port_ = snd_seq_create_simple_port(
        seq_, "out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (seq_port_ < 0) {
        fprintf(stderr, "belltracker: cannot create sequencer port\n");
        return false;
    }
    find_ur22_port();
    return true;
}

void BellTracker::find_ur22_port() {
    snd_seq_client_info_t* cinfo;
    snd_seq_port_info_t*   pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);

    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq_, cinfo) >= 0) {
        const char* cname = snd_seq_client_info_get_name(cinfo);
        if (strstr(cname, "UR22") || strstr(cname, "Steinberg")) {
            int client = snd_seq_client_info_get_client(cinfo);
            snd_seq_port_info_set_client(pinfo, client);
            snd_seq_port_info_set_port(pinfo, -1);
            while (snd_seq_query_next_port(seq_, pinfo) >= 0) {
                unsigned int cap = snd_seq_port_info_get_capability(pinfo);
                if ((cap & SND_SEQ_PORT_CAP_WRITE) &&
                    (cap & SND_SEQ_PORT_CAP_SUBS_WRITE)) {
                    ur22_client_ = client;
                    ur22_port_   = snd_seq_port_info_get_port(pinfo);
                    snd_seq_connect_to(seq_, seq_port_, ur22_client_, ur22_port_);
                    printf("belltracker: MIDI → %s %d:%d\n",
                           cname, ur22_client_, ur22_port_);
                    return;
                }
            }
        }
    }
    fprintf(stderr, "belltracker: UR22mkII not found — check 'aconnect -l'\n");
}

// Called only from midi_worker_run (non-RT thread)
void BellTracker::midi_note_on(int note, int channel, int vel) {
    if (!seq_) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, seq_port_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteon(&ev, channel & 0xF, note, vel);
    snd_seq_event_output_direct(seq_, &ev);
}

void BellTracker::midi_note_off(int note, int channel) {
    if (!seq_) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, seq_port_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteoff(&ev, channel & 0xF, note, 0);
    snd_seq_event_output_direct(seq_, &ev);
}

void BellTracker::midi_program_change(int channel, int program) {
    if (!seq_) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, seq_port_);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_pgmchange(&ev, channel & 0xF, program & 0x7F);
    snd_seq_event_output(seq_, &ev);
    snd_seq_drain_output(seq_);
    printf("belltracker: program change ch %d -> %d\n", channel, program);
}

void BellTracker::post_pc(int channel, int program) {
    post_midi({ 0, channel, 0, 0, 0, program });
}

// Send the voice (glockenspiel) to every channel bells will play on. Called
// after MIDI opens (covers cal confirmation pings on the default channel) and
// again after cal load/finalize (per-bell channels may differ via cal file).
void BellTracker::send_program_changes() {
    bool sent[16] = {};
    int def = (channel_override_ >= 0) ? channel_override_ : DEFAULT_BELL_CHANNEL;
    post_pc(def, GM_PROGRAM_GLOCKENSPIEL);
    sent[def & 0xF] = true;
    for (int i = 0; i < n_bells_; ++i) {
        int ch = bells_[i].channel & 0xF;
        if (!sent[ch] && ch != SYSTEM_CHANNEL) {
            post_pc(ch, GM_PROGRAM_GLOCKENSPIEL);
            sent[ch] = true;
        }
    }
}

// ── cal data persistence ──────────────────────────────────────────────────────
//
// Plain-text format, one bell per [bell] block. Text rather than binary so
// it's editable/diffable by hand if a bell needs manual correction between
// sessions (e.g. after re-tuning). Run only off the RT thread (save: detached
// thread from finalize_cal; load: init(), before JACK is activated).
//
// File layout:
//   BELLTRACKER_CAL_V1
//   n_bells=<N>
//   [bell]
//   idx=<i>
//   freq_hz=<f>
//   cents=<f>
//   midi_note=<i>
//   type=BELL|CHIME
//   attack_ms=<f>
//   template=<f>,<f>,...,<f>      (N values, comma-separated)
//   [bell]
//   ...

// ── note-name helpers ─────────────────────────────────────────────────────────
// Scientific pitch notation: MIDI 60 = C4 (middle C), octave = midi/12 - 1.
// This matches the label cast on a physical handbell: the bell stamped "C5"
// sounds ~523 Hz = MIDI 72 = "C5" here. (Handbell *written* notation is a
// transposing convention — written C4 sounds C5 — but belltracker deals in
// sounding pitch, so SPN keeps names aligned with the castings.)
static const char* NOTE_NAMES[12] =
    { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

static void midi_to_name(int midi, char* out, size_t n) {
    if (midi < 0 || midi > 127) { snprintf(out, n, "?"); return; }
    snprintf(out, n, "%s%d", NOTE_NAMES[midi % 12], midi / 12 - 1);
}

// Accepts C4, c#5, Db3, F#-1, etc. Returns MIDI 0-127, or -1 on parse failure.
static int name_to_midi(const char* name) {
    if (!name || !name[0]) return -1;
    int i = 0;
    char letter = toupper((unsigned char)name[i++]);
    static const int BASE[7] = { 9, 11, 0, 2, 4, 5, 7 };  // A B C D E F G
    if (letter < 'A' || letter > 'G') return -1;
    int semi = BASE[letter - 'A'];
    if (name[i] == '#')      { semi += 1; ++i; }
    else if (name[i] == 'b') { semi -= 1; ++i; }
    char* end = nullptr;
    long octave = strtol(name + i, &end, 10);
    if (end == name + i || *end != '\0') return -1;
    long midi = (octave + 1) * 12 + semi;
    return (midi >= 0 && midi <= 127) ? (int)midi : -1;
}

void BellTracker::save_cal_to_file(const std::string& path,
                                    const std::vector<BellData>& snapshot,
                                    int n_bells_snapshot) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        fprintf(stderr, "belltracker: could not write cal file %s\n", path.c_str());
        return;
    }
    f << std::setprecision(9);
    f << "BELLTRACKER_CAL_V1\n";
    f << "n_bells=" << n_bells_snapshot << "\n";
    for (int i = 0; i < n_bells_snapshot; ++i) {
        const BellData& b = snapshot[i];
        f << "[bell]\n";
        f << "idx="       << i                                                   << "\n";
        f << "freq_hz="   << b.freq_hz                                           << "\n";
        f << "cents="     << b.cents                                             << "\n";
        f << "midi_note=" << b.midi_note                                         << "\n";
        char nn[8]; midi_to_name(b.midi_note, nn, sizeof(nn));
        f << "note_name=" << nn                                                  << "\n";
        f << "channel="   << b.channel                                           << "\n";
        f << "type="      << (b.type == InstrumentType::BELL ? "BELL" : "CHIME") << "\n";
        f << "attack_ms=" << b.attack_ms                                         << "\n";
        f << "template=";
        for (size_t k = 0; k < b.nmf_template.size(); ++k) {
            if (k) f << ",";
            f << b.nmf_template[k];
        }
        f << "\n";
    }
    f.close();
    printf("belltracker: cal data saved to %s (%d bells)\n",
           path.c_str(), n_bells_snapshot);
}

bool BellTracker::load_cal_from_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    f.close();

    if (lines.empty() || lines[0] != "BELLTRACKER_CAL_V1") {
        fprintf(stderr, "belltracker: %s is not a valid cal file\n", path.c_str());
        return false;
    }

    int n_bells_expected = 0;
    if (lines.size() < 2 ||
        sscanf(lines[1].c_str(), "n_bells=%d", &n_bells_expected) != 1 ||
        n_bells_expected <= 0 || n_bells_expected > MAX_BELLS) {
        fprintf(stderr, "belltracker: %s has missing/invalid n_bells\n", path.c_str());
        return false;
    }

    std::vector<BellData> loaded;
    try {
        size_t i = 2;
        while (i < lines.size()) {
            if (lines[i] != "[bell]") { ++i; continue; }
            ++i;

            BellData b;
            int  idx = -1;
            char type_str[16] = {};
            char name_str[8]  = {};

            while (i < lines.size() && lines[i] != "[bell]") {
                const std::string& l = lines[i];
                if      (sscanf(l.c_str(), "idx=%d",       &idx)        == 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "freq_hz=%f",   &b.freq_hz)  == 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "cents=%f",     &b.cents)    == 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "midi_note=%d", &b.midi_note)== 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "note_name=%7s", name_str)    == 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "channel=%d",   &b.channel)   == 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "attack_ms=%f", &b.attack_ms)== 1) { ++i; continue; }
                else if (sscanf(l.c_str(), "type=%15s",    type_str)    == 1) {
                    b.type = (strcmp(type_str, "BELL") == 0) ? InstrumentType::BELL
                                                              : InstrumentType::CHIME;
                    ++i; continue;
                } else if (l.rfind("template=", 0) == 0) {
                    std::stringstream ss(l.substr(9));
                    std::string tok;
                    while (std::getline(ss, tok, ','))
                        b.nmf_template.push_back(std::stof(tok));
                    ++i; continue;
                }
                ++i; // unrecognized line — skip
            }

            if (idx < 0 || b.nmf_template.empty()) {
                fprintf(stderr, "belltracker: malformed bell entry in %s, aborting load\n",
                        path.c_str());
                return false;
            }

            if (b.channel < 0 || b.channel >= SYSTEM_CHANNEL) {
                fprintf(stderr, "belltracker: bell %d channel %d invalid "
                        "(0-%d), using %d\n", idx, b.channel,
                        SYSTEM_CHANNEL - 1, DEFAULT_BELL_CHANNEL);
                b.channel = DEFAULT_BELL_CHANNEL;
            }

            // midi_note absent or 0 in the file — fall back to note_name
            if (b.midi_note <= 0) {
                int from_name = name_to_midi(name_str);
                if (from_name < 0) {
                    fprintf(stderr, "belltracker: bell %d in %s has no usable "
                            "midi_note or note_name ('%s'), aborting load\n",
                            idx, path.c_str(), name_str);
                    return false;
                }
                b.midi_note = from_name;
                printf("belltracker: bell %d midi_note from note_name %s -> %d\n",
                       idx, name_str, from_name);
            }
            loaded.push_back(std::move(b));
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "belltracker: error parsing %s (%s), aborting load\n",
                path.c_str(), e.what());
        return false;
    }

    if ((int)loaded.size() != n_bells_expected) {
        fprintf(stderr, "belltracker: %s declared %d bells but found %zu, aborting load\n",
                path.c_str(), n_bells_expected, loaded.size());
        return false;
    }
    for (auto& b : loaded) {
        if ((int)b.nmf_template.size() != n_bells_expected) {
            fprintf(stderr, "belltracker: %s has a template of wrong length, aborting load\n",
                    path.c_str());
            return false;
        }
    }

    bells_     = std::move(loaded);
    n_bells_   = n_bells_expected;
    cal_armed_ = true;
    return true;
}
