# belltracker

Real-time handbell and choirchime MIDI detection and recording for Raspberry Pi + Steinberg UR22mkII.

Captures live audio from a microphone or direct input, identifies each bell or chime strike by pitch and instrument type, and outputs MIDI note events in real time via the UR22mkII's DIN MIDI output. Also records the full performance to a WAV file on a USB drive if one is present. Designed to run entirely headless — no display, no keyboard, no mouse. All operator feedback is delivered through MIDI.

See **GUIDE.md** for operator instructions (no technical background required).

---

## Hardware

| Item | Notes |
|---|---|
| Raspberry Pi 5 (or 4) | CMakeLists targets `-mcpu=cortex-a76` (Pi 5); Pi 4 users change to `cortex-a72` |
| Steinberg UR22mkII | USB audio interface; provides audio input and MIDI DIN output |
| ALSA device name | `hw:UR22mkII` — do not use `hw:UR22C` or `hw:UR22CmkII` |
| Microphone or direct input | Into UR22mkII input 1 |
| MIDI cable | From UR22mkII MIDI OUT to synth or sound module |
| USB drive (optional) | For WAV recording — auto-detected at startup |

---

## Quick start

```bash
# Build
cd ~/Belltracker
./setup.sh          # or: mkdir -p build && cd build && cmake .. && make

# Run JACK
jackd -d alsa -d hw:UR22mkII -r 48000 -p 256 -n 3 &

# Run (normal calibration)
./build/belltracker

# Run (skip calibration, load previous session's bell data)
./build/belltracker --load-cal

# Help
./build/belltracker --help
```

---

## Architecture

### State machines

The entire application is built around three explicit state machines. No ad hoc flags or boolean mode variables.

**`AppState`** — top-level mode:
```
CAL  →  PERF
```
Transitions via `finalize_cal()` (bell-1 repeat), the damped-strike bypass gesture, or `--load-cal` at startup. Once in PERF, never returns to CAL without a restart.

**`CalSubState`** — cal phase sub-states:
```
AWAITING_STRIKE
    │ onset detected
    ▼
CAPTURING  (500ms)
    │ capture complete
    ▼
analyze_bell()
    ├─ damped first strike  → BYPASS_LOADING (transient) → BYPASS_ACK_PENDING
    │                                                           │ 1s deadline
    │                                                           ▼
    │                                              success: PERF  /  fail: AWAITING_STRIKE
    │
    ├─ normal registration  → BELL_CONFIRM_PENDING
    │                              │ 1s deadline
    │                              ▼
    │                         AWAITING_STRIKE  (or finalize_cal if max bells)
    │
    └─ bell-1 repeat        → finalize_cal() → PERF
```

`BYPASS_LOADING` is transient — entered and exited synchronously within a single sample's handling. Both `BYPASS_ACK_PENDING` and `BELL_CONFIRM_PENDING` poll a stored JACK-time deadline each callback rather than sleeping a thread — keeps all shared state single-writer with no locking.

**`MidiWorkerState`** — MIDI worker thread:
```
IDLE  →  HOLDING_NOTE  →  IDLE
```
The worker dequeues `MidiCmd` structs posted by the RT callback. `IDLE` sends bare note-on/off events immediately. `HOLDING_NOTE` sleeps out the hold time, then sends note-off and any trailing gap, then returns to `IDLE`.

**`NoteState`** per bell — perf phase sounding state:
```
SILENT  →  SOUNDING  →  SILENT
```

### Mid-buffer state handoff

If `AppState` flips from `CAL` to `PERF` mid-JACK-buffer (e.g., bell-1 repeat detected at sample 47 of a 256-sample callback), the remaining 209 samples in that same callback are immediately handed to `perf_process()`. No samples are processed under a stale state, and none are dropped.

---

## Calibration phase

### Signal path

1. **Pre-onset ring buffer** (100ms, `CAL_PRE_RING_SAMP` = 4800 samples) — always running, so the attack transient is captured even though onset is detected after it has begun.
2. **Rolling RMS onset detection** (256-sample window, `ONSET_THRESHOLD` = 0.02) — also always running so it stays warm through captures and `BELL_CONFIRM_PENDING` periods.
3. **500ms sustain capture** (`CAL_CAPTURE_MS`) into `BellData::cal_buf`.
4. **`analyze_bell()`** — runs synchronously on the RT thread. Does not block on MIDI I/O (all MIDI is posted to the worker queue). Does block briefly on FFT and file I/O at cal-end — an accepted one-time exception masked by the cal-complete arpeggio.

### Bypass gesture (bell 1 only)

If the very first strike of a session decays in under `DAMPED_DECAY_MS` (40ms) — measured peak-to-release by `measure_decay_ms()` — the tracker interprets it as a cal-bypass command rather than a note registration. The technique is grasping the bell's brass casting while or just after striking, killing the ring almost immediately.

On bypass detection:
- `load_cal_from_file()` runs synchronously (same RT-thread exception as the FFT step).
- On success: Goertzel filters re-initialized, `AppState` flips to `PERF`, recording starts.
- State transitions to `BYPASS_ACK_PENDING`; the 1-second ack delay is a polled deadline, not a thread sleep.
- On deadline: bypass ping posted, then cal-complete arpeggio (success) or triple-ping error + return to `AWAITING_STRIKE` (failure).

### Instrument classification

`measure_attack_ms()` finds the time from onset to peak RMS in the capture buffer.
- Attack < `ATTACK_BELL_MS` (25ms) → `InstrumentType::BELL`
- Attack ≥ 25ms → `InstrumentType::CHIME`

### Pitch detection (YIN)

4096-sample window, starting **250ms after onset** for bells and chimes alike
(`PITCH_SKIP_MS`). The strike transient — clapper impact, broadband hash,
inharmonic partials settling — corrupts fundamental estimation; by 250ms the
hum/prime tones dominate. The 500ms sustain capture leaves 12000 samples after
the skip, ~3× the YIN window. A strike damped shorter than ~250ms will analyze
mostly room noise and fail the 60–8000Hz range check → re-strike (CAL wants
free, ringing strikes anyway).

### NMF template building

At `finalize_cal()`: a 32768-point radix-2 FFT (Hann-windowed, zero-padded) of each bell's full capture (pre-ring + sustain) builds a real spectral fingerprint per bell. Templates are normalized to unit L1 norm and stored in `BellData::nmf_template[n_bells]`. This is what makes simultaneous-onset separation possible in the perf phase — templates are derived from the actual acoustic character of each instrument in the room, not from ideal tuning tables.

### Per-bell confirmation

After each successful registration, `cal_state_` moves to `BELL_CONFIRM_PENDING`. When `confirm_deadline_s_` (now + `BELL_CONFIRM_DELAY_MS` = 1000ms) is reached, the bell's note plays `BELL_CONFIRM_REPEATS` (3) times at arpeggio timing. No new capture starts until the confirmation has finished playing.

Wall-clock time per bell during calibration: ~1s delay + 3 × 0.5s playback = ~2.5 seconds minimum. For a 20-bell set, calibration takes roughly 50 seconds. This is a deliberate design tradeoff for keeping `cal_state_`/`bells_`/`n_bells_` single-writer with no locking.

### Cal completion

Triggered by:
- Bell 1 struck a second time within `REPEAT_CENTS_TOL` (20 cents)
- `n_bells_` reaching `MAX_BELLS` (20) — confirmation plays first, then finalize

`finalize_cal()`:
1. Builds NMF templates via FFT.
2. Saves cal data to `belltracker_cal.dat` (detached write thread — does not block).
3. Posts the cal-complete arpeggio to the MIDI worker.
4. Starts WAV recording if a USB drive is present.
5. Sets `AppState::PERF`.

---

## Performance phase

### Goertzel filter bank

One Goertzel filter per registered bell, tuned to its measured fundamental,
evaluated every `GOERTZEL_WINDOW` = 1024 samples (21.3ms, ~47Hz bandwidth).
The window size is a detection-resolution/latency tradeoff: 128 samples
(375Hz bandwidth) made semitone-adjacent bells spectrally indistinguishable
to the bank.

### Iterative NMF deconvolution

Each window, the bank yields the observation vector **V** (per-bell-frequency
power). Solve V ≈ T·H for activations H via `NMF_ITERS` = 15 Lee-Seung
multiplicative updates, warm-started from the previous window. The template
Gram matrix T·Tᵀ is precomputed on the first perf window.

**Templates live in the Goertzel domain** — each bell's template is the same
1024-sample Goertzel bank's averaged response to that bell's cal capture
(sustain region, transient skipped). This is load-bearing: templates built
from a long FFT (1.46Hz bins) are near-diagonal, while V is smeared by the
bank's ~47Hz bandwidth; deconvolving smeared observations against diagonal
templates degenerates to H≈V, and closely spaced bell sets never cross
threshold (v1.2's original silent-perf bug). Matching domains lets the
updates genuinely un-mix neighbors — simulation-verified against 12 chromatic
bells from C4, soft and hard strikes, singles and chords.

Normalized activation `Hn = H[b]/ΣV` crosses `NMF_THRESHOLD` (0.25) → note-on.

**Cal file format is V2** — V1 files carry FFT-domain templates and are
rejected with a recalibrate message.

### Note-on / note-off

- `H[b] / total_energy >= NMF_THRESHOLD` (0.25) while `NoteState::SILENT` → note-on, state → `SOUNDING`.
- Sustain gate: while `SOUNDING`, any window above half-threshold (`NMF_THRESHOLD * 0.5`) resets the decay timer.
- `NOTE_OFF_DECAY_S` (2.5s) since last above half-threshold → note-off, state → `SILENT`.

---

## MIDI output

All MIDI sends go through an off-RT worker thread. The RT callback posts `MidiCmd` structs to a mutex/condvar queue — the push is brief (pointer copy + notify) and safe from within the JACK process callback.

**Channel assignment:** all bells default to a single channel, 0
(`DEFAULT_BELL_CHANNEL`). Each bell's channel is stored per-bell in the cal
file (`channel=` — hand-editable for per-bell routing), and `--channel=N`
overrides everything, file included. Valid range 0–14; channel 15 (MIDI
channel 16, 1-indexed) is reserved for system status prompts and is rejected
by the loader.

**Voice:** a Program Change → Glockenspiel (GM program 9,
`GM_PROGRAM_GLOCKENSPIEL`) is sent on every bell channel at CAL start, after
`--load-cal`, and at cal completion. The PC reaches all ALSA seq subscribers —
FluidSynth *and* the DIN out — so external synths switch patch too.

### Headless status prompts

| Event | Note(s) | Pattern | Meaning |
|---|---|---|---|
| Startup / waiting | C8 → E8 | Ascending pair, 250ms each; repeats every 5s until first strike | Audio connected, armed |
| Bell registered | Bell's note | 1s delay, then 3× at arpeggio timing | Bell confirmed |
| Cal-complete / bypass success | Each bell's note | Sequential arpeggio, 0.5s onset-to-onset | Entering PERF |
| Bypass recognized | E8 | Single, 250ms, after 1s delay | Damped strike accepted, loading |
| Error / play again (bypass failed) | E8 → C8 | Descending pair, 250ms each | No saved cal — continuing CAL |

Ascending = OK, descending = error — direction carries the meaning, no pitch identification needed. C8/E8 sit above any handbell.

**TEMPORARY:** `FORCE_MIDI_CHANNEL = 0` routes every outgoing event (bells, prompts, PCs) to MIDI channel 1 at the output layer. Per-bell channels, the cal-file `channel=` field, and `--channel` remain functional upstream; set the constant to -1 to restore per-bell routing.

---

## WAV recording

During PERF, all audio from the UR22mkII is recorded to a WAV file on the first USB drive found under `/media/mark/` or `/media/pi/`. Candidates are validated against `/proc/mounts` — only mounts backed by USB mass storage (`/dev/sd*`) qualify. SD-card partitions (an SD left in the slot auto-mounts `bootfs`/`rootfs` under `/media/` too), NVMe partitions, and plain directories are skipped with a log line. If no qualifying drive is present, recording is disabled.

**Format:** mono, 48kHz, 16-bit PCM.

**File naming:** `perf_001.wav`, `perf_002.wav`, etc. The highest existing number is scanned at startup; each session gets the next available number.

**RT safety:** `record_push()` in the JACK callback converts float→int16 and writes into a lock-free SPSC ring buffer (262144 samples ≈ 5.5s). A dedicated write thread (`record_writer_run`) drains the ring every 2 seconds (`RECORD_FLUSH_SECS`) and fsyncs every 5 drain cycles (`RECORD_FSYNC_EVERY`).

**Clean shutdown (UR22mkII unplugged):** JACK fires `jack_shutdown_cb` → destructor calls `stop_recording()` → write thread signals `record_stop_`, does a final ring drain, seeks to byte 0, patches the RIFF/data size fields with the actual sample count, fsyncs, closes. The WAV file is fully playable.

**Hard power cut:** The 44-byte WAV header has zeroed size fields (written as a placeholder at file open). The audio data is intact to the most recent fsync (~10s). Recovery:
```bash
sox --ignore-length perf_001.wav perf_001_fixed.wav
```

---

## Cal data persistence

Saved automatically at the end of every successful `finalize_cal()`. Plain text format — editable by hand if a single bell's data needs correction without a full re-cal.

```
BELLTRACKER_CAL_V1
n_bells=<N>
[bell]
idx=<i>
freq_hz=<f>
cents=<f>
midi_note=<i>
note_name=<C4|F#5|...>   (scientific pitch, MIDI 60 = C4; matches bell castings)
channel=<0-14>           (MIDI out channel for this bell; default 0)
type=BELL|CHIME
attack_ms=<f>
template=<f>,<f>,...   (N comma-separated values, one per registered bell)
[bell]
...
```

Float values written with `setprecision(9)` — exact round-trip verified.

**Load fallbacks:** `midi_note` missing or 0 → derived from `note_name`
(accepts sharps, flats, lowercase: `C#5`, `Db4`, `c5`); both unusable →
load aborts to normal CAL. `channel` missing → 0; out of range → warn + 0.
Editing note_name/midi_note changes what MIDI note *plays*, not what's
*detected* — detection follows `freq_hz` and the template.

Load failures (missing file, wrong format, wrong bell count, wrong template length) fall back to normal CAL rather than crashing or running with bad data.

**CLI flags:**
```bash
./belltracker                       # normal CAL, saves on completion
./belltracker --load-cal[=PATH]     # load PATH (default: belltracker_cal.dat), skip to PERF
./belltracker --save-cal=PATH       # override save path
./belltracker --channel=N           # all bells on MIDI channel N (0-14), beats cal file
./belltracker --debug               # verbose diagnostics (see Debug instrumentation)
./belltracker --help
```

---

## FluidSynth integration (optional)

belltracker is a pure MIDI source. FluidSynth runs as a second ALSA sequencer subscriber alongside the UR22mkII DIN out — ALSA seq ports support multiple simultaneous subscribers, so no source code changes are needed.

Deploy files in `deploy/`:

| File | Purpose |
|---|---|
| `fluidsynth.service` | Systemd user service — JACK audio out, ALSA seq MIDI in |
| `connect-fluidsynth.service` | Oneshot — runs after both belltracker and FluidSynth are up |
| `connect-belltracker-fluidsynth.sh` | `aconnect` routing script with retry loop |

Update the soundfont path in `fluidsynth.service` before enabling.

---

## Debug instrumentation

`--debug` (or `-d`) enables `[dbg]` diagnostics, off by default:

- **Cal state tracing** — every `CalSubState` transition, by name.
- **Waiting heartbeat** — every 2s in AWAITING_STRIKE: live RMS vs. onset
  threshold (`[dbg] waiting  rms=0.0007  thr=0.0200  bells=3  armed=1`) —
  the number to watch when setting UR22mkII input gain.
- **Per-strike analysis** — first-strike decay vs. the damped-bypass
  threshold, YIN result (freq/cents/attack/type), YIN window placement,
  repeat-check distance when armed.
- **Perf NMF dump** — 1/s: total Goertzel energy + top-3 normalized
  activations vs. `NMF_THRESHOLD`.

Several sites fire from the JACK RT callback (rare events or throttled ≤1Hz;
stdout is line-buffered), but printf from the RT thread can still cost an
occasional xrun — **bench use, not performances**. `~/btd.sh` runs services +
foreground debug in one step.

---

## Autostart after boot

All services run as **systemd user services** with linger enabled — the user session persists at boot without login, USB auto-mount still works under `/media/mark/`, audio group and RT permissions work as-is.

```bash
# One-time setup (run after testing the binary manually)
bash deploy/install-services.sh
```

The script enables linger, installs service files to `~/.config/systemd/user/`, and enables the services. `belltracker.service` starts with `--load-cal` by default.

**Service order:** `jackd` → `fluidsynth` → `belltracker` → `connect-fluidsynth` (oneshot)

**Shutdown:** unplugging the UR22mkII fires `jack_shutdown_cb`, which cleanly finalizes the WAV recording and exits 0. `Restart=on-failure` means a crash auto-restarts; a clean UR22 unplug does not.

**Logs:**
```bash
journalctl --user -u belltracker -f
journalctl --user -u jackd -f
```

---

## File layout

```
~/Belltracker/
├── belltracker.h           # shared types, enums, constants, class declaration
├── belltracker_cal.cpp     # cal phase, MIDI worker, init, persistence, helpers
├── belltracker_perf.cpp    # perf phase: Goertzel, NMF, note-on/off
├── belltracker_record.cpp  # WAV recording: USB detection, SPSC ring, write thread
├── main.cpp                # JACK client entry point, signal handling, shutdown
├── CMakeLists.txt
├── setup.sh
├── belltracker_cal.dat     # saved cal data (written after first cal session)
├── build/
│   └── belltracker         # compiled binary
├── deploy/
│   ├── install-services.sh         # one-shot autostart setup
│   ├── install-tools.sh            # recommended tools installer
│   ├── jackd.service
│   ├── belltracker.service
│   ├── fluidsynth.service
│   ├── connect-fluidsynth.service
│   └── connect-belltracker-fluidsynth.sh
├── README.md
└── GUIDE.md
```

---

## Constants reference

All in `belltracker.h`. Rebuild after any change.

| Constant | Default | Description |
|---|---|---|
| `MAX_BELLS` | 20 | Maximum bells per session |
| `REPEAT_CENTS_TOL` | 20.0 | ±cents tolerance for bell-1 repeat detection |
| `ONSET_THRESHOLD` | 0.02 | RMS onset threshold |
| `NMF_THRESHOLD` | 0.25 | Activation threshold for note-on |
| `NOTE_OFF_DECAY_S` | 2.5 | Seconds below half-threshold before auto note-off |
| `GOERTZEL_WINDOW` | 128 | Samples per NMF window (~2.7ms at 48kHz) |
| `CAL_PRE_RING_MS` | 100 | Pre-onset ring buffer duration |
| `CAL_CAPTURE_MS` | 500 | Post-onset sustain capture duration |
| `ATTACK_BELL_MS` | 25.0 | Bell/chime classifier boundary (ms) |
| `TEMPLATE_FFT_SIZE` | 32768 | FFT size for NMF template building |
| `NMF_ITERS` | 4 | Lee-Seung multiplicative-update iterations per window |
| `ARPEGGIO_NOTE_MS` | 400 | Note duration in cal-complete arpeggio and bell confirm |
| `ARPEGGIO_GAP_MS` | 100 | Gap after each arpeggio note (0.5s total onset-to-onset) |
| `BELL_CONFIRM_DELAY_MS` | 1000 | Delay between registration and confirmation playback |
| `BELL_CONFIRM_REPEATS` | 3 | Times a newly registered bell's note plays as confirmation |
| `READY_PING_INTERVAL_S` | 5.0 | Repeat interval for the waiting ping before first strike |
| `DECAY_RELEASE_FRAC` | 0.15 | Fraction of peak RMS counted as released (bypass detection) |
| `DAMPED_DECAY_MS` | 40.0 | Max peak-to-release time for a damped/grasped strike |
| `BYPASS_ACK_DELAY_MS` | 1000 | Delay between bypass detection and audible ack |
| `SYSTEM_CHANNEL` | 15 | Reserved MIDI channel for status prompts (0-indexed) |
| `CAL_FILE_DEFAULT` | `belltracker_cal.dat` | Default cal data save/load path |
| `RECORD_RING_SAMPS` | 262144 | SPSC ring buffer size (~5.5s at 48kHz) |
| `RECORD_FLUSH_SECS` | 2 | Disk write interval (seconds) |
| `RECORD_FSYNC_EVERY` | 5 | fsync every N write cycles (~10s) |

Constants that need empirical tuning against real instruments: `ONSET_THRESHOLD`, `NMF_THRESHOLD`, `ATTACK_BELL_MS`, `DECAY_RELEASE_FRAC`, `DAMPED_DECAY_MS`. Starting values are reasonable but were not derived from physical measurement.
