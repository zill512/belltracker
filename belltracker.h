#pragma once
// belltracker.h — Handbell MIDI tracker for Raspberry Pi 5 + Steinberg UR22mkII
// Architecture:
//   Cal phase : JACK audio → rolling pre-onset ring buffer (100ms)
//               On onset: drain ring buffer, capture 500ms sustain
//               Measure attack time → auto-classify bell vs chime per note
//               YIN fundamental on sustain window
//               FFT of full capture → real NMF spectral templates built on-Pi
//               Auto-terminates when bell 1 is repeated (armed after first bell)
//               Confirmation pings sent via off-RT worker thread (no xruns)
//   Perf phase: Goertzel bank (one filter per registered bell fundamental)
//               NMF projection → per-bell activation
//               ALSA sequencer MIDI note on/off → UR22mkII DIN out

#include <jack/jack.h>
#include <alsa/asoundlib.h>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <utility>
#include <cstdio>

// ── debug instrumentation (--debug / -d) ─────────────────────────────────────
// Gated diagnostic printfs: state transitions, waiting heartbeat with live RMS,
// per-strike analysis detail, throttled perf-phase NMF activations.
// NOTE: DBG fires from the JACK RT callback in several places. Emissions are
// rare events or throttled to <=1 Hz, and stdout is line-buffered (main.cpp),
// but printf from the RT thread can still occasionally cost an xrun — debug
// mode is for bench diagnosis, not performances.
extern bool g_debug;
#define DBG(...) do { if (g_debug) { printf("[dbg] " __VA_ARGS__); } } while (0)
static constexpr double DBG_WAIT_PERIOD_S = 2.0;   // waiting-heartbeat interval
static constexpr double DBG_PERF_PERIOD_S = 1.0;   // NMF activation dump interval

// ── tuneable constants ──────────────────────────────────────────────────────
static constexpr int    MAX_BELLS           = 20;
static constexpr float  REPEAT_CENTS_TOL    = 20.0f;
static constexpr float  ONSET_THRESHOLD     = 0.02f;
static constexpr float  NMF_THRESHOLD       = 0.25f;
static constexpr float  NOTE_OFF_DECAY_S    = 2.5f;
static constexpr int    GOERTZEL_WINDOW     = 128;
static constexpr int    SAMPLE_RATE         = 48000;

// Cal capture timing
static constexpr int    CAL_PRE_RING_MS     = 100;
static constexpr int    CAL_CAPTURE_MS      = 500;
static constexpr int    CAL_PRE_RING_SAMP   = SAMPLE_RATE * CAL_PRE_RING_MS  / 1000; // 4800
static constexpr int    CAL_CAPTURE_SAMP    = SAMPLE_RATE * CAL_CAPTURE_MS   / 1000; // 24000

// Attack classifier threshold
static constexpr float  ATTACK_BELL_MS      = 25.0f;

// FFT for template building — next power of 2 >= PRE_RING + CAPTURE (28800 → 32768)
static constexpr int    TEMPLATE_FFT_SIZE   = 32768;

// Cal-complete sequence: each registered bell plays in turn rather than as a
// simultaneous chord — easier to pick out individual notes by ear, and scales
// better as bell count grows (a 20-note chord is mostly noise; a 20-note
// arpeggio is still legible). The same timing (ARPEGGIO_NOTE_MS/GAP_MS) is
// reused for the per-bell confirmation ack below.
static constexpr int    ARPEGGIO_NOTE_MS    = 400;   // sound duration per note
static constexpr int    ARPEGGIO_GAP_MS     = 100;   // silence after each note
                                                       // before the next (together:
                                                       // 0.5s onset-to-onset spacing)

// Per-bell confirmation during cal is deliberately delayed and repeated: wait
// BELL_CONFIRM_DELAY_MS after registration, then play the bell's note 3 times
// at arpeggio timing — easier to identify by ear than a single brief ping,
// and the delay puts clear air between the strike's own decay and the ack.
// Independent of BYPASS_ACK_DELAY_MS below even though currently the same
// value — different gestures, no reason to couple their timing.
static constexpr int    BELL_CONFIRM_DELAY_MS = 1000;
static constexpr int    BELL_CONFIRM_REPEATS  = 3;

// Periodic "waiting for input" cue — repeats every READY_PING_INTERVAL_S while
// no audio has been struck yet, stops permanently the moment the first onset
// is detected (see heard_first_input_ in cal_process()).
static constexpr double READY_PING_INTERVAL_S = 5.0;

// MIDI output channels/voice — all bells default to a single channel; the cal
// file stores each bell's channel (editable), and --channel=N overrides both.
// Channel 15 stays reserved for system prompts.
static constexpr int    DEFAULT_BELL_CHANNEL      = 0;
static constexpr int    GM_PROGRAM_GLOCKENSPIEL   = 9;   // GM patch 10 (0-indexed)

// NMF deconvolution (perf phase)
static constexpr int    NMF_ITERS           = 4;      // multiplicative-update iterations per window
static constexpr float  NMF_EPS             = 1e-9f;  // denominator floor

// Cal data persistence — skip cal phase on subsequent runs
static constexpr const char* CAL_FILE_DEFAULT = "belltracker_cal.dat";

// ── WAV recording (perf phase → USB drive) ───────────────────────────────────
// Lock-free SPSC ring buffer between the RT callback (writer) and a dedicated
// disk-write thread (reader). Power-of-2 size enables masking instead of
// modulo. 262144 samples ≈ 5.5s — well above the 2s flush interval, giving
// ample headroom for short disk stalls.
static constexpr uint32_t RECORD_RING_SAMPS  = 1u << 18;  // 262144 samples, 512KB
static constexpr uint32_t RECORD_RING_MASK   = RECORD_RING_SAMPS - 1;
static constexpr int      RECORD_FLUSH_SECS  = 2;    // disk write interval
static constexpr int      RECORD_FSYNC_EVERY = 5;    // fsync every N write chunks

enum class RecordState { DISABLED, READY, RECORDING };

// ── Damped-strike cal bypass ──────────────────────────────────────────────────
// Bypass gesture: ringer grasps the bell's brass casting directly while/just
// after striking, rather than striking-then-palm-muting. The casting is held,
// not the clapper, so almost no free vibration develops — decay is much
// shorter than even a deliberate palm-mute. Both constants need empirical
// tuning against real grasped-casting strikes vs. normal free strikes (same
// workflow used to tune ATTACK_BELL_MS/ONSET_THRESHOLD).
static constexpr float  DECAY_RELEASE_FRAC  = 0.15f;   // fraction of peak RMS = "released"
static constexpr float  DAMPED_DECAY_MS     = 40.0f;   // below this peak→release time = damped

// Audible ack for the bypass gesture is deliberately delayed after detection —
// see the bypass handling in analyze_bell() for why.
static constexpr int    BYPASS_ACK_DELAY_MS = 1000;

// ── Headless MIDI status prompts ──────────────────────────────────────────────
// belltracker has no display in deployment — MIDI is the only UI. Channel 16
// (index 15) is reserved exclusively for these system prompts and is never
// assigned to a bell (bell_channel() wraps across 0-14 instead of 0-15, so
// up to 15 simultaneous bell channels remain — still well above any handbell
// choir's real polyphony). Each prompt is a short ping or ping-pattern on a
// fixed note, distinguishable by ear from bell confirmation pings (different
// channel) and from each other (note number / repetition pattern).
static constexpr int    SYSTEM_CHANNEL           = 15;  // reserved, never used for bells
static constexpr int    PROMPT_NOTE_READY        = 1;   // single ping: audio connected, armed
static constexpr int    PROMPT_NOTE_BYPASS       = 2;   // single ping: damped strike recognized
static constexpr int    PROMPT_NOTE_BYPASS_FAIL  = 3;   // triple ping: bypass requested, no saved cal
static constexpr int    PROMPT_VEL               = 100;
static constexpr int    PROMPT_DUR_MS            = 150;
static constexpr int    PROMPT_FAIL_DUR_MS       = 80;  // shorter/faster = reads as "error" by ear

enum class SystemPrompt { READY, CAL_BYPASS, CAL_BYPASS_FAILED };

// ── Goertzel state ──────────────────────────────────────────────────────────
struct GoertzelState {
    float coeff, s1, s2, freq_hz;

    void init(float freq, int window) {
        freq_hz = freq;
        float k = freq / ((float)SAMPLE_RATE / window);
        coeff = 2.0f * cosf(2.0f * M_PI * k / window);
        s1 = s2 = 0.0f;
    }
    void reset() { s1 = s2 = 0.0f; }
    void feed(float x) { float s0 = x + coeff*s1 - s2; s2=s1; s1=s0; }
    float power() const { return s1*s1 + s2*s2 - coeff*s1*s2; }
};

// ── instrument type ──────────────────────────────────────────────────────────
enum class InstrumentType { UNKNOWN, BELL, CHIME };

// ── per-bell sounding state (perf phase) ──────────────────────────────────────
enum class NoteState { SILENT, SOUNDING };

// ── per-bell data ────────────────────────────────────────────────────────────
struct BellData {
    float          freq_hz         = 0.0f;
    float          cents           = 0.0f;
    int            midi_note       = 0;
    InstrumentType type            = InstrumentType::UNKNOWN;
    float          attack_ms       = 0.0f;
    GoertzelState  goertzel;
    std::vector<float> nmf_template;
    std::vector<float> cal_buf;
    NoteState      note_state      = NoteState::SILENT;
    double         last_onset_time = 0.0;
    float          h_nmf           = 1.0f;  // warm-start activation for iterative NMF
    int            channel         = DEFAULT_BELL_CHANNEL;  // MIDI out channel (from cal file / --channel)
};

// ── MIDI worker command ──────────────────────────────────────────────────────
// Represents a note-on + hold + note-off (+ optional trailing silence)
// sequence to be executed off the RT thread. note == -1 is the poison pill
// (shut down worker).
struct MidiCmd {
    int  note;          // MIDI note number, or -1 for shutdown
    int  channel;       // 0-15
    int  velocity;      // 0-127
    int  duration_ms;   // hold time between note-on and note-off
    int  gap_ms = 0;    // extra silence after note-off, before the next queued
                         // command runs — lets back-to-back post_ping() calls
                         // form an evenly-spaced arpeggio instead of a
                         // back-to-back run with no gap between notes
    int  program = -1;  // >=0: program-change event on `channel`
                         // (note/velocity/duration/gap ignored). Deliberately
                         // LAST so existing positional 4/5-arg inits are unaffected.
};

// ── MIDI worker state machine ─────────────────────────────────────────────────
// Runs entirely in midi_worker_run() (off the RT thread). IDLE dequeues the
// next command; HOLDING_NOTE is mid-ping (note-on sent, waiting out the hold,
// then any trailing gap_ms, before note-off→IDLE). Bare perf note-on/off
// events never leave IDLE.
enum class MidiWorkerState { IDLE, HOLDING_NOTE };

// ── cal phase sub-state machine ───────────────────────────────────────────────
// AWAITING_STRIKE : listening for an onset (rolling RMS above threshold)
// CAPTURING       : mid-capture, draining samples into the active bell's buffer
// BELL_CONFIRM_PENDING : the 1s window before a newly registered bell's
//                   triple-ping confirmation fires — see analyze_bell()
// BYPASS_LOADING  : transient — entered/exited synchronously within
//                   analyze_bell() the instant a damped first strike is
//                   recognized; never observed persisting across a callback
// BYPASS_ACK_PENDING : the 1s window before the bypass's audible ack fires;
//                   RMS/pre-ring buffers keep running, but no new capture
//                   starts while in this state — see cal_process()
enum class CalSubState { AWAITING_STRIKE, CAPTURING, BELL_CONFIRM_PENDING,
                          BYPASS_LOADING, BYPASS_ACK_PENDING };

// ── app states ───────────────────────────────────────────────────────────────
enum class AppState { CAL, PERF };

// ── main class ───────────────────────────────────────────────────────────────
class BellTracker {
public:
    BellTracker();
    ~BellTracker();

    bool init(const std::string& load_cal_path = "",
              const std::string& save_cal_path = CAL_FILE_DEFAULT,
              int channel_override = -1);
    void process(const float* in, jack_nframes_t nframes);
    void notify_ready();    // audible "armed" cue after audio connects
    void stop_recording();  // flush + close WAV; safe to call from shutdown handlers

    jack_port_t*    input_port   = nullptr;
    jack_client_t*  jack_client_ = nullptr;

private:
    // ALSA sequencer
    snd_seq_t*  seq_         = nullptr;
    int         seq_port_    = -1;
    int         ur22_client_ = -1;
    int         ur22_port_   = -1;

    // app state
    AppState    state_       = AppState::CAL;

    // bell registry
    std::vector<BellData> bells_;
    int         n_bells_     = 0;
    bool        cal_armed_   = false;

    // pre-onset ring buffer
    float       pre_ring_[CAL_PRE_RING_SAMP] = {};
    int         pre_ring_pos_  = 0;
    bool        pre_ring_full_ = false;

    // cal sub-state machine
    CalSubState cal_state_     = CalSubState::AWAITING_STRIKE;
    int         onset_counter_ = 0;

    // periodic "waiting for input" ping — see READY_PING_INTERVAL_S
    double      last_ready_ping_s_ = 0.0;
    bool        heard_first_input_ = false;

    // debug instrumentation throttles (--debug)
    double      dbg_wait_last_s_ = 0.0;
    double      dbg_perf_last_s_ = 0.0;
    // logged cal sub-state transition — all writes to cal_state_ go through
    // this so --debug traces every transition with names
    void        set_cal_state(CalSubState s);

    // BYPASS_ACK_PENDING data — populated by analyze_bell() when entering the
    // state, consumed by cal_process() when the deadline passes. Living as
    // member state (not captured by a thread/lambda) is what lets the ack
    // delay be expressed as an ordinary state transition instead of an
    // ad hoc timer thread.
    double      bypass_ack_deadline_s_ = 0.0;
    bool        bypass_loaded_         = false;
    std::vector<std::pair<int,int>> bypass_chord_notes_;  // (midi_note, channel)

    // BELL_CONFIRM_PENDING data — same pattern as the bypass ack above, for
    // the per-bell delayed/repeated confirmation during normal cal.
    double      confirm_deadline_s_      = 0.0;
    int         confirm_note_            = 0;
    int         confirm_channel_         = 0;
    bool        confirm_then_finalize_   = false;  // this was the bell that
                                                     // hit MAX_BELLS — call
                                                     // finalize_cal() once the
                                                     // confirmation has played

    // rolling RMS
    float       rms_buf_[256] = {};
    int         rms_pos_      = 0;

    // perf state
    int         goertzel_pos_ = 0;

    // ── WAV recording ─────────────────────────────────────────────────────────
    RecordState      record_state_    = RecordState::DISABLED;
    std::string      record_dir_;             // USB mount path, empty = disabled
    int              record_file_num_ = 1;    // next file number (scanned at init)
    int              record_fd_       = -1;   // POSIX file descriptor
    uint32_t         record_samples_  = 0;    // total samples written this file

    // Lock-free SPSC ring: RT thread (perf_process) is sole writer; record_writer_
    // is sole reader. Unsigned 32-bit indices wrap naturally — subtraction remains
    // correct across wrap-around, and & RECORD_RING_MASK gives the array slot.
    int16_t          record_ring_[RECORD_RING_SAMPS];
    std::atomic<uint32_t> record_ring_w_{0};   // write index (RT only)
    std::atomic<uint32_t> record_ring_r_{0};   // read index (write thread only)

    std::thread           record_writer_;
    std::atomic<bool>     record_stop_{false};

    void    find_usb_drive();
    void    find_next_file_num();
    void    start_recording();         // opens file, starts write thread; no-op if DISABLED
    void    close_recording_file();    // patches header, fsyncs, closes; called by write thread
    void    record_push(const float* buf, int n);  // RT-safe: lock-free ring push
    void    record_writer_run();

    // cal data persistence
    std::string save_cal_path_ = CAL_FILE_DEFAULT;
    void        save_cal_to_file(const std::string& path,
                                  const std::vector<BellData>& snapshot,
                                  int n_bells_snapshot);
    bool        load_cal_from_file(const std::string& path);

    // NMF template Gram matrix (T^T T), built once on first perf window since
    // templates are fixed after cal — avoids recomputing an O(n_bells^2) inner
    // product every GOERTZEL_WINDOW (~2.7ms) for no reason.
    float       nmf_gram_[MAX_BELLS][MAX_BELLS] = {};
    bool        nmf_gram_ready_ = false;
    void        build_nmf_gram();

    // ── MIDI worker thread ──────────────────────────────────────────────────
    // RT thread posts MidiCmds; worker executes them with blocking sleeps.
    // Queue is also used for perf note-on/off (duration_ms == 0 → no sleep,
    // note-off posted separately).
    std::queue<MidiCmd>     midi_queue_;
    std::mutex              midi_mutex_;
    std::condition_variable midi_cv_;
    std::thread             midi_worker_;
    void    midi_worker_run();
    void    post_midi(MidiCmd cmd);          // safe from RT and non-RT
    void    post_ping(int note, int ch, int vel, int dur_ms, int gap_ms = 0); // note-on+hold+off(+gap)
    void    post_pc(int channel, int program);   // queue a program change
    void    send_program_changes();              // PC (glockenspiel) on all bell channels
    int     channel_override_ = -1;              // --channel=N; -1 = use cal file / default

    // ── internal helpers ──
    void    cal_process(const float* in, jack_nframes_t nframes);
    void    perf_process(const float* in, jack_nframes_t nframes);
    void    start_capture();
    void    analyze_bell(BellData& b);
    float   measure_attack_ms(const std::vector<float>& buf);
    float   measure_decay_ms(const std::vector<float>& buf);
    float   yin_fundamental(const float* buf, int len, float sr);
    void    build_nmf_templates();
    void    radix2_fft(float* re, float* im, int n);
    float   freq_to_cents(float freq);
    int     freq_to_midi(float freq);
    void    finalize_cal();
    void    midi_note_on(int note, int channel, int vel = 100);
    void    midi_note_off(int note, int channel);
    void    midi_program_change(int channel, int program);
    void    post_system_prompt(SystemPrompt p);
    int     bell_channel(int i) const { return bells_[i].channel; }
    bool    open_midi();
    void    find_ur22_port();
};
