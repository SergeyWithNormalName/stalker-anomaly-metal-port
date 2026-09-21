#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
MSC="${XRAY_METAL_SHADER_CONVERTER_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter}"
SOURCE_ROOT=""
DEF_VERTEX_OUTPUT=""
DEF_OBJECTS_OUTPUT=""

usage() {
    echo "Usage: compile_r4_static_families_msc.sh --source-root DIR --def-vertex-output DIR --def-objects-output DIR [--dxc FILE] [--msc FILE]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root) SOURCE_ROOT="$2"; shift 2 ;;
        --def-vertex-output) DEF_VERTEX_OUTPUT="$2"; shift 2 ;;
        --def-objects-output) DEF_OBJECTS_OUTPUT="$2"; shift 2 ;;
        --dxc) DXC="$2"; shift 2 ;;
        --msc) MSC="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

SOURCES=(deffer_base_flat.vs deffer_base_flat.ps deffer_base_aref_flat.ps deffer_base_bump.vs deffer_base_bump.ps deffer_base_steep-hq.ps)
if [[ -z "$SOURCE_ROOT" || -z "$DEF_VERTEX_OUTPUT" || -z "$DEF_OBJECTS_OUTPUT" ||
      ! -x "$DXC" || ! -x "$MSC" ]]; then
    echo "Missing source root, output directories, DXC, or MSC" >&2
    exit 1
fi
for SOURCE in "${SOURCES[@]}"; do
    if [[ ! -f "$SOURCE_ROOT/$SOURCE" ]]; then
        echo "Missing static-family shader source: $SOURCE_ROOT/$SOURCE" >&2
        exit 1
    fi
done

safe_output() {
    local REQUESTED="$1"
    local PARENT
    PARENT="$(cd "$(dirname "$REQUESTED")" && pwd -P)"
    local ABSOLUTE="$PARENT/$(basename "$REQUESTED")"
    if [[ -L "$ABSOLUTE" ]]; then
        echo "Refusing a symlink as shader output: $ABSOLUTE" >&2
        exit 1
    fi
    case "$ABSOLUTE/" in
        "$PROJECT_ROOT/runtime/"*) echo "Refusing generated shaders inside the live runtime" >&2; exit 1 ;;
    esac
    if [[ -d "$ABSOLUTE" ]] && [[ -n "$(find "$ABSOLUTE" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
        echo "Output directory must be new or empty: $ABSOLUTE" >&2
        exit 1
    fi
    mkdir -p "$ABSOLUTE"
    echo "$ABSOLUTE"
}

DEF_VERTEX_DIR="$(safe_output "$DEF_VERTEX_OUTPUT")"
DEF_OBJECTS_DIR="$(safe_output "$DEF_OBJECTS_OUTPUT")"

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

compile_shader() {
    local DIRECTORY="$1"
    local SOURCE="$2"
    local OUTPUT="$3"
    local PROFILE="$4"
    shift 4
    local DXIL="$DIRECTORY/$OUTPUT.dxil"
    local METALLIB="$DIRECTORY/$OUTPUT.metallib"
    local REFLECTION="$DIRECTORY/$OUTPUT.json"
    local STDERR="$DIRECTORY/$OUTPUT.stderr"
    "$DXC" -T "$PROFILE" -E main -HV 2016 -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" "$@" "$SOURCE_ROOT/$SOURCE" -Fo "$DXIL" 2> "$STDERR"
    "$MSC" "$DXIL" -o "$METALLIB" --output-reflection-file "$REFLECTION" \
        --deployment-os=macOS --minimum-os-build-version=14.0.0 --minimum-gpu-family=Metal3
    [[ -s "$DXIL" && -s "$METALLIB" && -s "$REFLECTION" ]] || {
        echo "Incomplete static-family output: $OUTPUT" >&2
        exit 1
    }
}

compile_shader "$DEF_VERTEX_DIR" deffer_base_flat.vs deffer_base_flat.vs vs_6_0 -D USE_R2_STATIC_SUN=1
compile_shader "$DEF_VERTEX_DIR" deffer_base_flat.ps deffer_base_flat.ps ps_6_0 -D USE_R2_STATIC_SUN=1
compile_shader "$DEF_VERTEX_DIR" deffer_base_aref_flat.ps deffer_base_aref_flat.ps ps_6_0 -D USE_R2_STATIC_SUN=1

compile_shader "$DEF_OBJECTS_DIR" deffer_base_flat.vs deffer_base_flat_lod.vs vs_6_0
compile_shader "$DEF_OBJECTS_DIR" deffer_base_flat.ps deffer_base_flat_lod.ps ps_6_0
compile_shader "$DEF_OBJECTS_DIR" deffer_base_bump.vs deffer_base_bump_lod.vs vs_6_0
compile_shader "$DEF_OBJECTS_DIR" deffer_base_bump.ps deffer_base_bump_lod.ps ps_6_0
compile_shader "$DEF_OBJECTS_DIR" deffer_base_bump.vs deffer_base_steep_hq_lod.vs vs_6_0
compile_shader "$DEF_OBJECTS_DIR" deffer_base_steep-hq.ps deffer_base_steep_hq_lod.ps ps_6_0

write_receipt() {
    local DIRECTORY="$1"
    local SCOPE="$2"
    shift 2
    local RECEIPT="$DIRECTORY/static-family-msc-receipt.txt"
    {
        echo "evidence_scope=$SCOPE"
        echo "source_root=$(realpath "$SOURCE_ROOT")"
        echo "source_tree_sha256=$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
        echo "dxc_path=$(realpath "$DXC")"
        "$DXC" --version | sed 's/^/dxc_version=/'
        echo "msc_path=$(realpath "$MSC")"
        "$MSC" --version | sed 's/^/msc_version=/'
        echo "common_define_args=${COMMON_DEFINES[*]}"
        for OUTPUT in "$@"; do
            printf 'output=%s\tdxil_sha256=%s\tmetallib_sha256=%s\treflection_sha256=%s\twarnings=%s\n' \
                "$OUTPUT" \
                "$(shasum -a 256 "$DIRECTORY/$OUTPUT.dxil" | awk '{print $1}')" \
                "$(shasum -a 256 "$DIRECTORY/$OUTPUT.metallib" | awk '{print $1}')" \
                "$(shasum -a 256 "$DIRECTORY/$OUTPUT.json" | awk '{print $1}')" \
                "$(grep -c 'warning:' "$DIRECTORY/$OUTPUT.stderr" || true)"
        done
    } > "$RECEIPT"
}

write_receipt "$DEF_VERTEX_DIR" exact_l05_bar_def_vertex_and_aref_v_static_color_flat \
    deffer_base_flat.vs deffer_base_flat.ps deffer_base_aref_flat.ps
write_receipt "$DEF_OBJECTS_DIR" exact_l05_bar_def_objects_lod_flat_bump_steep \
    deffer_base_flat_lod.vs deffer_base_flat_lod.ps deffer_base_bump_lod.vs \
    deffer_base_bump_lod.ps deffer_base_steep_hq_lod.vs deffer_base_steep_hq_lod.ps

echo "R4_STATIC_FAMILIES_MSC_READY shaders=9 def_vertex=$DEF_VERTEX_DIR def_objects=$DEF_OBJECTS_DIR"
