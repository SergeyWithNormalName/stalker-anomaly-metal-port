#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
OUTPUT_DIR=""

usage() {
    echo "Usage: compile_r4_clouds_msc.sh --source-root DIR --output DIR [--dxc FILE] [--msc FILE]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root) SOURCE_ROOT="$2"; shift 2 ;;
        --output) OUTPUT_DIR="$2"; shift 2 ;;
        --dxc) DXC="$2"; shift 2 ;;
        --msc) MSC="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" || ! -x "$DXC" || ! -x "$MSC" ||
      ! -f "$SOURCE_ROOT/clouds.vs" || ! -f "$SOURCE_ROOT/clouds.ps" ]]; then
    echo "Missing source root, output directory, DXC, MSC, or clouds shader" >&2
    exit 2
fi

SOURCE_ROOT="$(cd "$SOURCE_ROOT" && pwd)"
OUTPUT_DIR="$(mkdir -p "$OUTPUT_DIR" && cd "$OUTPUT_DIR" && pwd)"
if [[ -n "$(find "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "Cloud output directory must be empty: $OUTPUT_DIR" >&2
    exit 2
fi

COMMON_DEFINES=(
    -DUSE_R2 -DUSE_DX10 -DUSE_DX11 -DUSE_SOFT_WATER -DSSFX_WATER_QUALITY=1
    -DSSFX_WATER_PARALLAX=2 -DSM_5_0 -DMSAA_OPTIMIZATION=0
)
for stage in vs ps; do
    source="$SOURCE_ROOT/clouds.$stage"
    dxil="$OUTPUT_DIR/clouds.$stage.dxil"
    metallib="$OUTPUT_DIR/clouds.$stage.metallib"
    reflection="$OUTPUT_DIR/clouds.$stage.json"
    stderr="$OUTPUT_DIR/clouds.$stage.stderr"
    profile="$([[ "$stage" == vs ]] && echo vs_6_0 || echo ps_6_0)"
    "$DXC" -T "$profile" -E main -HV 2016 -I "$SOURCE_ROOT" "${COMMON_DEFINES[@]}" \
        "$source" -Fo "$dxil" 2>"$stderr"
    "$MSC" "$dxil" -o "$metallib" --output-reflection-file "$reflection" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
    [[ -s "$dxil" && -s "$metallib" && -s "$reflection" ]] || {
        echo "Incomplete clouds output for $stage" >&2
        exit 1
    }
done

receipt="$OUTPUT_DIR/clouds-msc-receipt.txt"
{
    echo "evidence_scope=exact_active_r4_gamma_clouds_non_msaa_dxc_msc_artifacts"
    echo "source_root=$SOURCE_ROOT"
    echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
    echo "permutation=clouds_non_msaa_r4_gamma"
    echo "dxc_path=$(realpath "$DXC")"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "msc_path=$(realpath "$MSC")"
    "$MSC" --version | sed 's/^/msc_version=/'
    echo "common_define_args=${COMMON_DEFINES[*]}"
    for stage in vs ps; do
        printf 'output=clouds.%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
            "$stage" "$(shasum -a 256 "$OUTPUT_DIR/clouds.$stage.dxil" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_DIR/clouds.$stage.metallib" | awk '{print $1}')" \
            "$(shasum -a 256 "$OUTPUT_DIR/clouds.$stage.json" | awk '{print $1}')" \
            "$(wc -l < "$OUTPUT_DIR/clouds.$stage.stderr" | tr -d ' ')"
    done
} > "$receipt"

echo "R4_CLOUDS_MSC_READY shaders=2 receipt=$receipt"
