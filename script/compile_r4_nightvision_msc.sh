#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    echo "Usage: compile_r4_nightvision_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]"
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

SOURCES=(common.h night_vision.h stub_screen_space.vs pp_blur.ps nightvision_gen_1.ps nightvision_gen_2.ps nightvision_gen_3.ps)
if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ]] || ! command -v jq >/dev/null 2>&1; then
    echo "Missing source root, output directory, DXC, MSC, or jq" >&2
    exit 1
fi
for SOURCE in "${SOURCES[@]}"; do
    [[ -f "$SOURCE_ROOT/$SOURCE" ]] || {
        echo "Missing R4 night vision shader source: $SOURCE_ROOT/$SOURCE" >&2
        exit 1
    }
done

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the night vision shader output: $OUTPUT_ABS" >&2
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

require_dxil_text() {
    local FILE="$1"
    local NEEDLE="$2"
    grep -F -q "$NEEDLE" "$FILE" || {
        echo "DXIL reflection gate failed for $(basename "$FILE"): $NEEDLE" >&2
        exit 1
    }
}

verify_dxil() {
    local OUTPUT="$1"
    local STAGE="$2"
    local DUMP="$OUTPUT_ABS/$OUTPUT.dxil.reflection.txt"
    if [[ "$STAGE" == "vs" ]]; then
        require_dxil_text "$DUMP" 'static_globals                    cbuffer'
        require_dxil_text "$DUMP" 'static_globals;                                 ; Offset:    0 Size:   528'
        require_dxil_text "$DUMP" 'float4 screen_res;                            ; Offset:  416'
        require_dxil_text "$DUMP" 'static_globals                    cbuffer      NA          NA     CB0            cb0'
        return
    fi
    if [[ "$OUTPUT" == "pp_blur.ps" ]]; then
        require_dxil_text "$DUMP" '$Globals;                                       ; Offset:    0 Size:   304'
        require_dxil_text "$DUMP" 'float4 shader_param_8;                        ; Offset:  256'
        require_dxil_text "$DUMP" 'float4 shader_param_7;                        ; Offset:  272'
        require_dxil_text "$DUMP" 'float4 blur_params;                           ; Offset:  288'
        require_dxil_text "$DUMP" 'static_globals;                                 ; Offset:    0 Size:   528'
        require_dxil_text "$DUMP" 's_position'
        require_dxil_text "$DUMP" 's_image'
        require_dxil_text "$DUMP" 'smp_rtlinear'
        return
    fi
    require_dxil_text "$DUMP" '$Globals;                                       ; Offset:    0 Size:   192'
    require_dxil_text "$DUMP" 'static_globals;                                 ; Offset:    0 Size:   528'
    require_dxil_text "$DUMP" 'float4 shader_param_8;                        ; Offset:  160'
    require_dxil_text "$DUMP" 'float4 shader_param_7;                        ; Offset:  176'
    require_dxil_text "$DUMP" 'float4 timers;                                ; Offset:  256'
    require_dxil_text "$DUMP" 'float4 screen_res;                            ; Offset:  416'
    require_dxil_text "$DUMP" '$Globals                          cbuffer      NA          NA     CB0            cb0'
    require_dxil_text "$DUMP" 'static_globals                    cbuffer      NA          NA     CB1            cb1'
    require_dxil_text "$DUMP" 'smp_rtlinear                      sampler'
    for RESOURCE in s_position s_image s_blur_2 s_blur_4 s_blur_8; do
        require_dxil_text "$DUMP" "$RESOURCE"
    done
}

verify_msc() {
    local OUTPUT="$1"
    local STAGE="$2"
    local REFLECTION="$OUTPUT_ABS/$OUTPUT.reflection.json"
    if [[ "$STAGE" == "vs" ]]; then
        jq -e '
            .ShaderType == "Vertex" and .ResourceCount == 1 and
            .TopLevelArgumentBuffer == [
                {"EltOffset":0,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"CBV"}
            ] and
            .state.vertex_inputs == [
                {"columnCount":2,"elementType":"Float","index":0,"name":"texcoord0"},
                {"columnCount":4,"elementType":"Float","index":1,"name":"positiont0"}
            ] and
            .state.vertex_outputs == [
                {"columnCount":4,"elementType":"Float","index":1,"name":"sv_position0"},
                {"columnCount":2,"elementType":"Float","index":2,"name":"texcoord0"}
            ]
        ' "$REFLECTION" >/dev/null || {
            echo "Unexpected fullscreen VS MSC reflection contract: $OUTPUT" >&2
            exit 1
        }
        return
    fi
    if [[ "$OUTPUT" == "pp_blur.ps" ]]; then
        jq -e '
            .ShaderType == "Fragment" and .ResourceCount == 5 and
            .TopLevelArgumentBuffer == [
                {"EltOffset":0,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"SRV"},
                {"EltOffset":24,"Name":"","Size":24,"Slot":1,"Space":0,"Type":"SRV"},
                {"EltOffset":48,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"CBV"},
                {"EltOffset":72,"Name":"","Size":24,"Slot":1,"Space":0,"Type":"CBV"},
                {"EltOffset":96,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"Sampler"}
            ] and .state.num_render_targets == 1 and .state.discards == false
        ' "$REFLECTION" >/dev/null || {
            echo "Unexpected night vision blur MSC reflection contract: $OUTPUT" >&2
            exit 1
        }
        return
    fi
    jq -e '
        .ShaderType == "Fragment" and .ResourceCount == 8 and
        .TopLevelArgumentBuffer == [
            {"EltOffset":0,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"SRV"},
            {"EltOffset":24,"Name":"","Size":24,"Slot":1,"Space":0,"Type":"SRV"},
            {"EltOffset":48,"Name":"","Size":24,"Slot":2,"Space":0,"Type":"SRV"},
            {"EltOffset":72,"Name":"","Size":24,"Slot":3,"Space":0,"Type":"SRV"},
            {"EltOffset":96,"Name":"","Size":24,"Slot":4,"Space":0,"Type":"SRV"},
            {"EltOffset":120,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"CBV"},
            {"EltOffset":144,"Name":"","Size":24,"Slot":1,"Space":0,"Type":"CBV"},
            {"EltOffset":168,"Name":"","Size":24,"Slot":0,"Space":0,"Type":"Sampler"}
        ] and .state.num_render_targets == 1 and .state.discards == false
    ' "$REFLECTION" >/dev/null || {
        echo "Unexpected night vision PS MSC reflection contract: $OUTPUT" >&2
        exit 1
    }
}

SHADERS=(
    'vs|stub_screen_space.vs'
    'ps|pp_blur.ps'
    'ps|nightvision_gen_1.ps'
    'ps|nightvision_gen_2.ps'
    'ps|nightvision_gen_3.ps'
)
for ENTRY in "${SHADERS[@]}"; do
    STAGE="${ENTRY%%|*}"
    SOURCE="${ENTRY#*|}"
    PROFILE="${STAGE}_6_0"
    DXIL="$OUTPUT_ABS/$SOURCE.dxil"
    METALLIB="$OUTPUT_ABS/$SOURCE.metallib"
    REFLECTION="$OUTPUT_ABS/$SOURCE.reflection.json"
    STDERR="$OUTPUT_ABS/$SOURCE.stderr"
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" "$SOURCE_ROOT/$SOURCE" -Fo "$DXIL" 2> "$STDERR"
    "$DXC" -dumpbin "$DXIL" > "$DXIL.reflection.txt"
    verify_dxil "$SOURCE" "$STAGE"
    "$MSC" "$DXIL" -o "$METALLIB" --output-reflection-file "$REFLECTION" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
    [[ -s "$DXIL" && -s "$METALLIB" && -s "$REFLECTION" ]] || {
        echo "Incomplete night vision output: $SOURCE" >&2
        exit 1
    }
    verify_msc "$SOURCE" "$STAGE"
done

RECEIPT="$OUTPUT_ABS/nightvision-msc-receipt.txt"
{
    echo "evidence_scope=exact_r4_global_nightvision_gen1_gen2_gen3_and_blur_pyramid_dxc_dxil_msc_artifacts"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "entry_point=main"
    echo "hlsl_version=2016"
    echo "common_define_args=${COMMON_DEFINES[*]}"
    echo "vs_contract=stub_screen_space.vs:resources=1,tlab=CBV:b0@0,size=24,static_globals=528,screen_res=416"
    echo "ps_contract=nightvision_gen_[1,2,3].ps:resources=8,tlab=SRV:t0@0,t1@24,t2@48,t3@72,t4@96;CBV:b0@120,b1@144;Sampler:s0@168,size=192,rt=1"
    echo "ps_constant_layout=globals=192:shader_param_8@160,shader_param_7@176;static_globals=528:timers@256,screen_res@416"
    echo "blur_contract=pp_blur.ps:resources=5,tlab=SRV:t0@0,t1@24;CBV:b0@48,b1@72;Sampler:s0@96,size=120,rt=1,globals=304:shader_param_8@256,shader_param_7@272,blur_params@288"
    for ENTRY in "${SHADERS[@]}"; do
        STAGE="${ENTRY%%|*}"
        SOURCE="${ENTRY#*|}"
        printf 'stage=%s\tsource=%s\tsource_sha256=%s\tdxil_sha256=%s\tdxil_reflection_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
            "$STAGE" "$SOURCE" \
            "$(shasum -a 256 "$SOURCE_ROOT/$SOURCE" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.dxil.reflection.txt" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_ABS/$SOURCE.reflection.json" | awk '{print $1}')" \
            "$(grep -c 'warning:' "$OUTPUT_ABS/$SOURCE.stderr" || true)"
    done
} > "$RECEIPT"

echo "R4_NIGHTVISION_MSC_READY shaders=4 receipt=$RECEIPT"
