# Belltracker — Operator's Guide

Belltracker listens to handbells, choirchimes, or similar pitched percussion through a
microphone, identifies each note as it is played, and sends the matching MIDI note to a
synthesizer or sound module in real time. It also records the full performance as an
audio file.

The system runs entirely headless — no screen, no keyboard, no mouse near the
instruments. All feedback during setup and performance comes through sound, played
back through whatever synth is connected.

---

## What you need

| Item | Notes |
|---|---|
| Raspberry Pi (in its enclosure) | Pre-configured, no setup needed |
| Steinberg UR22mkII audio interface | Connects to the Pi via USB |
| Microphone or direct input | Into UR22mkII input 1 |
| Standard MIDI cable | From UR22mkII MIDI OUT to your synth or sound module |
| Power supply for the Pi | USB-C |
| USB drive (optional) | For recording — a real USB stick/drive, plugged in before powering on (the Pi's SD card is never used for recording) |

---

## Starting up

1. Plug in the UR22mkII (USB to Pi), the microphone, and the MIDI cable to your synth.
2. Plug in the USB drive if you want to record.
3. Apply power to the Pi.
4. Wait about 20–30 seconds for the system to start.
5. You will hear a single short ping through your synth. That ping repeats every 5
   seconds for as long as nothing has been struck. This is the system's "waiting"
   signal.

If you don't hear the ping within 30 seconds, check the MIDI cable and make sure your
synth is powered on and set to receive MIDI.

---

## Two ways to begin

### Option A — Full calibration (first time, or new bell set)

Calibration teaches the system which notes are in your piece. Do this the first time
you use a new set of bells.

Strike each bell once, in any order. After each strike:

- The system listens for about half a second.
- About one second later, you hear that bell's note played back three times. That is
  your confirmation — the bell was registered.

Wait for the three-note confirmation to finish before striking the next bell.

**To end calibration:** strike bell 1 again (the same bell you struck first). The
system recognizes the repeat and plays every registered bell back in sequence, one at
a time. When that sequence finishes, the system is in performance mode.

The calibration data is saved automatically. Next time, you can use Option B.

### Option B — Skip calibration (same bells as last time)

If the same set of bells was calibrated in a previous session, you can skip straight
to performance mode with a single gesture.

**The gesture:** Bell 1's ringer grasps the bell's brass casting firmly while
striking — or immediately after. The grip kills the ring almost immediately,
producing a very short dead sound. This must be the first thing struck after startup;
nothing else should have been struck yet.

**What happens:**

1. About one second after the grasp strike, you hear a single ping — the system
   recognized the gesture and is loading the previous session's data.
2. A moment later, one of two things happens:
   - **Success**: Every registered bell plays in sequence, the same as at the end of
     a normal calibration. Performance mode begins.
   - **No saved data**: Three rapid pings in a row — a clearly different sound from
     the single success ping. The system drops back into normal calibration. Just
     strike each bell as usual.

If a grasp strike feels like it rang a little (you can hear the bell sustain
briefly), the system will treat it as bell 1's actual note and begin a normal
calibration. That's fine — just continue calibrating normally.

---

## Performance mode

Once in performance mode (confirmed by the sequential playback of all registered
bells), every strike is detected in real time and sent to the synth as a MIDI note.
The system can detect multiple bells struck simultaneously — chords and overlapping
strikes are handled.

Notes stay on for as long as the bell rings. The system sends a note-off
automatically about 2.5 seconds after the sound fades, even if the bell is never
explicitly damped.

---

## Recording

If a USB drive was plugged in before powering on, the system records the full
performance automatically as soon as performance mode starts. Files are named
`perf_001.wav`, `perf_002.wav`, and so on — each session gets a new file. You do not
need to do anything to start or stop recording; it runs in the background throughout
the performance.

To get the files off the drive: unplug the USB drive after stopping the system (see
below) and plug it into a computer.

Only actual USB drives are used — an SD card sitting in the Pi's card slot is
ignored, so recordings can never land on the system's own storage.

All sounds — status pings, calibration confirmations, and performance notes —
play through the synth in a glockenspiel voice, set automatically at startup.

---

## Stopping

Unplug the UR22mkII from the Pi. The system detects this, finalizes the recording
file so it plays back correctly, and shuts down cleanly. The Pi itself stays
powered — the next time the UR22mkII is plugged back in, the system restarts
automatically within a few seconds.

Do not yank power from the Pi itself while a recording is in progress if you can
avoid it. The recording file will be intact to the last save point (roughly every 10
seconds) but the file header will need a quick repair before it plays back:

```
sox --ignore-length perf_001.wav perf_001_fixed.wav
```

---

## Status sounds — what everything means

| What you hear | When | Meaning |
|---|---|---|
| Single short ping, repeating every 5 seconds | At startup, before anything is struck | System armed, waiting |
| That bell's note, played 3 times, about 1 second after a strike | During calibration, after each bell | Bell registered successfully |
| Every bell's note in sequence, one at a time | At the end of calibration | Performance mode active |
| Single ping, about 1 second after a grasp strike | Skip-cal gesture | Loading previous session's data |
| Three rapid pings | After skip-cal attempt | No saved data found — calibrate normally |

---

## Acoustic tips

- Position the microphone reasonably close to the bells and away from other loud
  sources (other instruments, air conditioning, audience noise during a rehearsal).
- Strike bell 1 with similar force both times (first registration and the repeat to
  end cal). A significantly quieter repeat can occasionally not be recognized.
- A grasp strike should be inaudible or nearly so — the ring should stop within a
  tiny fraction of a second. If you can hear the bell sustain even briefly, grip
  closer to the casting or grip earlier.
- Input level on the UR22mkII: set it so individual strikes are clearly audible
  without the input LED clipping. Too quiet and strikes may not be detected; too
  loud and the signal clips.

---

## Troubleshooting

**No startup ping after 30 seconds:** Check the MIDI cable from UR22mkII MIDI OUT to
your synth. Confirm the synth is on and receiving MIDI. Try disconnecting and
reconnecting the UR22mkII.

**Bell not being detected during calibration:** Input level may be too low. Turn up
the input gain on the UR22mkII.

**Bell detected but wrong note plays:** The pitch detection may have locked onto a
harmonic instead of the fundamental. Strike the bell more cleanly (less damping,
centered strike) and try again during the same calibration session — re-striking any
unconfirmed bell before ending cal will re-register it.

**System does not recognize the bell-1 repeat to end calibration:** Strike bell 1 in
the same position and with similar force as the first time. Wait for the previous
bell's confirmation to finish before striking.

**Grasp strike starts a normal calibration instead of loading saved data:** The
strike rang long enough to be treated as a normal note. Re-grip the casting more
firmly and try again — or just continue calibrating normally; the result is the same.

**Grasp gesture gives three rapid pings:** No saved calibration data exists yet for
this bell set. Calibrate normally this session; the data will be saved automatically
and the gesture will work next time.

**Notes playing when no bell is struck (false triggers):** Background noise is
crossing the onset threshold. Reduce mic gain slightly or move the mic away from
noise sources.

**Recording files not appearing on the USB drive:** The drive may have been plugged
in after the system started. Recording is detected at startup only — power cycle
with the drive already inserted.

---

## Quick-start card

```
STARTUP
 1. Connect: UR22mkII (USB→Pi), mic→UR22 input 1,
    MIDI cable UR22 OUT→synth, USB drive (if recording).
 2. Power on Pi. Wait for startup ping (~20–30s).

FIRST TIME WITH THESE BELLS — calibrate:
 3. Strike each bell once. Wait for 3-note confirmation each time.
 4. Re-strike bell 1 to end cal.
 5. Hear each bell play in sequence → performance mode.

SAME BELLS AS LAST SESSION — skip cal:
 3. Bell 1 ringer grasps brass casting while striking.
 4. Wait ~1 second. Single ping.
 5. Hear each bell play in sequence → performance mode.
    (Three rapid pings = no saved data, calibrate normally instead.)

PERFORM.

STOP:
 Unplug UR22mkII from Pi. Recording is finalized automatically.
```

---

## Reference

| Parameter | Value | Notes |
|---|---|---|
| Max bells per session | 20 | |
| Pre-onset buffer | 100 ms | Audio captured just before each strike |
| Sustain capture | 500 ms | Audio captured after each strike |
| Confirmation delay | ~1 second | After strike, before confirmation plays |
| Confirmation repeats | 3× | Each at 0.5 s spacing |
| Note-off decay | ~2.5 seconds | After bell ring fades |
| Bypass ack delay | ~1 second | Between grasp gesture and hearing response |
| Recording format | Mono WAV, 48 kHz, 16-bit | |
| Recording files | perf_001.wav, perf_002.wav… | New file each session |
| Synth voice | Glockenspiel | Set automatically on the synth at startup |

To adjust any of these, an engineer can edit the constants in `belltracker.h` and
rebuild. See README.md for the full technical reference.
