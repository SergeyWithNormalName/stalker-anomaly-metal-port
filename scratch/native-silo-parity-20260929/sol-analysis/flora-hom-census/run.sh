#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
stage="$repo/scratch/native-silo-parity-20260929/sol-analysis/flora-hom-census"
parser="$repo/scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/draft/src/Layers/xrRenderMetal"
level="$repo/scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/runtime/gamedata/levels/l01_escape"
clang++ -std=c++17 -O2 -I "$parser" "$stage/produce.cpp" "$parser/R4LevelGeometry.cpp" -o /tmp/r4-flora-hom-census-producer
/tmp/r4-flora-hom-census-producer "$level/level" "$level/level.geom" \
  "$repo/scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv" \
  "$stage/main-flora-hom-frame-700.bin"
python3 "$stage/validate.py"
