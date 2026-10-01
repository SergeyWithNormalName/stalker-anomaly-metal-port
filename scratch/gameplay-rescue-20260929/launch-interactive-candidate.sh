#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RESCUE="$ROOT/scratch/gameplay-rescue-20260929"
APP="${RESCUE_APP:-$RESCUE/candidate-v4/Anomaly.app}"
BASE="${RESCUE_BASE_RUNTIME:-$RESCUE/runs/v4-combined-on-01/runtime}"
RT="${RESCUE_INTERACTIVE_RUNTIME:-$RESCUE/interactive-candidate/runtime}"
OUT="$(dirname "$RT")/output"
SAVE="${RESCUE_SAVE_NAME:-rescue-cordon-control}"
SHADER_ROOT="$ROOT/build/metal-runtime-shaders"

if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second process' >&2
    exit 1
fi
[[ -x "$APP/Contents/MacOS/AnomalyDX11" ]] || { echo "Candidate executable missing: $APP" >&2; exit 1; }
[[ -f "$BASE/appdata/savedgames/$SAVE.scop" ]] || { echo "Save '$SAVE' missing from base runtime: $BASE" >&2; exit 1; }
[[ -d "$SHADER_ROOT/shadow-casters-raster-depth-v1" && -d "$SHADER_ROOT/flora-shadow-instanced-e08" ]] || { echo 'Qualified V4 shader bundle missing' >&2; exit 1; }

if [[ ! -d "$RT" ]]; then
    mkdir -p "$(dirname "$RT")"
    ditto "$BASE" "$RT"
    printf '%s\n' 'runtime cloned from v4-combined-on-01; archives remain symlinked to the normal installed content' > "$(dirname "$RT")/CLONE-INFO.txt"
    /usr/bin/sed -i '' 's/^rs_stats[[:space:]].*/rs_stats off/' "$RT/appdata/user.ltx"
    mkdir -p "$OUT"
fi
[[ -f "$RT/fsgame.ltx" && -f "$RT/appdata/savedgames/$SAVE.scop" ]] || { echo "Existing candidate runtime is incomplete: $RT" >&2; exit 1; }
mkdir -p "$OUT"

export XRAY_PROJECT_ROOT="$ROOT"
source "$ROOT/script/xray_metal_shader_env.sh"
unset XRAY_PARITY_HARNESS XRAY_PARITY_HARNESS_SCRIPT PERF_GAP_PHASES RESCUE_WARMUP_MS RESCUE_MEASURE_MS \
    MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR MTL_CAPTURE_ENABLED \
    METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS XRAY_METAL_E01_FRAMES XRAY_METAL_E07_FRAMES \
    XRAY_METAL_E07_DIAGNOSTIC XRAY_METAL_E07_NPC_COUNT XRAY_METAL_E08_GPU_PASSES \
    XRAY_METAL_LIGHT_HOM_DIAGNOSTIC XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR \
    XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE \
    XRAY_METAL_LOCAL_LIGHT_FACE_CULL XRAY_METAL_COMPUTE_SKIN XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR \
    XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY XRAY_METAL_E08_FLORA_INSTANCING_OFF
export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADER_ROOT/shadow-casters-raster-depth-v1"
export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$SHADER_ROOT/flora-shadow-instanced-e08"
export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1

printf '%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$OUT/last-launch-time.txt"
shasum -a 256 "$APP/Contents/MacOS/AnomalyDX11" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE.scop" > "$OUT/last-launch-inputs.sha256"
echo "Launching V4 candidate directly. Runtime: $RT"
cd "$RT"
exec "$APP/Contents/MacOS/AnomalyDX11" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE\""
