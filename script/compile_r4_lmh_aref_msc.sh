#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    echo "Usage: compile_r4_lmh_aref_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root) SOURCE_ROOT="$2"; shift 2 ;;
        --output) OUTPUT_DIR="$2"; shift 2 ;;
        --dxc) DXC="$2"; shift 2 ;;
        --msc) MSC="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

SOURCE=deffer_base_lmh_aref_flat.ps
if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ||
      ! -f "$SOURCE_ROOT/$SOURCE" ]]; then
    echo "Missing source root, output directory, DXC, MSC, or LMH aref source" >&2
    exit 1
fi

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the LMH aref shader output: $OUTPUT_ABS" >&2
    exit 1
fi
case "$OUTPUT_ABS/" in
    "$PROJECT_ROOT/runtime/"*) echo "Refusing generated shaders inside the live runtime" >&2; exit 1 ;;
esac
if [[ -d "$OUTPUT_ABS" ]] && [[ -n "$(find "$OUTPUT_ABS" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "Output directory must be new or empty: $OUTPUT_ABS" >&2
    exit 1
fi
mkdir -p "$OUTPUT_ABS"

COMMON_DEFINES=(
    -D SMAP_size=2048 -D FP16_FILTER=1 -D FP16_BLEND=1
    -D USE_HWSMAP=1 -D USE_HWSMAP_PCF=1 -D USE_BRANCHING=1
    -D USE_VTF=1 -D SM_5=1 -D SSFX_INT_GRASS=8
    -D SSFX_SSR_QUALITY=0 -D SSFX_WATER_QUALITY=1
    -D SSFX_WATER_PARALLAX=2 -D SSFX_IL_QUALITY=16
    -D SSFX_AO_QUALITY=2 -D SSFX_POM_REFINE=0
    -D SSFX_TERRA_POM_REFINE=0 -D SSFX_SSS_DIR_QUALITY=6
    -D SSFX_SSS_OMNI_QUALITY=2 -D SSFX_MODEXE=1 -D SKIN_NONE=1
)

DXIL="$OUTPUT_ABS/$SOURCE.dxil"
METALLIB="$OUTPUT_ABS/$SOURCE.metallib"
REFLECTION="$OUTPUT_ABS/$SOURCE.json"
STDERR="$OUTPUT_ABS/$SOURCE.stderr"
"$DXC" -T ps_6_0 -E main -HV 2016 -I "$SOURCE_ROOT" \
    "${COMMON_DEFINES[@]}" "$SOURCE_ROOT/$SOURCE" -Fo "$DXIL" 2> "$STDERR"
"$MSC" "$DXIL" -o "$METALLIB" --output-reflection-file "$REFLECTION" \
    --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
[[ -s "$DXIL" && -s "$METALLIB" && -s "$REFLECTION" ]] || {
    echo "Incomplete LMH aref output" >&2
    exit 1
}

RECEIPT="$OUTPUT_ABS/lmh-aref-msc-receipt.txt"
{
    echo "evidence_scope=exact_r4_lmh_flat_alpha_test_dxc_msc_artifact"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "common_define_args=${COMMON_DEFINES[*]}"
    printf 'stage=ps\tmetallib_sha256=%s\treflection_sha256=%s\tdxil_sha256=%s\twarnings=%s\n' \
        "$(shasum -a 256 "$METALLIB" | awk '{print $1}')" \
        "$(shasum -a 256 "$REFLECTION" | awk '{print $1}')" \
        "$(shasum -a 256 "$DXIL" | awk '{print $1}')" \
        "$(grep -c 'warning:' "$STDERR" || true)"
} > "$RECEIPT"

echo "R4_LMH_AREF_MSC_READY shaders=1 receipt=$RECEIPT"
