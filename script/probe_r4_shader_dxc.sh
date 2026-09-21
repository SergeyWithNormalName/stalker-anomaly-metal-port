#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXC="${XRAY_DXC_EXECUTABLE:-$PROJECT_ROOT/build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc}"
SOURCE_ROOT=""
OUTPUT_DIR=""
MANIFEST="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/representative-gate.tsv"

usage() {
    cat <<'EOF'
Usage:
  probe_r4_shader_dxc.sh --source-root DIR --output DIR [--dxc FILE]

Compiles the manifest-listed representative real R4/GAMMA shaders to Shader Model 6 DXIL.
The source root must be produced by materialize_r4_shader_sources.sh.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source-root)
            SOURCE_ROOT="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --dxc)
            DXC="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$SOURCE_ROOT" || -z "$OUTPUT_DIR" ]]; then
    usage >&2
    exit 2
fi
if [[ ! -x "$DXC" || ! -f "$SOURCE_ROOT/common.h" || ! -f "$MANIFEST" ]]; then
    echo "Missing DXC, materialized shader source root, or manifest" >&2
    exit 1
fi

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the DXIL output: $OUTPUT_ABS" >&2
    exit 1
fi
case "$OUTPUT_ABS/" in
    "$PROJECT_ROOT/runtime/"*)
        echo "Refusing to write DXIL inside the live runtime" >&2
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
)

PASS_COUNT=0
EXPECTED_COUNT="$(awk -F '\t' 'NF && $1 !~ /^#/ { count += 1 } END { print count + 0 }' "$MANIFEST")"
DXC_REAL="$(realpath "$DXC")"
DXC_SHA256="$(shasum -a 256 "$DXC_REAL" | awk '{print $1}')"
DXC_LIBRARY="$DXC_REAL"
if [[ -f "$(dirname "$DXC_REAL")/../lib/libdxcompiler.dylib" ]]; then
    DXC_LIBRARY="$(realpath "$(dirname "$DXC_REAL")/../lib/libdxcompiler.dylib")"
fi
DXC_LIBRARY_SHA256="$(shasum -a 256 "$DXC_LIBRARY" | awk '{print $1}')"
MANIFEST_SHA256="$(shasum -a 256 "$MANIFEST" | awk '{print $1}')"
SOURCE_TREE_SHA256="$(cd "$SOURCE_ROOT" && find -s . -type f -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
COMPAT_PATCH_DIR="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/compat"
COMPAT_PATCH_SET_SHA256="$(cd "$COMPAT_PATCH_DIR" && find -s . -type f -name '*.patch' -exec shasum -a 256 {} \; | shasum -a 256 | awk '{print $1}')"
MATERIALIZATION_RECEIPT="$(realpath "$SOURCE_ROOT/../../..")/materialization-receipt.txt"
MATERIALIZATION_RECEIPT_SHA256="unavailable"
if [[ -f "$MATERIALIZATION_RECEIPT" ]]; then
    MATERIALIZATION_RECEIPT_SHA256="$(shasum -a 256 "$MATERIALIZATION_RECEIPT" | awk '{print $1}')"
fi
RECEIPT="$OUTPUT_ABS/dxc-gate-receipt.txt"

{
    echo "evidence_scope=dxc_only_no_msc_metallib_load_or_pso_claim"
    echo "dxc_path=$DXC_REAL"
    "$DXC" --version | sed 's/^/dxc_version=/'
    echo "dxc_sha256=$DXC_SHA256"
    echo "dxc_library_sha256=$DXC_LIBRARY_SHA256"
    echo "source_root=$(realpath "$SOURCE_ROOT")"
    echo "entry_point=main"
    echo "hlsl_version=2016"
    echo "include_path=$(realpath "$SOURCE_ROOT")"
    echo "source_tree_sha256=$SOURCE_TREE_SHA256"
    echo "materialization_receipt_sha256=$MATERIALIZATION_RECEIPT_SHA256"
    echo "manifest_sha256=$MANIFEST_SHA256"
    echo "compat_patch_set_sha256=$COMPAT_PATCH_SET_SHA256"
    echo "common_define_args=${COMMON_DEFINES[*]}"
} > "$RECEIPT"

while IFS=$'\t' read -r SHADER_CLASS PROFILE SOURCE PERMUTATION; do
    if [[ -z "$SHADER_CLASS" || "$SHADER_CLASS" == \#* ]]; then
        continue
    fi

    SKIN_DEFINE="SKIN_NONE=1"
    if [[ "$PERMUTATION" == *skin_0* ]]; then
        SKIN_DEFINE="SKIN_0=1"
    fi

    DXIL_PATH="$OUTPUT_ABS/$SOURCE.dxil"
    STDERR_PATH="$OUTPUT_ABS/$SOURCE.stderr"
    "$DXC" \
        -T "$PROFILE" \
        -E main \
        -HV 2016 \
        -I "$SOURCE_ROOT" \
        "${COMMON_DEFINES[@]}" \
        -D "$SKIN_DEFINE" \
        "$SOURCE_ROOT/$SOURCE" \
        -Fo "$DXIL_PATH" \
        2> "$STDERR_PATH"

    if [[ ! -s "$DXIL_PATH" ]]; then
        echo "DXC gate produced no DXIL: $SHADER_CLASS ($SOURCE)" >&2
        exit 1
    fi
    PASS_COUNT=$((PASS_COUNT + 1))
    SOURCE_SHA256="$(shasum -a 256 "$SOURCE_ROOT/$SOURCE" | awk '{print $1}')"
    DXIL_SHA256="$(shasum -a 256 "$DXIL_PATH" | awk '{print $1}')"
    WARNING_COUNT="$(grep -c 'warning:' "$STDERR_PATH" || true)"
    printf 'shader=%s\tprofile=%s\tpermutation=%s\tskin_define=%s\tsource_sha256=%s\tdxil_sha256=%s\twarnings=%s\n' \
        "$SOURCE" "$PROFILE" "$PERMUTATION" "$SKIN_DEFINE" "$SOURCE_SHA256" "$DXIL_SHA256" "$WARNING_COUNT" \
        >> "$RECEIPT"
    echo "DXC_PASS class=$SHADER_CLASS source=$SOURCE profile=$PROFILE permutation=$PERMUTATION"
done < "$MANIFEST"

if [[ "$PASS_COUNT" -ne "$EXPECTED_COUNT" ]]; then
    echo "Expected $EXPECTED_COUNT representative shaders, compiled $PASS_COUNT" >&2
    exit 1
fi

echo "shader_count=$PASS_COUNT" >> "$RECEIPT"

echo "R4_DXC_GATE_READY shaders=$PASS_COUNT receipt=$RECEIPT"
