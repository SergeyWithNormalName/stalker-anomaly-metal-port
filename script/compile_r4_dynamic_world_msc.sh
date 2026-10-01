#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    echo "Usage: compile_r4_dynamic_world_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"
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

SOURCES=(deffer_base_bump.vs deffer_base_bump.ps deffer_base_flat.vs deffer_base_flat.ps deffer_model_bump.vs deffer_model_flat.vs model_def_lplanes.vs base_lplanes.ps)
if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ]]; then
    echo "Missing source root, output directory, DXC, or MSC" >&2
    exit 1
fi
for SOURCE in "${SOURCES[@]}"; do
    if [[ ! -f "$SOURCE_ROOT/$SOURCE" ]]; then
        echo "Missing dynamic-world shader source: $SOURCE_ROOT/$SOURCE" >&2
        exit 1
    fi
done

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the dynamic-world shader output: $OUTPUT_ABS" >&2
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
    -D SSFX_SSS_OMNI_QUALITY=2 -D SSFX_MODEXE=1
)

compile_shader() {
    local SOURCE="$1"
    local PROFILE="$2"
    local SKIN_DEFINE="$3"
    local DXIL="$OUTPUT_ABS/$SOURCE.dxil"
    local METALLIB="$OUTPUT_ABS/$SOURCE.metallib"
    local REFLECTION="$OUTPUT_ABS/$SOURCE.json"
    local STDERR="$OUTPUT_ABS/$SOURCE.stderr"
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" -D "$SKIN_DEFINE=1" \
        "$SOURCE_ROOT/$SOURCE" -Fo "$DXIL" 2> "$STDERR"
    "$MSC" "$DXIL" -o "$METALLIB" --output-reflection-file "$REFLECTION" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 \
        --minimum-gpu-family=Metal3
    if [[ ! -s "$DXIL" || ! -s "$METALLIB" || ! -s "$REFLECTION" ]]; then
        echo "Incomplete dynamic-world output: $SOURCE" >&2
        exit 1
    fi
}

compile_shader deffer_base_bump.vs vs_6_0 SKIN_NONE
compile_shader deffer_base_bump.ps ps_6_0 SKIN_NONE
compile_shader deffer_base_flat.vs vs_6_0 SKIN_NONE
compile_shader deffer_base_flat.ps ps_6_0 SKIN_NONE
compile_shader deffer_model_bump.vs vs_6_0 SKIN_0
compile_shader deffer_model_flat.vs vs_6_0 SKIN_0
compile_shader model_def_lplanes.vs vs_6_0 SKIN_0
compile_shader base_lplanes.ps ps_6_0 SKIN_NONE

RECEIPT="$OUTPUT_ABS/dynamic-world-msc-receipt.txt"
{
    echo "evidence_scope=exact_live_dynamic_ogf_base_cpu_skinned_and_models_lightplanes_dxc_msc_artifacts"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "entry_point=main"
    echo "hlsl_version=2016"
    echo "common_define_args=${COMMON_DEFINES[*]}"
    echo "base_skin_define=SKIN_NONE=1"
    echo "model_skin_define=SKIN_0=1"
    for SOURCE in "${SOURCES[@]}"; do
        printf 'output=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
            "$SOURCE" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.json" | awk '{print $1}')" \
            "$(grep -c 'warning:' "$OUTPUT_ABS/$SOURCE.stderr" || true)"
    done
} > "$RECEIPT"

echo "R4_DYNAMIC_WORLD_MSC_READY shaders=${#SOURCES[@]} receipt=$RECEIPT"
