#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../../../" && pwd)
stage="$repo/scratch/native-silo-parity-20260929/sol-analysis/flora-hom-census/geometry-match"
parser="$repo/scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/draft/src/Layers/xrRenderMetal"
level="$repo/scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/runtime/gamedata/levels/l01_escape"
hom="$repo/scratch/release_runs/20260928-light-hom-assets/extracted/level_l01_escape__004__level.hom"
clang++ -std=c++17 -O2 -I "$parser" "$stage/screen.cpp" "$parser/R4LevelGeometry.cpp" -o /tmp/r4-hom-triangle-screen
/tmp/r4-hom-triangle-screen "$level/level" "$level/level.geom" "$hom" "$stage/matches.csv" | tee "$stage/screen-summary.txt"
python3 "$stage/summarize.py"
