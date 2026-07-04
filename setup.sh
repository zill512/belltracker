#!/usr/bin/env bash
# setup.sh — install dependencies and build belltracker
# Target: Raspberry Pi 5, Raspberry Pi OS Bookworm (64-bit)
set -e
echo "=== belltracker setup (Pi 5) ==="

sudo apt-get update -qq
sudo apt-get install -y \
    build-essential cmake pkg-config \
    jackd2 \
    libjack-jackd2-dev \
    libasound2-dev \
    fluidsynth \
    fluid-soundfont-gm \
    a2jmidid

# Realtime audio permissions (no-op if already configured)
sudo usermod -aG audio "$USER"

mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"
cd ..

echo ""
echo "=== Build complete ==="
echo ""
echo "Manual test (before installing services):"
echo "  1. Start JACK:"
echo "       jackd -d alsa -d hw:UR22mkII -r 48000 -p 256 -n 3 &"
echo "  2. Run belltracker:"
echo "       ./build/belltracker"
echo ""
echo "  MIDI routes automatically to UR22mkII DIN out."
echo "  If not found, check: aconnect -l"
echo ""
echo "For autostart after boot, run once:"
echo "  bash deploy/install-services.sh"
echo ""
echo "If you were just added to the 'audio' group, log out and back in first."
