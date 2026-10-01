#!/usr/bin/env bash
set -euo pipefail
project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${1:-$project_root/scratch/dynamic-prepared-state-tests}"
mkdir -p "$output_dir"
compiler="${CXX:-clang++}"
source_file="$project_root/src/Layers/xrRenderMetal/tests/R4DynamicWorldPreparedStateProbe.cpp"
include_dir="$project_root/src/Layers/xrRenderMetal"
for variant in optimized sanitized; do
    extra=(-Werror=return-type)
    if [[ "$variant" == sanitized ]]; then
        extra+=(-fsanitize=undefined -fno-sanitize-recover=all)
    fi
    "$compiler" -std=c++17 -O2 -DNDEBUG -Wall -Wextra -pedantic \
        "${extra[@]}" -I "$include_dir" "$source_file" -o "$output_dir/$variant"
    "$output_dir/$variant"
done
