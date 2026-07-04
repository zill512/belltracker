#!/bin/bash
# install-tools.sh — all recommended tools for belltracker Pi 5
#
# Run from ~/Belltracker/deploy/ or anywhere:
#   bash deploy/install-tools.sh
#
# Safe to re-run — apt is idempotent.

set -euo pipefail

echo "=== belltracker tool installer ==="
echo ""

# ── update package list ───────────────────────────────────────────────────────
echo "[1/7] Updating package list..."
sudo apt-get update -qq
echo ""

# ── audio / DSP ───────────────────────────────────────────────────────────────
echo "[2/7] Audio and DSP tools..."
sudo apt-get install -y \
    sox \
    libsox-fmt-all \
    aubio-tools \
    ladspa-sdk \
    cmt \
    jack-tools \
    qjackctl \
    fluidsynth

echo ""
echo "  sox          — audio file conversion and repair (WAV header fix: sox --ignore-length)"
echo "  aubio-tools  — CLI pitch/onset detection (aubionotes, aubioonset, aubiopitch)"
echo "  ladspa-sdk   — LADSPA plugin SDK"
echo "  cmt          — LADSPA plugin collection (filters, delays, etc.)"
echo "  jack-tools   — jack-scope, jack-monitor, jack-plumbing"
echo "  qjackctl     — JACK patchbay GUI (useful when monitor is attached)"
echo "  fluidsynth   — software MIDI synth (if not already installed)"
echo ""

# ── measurement / RT ─────────────────────────────────────────────────────────
echo "[3/7] Measurement and RT latency tools..."
sudo apt-get install -y \
    htop \
    iotop \
    realtime-tests \
    sysstat

echo ""
echo "  htop          — per-core CPU monitor"
echo "  iotop         — real-time disk I/O monitor (validate USB write thread)"
echo "  realtime-tests — includes cyclictest (RT scheduling latency)"
echo "  sysstat        — iostat, mpstat for logging system stats over time"
echo ""

# ── USB filesystem support ────────────────────────────────────────────────────
echo "[4/7] USB filesystem support..."
sudo apt-get install -y \
    ntfs-3g \
    exfat-fuse \
    exfatprogs

echo ""
echo "  ntfs-3g    — read/write NTFS (USB drives formatted on Windows)"
echo "  exfat-fuse — read/write exFAT (large drives formatted on Mac/Windows)"
echo "  exfatprogs — exFAT userspace tools (mkfs, fsck)"
echo ""

# ── development ───────────────────────────────────────────────────────────────
echo "[5/7] Development tools..."
sudo apt-get install -y \
    gdb \
    valgrind \
    clang \
    clang-tidy \
    ccache \
    tmux

echo ""
echo "  gdb        — debugger"
echo "  valgrind   — memory error/leak detection"
echo "  clang      — alternative compiler + static analysis"
echo "  clang-tidy — lint/static analysis on belltracker source"
echo "  ccache     — compiler cache (speeds up repeated rebuilds)"
echo "  tmux       — terminal multiplexer (detach/reattach SSH sessions from iPad)"
echo ""

# ── system utilities ──────────────────────────────────────────────────────────
echo "[6/7] System utilities..."
sudo apt-get install -y \
    logrotate \
    lshw \
    usbutils

echo ""
echo "  logrotate  — rotate belltracker systemd logs automatically"
echo "  lshw       — list hardware (confirm USB drive is recognized)"
echo "  usbutils   — lsusb (identify connected USB devices including UR22mkII)"
echo ""

# ── clean up ──────────────────────────────────────────────────────────────────
echo "[7/7] Cleaning up..."
sudo apt-get autoremove -y -qq
sudo apt-get clean
echo ""

# ── post-install notes ────────────────────────────────────────────────────────
echo "=== Done ==="
echo ""
echo "Key commands to remember:"
echo ""
echo "  RT latency test (run while belltracker is active):"
echo "    sudo cyclictest --mlockall --smp --priority=80 --interval=200 --distance=0 -D 30"
echo "    Watch 'Max' column — should stay under ~200µs for stable JACK at 256 frames/48kHz"
echo ""
echo "  Repair a WAV file with zeroed header (power-yank recovery):"
echo "    sox --ignore-length perf_001.wav perf_001_fixed.wav"
echo ""
echo "  Pitch detection on a recorded performance file (ground-truth check):"
echo "    aubionotes -i perf_001.wav"
echo ""
echo "  Watch CPU temp and throttle state during a run:"
echo "    watch -n 2 'vcgencmd measure_temp && vcgencmd get_throttled'"
echo ""
echo "  Check USB drive filesystem type:"
echo "    lsblk -f"
echo ""
echo "  tmux quick-start (so SSH disconnect doesn't kill your session):"
echo "    tmux            # start"
echo "    tmux attach     # reattach after disconnect"
echo "    Ctrl-b d        # detach (leaves session running)"
echo ""
