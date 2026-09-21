#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DXC_TAG="v1.9.2607"
DXC_COMMIT="0d3ee6b551b8fa768fbf825300ebab81047ef6a8"
TOOLCHAIN_ROOT="${XRAY_DIRECT_METAL_TOOLCHAIN_ROOT:-$PROJECT_ROOT/build/direct-metal-toolchain}"
DXC_SOURCE_DIR="${XRAY_DXC_SOURCE_DIR:-$TOOLCHAIN_ROOT/src/DirectXShaderCompiler-$DXC_TAG}"
DXC_BUILD_DIR="${XRAY_DXC_BUILD_DIR:-$TOOLCHAIN_ROOT/dxc-$DXC_TAG}"

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "DXC macOS build requires Darwin" >&2
    exit 1
fi

mkdir -p "$TOOLCHAIN_ROOT/src"

if [[ ! -d "$DXC_SOURCE_DIR/.git" ]]; then
    git clone --depth 1 --branch "$DXC_TAG" \
        https://github.com/microsoft/DirectXShaderCompiler.git "$DXC_SOURCE_DIR"
fi

ACTUAL_COMMIT="$(git -C "$DXC_SOURCE_DIR" rev-parse HEAD)"
if [[ "$ACTUAL_COMMIT" != "$DXC_COMMIT" ]]; then
    echo "DXC source mismatch: expected $DXC_COMMIT, found $ACTUAL_COMMIT" >&2
    exit 1
fi

git -C "$DXC_SOURCE_DIR" submodule update --init --recursive --depth 1

cmake -S "$DXC_SOURCE_DIR" -B "$DXC_BUILD_DIR" \
    -C "$DXC_SOURCE_DIR/cmake/caches/PredefinedParams.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DHLSL_INCLUDE_TESTS=OFF \
    -DENABLE_SPIRV_CODEGEN=OFF \
    -DSPIRV_BUILD_TESTS=OFF \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -G Ninja

cmake --build "$DXC_BUILD_DIR" --target dxc -j "${XRAY_DXC_BUILD_JOBS:-6}"
"$DXC_BUILD_DIR/bin/dxc" --version

echo "DXC_READY executable=$DXC_BUILD_DIR/bin/dxc commit=$ACTUAL_COMMIT"
