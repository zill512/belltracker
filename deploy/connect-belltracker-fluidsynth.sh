#!/bin/bash
# connect-belltracker-fluidsynth.sh
# Route belltracker's ALSA sequencer output port to FluidSynth's ALSA seq input.
# The UR22mkII DIN out connection is made by belltracker itself; this adds
# FluidSynth as a second destination from the same source.
# RECONSTRUCTED 2026-07-02 from project record — diff against chat-3 original if available.

MAX_TRIES=30

for i in $(seq 1 "$MAX_TRIES"); do
    SRC_CLIENT=$(aconnect -o -l 2>/dev/null | grep "client.*belltracker" | grep -oP "client \K[0-9]+")
    DST_CLIENT=$(aconnect -i -l 2>/dev/null | grep -i "client.*FLUID Synth" | grep -oP "client \K[0-9]+")

    if [ -n "$SRC_CLIENT" ] && [ -n "$DST_CLIENT" ]; then
        SRC_PORT=0
        DST_PORT=0
        # already connected? aconnect exits nonzero on duplicate — treat as success
        if aconnect "${SRC_CLIENT}:${SRC_PORT}" "${DST_CLIENT}:${DST_PORT}" 2>/dev/null; then
            echo "Connecting belltracker (${SRC_CLIENT}:${SRC_PORT}) -> FluidSynth (${DST_CLIENT}:${DST_PORT})"
            exit 0
        else
            # verify connection actually exists (duplicate case)
            if aconnect -l | grep -A2 "client ${SRC_CLIENT}:" | grep -q "${DST_CLIENT}:"; then
                echo "belltracker -> FluidSynth already connected"
                exit 0
            fi
        fi
    fi
    sleep 1
done

echo "ERROR: could not connect belltracker -> FluidSynth after ${MAX_TRIES}s" >&2
exit 1
