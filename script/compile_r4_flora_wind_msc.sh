#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root) SOURCE_ROOT="$2"; shift 2 ;;
        --output) OUTPUT_DIR="$2"; shift 2 ;;
        --dxc) DXC="$2"; shift 2 ;;
        --msc) MSC="$2"; shift 2 ;;
        --help|-h) echo "Usage: compile_r4_flora_wind_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done
if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ]]; then
    echo "Missing source root, output directory, DXC, or MSC" >&2; exit 1
fi
SOURCES=(deffer_tree_branch_bump-hq.vs deffer_tree_branch_aref_bump-hq.ps deffer_tree_flat.vs deffer_base_flat.ps)
for SOURCE in "${SOURCES[@]}"; do
    [[ -f "$SOURCE_ROOT/$SOURCE" ]] || { echo "Missing Flora source: $SOURCE_ROOT/$SOURCE" >&2; exit 1; }
done
OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
[[ ! -L "$OUTPUT_ABS" ]] || { echo "Refusing symlink output: $OUTPUT_ABS" >&2; exit 1; }
case "$OUTPUT_ABS/" in "$PROJECT_ROOT/runtime/"*) echo "Refusing generated shaders inside live runtime" >&2; exit 1 ;; esac
if [[ -d "$OUTPUT_ABS" ]] && [[ -n "$(find "$OUTPUT_ABS" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "Output directory must be new or empty: $OUTPUT_ABS" >&2; exit 1
fi
mkdir -p "$OUTPUT_ABS"

COMMON_DEFINES=(
    -D SMAP_size=2048 -D FP16_FILTER=1 -D FP16_BLEND=1 -D USE_HWSMAP=1 -D USE_HWSMAP_PCF=1
    -D USE_BRANCHING=1 -D USE_VTF=1 -D SM_5=1 -D SSFX_INT_GRASS=8 -D SSFX_WIND=1
    -D SSFX_SSR_QUALITY=0 -D SSFX_WATER_QUALITY=1 -D SSFX_WATER_PARALLAX=2
    -D SSFX_IL_QUALITY=16 -D SSFX_AO_QUALITY=2 -D SSFX_POM_REFINE=0 -D SSFX_TERRA_POM_REFINE=0
    -D SSFX_SSS_DIR_QUALITY=6 -D SSFX_SSS_OMNI_QUALITY=2 -D SSFX_MODEXE=1 -D SKIN_NONE=1
)
compile_shader() {
    local SOURCE="$1" OUTPUT="$2" PROFILE="$3"
    shift 3
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" "${COMMON_DEFINES[@]}" "$@" \
        "$SOURCE_ROOT/$SOURCE" -Fo "$OUTPUT_ABS/$OUTPUT.dxil" 2> "$OUTPUT_ABS/$OUTPUT.stderr"
    "$MSC" "$OUTPUT_ABS/$OUTPUT.dxil" -o "$OUTPUT_ABS/$OUTPUT.metallib" \
        --output-reflection-file "$OUTPUT_ABS/$OUTPUT.reflection.json" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
}
compile_shader deffer_tree_branch_bump-hq.vs leaf_wave_hq.vs vs_6_0 -D USE_PARALLAX=1
compile_shader deffer_tree_branch_aref_bump-hq.ps leaf_wave_hq.ps ps_6_0 -D USE_PARALLAX=1
compile_shader deffer_tree_flat.vs trunk_wave_hq.vs vs_6_0
compile_shader deffer_base_flat.ps trunk_wave_hq.ps ps_6_0

RECEIPT="$OUTPUT_ABS/flora-l05-bar-wind-msc-receipt.txt"
{
    echo "evidence_scope=l05_bar_regular_tree_flora_with_ssfx_wind_and_original_opaque_trunk_ps"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    SOURCE_TREE_SHA256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} + | shasum -a 256 | awk '{print $1}')
    echo "source_tree_sha256=$SOURCE_TREE_SHA256"
    echo "dxc_path=$(realpath "$DXC")"; "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"; "$MSC" --version | sed 's/^/msc_version=/'
    echo "defines=${COMMON_DEFINES[*]}"
    echo "leaf_vertex_source=deffer_tree_branch_bump-hq.vs"
    echo "leaf_fragment_source=deffer_tree_branch_aref_bump-hq.ps"
    echo "trunk_vertex_source=deffer_tree_flat.vs"
    echo "trunk_fragment_source=deffer_base_flat.ps"
    for SHADER in leaf_wave_hq.vs leaf_wave_hq.ps trunk_wave_hq.vs trunk_wave_hq.ps; do
        printf 'shader=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
            "$SHADER" "$(shasum -a 256 "$OUTPUT_ABS/$SHADER.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SHADER.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SHADER.reflection.json" | awk '{print $1}')" \
            "$(grep -c 'warning:' "$OUTPUT_ABS/$SHADER.stderr" || true)"
    done
} > "$RECEIPT"
echo "R4_FLORA_WIND_MSC_READY shaders=4 receipt=$RECEIPT"
