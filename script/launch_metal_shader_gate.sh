#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build/metal-bootstrap"
MSC_INCLUDE="$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/include"
MANIFEST="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/representative-gate.tsv"
RECEIPT=""
SHADERS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --receipt)
            RECEIPT="$2"
            shift 2
            ;;
        --help|-h)
            echo "usage: launch_metal_shader_gate.sh [--receipt FILE] shader.metallib [...]"
            exit 0
            ;;
        *)
            SHADERS+=("$1")
            shift
            ;;
    esac
done

if [[ "${#SHADERS[@]}" -lt 1 ]]; then
    echo "usage: launch_metal_shader_gate.sh [--receipt FILE] shader.metallib [...]" >&2
    exit 2
fi
if [[ ! -f "$MSC_INCLUDE/metal_irconverter_runtime/metal_irconverter_runtime.h" ]]; then
    echo "missing Metal Shader Converter runtime headers: $MSC_INCLUDE" >&2
    exit 1
fi

METAL_DIR="$(cd "$(dirname "${SHADERS[0]}")" && pwd -P)"
for shader in "${SHADERS[@]}"; do
    if [[ "$(cd "$(dirname "$shader")" && pwd -P)" != "$METAL_DIR" ]]; then
        echo "All metallibs must come from one receipted MSC gate directory" >&2
        exit 1
    fi
done
MSC_VALIDATION_ARGS=(
    --manifest "$MANIFEST"
    --metal-dir "$METAL_DIR"
)
for shader in "${SHADERS[@]}"; do
    MSC_VALIDATION_ARGS+=(--metallib "$shader")
done
python3 "$PROJECT_ROOT/script/validate_msc_gate_receipt.py" "${MSC_VALIDATION_ARGS[@]}"

cmake -S "$PROJECT_ROOT/src/Layers/xrRenderMetal" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DXRAY_METAL_IR_RUNTIME_INCLUDE_DIR="$MSC_INCLUDE"
cmake --build "$BUILD_DIR" --target XRay.Metal.ShaderGate -j 4

set +e
GATE_OUTPUT="$("$BUILD_DIR/AnomalyMetalShaderGate" "${SHADERS[@]}" 2>&1)"
GATE_STATUS=$?
set -e
printf '%s\n' "$GATE_OUTPUT"
if [[ "$GATE_STATUS" -ne 0 ]]; then
    exit "$GATE_STATUS"
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
        echo "evidence_scope=metal_runtime_library_function_and_representative_pso_constructibility"
        echo "harness_sha256=$(shasum -a 256 "$PROJECT_ROOT/src/Layers/xrRenderMetal/MetalShaderGate.m" | awk '{print $1}')"
        echo "executable_sha256=$(shasum -a 256 "$BUILD_DIR/AnomalyMetalShaderGate" | awk '{print $1}')"
        echo "msc_runtime_header_sha256=$(shasum -a 256 "$MSC_INCLUDE/metal_irconverter_runtime/metal_irconverter_runtime.h" | awk '{print $1}')"
        echo "msc_receipt_sha256=$(shasum -a 256 "$METAL_DIR/msc-gate-receipt.txt" | awk '{print $1}')"
        echo "msc_receipt_validation=complete"
        echo "msc_receipt_validator_sha256=$(shasum -a 256 "$PROJECT_ROOT/script/validate_msc_gate_receipt.py" | awk '{print $1}')"
        echo "manifest_sha256=$(shasum -a 256 "$MANIFEST" | awk '{print $1}')"
        echo "metal_api_validation=${MTL_DEBUG_LAYER:-0}"
        echo "metal_shader_validation=${MTL_SHADER_VALIDATION:-0}"
        echo "shader_count=${#SHADERS[@]}"
        for shader in "${SHADERS[@]}"; do
            printf 'metallib=%s\tsha256=%s\n' \
                "$(realpath "$shader")" "$(shasum -a 256 "$shader" | awk '{print $1}')"
        done
        printf '%s\n' "$GATE_OUTPUT" | sed 's/^/runtime_output=/'
    } > "$RECEIPT_ABS"
    echo "METAL_SHADER_GATE_RECEIPT_READY path=$RECEIPT_ABS"
fi
