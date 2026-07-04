#!/usr/bin/env bash
# start-belltracker.sh — start the tracker (+ MIDI routing to FluidSynth)
# Default: as a service. With -f/--fg: foreground in this terminal for
# testing (stops the service first; Ctrl+C to quit).
set -e
cd "$(dirname "$0")/.."          # → ~/Belltracker

if [ "${1:-}" = "-f" ] || [ "${1:-}" = "--fg" ]; then
    systemctl --user stop belltracker 2>/dev/null || true
    shift
    exec ./build/belltracker "$@"   # extra args pass through, e.g. --load-cal
fi

systemctl --user start belltracker
systemctl --user start connect-fluidsynth 2>/dev/null || true
sleep 1
systemctl --user --no-pager status belltracker | grep -E "●|Active:"
echo "logs: journalctl --user -u belltracker -f"
