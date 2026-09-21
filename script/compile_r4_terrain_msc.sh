#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    echo "Usage: compile_r4_terrain_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"
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

if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ||
      ! -f "$SOURCE_ROOT/deffer_terrain_flat_d.vs" ||
      ! -f "$SOURCE_ROOT/deffer_terrain_high_flat_d.ps" ]]; then
    echo "Missing source root, output directory, DXC, MSC, or terrain shader" >&2
    exit 1
fi

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the terrain shader output: $OUTPUT_ABS" >&2
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
    -D SMAP_size=2048
    -D FP16_FILTER=1
    -D FP16_BLEND=1
    -D USE_HWSMAP=1
    -D USE_HWSMAP_PCF=1
    -D USE_BRANCHING=1
    -D USE_VTF=1
    -D SM_5=1
    -D SSFX_INT_GRASS=8
    -D SSFX_SSR_QUALITY=0
    -D SSFX_WATER_QUALITY=1
    -D SSFX_WATER_PARALLAX=2
    -D SSFX_IL_QUALITY=16
    -D SSFX_AO_QUALITY=2
    -D SSFX_POM_REFINE=0
    -D SSFX_TERRA_POM_REFINE=0
    -D SSFX_SSS_DIR_QUALITY=6
    -D SSFX_SSS_OMNI_QUALITY=2
    -D SSFX_MODEXE=1
    -D SKIN_NONE=1
)

VERTEX_SOURCE="deffer_terrain_flat_d.vs"
FRAGMENT_SOURCE="deffer_terrain_high_flat_d.ps"
VERTEX_DXIL="$OUTPUT_ABS/$VERTEX_SOURCE.dxil"
FRAGMENT_DXIL="$OUTPUT_ABS/$FRAGMENT_SOURCE.dxil"
VERTEX_METALLIB="$OUTPUT_ABS/$VERTEX_SOURCE.metallib"
FRAGMENT_METALLIB="$OUTPUT_ABS/$FRAGMENT_SOURCE.metallib"
VERTEX_JSON="$OUTPUT_ABS/$VERTEX_SOURCE.json"
FRAGMENT_JSON="$OUTPUT_ABS/$FRAGMENT_SOURCE.json"

"$DXC" -T vs_6_0 -E main -HV 2016 -I "$SOURCE_ROOT" "${COMMON_DEFINES[@]}" \
    "$SOURCE_ROOT/$VERTEX_SOURCE" -Fo "$VERTEX_DXIL" 2> "$OUTPUT_ABS/$VERTEX_SOURCE.stderr"
"$DXC" -T ps_6_0 -E main -HV 2016 -I "$SOURCE_ROOT" "${COMMON_DEFINES[@]}" \
    "$SOURCE_ROOT/$FRAGMENT_SOURCE" -Fo "$FRAGMENT_DXIL" 2> "$OUTPUT_ABS/$FRAGMENT_SOURCE.stderr"
"$MSC" "$VERTEX_DXIL" -o "$VERTEX_METALLIB" --output-reflection-file "$VERTEX_JSON" \
    --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
"$MSC" "$FRAGMENT_DXIL" -o "$FRAGMENT_METALLIB" --output-reflection-file "$FRAGMENT_JSON" \
    --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3

RECEIPT="$OUTPUT_ABS/terrain-l05-bar-msc-receipt.txt"
{
    echo "evidence_scope=exact_l05_bar_hq_terrain_dxc_msc_artifacts"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "vertex_source=$VERTEX_SOURCE"
    echo "fragment_source=$FRAGMENT_SOURCE"
    echo "vertex_metallib_sha256=$(shasum -a 256 "$VERTEX_METALLIB" | awk '{print $1}')"
    echo "fragment_metallib_sha256=$(shasum -a 256 "$FRAGMENT_METALLIB" | awk '{print $1}')"
    echo "vertex_reflection_sha256=$(shasum -a 256 "$VERTEX_JSON" | awk '{print $1}')"
    echo "fragment_reflection_sha256=$(shasum -a 256 "$FRAGMENT_JSON" | awk '{print $1}')"
    echo "common_define_args=${COMMON_DEFINES[*]}"
} > "$RECEIPT"

echo "R4_TERRAIN_MSC_READY shaders=2 receipt=$RECEIPT"
