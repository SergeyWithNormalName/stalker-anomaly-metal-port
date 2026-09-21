#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/metal-bootstrap"
RECEIPT=""
BOOTSTRAP_ARGS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --receipt)
            RECEIPT="$2"
            shift 2
            ;;
        --help|-h)
            echo "usage: launch_metal_bootstrap.sh [--receipt FILE] [--frames COUNT]"
            exit 0
            ;;
        *)
            BOOTSTRAP_ARGS+=("$1")
            shift
            ;;
    esac
done

cmake -S "$PROJECT_ROOT/src/Layers/xrRenderMetal" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$BUILD_DIR" --target XRay.Metal.Bootstrap -j 4

set +e
BOOTSTRAP_OUTPUT="$("$BUILD_DIR/AnomalyMetalBootstrap" "${BOOTSTRAP_ARGS[@]}" 2>&1)"
BOOTSTRAP_STATUS=$?
set -e
printf '%s\n' "$BOOTSTRAP_OUTPUT"
if [[ "$BOOTSTRAP_STATUS" -ne 0 ]]; then
    exit "$BOOTSTRAP_STATUS"
fi

if [[ -n "$RECEIPT" ]]; then
    RECEIPT_PARENT="$(cd "$(dirname "$RECEIPT")" && pwd -P)"
    RECEIPT_ABS="$RECEIPT_PARENT/$(basename "$RECEIPT")"
    if [[ -L "$RECEIPT_ABS" || -e "$RECEIPT_ABS" ]]; then
        echo "Receipt path must be new and not a symlink: $RECEIPT_ABS" >&2
        exit 1
    fi
    case "$RECEIPT_ABS" in
        "$PROJECT_ROOT/runtime/"*)
            echo "Refusing to write a receipt inside the live runtime" >&2
            exit 1
            ;;
    esac

    {
        echo "evidence_scope=standalone_r4_metal_lifecycle_and_resource_constructibility_no_engine_draw_claim"
        echo "interface_sha256=$(shasum -a 256 "$PROJECT_ROOT/src/Layers/xrRenderMetal/IR4Backend.h" | awk '{print $1}')"
        echo "resource_interface_sha256=$(shasum -a 256 "$PROJECT_ROOT/src/Layers/xrRenderMetal/IR4Resources.h" | awk '{print $1}')"
        echo "backend_sha256=$(shasum -a 256 "$PROJECT_ROOT/src/Layers/xrRenderMetal/MetalR4Backend.mm" | awk '{print $1}')"
        echo "bootstrap_sha256=$(shasum -a 256 "$PROJECT_ROOT/src/Layers/xrRenderMetal/MetalBootstrap.mm" | awk '{print $1}')"
        echo "executable_sha256=$(shasum -a 256 "$BUILD_DIR/AnomalyMetalBootstrap" | awk '{print $1}')"
        echo "metal_api_validation=${MTL_DEBUG_LAYER:-0}"
        echo "metal_shader_validation=${MTL_SHADER_VALIDATION:-0}"
        echo "arguments=${BOOTSTRAP_ARGS[*]}"
        printf '%s\n' "$BOOTSTRAP_OUTPUT" | sed 's/^/runtime_output=/'
    } > "$RECEIPT_ABS"
    echo "METAL_BOOTSTRAP_RECEIPT_READY path=$RECEIPT_ABS"
fi
