#!/bin/bash
# install-services.sh — set up belltracker auto-start after boot
#
# Run once from ~/Belltracker after the binary is built and tested:
#   cd ~/Belltracker && bash deploy/install-services.sh
#
# What this does:
#   1. Enables "linger" for your user — keeps your systemd user session alive
#      without a login shell, so services start at boot instead of at login.
#   2. Installs all service files into ~/.config/systemd/user/ (user services,
#      not system services — no sudo needed for the services themselves, audio
#      and USB permissions work normally, USB drives still auto-mount under
#      /media/mark/).
#   3. Enables and starts the services.
#
# RECONSTRUCTED 2026-07-02 from project record — diff against chat-3 original if available.

set -euo pipefail
cd "$(dirname "$0")/.."   # → ~/Belltracker

echo "=== belltracker service installer ==="

# 1. linger — user services start at boot, persist without login
sudo loginctl enable-linger "$USER"

# 2. install unit files
UNIT_DIR="$HOME/.config/systemd/user"
mkdir -p "$UNIT_DIR"
cp deploy/jackd.service              "$UNIT_DIR/"
cp deploy/belltracker.service        "$UNIT_DIR/"
cp deploy/connect-fluidsynth.service "$UNIT_DIR/"

SOUNDFONT=/usr/share/sounds/sf2/FluidR3_GM.sf2
ENABLE_FLUID=0
if [ -f "$SOUNDFONT" ]; then
    cp deploy/fluidsynth.service "$UNIT_DIR/"
    ENABLE_FLUID=1
else
    echo "NOTE: $SOUNDFONT not found — skipping fluidsynth.service"
    echo "      (install with: sudo apt-get install fluid-soundfont-gm, then re-run)"
fi

systemctl --user daemon-reload

# 3. enable
systemctl --user enable jackd belltracker
if [ "$ENABLE_FLUID" -eq 1 ]; then
    systemctl --user enable fluidsynth connect-fluidsynth
fi

echo ""
echo "=== Installed ==="
echo ""
echo "To start immediately without rebooting:"
echo "  systemctl --user start jackd"
echo "  systemctl --user start fluidsynth    # if soundfont is configured"
echo "  systemctl --user start belltracker"
echo ""
echo "Useful commands:"
echo "  systemctl --user status belltracker  # check running state"
echo "  journalctl --user -u belltracker -f  # follow live log output"
echo "  journalctl --user -u jackd -f        # JACK log"
echo "  systemctl --user stop belltracker    # stop for manual testing"
echo ""
echo "To disable autostart entirely:"
echo "  systemctl --user disable jackd fluidsynth belltracker connect-fluidsynth"
