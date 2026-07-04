#!/usr/bin/env bash
# start-services.sh — start audio infrastructure (JACK + FluidSynth)
set -e
systemctl --user start jackd
echo "jackd started"
# wait for JACK to accept clients before fluidsynth attaches
for i in $(seq 1 10); do
    jack_lsp >/dev/null 2>&1 && break
    sleep 1
done
if systemctl --user list-unit-files fluidsynth.service 2>/dev/null | grep -q fluidsynth; then
    systemctl --user start fluidsynth
    echo "fluidsynth started"
else
    echo "fluidsynth.service not installed — skipping"
fi
systemctl --user --no-pager status jackd fluidsynth 2>/dev/null | grep -E "●|Active:"
