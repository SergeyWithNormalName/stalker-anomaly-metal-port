#!/bin/sh
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$HERE"
MOTION=${1:-frame700-motion.csv}
OUTPUT=${2:-frame700-replay.csv}
case "$MOTION" in
    frame700-motion.csv)
        python3 make_motion_fixture.py --output "$MOTION"
        ;;
    frame700-motion-active.csv)
        python3 make_motion_fixture.py \
            --wind-json ../../../../runs/portal-active-inputs-01/native/output/flora-wind.json \
            --output "$MOTION"
        ;;
    *)
        echo "unsupported motion snapshot: $MOTION" >&2
        exit 2
        ;;
esac
clang++ -std=c++17 -O2 -Wall -Wextra -Wpedantic replay.cpp -o replay
./replay \
    ../../../ssa-source-rule/wind-bounds/mesh-extrema/flora-mesh-extrema.csv \
    ../../offline-portal-frusta/all-geometry-envelopes.csv \
    ../../offline-portal-frusta/flora-portal-envelope-roi.csv \
    "$MOTION" \
    "$OUTPUT"
