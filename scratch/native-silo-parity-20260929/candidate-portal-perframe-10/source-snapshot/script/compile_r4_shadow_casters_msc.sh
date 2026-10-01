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
        --help|-h)
            echo "Usage: compile_r4_shadow_casters_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ]]; then
    echo "Missing source root, output directory, DXC, or MSC" >&2
    exit 1
fi
SOURCES=(shadow_direct_tree.vs shadow_direct_tree_aref.vs shadow_direct_tree_s.vs shadow_direct_tree_s_aref.vs \
    shadow_direct_base.ps shadow_direct_base_aref.ps deffer_grass.vs deffer_grass.ps)
for SOURCE in "${SOURCES[@]}"; do
    if [[ ! -f "$SOURCE_ROOT/$SOURCE" ]]; then
        echo "Missing shadow-caster source: $SOURCE_ROOT/$SOURCE" >&2
        exit 1
    fi
done

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as shadow-caster output: $OUTPUT_ABS" >&2
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
    -D SMAP_size=2048 -D FP16_FILTER=1 -D FP16_BLEND=1 -D USE_HWSMAP=1 -D USE_HWSMAP_PCF=1
    -D USE_BRANCHING=1 -D USE_VTF=1 -D SM_5=1 -D SSFX_INT_GRASS=8
    -D SSFX_SSR_QUALITY=0 -D SSFX_WATER_QUALITY=1 -D SSFX_WATER_PARALLAX=2
    -D SSFX_IL_QUALITY=16 -D SSFX_AO_QUALITY=2 -D SSFX_POM_REFINE=0 -D SSFX_TERRA_POM_REFINE=0
    -D SSFX_SSS_DIR_QUALITY=6 -D SSFX_SSS_OMNI_QUALITY=2 -D SSFX_MODEXE=1 -D SKIN_NONE=1
)

compile_shader() {
    local SOURCE="$1" OUTPUT="$2" PROFILE="$3"
    shift 3
    local SOURCE_PATH="$SOURCE_ROOT/$SOURCE"
    if [[ -f "$SOURCE" ]]; then SOURCE_PATH="$SOURCE"; fi
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" "$@" "$SOURCE_PATH" -Fo "$OUTPUT_ABS/$OUTPUT.dxil" \
        2> "$OUTPUT_ABS/$OUTPUT.stderr"
    "$MSC" "$OUTPUT_ABS/$OUTPUT.dxil" -o "$OUTPUT_ABS/$OUTPUT.metallib" \
        --output-reflection-file "$OUTPUT_ABS/$OUTPUT.reflection.json" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
}

compile_shader shadow_direct_tree.vs shadow_direct_tree.vs vs_6_0
compile_shader shadow_direct_tree_aref.vs shadow_direct_tree_aref.vs vs_6_0
compile_shader shadow_direct_tree_s.vs shadow_direct_tree_s.vs vs_6_0
compile_shader shadow_direct_tree_s_aref.vs shadow_direct_tree_s_aref.vs vs_6_0
# Windows writes hardware depth; its optional color output is zero/texture
# color. Metal consumers sample our R32 color attachment as depth instead.
# Keep the original alpha/discard function, but return post-raster depth.
cat > "$OUTPUT_ABS/shadow_direct_base_depth.ps.hlsl" <<'EOF'
#define main original_shadow_main
#include "shadow_direct_base.ps"
#undef main
float main(float4 rasterPosition : SV_Position) : SV_Target0
{
    original_shadow_main();
    return rasterPosition.z;
}
EOF
cat > "$OUTPUT_ABS/shadow_direct_base_aref_depth.ps.hlsl" <<'EOF'
#define main original_shadow_main
#include "shadow_direct_base_aref.ps"
#undef main
float main(p_shadow_direct_aref I, float4 rasterPosition : SV_Position) : SV_Target0
{
    original_shadow_main(I);
    return rasterPosition.z;
}
EOF
compile_shader "$OUTPUT_ABS/shadow_direct_base_depth.ps.hlsl" shadow_direct_base_color.ps ps_6_0
compile_shader "$OUTPUT_ABS/shadow_direct_base_aref_depth.ps.hlsl" shadow_direct_base_aref_color.ps ps_6_0
compile_shader deffer_grass.vs deffer_grass_shadow.vs vs_6_0

# This wrapper preserves the Windows detail G-buffer alpha and screen-space
# dither/discard path exactly.  Its only adaptation is the R32Float color
# result required by the Metal depth-only shadow pass.
cat > "$OUTPUT_ABS/deffer_grass_shadow.ps.hlsl" <<'EOF'
#define main deffer_grass_gbuffer_main
#include "deffer_grass.ps"
#undef main

float main(p_bumped I, float4 rasterPosition : SV_Position) : SV_Target0
{
    deffer_grass_gbuffer_main(I);
    return rasterPosition.z;
}
EOF
compile_shader "$OUTPUT_ABS/deffer_grass_shadow.ps.hlsl" deffer_grass_shadow.ps ps_6_0

RECEIPT="$OUTPUT_ABS/shadow-casters-msc-receipt.txt"
{
    echo "evidence_scope=isolated_r4_static_flora_and_detail_shadow_shader_outputs"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "define_args=${COMMON_DEFINES[*]}"
    echo "tree_color_output=post_raster_depth_v1; original opaque/alpha functions retain discard behavior"
    echo "detail_ps_wrapper=deffer_grass_shadow.ps.hlsl; it calls the original deffer_grass.ps main and only changes SV_Target0 to SV_Position.z"
    for SHADER in shadow_direct_tree.vs shadow_direct_tree_aref.vs shadow_direct_tree_s.vs shadow_direct_tree_s_aref.vs \
        shadow_direct_base_color.ps shadow_direct_base_aref_color.ps deffer_grass_shadow.vs deffer_grass_shadow.ps; do
        printf 'shader=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
            "$SHADER" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SHADER.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SHADER.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SHADER.reflection.json" | awk '{print $1}')" \
            "$(grep -c 'warning:' "$OUTPUT_ABS/$SHADER.stderr" || true)"
    done
} > "$RECEIPT"
echo "R4_SHADOW_CASTERS_MSC_READY shaders=8 receipt=$RECEIPT"
