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
        *) echo "Usage: $0 --source-root DIR --output DIR" >&2; exit 2 ;;
    esac
done

[[ -n "$SOURCE_ROOT" && -n "$OUTPUT_DIR" && -x "$DXC" && -x "$MSC" ]] || {
    echo "Missing source root, output directory, DXC, or MSC" >&2; exit 1;
}
for SOURCE in stub_screen_space.vs ssfx_rain.ps effects_rain.vs effects_rain.ps; do
    [[ -f "$SOURCE_ROOT/$SOURCE" ]] || { echo "Missing rain source: $SOURCE_ROOT/$SOURCE" >&2; exit 1; }
done
if [[ -d "$OUTPUT_DIR" && -n "$(find "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "Output directory must be new or empty: $OUTPUT_DIR" >&2; exit 1
fi
mkdir -p "$OUTPUT_DIR"

DEFINES=(
    -D SMAP_size=2048 -D FP16_FILTER=1 -D FP16_BLEND=1 -D USE_HWSMAP=1
    -D USE_HWSMAP_PCF=1 -D USE_BRANCHING=1 -D USE_VTF=1 -D SM_5=1
    -D SSFX_INT_GRASS=8 -D SSFX_RAIN_QUALITY=2
)
compile() {
    local SOURCE="$1" OUTPUT="$2" PROFILE="$3"
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" "${DEFINES[@]}" \
        "$SOURCE_ROOT/$SOURCE" -Fo "$OUTPUT_DIR/$OUTPUT.dxil" 2> "$OUTPUT_DIR/$OUTPUT.stderr"
    "$MSC" "$OUTPUT_DIR/$OUTPUT.dxil" -o "$OUTPUT_DIR/$OUTPUT.metallib" \
        --output-reflection-file "$OUTPUT_DIR/$OUTPUT.reflection.json" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
    jq -e --arg type "${PROFILE%%_*}" '
        .ShaderType == (if $type == "vs" then "Vertex" else "Fragment" end) and
        (.TopLevelArgumentBuffer | type == "array") and (.ResourceCount | type == "number")
    ' "$OUTPUT_DIR/$OUTPUT.reflection.json" >/dev/null
}
compile effects_rain.vs effects_rain.vs vs_6_0
compile effects_rain.ps effects_rain.ps ps_6_0
compile stub_screen_space.vs stub_screen_space.vs vs_6_0
compile ssfx_rain.ps ssfx_rain.ps ps_6_0
jq -e '.ResourceCount == 1 and (.TopLevelArgumentBuffer | length) == 1' "$OUTPUT_DIR/stub_screen_space.vs.reflection.json" >/dev/null
jq -e '.ResourceCount == 4 and (.TopLevelArgumentBuffer | length) == 4 and .state.num_render_targets == 1' "$OUTPUT_DIR/ssfx_rain.ps.reflection.json" >/dev/null
jq -e '.ResourceCount == 2 and (.TopLevelArgumentBuffer | length) == 2' "$OUTPUT_DIR/effects_rain.vs.reflection.json" >/dev/null
jq -e '.ResourceCount == 8 and (.TopLevelArgumentBuffer | length) == 8 and .state.num_render_targets == 3' "$OUTPUT_DIR/effects_rain.ps.reflection.json" >/dev/null
{
    echo "evidence_scope=exact_r4_effects_rain_dxc_dxil_msc"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "ssfx_rain_quality=2"
    for OUTPUT in stub_screen_space.vs ssfx_rain.ps effects_rain.vs effects_rain.ps; do
        printf 'output=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\tshader_type=%s\tresources=%s\ttlab_entries=%s\trt_count=%s\n' \
            "$OUTPUT" "$(shasum -a 256 "$OUTPUT_DIR/$OUTPUT.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_DIR/$OUTPUT.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_DIR/$OUTPUT.reflection.json" | awk '{print $1}')" \
            "$(jq -r .ShaderType "$OUTPUT_DIR/$OUTPUT.reflection.json")" \
            "$(jq -r .ResourceCount "$OUTPUT_DIR/$OUTPUT.reflection.json")" \
            "$(jq '.TopLevelArgumentBuffer | length' "$OUTPUT_DIR/$OUTPUT.reflection.json")" \
            "$(jq -r '.state.num_render_targets // 0' "$OUTPUT_DIR/$OUTPUT.reflection.json")"
    done
} > "$OUTPUT_DIR/rain-msc-receipt.txt"
echo "R4_RAIN_MSC_READY shaders=4 passes=ssfx_rain_1_8+effects_rain receipt=$OUTPUT_DIR/rain-msc-receipt.txt"
