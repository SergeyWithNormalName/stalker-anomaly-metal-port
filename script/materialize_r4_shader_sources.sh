#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ARCHIVE="$PROJECT_ROOT/runtime/macos-gamma/profile/db/shaders.db0"
OVERLAY="$PROJECT_ROOT/runtime/macos-gamma/profile/gamedata/shaders"
OUTPUT_DIR=""
CONVERTER="${XRAY_GAMMA_CONVERTER:-}"
CROSSOVER_WINE="${XRAY_CROSSOVER_WINE:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/CrossOver-Hosted Application/wine}"
CROSSOVER_BOTTLE="${XRAY_CROSSOVER_BOTTLE:-}"

usage() {
    cat <<'EOF'
Usage:
  materialize_r4_shader_sources.sh --output DIR --crossover-bottle NAME [options]

Options:
  --archive FILE             Active GAMMA shaders.db0 archive
  --overlay DIR              Materialized winning gamedata/shaders overlay
  --converter FILE           GAMMA converter.exe (auto-discovered when possible)
  --crossover-wine FILE      CrossOver wine wrapper
  --crossover-bottle NAME    Existing CrossOver bottle used to run converter.exe
  --output DIR               New or empty output directory outside runtime/

Environment equivalents:
  XRAY_GAMMA_CONVERTER, XRAY_CROSSOVER_WINE, XRAY_CROSSOVER_BOTTLE
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --archive)
            ARCHIVE="$2"
            shift 2
            ;;
        --overlay)
            OVERLAY="$2"
            shift 2
            ;;
        --converter)
            CONVERTER="$2"
            shift 2
            ;;
        --crossover-wine)
            CROSSOVER_WINE="$2"
            shift 2
            ;;
        --crossover-bottle)
            CROSSOVER_BOTTLE="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
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

if [[ -z "$OUTPUT_DIR" || -z "$CROSSOVER_BOTTLE" ]]; then
    usage >&2
    exit 2
fi

if [[ ! -f "$ARCHIVE" || ! -d "$OVERLAY" || ! -x "$CROSSOVER_WINE" ]]; then
    echo "Missing archive, overlay, or CrossOver wine wrapper" >&2
    exit 1
fi

ARCHIVE_REAL="$(realpath "$ARCHIVE")"
ARCHIVE_DIR="$(dirname "$ARCHIVE_REAL")"
if [[ -z "$CONVERTER" ]]; then
    GAME_ROOT="$(cd "$ARCHIVE_DIR/.." && pwd -P)"
    CONVERTER="$GAME_ROOT/tools/converter.exe"
fi
if [[ ! -f "$CONVERTER" ]]; then
    echo "Missing GAMMA converter.exe: $CONVERTER" >&2
    exit 1
fi

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the materialization output: $OUTPUT_ABS" >&2
    exit 1
fi
case "$OUTPUT_ABS/" in
    "$PROJECT_ROOT/runtime/"*)
        echo "Refusing to materialize shader sources inside the live runtime" >&2
        exit 1
        ;;
esac

if [[ -d "$OUTPUT_ABS" ]] && [[ -n "$(find "$OUTPUT_ABS" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "Output directory must be new or empty: $OUTPUT_ABS" >&2
    exit 1
fi

mkdir -p "$OUTPUT_ABS/base"

"$CROSSOVER_WINE" \
    --bottle "$CROSSOVER_BOTTLE" \
    --workdir "$OUTPUT_ABS" \
    --no-gui \
    "$CONVERTER" -- \
    -unpack -xdb "$ARCHIVE_REAL" -dir "$OUTPUT_ABS/base"

if [[ ! -f "$OUTPUT_ABS/base/shaders/r3/common.h" ]]; then
    echo "Archive extraction did not produce shaders/r3/common.h" >&2
    exit 1
fi

rsync -aL "$OVERLAY/" "$OUTPUT_ABS/base/shaders/"

COMPAT_PATCH_DIR="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/compat"
for COMPAT_PATCH in "$COMPAT_PATCH_DIR"/*.patch; do
    if [[ -f "$COMPAT_PATCH" ]]; then
        # GAMMA shader sources use CRLF; -l keeps the patch content readable
        # while matching whitespace and line endings in the materialized copy.
        patch --batch --forward -l -d "$OUTPUT_ABS/base/shaders" -p0 < "$COMPAT_PATCH"
    fi
done

if [[ ! -f "$OUTPUT_ABS/base/shaders/r3/deffer_base_bump.vs" ]]; then
    echo "Winning overlay did not produce deffer_base_bump.vs" >&2
    exit 1
fi

ARCHIVE_SHA256="$(shasum -a 256 "$ARCHIVE_REAL" | awk '{print $1}')"
PROFILE_MANIFEST="$PROJECT_ROOT/runtime/macos-gamma/profile/profile-manifest.json"
PROFILE_MANIFEST_SHA256="unavailable"
if [[ -f "$PROFILE_MANIFEST" ]]; then
    PROFILE_MANIFEST_SHA256="$(shasum -a 256 "$PROFILE_MANIFEST" | awk '{print $1}')"
fi
SOURCE_COUNT="$(find "$OUTPUT_ABS/base/shaders/r3" -type f | wc -l | tr -d ' ')"

{
    echo "archive_sha256=$ARCHIVE_SHA256"
    echo "profile_manifest_sha256=$PROFILE_MANIFEST_SHA256"
    echo "r3_source_count=$SOURCE_COUNT"
    echo "source_root=$OUTPUT_ABS/base/shaders/r3"
} > "$OUTPUT_ABS/materialization-receipt.txt"

echo "R4_SHADER_SOURCES_READY root=$OUTPUT_ABS/base/shaders/r3 files=$SOURCE_COUNT archive_sha256=$ARCHIVE_SHA256"
