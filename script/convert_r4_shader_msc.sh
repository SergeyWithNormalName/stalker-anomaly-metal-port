#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
MANIFEST="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/representative-gate.tsv"
DXIL_DIR=""
OUTPUT_DIR=""

usage() {
    cat <<'EOF'
Usage:
  convert_r4_shader_msc.sh --dxil-dir DIR --output DIR [--msc FILE]

Converts the manifest-listed representative R4/GAMMA DXIL shaders to metallib and emits
validated Metal Shader Converter reflection JSON.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dxil-dir)
            DXIL_DIR="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --msc)
            MSC="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$DXIL_DIR" || -z "$OUTPUT_DIR" ]]; then
    usage >&2
    exit 2
fi
if [[ ! -x "$MSC" || ! -f "$DXIL_DIR/dxc-gate-receipt.txt" || ! -f "$MANIFEST" ]]; then
    echo "Missing MSC, DXC gate directory, or manifest" >&2
    exit 1
fi

python3 "$PROJECT_ROOT/script/validate_dxc_gate_receipt.py" \
    --manifest "$MANIFEST" \
    --dxil-dir "$DXIL_DIR"

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the metallib output: $OUTPUT_ABS" >&2
    exit 1
fi
case "$OUTPUT_ABS/" in
    "$PROJECT_ROOT/runtime/"*)
        echo "Refusing to write metallib files inside the live runtime" >&2
        exit 1
        ;;
esac
if [[ -d "$OUTPUT_ABS" ]] && [[ -n "$(find "$OUTPUT_ABS" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "Output directory must be new or empty: $OUTPUT_ABS" >&2
    exit 1
fi
mkdir -p "$OUTPUT_ABS"

MSC_REAL="$(realpath "$MSC")"
RECEIPT="$OUTPUT_ABS/msc-gate-receipt.txt"
{
    echo "evidence_scope=msc_conversion_and_structural_reflection_only_no_runtime_load_or_pso_claim"
    echo "msc_path=$MSC_REAL"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "msc_sha256=$(shasum -a 256 "$MSC_REAL" | awk '{print $1}')"
    echo "dxc_receipt_sha256=$(shasum -a 256 "$DXIL_DIR/dxc-gate-receipt.txt" | awk '{print $1}')"
    echo "dxc_receipt_validation=complete"
    echo "dxc_receipt_validator_sha256=$(shasum -a 256 "$PROJECT_ROOT/script/validate_dxc_gate_receipt.py" | awk '{print $1}')"
    echo "manifest_sha256=$(shasum -a 256 "$MANIFEST" | awk '{print $1}')"
    echo "reflection_validator_sha256=$(shasum -a 256 "$PROJECT_ROOT/script/validate_msc_reflection.py" | awk '{print $1}')"
    echo "deployment_os=macOS"
    echo "minimum_os_build_version=14.0.0"
    echo "minimum_gpu_family=Metal3"
} > "$RECEIPT"

PASS_COUNT=0
EXPECTED_COUNT="$(awk -F '\t' 'NF && $1 !~ /^#/ { count += 1 } END { print count + 0 }' "$MANIFEST")"
while IFS=$'\t' read -r SHADER_CLASS PROFILE SOURCE PERMUTATION; do
    if [[ -z "$SHADER_CLASS" || "$SHADER_CLASS" == \#* ]]; then
        continue
    fi

    DXIL_PATH="$DXIL_DIR/$SOURCE.dxil"
    METALLIB_PATH="$OUTPUT_ABS/$SOURCE.metallib"
    REFLECTION_PATH="$OUTPUT_ABS/$SOURCE.json"
    "$MSC" "$DXIL_PATH" \
        -o "$METALLIB_PATH" \
        --output-reflection-file "$REFLECTION_PATH" \
        --deployment-os=macOS \
        --minimum-os-build-version=14.0.0 \
        --minimum-gpu-family=Metal3

    if [[ ! -s "$METALLIB_PATH" || ! -s "$REFLECTION_PATH" ]]; then
        echo "MSC gate produced incomplete output: $SHADER_CLASS ($SOURCE)" >&2
        exit 1
    fi
    printf 'shader=%s\tprofile=%s\tpermutation=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\n' \
        "$SOURCE" "$PROFILE" "$PERMUTATION" \
        "$(shasum -a 256 "$DXIL_PATH" | awk '{print $1}')" \
        "$(shasum -a 256 "$METALLIB_PATH" | awk '{print $1}')" \
        "$(shasum -a 256 "$REFLECTION_PATH" | awk '{print $1}')" \
        >> "$RECEIPT"
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "MSC_PASS class=$SHADER_CLASS source=$SOURCE profile=$PROFILE"
done < "$MANIFEST"

if [[ "$PASS_COUNT" -ne "$EXPECTED_COUNT" ]]; then
    echo "Expected $EXPECTED_COUNT representative shaders, converted $PASS_COUNT" >&2
    exit 1
fi

python3 "$PROJECT_ROOT/script/validate_msc_reflection.py" \
    --manifest "$MANIFEST" \
    --reflection-dir "$OUTPUT_ABS"

echo "shader_count=$PASS_COUNT" >> "$RECEIPT"
echo "R4_MSC_GATE_READY shaders=$PASS_COUNT receipt=$RECEIPT"
