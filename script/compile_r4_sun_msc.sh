#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    cat <<'EOF'
Usage:
  compile_r4_sun_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]

Compiles the active non-MSAA, non-minmax R4 direct-sun mask and cascade
shaders to DXIL and Metal Shader Converter metallibs with reflection receipt.
EOF
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

SHADERS=(
    stub_notransform_t.vs
    accum_sun_mask_nomsaa.ps
    accum_sun.vs
    accum_sun_near_nomsaa_nominmax.ps
    accum_sun_far_nomsaa.ps
    shadow_direct_base.vs
    shadow_direct_base_aref.vs
)
if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ]]; then
    echo "Missing source root, output directory, DXC, or MSC" >&2
    exit 1
fi
for SHADER in "${SHADERS[@]}"; do
    if [[ ! -f "$SOURCE_ROOT/$SHADER" ]]; then
        echo "Missing direct-sun shader: $SOURCE_ROOT/$SHADER" >&2
        exit 1
    fi
done
COMPAT_SHADOW_PS="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/compat/shadow_direct_base_color.ps"
if [[ ! -f "$COMPAT_SHADOW_PS" ]]; then
    echo "Missing Metal R32 shadow-depth compatibility shader: $COMPAT_SHADOW_PS" >&2
    exit 1
fi
COMPAT_SHADOW_AREF_PS="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/compat/shadow_direct_base_aref_color.ps"
if [[ ! -f "$COMPAT_SHADOW_AREF_PS" ]]; then
    echo "Missing Metal R32 alpha-tested shadow-depth compatibility shader: $COMPAT_SHADOW_AREF_PS" >&2
    exit 1
fi

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the direct-sun shader output: $OUTPUT_ABS" >&2
    exit 1
fi
case "$OUTPUT_ABS/" in
    "$PROJECT_ROOT/runtime/"*)
        echo "Refusing to write generated shaders inside the live runtime" >&2
        exit 1
        ;;
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

RECEIPT="$OUTPUT_ABS/sun-msc-receipt.txt"
{
    echo "evidence_scope=exact_r4_non_msaa_non_minmax_direct_sun_dxc_msc_artifacts"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "entry_point=main"
    echo "hlsl_version=2016"
    echo "permutation=direct_sun_non_msaa_non_minmax_skin_none"
    echo "common_define_args=${COMMON_DEFINES[*]}"
} > "$RECEIPT"

COMPILE_SOURCES=("${SHADERS[@]}" shadow_direct_base_color.ps shadow_direct_base_aref_color.ps)
for SOURCE in "${COMPILE_SOURCES[@]}"; do
    STAGE="${SOURCE##*.}"
    PROFILE="${STAGE}_6_0"
    SOURCE_PATH="$SOURCE_ROOT/$SOURCE"
    if [[ "$SOURCE" == "shadow_direct_base_color.ps" ]]; then
        SOURCE_PATH="$COMPAT_SHADOW_PS"
    elif [[ "$SOURCE" == "shadow_direct_base_aref_color.ps" ]]; then
        SOURCE_PATH="$COMPAT_SHADOW_AREF_PS"
    fi
    DXIL="$OUTPUT_ABS/$SOURCE.dxil"
    METALLIB="$OUTPUT_ABS/$SOURCE.metallib"
    REFLECTION="$OUTPUT_ABS/$SOURCE.json"
    STDERR="$OUTPUT_ABS/$SOURCE.stderr"
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" "$SOURCE_PATH" -Fo "$DXIL" 2> "$STDERR"
    "$MSC" "$DXIL" -o "$METALLIB" --output-reflection-file "$REFLECTION" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 \
        --minimum-gpu-family=Metal3
    if [[ ! -s "$DXIL" || ! -s "$METALLIB" || ! -s "$REFLECTION" ]]; then
        echo "Incomplete direct-sun output for $SOURCE" >&2
        exit 1
    fi
    printf 'stage=%s\tsource=%s\tsource_sha256=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
        "$STAGE" "$SOURCE" \
        "$(shasum -a 256 "$SOURCE_PATH" | awk '{print $1}')" \
        "$(shasum -a 256 "$DXIL" | awk '{print $1}')" \
        "$(shasum -a 256 "$METALLIB" | awk '{print $1}')" \
        "$(shasum -a 256 "$REFLECTION" | awk '{print $1}')" \
        "$(grep -c 'warning:' "$STDERR" || true)" >> "$RECEIPT"
done

# The alpha-tested local-shadow path is admitted only when reflection proves
# that the original R4 material ABI survived DXIL -> MSC conversion.  This is
# deliberately a build-time fail-closed check; EngineScene repeats it before
# it selects the aref caster pipeline at runtime.
AREF_VS_REFLECTION="$OUTPUT_ABS/shadow_direct_base_aref.vs.json"
AREF_PS_REFLECTION="$OUTPUT_ABS/shadow_direct_base_aref_color.ps.json"
if ! grep -Fq '"ResourceCount":1' "$AREF_VS_REFLECTION" ||
   ! grep -Fq '"Type":"CBV"' "$AREF_VS_REFLECTION" ||
   ! grep -Fq '"name":"texcoord0"' "$AREF_VS_REFLECTION" ||
   ! grep -Fq '"name":"position0"' "$AREF_VS_REFLECTION" ||
   ! grep -Fq '"ResourceCount":2' "$AREF_PS_REFLECTION" ||
   ! grep -Fq '"Type":"SRV"' "$AREF_PS_REFLECTION" ||
   ! grep -Fq '"Type":"Sampler"' "$AREF_PS_REFLECTION" ||
   ! grep -Fq '"discards":true' "$AREF_PS_REFLECTION"; then
    echo "Alpha-tested local-shadow MSC reflection ABI mismatch" >&2
    exit 1
fi

echo "R4_SUN_MSC_READY shaders=${#COMPILE_SOURCES[@]} receipt=$RECEIPT"
