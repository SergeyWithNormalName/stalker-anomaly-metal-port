#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "$0")/.." && pwd)"
output_dir="${1:-$project_root/scratch/luabind-narrow-tests}"
mkdir -p "$output_dir"
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists luajit; then
    read -r -a lua_flags <<<"$(pkg-config --cflags --libs luajit)"
elif [[ -f /opt/homebrew/include/luajit-2.1/lua.h ]]; then
    lua_flags=(-I/opt/homebrew/include/luajit-2.1 -L/opt/homebrew/lib -lluajit-5.1)
else
    echo "LuaJIT development headers/library are required for the isolated probe." >&2
    exit 2
fi
compiler="${CXX:-clang++}"
common=(-std=c++17 -O2 -DNDEBUG -DLUABIND_NO_EXCEPTIONS -fms-extensions
    -Wno-ignored-attributes -I"$project_root/src/3rd party" -I"$project_root/sdk/include"
    "$project_root/src/3rd party/luabind/tests/NarrowNumericProbe.cpp")

for mode in optimized unsigned-char ubsan; do
    extra=(-Werror=return-type)
    case "$mode" in
        unsigned-char) extra+=(-funsigned-char) ;;
        ubsan) extra+=(-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all) ;;
    esac
    "$compiler" "${common[@]}" "${extra[@]}" "${lua_flags[@]}" -o "$output_dir/$mode"
    "$output_dir/$mode"
done
