// belltracker_perf.cpp — performance phase: Goertzel bank + iterative NMF deconvolution + MIDI
#include "belltracker.h"
#include <cstdio>
#include <cstring>

// ── build_nmf_gram ───────────────────────────────────────────────────────────
// Computes the n_bells × n_bells template Gram matrix (T_b · T_b') once, the
// first time perf_process runs. Templates are fixed after cal (built from FFT
// of cal captures in finalize_cal), so this is invariant for the whole perf
// session — no reason to recompute it every 2.7ms window.
void BellTracker::build_nmf_gram() {
    for (int b = 0; b < n_bells_; ++b) {
        for (int b2 = 0; b2 <= b; ++b2) {
            float d = 0.0f;
            for (int k = 0; k < n_bells_; ++k)
                d += bells_[b].nmf_template[k] * bells_[b2].nmf_template[k];
            nmf_gram_[b][b2] = nmf_gram_[b2][b] = d;
        }
    }
    nmf_gram_ready_ = true;
}

void BellTracker::perf_process(const float* in, jack_nframes_t nframes) {
    if (!nmf_gram_ready_) build_nmf_gram();

    // stream audio to USB recording (lock-free, no-op if DISABLED/READY)
    if (record_state_ == RecordState::RECORDING)
        record_push(in, (int)nframes);

    const double jack_time_s =
        (double)jack_last_frame_time(jack_client_) / SAMPLE_RATE;

    for (jack_nframes_t i = 0; i < nframes; ++i) {
        float x = in[i];

        for (int b = 0; b < n_bells_; ++b)
            bells_[b].goertzel.feed(x);

        if (++goertzel_pos_ >= GOERTZEL_WINDOW) {
            goertzel_pos_ = 0;

            // snapshot Goertzel energy vector V (observed spectrum this window)
            float V[MAX_BELLS] = {};
            float total = 0.0f;
            for (int b = 0; b < n_bells_; ++b) {
                V[b]  = bells_[b].goertzel.power();
                total += V[b];
                bells_[b].goertzel.reset();
            }
            if (total < 1e-9f) continue;

            // ── Iterative multiplicative-update NMF deconvolution ──────────
            // Old approach: H[b] = (V · T_b) / (||T_b|| * total) — a single-step
            // cosine-similarity projection. This works fine for isolated single
            // strikes but breaks down for simultaneous onsets: when two bells'
            // templates overlap in shared Goertzel bins (real templates, not
            // identity vectors), each bell's score is inflated by leakage from
            // the other, and `total` in the denominator further dilutes both
            // scores during co-sounding passages — exactly the polyphonic case
            // we care about.
            //
            // New approach: solve V ≈ T·H for the activation vector H via a
            // few Lee-Seung multiplicative-update iterations, with T fixed
            // (the template matrix, columns = each bell's spectral fingerprint).
            // This deconvolves overlapping templates instead of just measuring
            // raw overlap, so two bells ringing together each converge toward
            // their true individual activation rather than a blended score.
            // Warm-starting H from the previous window's converged value
            // (bells sustain over many windows) speeds convergence and keeps
            // continuity between windows.
            float H[MAX_BELLS];
            for (int b = 0; b < n_bells_; ++b)
                H[b] = (bells_[b].h_nmf > 1e-6f) ? bells_[b].h_nmf : 1.0f;

            float WtV[MAX_BELLS];
            for (int b = 0; b < n_bells_; ++b) {
                float dot = 0.0f;
                for (int k = 0; k < n_bells_; ++k)
                    dot += bells_[b].nmf_template[k] * V[k];
                WtV[b] = dot;
            }

            for (int it = 0; it < NMF_ITERS; ++it) {
                float Hnew[MAX_BELLS];
                for (int b = 0; b < n_bells_; ++b) {
                    float denom = NMF_EPS;
                    for (int b2 = 0; b2 < n_bells_; ++b2)
                        denom += nmf_gram_[b][b2] * H[b2];
                    Hnew[b] = H[b] * (WtV[b] / denom);
                }
                memcpy(H, Hnew, sizeof(float) * n_bells_);
            }

            // throttled activation dump: top-3 normalized activations
            if (g_debug && jack_time_s - dbg_perf_last_s_ >= DBG_PERF_PERIOD_S) {
                int t[3] = { -1, -1, -1 };
                for (int b = 0; b < n_bells_; ++b) {
                    if      (t[0] < 0 || H[b] > H[t[0]]) { t[2]=t[1]; t[1]=t[0]; t[0]=b; }
                    else if (t[1] < 0 || H[b] > H[t[1]]) { t[2]=t[1]; t[1]=b; }
                    else if (t[2] < 0 || H[b] > H[t[2]]) { t[2]=b; }
                }
                char line[128];
                int  n = snprintf(line, sizeof(line), "b%d=%.3f",
                                  t[0]+1, H[t[0]]/total);
                if (t[1] >= 0)
                    n += snprintf(line+n, sizeof(line)-n, "  b%d=%.3f",
                                  t[1]+1, H[t[1]]/total);
                if (t[2] >= 0 && n < (int)sizeof(line))
                    snprintf(line+n, sizeof(line)-n, "  b%d=%.3f",
                             t[2]+1, H[t[2]]/total);
                DBG("perf total=%.5f  thr=%.2f  top: %s\n",
                    total, NMF_THRESHOLD, line);
                dbg_perf_last_s_ = jack_time_s;
            }

            // normalize by total observed energy so NMF_THRESHOLD keeps
            // roughly the same scale/meaning it had under the old metric for
            // isolated strikes (near-orthogonal templates → H[b] ≈ V[b]).
            // Re-verify NMF_THRESHOLD empirically against real polyphonic
            // recordings — the deconvolved scale won't be identical to the
            // old cosine-projection scale in dense passages.
            for (int b = 0; b < n_bells_; ++b) {
                bells_[b].h_nmf = H[b];
                float Hn = H[b] / total;

                if (Hn >= NMF_THRESHOLD && bells_[b].note_state == NoteState::SILENT) {
                    bells_[b].note_state      = NoteState::SOUNDING;
                    bells_[b].last_onset_time = jack_time_s;
                    post_midi({ bells_[b].midi_note, bell_channel(b), 100, 0 });
                    printf("NOTE ON  bell %2d  MIDI %3d  ch %2d  H=%.3f\n",
                           b+1, bells_[b].midi_note, bell_channel(b), Hn);
                }

                if (bells_[b].note_state == NoteState::SOUNDING && Hn >= NMF_THRESHOLD * 0.5f)
                    bells_[b].last_onset_time = jack_time_s;
            }

            // auto note-off
            for (int b = 0; b < n_bells_; ++b) {
                if (bells_[b].note_state == NoteState::SOUNDING &&
                    (jack_time_s - bells_[b].last_onset_time) > NOTE_OFF_DECAY_S) {
                    bells_[b].note_state = NoteState::SILENT;
                    post_midi({ bells_[b].midi_note, bell_channel(b), 0, 0 });
                    printf("NOTE OFF bell %2d  MIDI %3d  ch %2d\n",
                           b+1, bells_[b].midi_note, bell_channel(b));
                }
            }
        }
    }
}
