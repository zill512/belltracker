// belltracker_record.cpp — USB WAV recording for the perf phase
//
// Architecture:
//   RT callback (perf_process) → record_push() → SPSC ring buffer
//   record_writer_run() (dedicated thread) → drain ring → write to disk → fsync
//
// On clean shutdown (UR22 unplug → jack_shutdown_cb → BellTracker destructor →
// stop_recording()):
//   - record_stop_ is set
//   - write thread does final drain, seeks to byte 0, patches WAV header with
//     correct sizes, fsyncs, closes
//   - clean playable WAV guaranteed
//
// If power is yanked instead of unplugging the UR22:
//   - The RIFF/data size fields at byte 4 and byte 40 remain at 0 (placeholder)
//   - The audio data itself is intact to the last fsynced chunk (every ~10s)
//   - Recovery: `sox perf_NNN.wav --ignore-length perf_NNN_fixed.wav`
//     or any tool that ignores the size fields (VLC, Audacity, ffmpeg all do)

#include "belltracker.h"

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <vector>
#include <chrono>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

// ── WAV header ────────────────────────────────────────────────────────────────
// Little-endian: correct on ARM (Pi 5) and x86.
// Called twice: once at file open (num_samples=0 placeholder), once on close
// (actual count). lseek to 0 before each write.
static void write_wav_header(int fd, uint32_t num_samples) {
    uint32_t data_bytes   = num_samples * 2u;        // 16-bit mono
    uint32_t chunk_size   = 36u + data_bytes;
    uint32_t sub1_size    = 16u;
    uint16_t audio_fmt    = 1u;   // PCM
    uint16_t num_ch       = 1u;   // mono
    uint32_t sample_rate  = 48000u;
    uint32_t byte_rate    = 96000u; // 48000 * 1 * 2
    uint16_t block_align  = 2u;
    uint16_t bits         = 16u;

    uint8_t hdr[44];
    memcpy(hdr +  0, "RIFF",        4);
    memcpy(hdr +  4, &chunk_size,   4);
    memcpy(hdr +  8, "WAVE",        4);
    memcpy(hdr + 12, "fmt ",        4);
    memcpy(hdr + 16, &sub1_size,    4);
    memcpy(hdr + 20, &audio_fmt,    2);
    memcpy(hdr + 22, &num_ch,       2);
    memcpy(hdr + 24, &sample_rate,  4);
    memcpy(hdr + 28, &byte_rate,    4);
    memcpy(hdr + 32, &block_align,  2);
    memcpy(hdr + 34, &bits,         2);
    memcpy(hdr + 36, "data",        4);
    memcpy(hdr + 40, &data_bytes,   4);

    lseek(fd, 0, SEEK_SET);
    ::write(fd, hdr, sizeof(hdr));
}

// ── USB drive detection ───────────────────────────────────────────────────────
// Only accept mounts backed by USB mass-storage block devices (/dev/sd*).
// An SD card left in the slot auto-mounts its bootfs/rootfs partitions under
// /media/<user>/ too (/dev/mmcblk0pN) — recording must never target those,
// and never a plain directory on the root filesystem either.
static bool is_usb_block_mount(const std::string& mnt) {
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return false;
    // /proc/mounts escapes spaces in mount points as \040
    std::string esc;
    for (char c : mnt) { if (c == ' ') esc += "\\040"; else esc += c; }
    char dev[256], mp[512];
    bool ok = false;
    while (fscanf(f, "%255s %511s %*[^\n]", dev, mp) == 2) {
        if (esc == mp) { ok = (strncmp(dev, "/dev/sd", 7) == 0); break; }
    }
    fclose(f);
    return ok;
}

void BellTracker::find_usb_drive() {
    // Raspberry Pi OS auto-mounts USB drives under /media/<user>/<label>/
    const char* bases[] = { "/media/mark", "/media/pi", nullptr };

    for (int b = 0; bases[b] && record_dir_.empty(); ++b) {
        DIR* d = opendir(bases[b]);
        if (!d) continue;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string path = std::string(bases[b]) + "/" + ent->d_name;
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                if (!is_usb_block_mount(path)) {
                    printf("belltracker: skipping %s (not a USB drive)\n",
                           path.c_str());
                    continue;
                }
                record_dir_ = path;
                break;
            }
        }
        closedir(d);
    }

    if (record_dir_.empty()) {
        printf("belltracker: no USB drive found — recording disabled\n");
        return;
    }

    find_next_file_num();
    record_state_ = RecordState::READY;
    printf("belltracker: USB recording → %s  (next: perf_%03d.wav)\n",
           record_dir_.c_str(), record_file_num_);
}

void BellTracker::find_next_file_num() {
    record_file_num_ = 1;
    DIR* d = opendir(record_dir_.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        int n = 0;
        if (sscanf(ent->d_name, "perf_%d.wav", &n) == 1 && n >= record_file_num_)
            record_file_num_ = n + 1;
    }
    closedir(d);
}

// ── start_recording ───────────────────────────────────────────────────────────
// Called when entering PERF (from finalize_cal, bypass, --load-cal paths).
// No-op if no USB drive found at startup.
void BellTracker::start_recording() {
    if (record_state_ != RecordState::READY) return;

    char fname[32];
    snprintf(fname, sizeof(fname), "perf_%03d.wav", record_file_num_++);
    std::string path = record_dir_ + "/" + fname;

    record_fd_ = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (record_fd_ < 0) {
        fprintf(stderr, "belltracker: cannot open recording file %s: %s\n",
                path.c_str(), strerror(errno));
        return;
    }

    // placeholder header — sizes patched on close
    write_wav_header(record_fd_, 0);

    record_samples_ = 0;
    record_ring_w_.store(0, std::memory_order_relaxed);
    record_ring_r_.store(0, std::memory_order_relaxed);
    record_stop_.store(false, std::memory_order_relaxed);
    record_state_ = RecordState::RECORDING;

    // start write thread now that fd and ring are initialized
    record_writer_ = std::thread(&BellTracker::record_writer_run, this);

    printf("belltracker: recording → %s\n", path.c_str());
}

// ── stop_recording ────────────────────────────────────────────────────────────
// Public — safe to call from any thread (shutdown callbacks, destructor).
// Idempotent: no-op if not currently recording.
void BellTracker::stop_recording() {
    if (record_state_ != RecordState::RECORDING) return;
    record_stop_.store(true, std::memory_order_release);
    if (record_writer_.joinable()) record_writer_.join();
    // close_recording_file() was called by the write thread before it exited
    record_state_ = RecordState::DISABLED;
}

// ── close_recording_file ──────────────────────────────────────────────────────
// Called by record_writer_run() as its last act. Not to be called from outside.
void BellTracker::close_recording_file() {
    if (record_fd_ < 0) return;
    write_wav_header(record_fd_, record_samples_);
    fsync(record_fd_);
    close(record_fd_);
    record_fd_ = -1;
    printf("belltracker: recording closed — %u samples (%.1f s)\n",
           record_samples_, (float)record_samples_ / 48000.0f);
}

// ── record_push ───────────────────────────────────────────────────────────────
// Lock-free SPSC write: only the RT callback calls this.
// Converts float → int16, clips, pushes into ring. Drops samples on overflow
// (shouldn't happen: ~5.5s ring, 2s flush interval, so a disk stall of >3.5s
// would be needed to overflow).
void BellTracker::record_push(const float* buf, int n) {
    uint32_t w     = record_ring_w_.load(std::memory_order_relaxed);
    uint32_t r     = record_ring_r_.load(std::memory_order_acquire);
    uint32_t avail = RECORD_RING_SAMPS - (w - r);

    // guard against overflow (w - r > RING shouldn't happen, but be safe)
    if (avail == 0 || avail > RECORD_RING_SAMPS) return;

    uint32_t write_n = ((uint32_t)n < avail) ? (uint32_t)n : avail;

    for (uint32_t i = 0; i < write_n; ++i) {
        float s = buf[i];
        if (s >  1.0f) s =  1.0f;
        if (s < -1.0f) s = -1.0f;
        record_ring_[(w + i) & RECORD_RING_MASK] = (int16_t)(s * 32767.0f);
    }
    record_ring_w_.store(w + write_n, std::memory_order_release);
}

// ── record_writer_run ─────────────────────────────────────────────────────────
// Disk-write thread. Wakes every 100ms, drains ring when enough time has
// elapsed, fsyncs periodically. On stop signal: final drain, then closes file.
void BellTracker::record_writer_run() {
    std::vector<int16_t> drain_buf;
    drain_buf.reserve(RECORD_RING_SAMPS);
    int  fsync_count    = 0;
    int  ticks_per_flush = RECORD_FLUSH_SECS * 10;  // 100ms ticks
    int  tick_count      = 0;

    auto drain = [&]() {
        uint32_t w     = record_ring_w_.load(std::memory_order_acquire);
        uint32_t r     = record_ring_r_.load(std::memory_order_relaxed);
        uint32_t count = w - r;
        if (count == 0 || record_fd_ < 0) return;

        drain_buf.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            drain_buf[i] = record_ring_[(r + i) & RECORD_RING_MASK];
        record_ring_r_.store(r + count, std::memory_order_release);

        ssize_t to_write = (ssize_t)(count * sizeof(int16_t));
        ssize_t written  = ::write(record_fd_, drain_buf.data(), (size_t)to_write);

        if (written < 0) {
            if (errno == ENOSPC) {
                fprintf(stderr, "belltracker: USB drive full — recording stopped\n");
                record_stop_.store(true, std::memory_order_release);
            }
            return;
        }
        record_samples_ += (uint32_t)(written / (ssize_t)sizeof(int16_t));

        if (++fsync_count >= RECORD_FSYNC_EVERY) {
            fsync(record_fd_);
            fsync_count = 0;
        }
    };

    while (!record_stop_.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (++tick_count >= ticks_per_flush) {
            drain();
            tick_count = 0;
        }
    }

    // final drain of anything still in the ring after stop signal
    drain();
    close_recording_file();
}
