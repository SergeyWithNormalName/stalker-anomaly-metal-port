#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    echo "Usage: compile_r4_hud_particles_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"
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

SOURCES=(
    deffer_model_hud_bump-hq.vs deffer_base_hud_bump-hq.ps
    deffer_model_hud_flat.vs deffer_base_hud_flat.ps particle.vs particle.ps particle_hard.ps
    particle_distort.ps stub_notransform_aa_aa.vs combine_2_naa_d.ps
    # 3DSS precise-scope chain: scope_back and zwrite share the skinned model
    # vertex shader; the reticle has its own skinned vertex stage.
    deffer_model_flat.vs deffer_base_flat.ps models_scope_zwrite.ps
    models_scope_reticle.vs models_scope_reticle_precise.ps
)
if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ]] || ! command -v jq >/dev/null 2>&1; then
    echo "Missing source root, output directory, DXC, MSC, or jq" >&2
    exit 1
fi
for SOURCE in "${SOURCES[@]}"; do
    if [[ ! -f "$SOURCE_ROOT/$SOURCE" ]]; then
        echo "Missing HUD/particle shader source: $SOURCE_ROOT/$SOURCE" >&2
        exit 1
    fi
done

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the HUD/particle shader output: $OUTPUT_ABS" >&2
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
    local OUTPUT="$2"
    local PROFILE="$3"
    shift 3
    local DXIL="$OUTPUT_ABS/$OUTPUT.dxil"
    local METALLIB="$OUTPUT_ABS/$OUTPUT.metallib"
    local REFLECTION="$OUTPUT_ABS/$OUTPUT.reflection.json"
    local STDERR="$OUTPUT_ABS/$OUTPUT.stderr"
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" "$@" "$SOURCE_ROOT/$SOURCE" -Fo "$DXIL" 2> "$STDERR"
    "$MSC" "$DXIL" -o "$METALLIB" --output-reflection-file "$REFLECTION" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
    [[ -s "$DXIL" && -s "$METALLIB" && -s "$REFLECTION" ]] || {
        echo "Incomplete HUD/particle output: $OUTPUT" >&2
        exit 1
    }
}

verify_reflection() {
    local OUTPUT="$1"
    local EXPECTED_TYPE="$2"
    local REFLECTION="$OUTPUT_ABS/$OUTPUT.reflection.json"
    jq -e --arg expected_type "$EXPECTED_TYPE" '
        .ShaderType == $expected_type and
        (.TopLevelArgumentBuffer | type == "array") and
        (.state | type == "object") and
        (.ResourceCount | type == "number") and
        ($expected_type != "Fragment" or (.state.num_render_targets | type == "number"))
    ' "$REFLECTION" >/dev/null || {
        echo "Invalid MSC reflection contract for $OUTPUT" >&2
        exit 1
    }
}

verify_scope_reflection() {
    local OUTPUT="$1"
    local EXPECTED_TYPE="$2"
    local EXPECTED_RESOURCES="$3"
    local EXPECTED_TLAB="$4"
    local EXPECTED_RTS="$5"
    local EXPECTED_VERTEX_INPUTS="$6"
    local EXPECTED_VERTEX_OUTPUTS="$7"
    local REFLECTION="$OUTPUT_ABS/$OUTPUT.reflection.json"
    jq -e \
        --arg expected_type "$EXPECTED_TYPE" \
        --argjson expected_resources "$EXPECTED_RESOURCES" \
        --argjson expected_tlab "$EXPECTED_TLAB" \
        --argjson expected_rts "$EXPECTED_RTS" \
        --argjson expected_vertex_inputs "$EXPECTED_VERTEX_INPUTS" \
        --argjson expected_vertex_outputs "$EXPECTED_VERTEX_OUTPUTS" '
        .ShaderType == $expected_type and
        .ResourceCount == $expected_resources and
        (.TopLevelArgumentBuffer | length) == $expected_tlab and
        ($expected_type != "Fragment" or .state.num_render_targets == $expected_rts) and
        ($expected_type != "Vertex" or
            ((.state.vertex_inputs | length) == $expected_vertex_inputs and
             (.state.vertex_outputs | length) == $expected_vertex_outputs))
    ' "$REFLECTION" >/dev/null || {
        echo "Unexpected 3DSS scope MSC reflection contract for $OUTPUT" >&2
        exit 1
    }
}

verify_hud_skin4_reflection() {
    local OUTPUT="$1"
    local EXPECTED_VERTEX_OUTPUTS="$2"
    local REFLECTION="$OUTPUT_ABS/$OUTPUT.reflection.json"
    jq -e \
        --argjson expected_vertex_outputs "$EXPECTED_VERTEX_OUTPUTS" '
        .ShaderType == "Vertex" and
        .ResourceCount == 2 and
        .TopLevelArgumentBuffer == [
            {"EltOffset":0,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"CBV"},
            {"EltOffset":24,"Name":"","Size":24,"Slot":1,"Space":0,"Type":"CBV"}
        ] and
        .state.vertex_inputs == [
            {"columnCount":4,"elementType":"Float","index":0,"name":"position0"},
            {"columnCount":4,"elementType":"Float","index":1,"name":"normal0"},
            {"columnCount":4,"elementType":"Float","index":2,"name":"tangent0"},
            {"columnCount":4,"elementType":"Float","index":3,"name":"binormal0"},
            {"columnCount":2,"elementType":"Float","index":4,"name":"texcoord0"},
            {"columnCount":4,"elementType":"Float","index":5,"name":"texcoord1"}
        ] and
        .state.vertex_outputs == $expected_vertex_outputs
    ' "$REFLECTION" >/dev/null || {
        echo "Unexpected HUD SKIN_4 MSC reflection contract for $OUTPUT" >&2
        exit 1
    }
}

compile_shader deffer_model_hud_bump-hq.vs deffer_model_hud_bump-hq.vs.vs vs_6_0 -D SKIN_0=1
compile_shader deffer_base_hud_bump-hq.ps deffer_base_hud_bump-hq.ps.ps ps_6_0 -D SKIN_0=1
compile_shader deffer_model_hud_flat.vs deffer_model_hud_flat.vs.vs vs_6_0 -D SKIN_0=1
compile_shader deffer_base_hud_flat.ps deffer_base_hud_flat.ps.ps ps_6_0 -D SKIN_0=1
# Keep the established CPU-skinned SKIN_0 artifacts above unchanged. These
# independent names identify the native 40-byte four-weight hardware stream.
compile_shader deffer_model_hud_bump-hq.vs deffer_model_hud_bump-hq.vs.skin4 vs_6_0 -D SKIN_4=1
compile_shader deffer_model_hud_flat.vs deffer_model_hud_flat.vs.skin4 vs_6_0 -D SKIN_4=1
compile_shader particle.vs particle.vs.vs vs_6_0 -D USE_SOFT_PARTICLES=1
compile_shader particle.ps particle.ps.ps ps_6_0 -D USE_SOFT_PARTICLES=1
compile_shader particle.vs particle_hard.vs vs_6_0
compile_shader particle_hard.ps particle_hard.ps ps_6_0
compile_shader particle_distort.ps particle_distort.ps ps_6_0
compile_shader stub_notransform_aa_aa.vs combine_2.vs vs_6_0
compile_shader combine_2_naa_d.ps combine_2.ps ps_6_0

# models_scope_back.s and models_scope_zwrite.s both begin with
# deffer_model_flat. The SKIN_0 variant below is the exact skeletal vertex
# contract used by 3DSS; the scope fragments are cooked under the same active
# permutation for a self-contained, production-ready scope bundle.
compile_shader deffer_model_flat.vs deffer_model_flat.vs vs_6_0 -D SKIN_0=1
compile_shader deffer_base_flat.ps deffer_base_flat.ps ps_6_0 -D SKIN_0=1
compile_shader models_scope_zwrite.ps models_scope_zwrite.ps ps_6_0 -D SKIN_0=1
compile_shader models_scope_reticle.vs models_scope_reticle.vs vs_6_0 -D SKIN_0=1
compile_shader models_scope_reticle_precise.ps models_scope_reticle_precise.ps ps_6_0 -D SKIN_0=1

for OUTPUT_TYPE in \
    'deffer_model_hud_bump-hq.vs.vs Vertex' \
    'deffer_base_hud_bump-hq.ps.ps Fragment' \
    'deffer_model_hud_flat.vs.vs Vertex' \
    'deffer_base_hud_flat.ps.ps Fragment' \
    'deffer_model_hud_bump-hq.vs.skin4 Vertex' \
    'deffer_model_hud_flat.vs.skin4 Vertex' \
    'particle.vs.vs Vertex' \
    'particle.ps.ps Fragment' \
    'particle_hard.vs Vertex' \
    'particle_hard.ps Fragment' \
    'particle_distort.ps Fragment' \
    'combine_2.vs Vertex' \
    'combine_2.ps Fragment' \
    'deffer_model_flat.vs Vertex' \
    'deffer_base_flat.ps Fragment' \
    'models_scope_zwrite.ps Fragment' \
    'models_scope_reticle.vs Vertex' \
    'models_scope_reticle_precise.ps Fragment'; do
    read -r OUTPUT EXPECTED_TYPE <<< "$OUTPUT_TYPE"
    verify_reflection "$OUTPUT" "$EXPECTED_TYPE"
done

# These values are the exact MSC 4.0.1 reflection contracts for this source
# tree and define the TLAB/RT boundary that a future scope adapter must honor.
verify_scope_reflection deffer_model_flat.vs Vertex 2 2 0 5 7
verify_scope_reflection deffer_base_flat.ps Fragment 4 4 4 0 0
verify_scope_reflection models_scope_zwrite.ps Fragment 1 1 4 0 0
verify_scope_reflection models_scope_reticle.vs Vertex 1 1 0 5 10
verify_scope_reflection models_scope_reticle_precise.ps Fragment 14 14 1 0 0

# SKIN_4 must retain the exact two-CBV TLAB while exposing the hardware
# TEXCOORD1 indices after the five SKIN_0 attributes. These arrays lock the
# complete source-specific interpolator contracts, not merely their counts.
HUD_SKIN4_BUMP_VERTEX_OUTPUTS='[
    {"columnCount":2,"elementType":"Float","index":1,"name":"texcoord0"},
    {"columnCount":4,"elementType":"Float","index":2,"name":"texcoord1"},
    {"columnCount":3,"elementType":"Float","index":3,"name":"texcoord2"},
    {"columnCount":3,"elementType":"Float","index":4,"name":"texcoord3"},
    {"columnCount":3,"elementType":"Float","index":5,"name":"texcoord4"},
    {"columnCount":4,"elementType":"Float","index":6,"name":"texcoord7"},
    {"columnCount":4,"elementType":"Float","index":7,"name":"vel_current0"},
    {"columnCount":4,"elementType":"Float","index":8,"name":"vel_previous0"},
    {"columnCount":4,"elementType":"Float","index":9,"name":"sss_extra0"},
    {"columnCount":4,"elementType":"Float","index":10,"name":"sv_position0"}
]'
HUD_SKIN4_FLAT_VERTEX_OUTPUTS='[
    {"columnCount":2,"elementType":"Float","index":1,"name":"texcoord0"},
    {"columnCount":4,"elementType":"Float","index":2,"name":"texcoord1"},
    {"columnCount":3,"elementType":"Float","index":3,"name":"texcoord2"},
    {"columnCount":4,"elementType":"Float","index":4,"name":"texcoord7"},
    {"columnCount":4,"elementType":"Float","index":5,"name":"vel_current0"},
    {"columnCount":4,"elementType":"Float","index":6,"name":"vel_previous0"},
    {"columnCount":4,"elementType":"Float","index":7,"name":"sv_position0"}
]'
verify_hud_skin4_reflection deffer_model_hud_bump-hq.vs.skin4 "$HUD_SKIN4_BUMP_VERTEX_OUTPUTS"
verify_hud_skin4_reflection deffer_model_hud_flat.vs.skin4 "$HUD_SKIN4_FLAT_VERTEX_OUTPUTS"

RECEIPT="$OUTPUT_ABS/hud-particles-msc-receipt.txt"
{
    echo "evidence_scope=exact_r4_hud_particle_distortion_combine2_and_3dss_precise_scope_dxc_msc_artifacts"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "reflection_validator=$(command -v jq)"
    jq --version | sed 's/^/reflection_validator_version=/'
    echo "hud_define=SKIN_0=1"
    echo "hud_skin4_define=SKIN_4=1"
    echo "hud_skin4_bump_vertex=deffer_model_hud_bump-hq.vs.skin4"
    echo "hud_skin4_flat_vertex=deffer_model_hud_flat.vs.skin4"
    echo "hud_skin4_vertex_abi=P4/C4W0/C4W1/C4W2/UV2/IndicesC4"
    echo "hud_skin4_vertex_stride=40"
    echo "soft_particle_define=USE_SOFT_PARTICLES=1"
    echo "hard_particle_define=none"
    echo "scope_vertex_define=SKIN_0=1"
    echo "scope_fragment_define=SKIN_0=1"
    echo "scope_back_pair=deffer_model_flat.vs+deffer_base_flat.ps"
    echo "scope_zwrite_pair=deffer_model_flat.vs+models_scope_zwrite.ps"
    echo "scope_precise_reticle_pair=models_scope_reticle.vs+models_scope_reticle_precise.ps"
    for OUTPUT in deffer_model_hud_bump-hq.vs.vs deffer_base_hud_bump-hq.ps.ps \
        deffer_model_hud_flat.vs.vs deffer_base_hud_flat.ps.ps \
        deffer_model_hud_bump-hq.vs.skin4 deffer_model_hud_flat.vs.skin4 particle.vs.vs \
        particle.ps.ps particle_hard.vs particle_hard.ps particle_distort.ps \
        combine_2.vs combine_2.ps deffer_model_flat.vs deffer_base_flat.ps \
        models_scope_zwrite.ps models_scope_reticle.vs models_scope_reticle_precise.ps; do
        printf 'output=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\tshader_type=%s\tresource_count=%s\ttlab_entries=%s\trt_count=%s\n' \
            "$OUTPUT" \
            "$(shasum -a 256 "$OUTPUT_ABS/$OUTPUT.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$OUTPUT.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$OUTPUT.reflection.json" | awk '{print $1}')" \
            "$(grep -c 'warning:' "$OUTPUT_ABS/$OUTPUT.stderr" || true)" \
            "$(jq -r '.ShaderType' "$OUTPUT_ABS/$OUTPUT.reflection.json")" \
            "$(jq -r '.ResourceCount' "$OUTPUT_ABS/$OUTPUT.reflection.json")" \
            "$(jq '.TopLevelArgumentBuffer | length' "$OUTPUT_ABS/$OUTPUT.reflection.json")" \
            "$(jq -r '.state.num_render_targets' "$OUTPUT_ABS/$OUTPUT.reflection.json")"
    done
} > "$RECEIPT"

echo "R4_HUD_PARTICLES_MSC_READY shaders=18 receipt=$RECEIPT"
