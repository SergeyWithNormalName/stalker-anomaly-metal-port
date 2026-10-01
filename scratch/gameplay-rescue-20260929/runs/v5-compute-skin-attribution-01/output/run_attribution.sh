#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
STAGE="$ROOT/scratch/gameplay-rescue-20260929/compute-oracle-stage/attribution-harness"
RUN_ID="${RESCUE_RUN_ID:-v5-compute-skin-attribution-01}"
RUN_ROOT="$ROOT/scratch/gameplay-rescue-20260929/runs/$RUN_ID"
BASE_RUNTIME="$ROOT/scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime"
SAVE_NAME="rescue-cordon-control"
APP="$ROOT/scratch/gameplay-rescue-20260929/candidate-v5/Anomaly.app"
GAME_EXE="$APP/Contents/MacOS/AnomalyDX11"
EXPECTED_GAME_EXE_SHA256="5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f"
SHADOW_DIR="$ROOT/build/metal-runtime-shaders/shadow-casters-raster-depth-v1"
FLORA_DIR="$ROOT/build/metal-runtime-shaders/flora-shadow-instanced-e08"
OUT="$RUN_ROOT/output"
RT="$RUN_ROOT/runtime"
MAX_OWNED_SECONDS=180

if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second owned process' >&2
    exit 1
fi
if [[ -e "$RUN_ROOT" ]]; then
    echo "Run directory already exists: $RUN_ROOT" >&2
    echo 'Choose a fresh RESCUE_RUN_ID; existing evidence is preserved.' >&2
    exit 1
fi
if [[ ! -x "$GAME_EXE" ]]; then
    echo "V5 candidate executable not found: $GAME_EXE" >&2
    exit 1
fi
ACTUAL_GAME_EXE_SHA256="$(/usr/bin/shasum -a 256 "$GAME_EXE" | /usr/bin/awk '{print $1}')"
if [[ "$ACTUAL_GAME_EXE_SHA256" != "$EXPECTED_GAME_EXE_SHA256" ]]; then
    echo "V5 candidate executable hash mismatch: $ACTUAL_GAME_EXE_SHA256" >&2
    exit 1
fi
if [[ ! -f "$BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop" ]]; then
    echo "Canonical save is missing: $BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop" >&2
    exit 1
fi
if [[ ! -f "$SHADOW_DIR/shadow_direct_tree.vs.metallib" ||
      ! -f "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" ||
      ! -f "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" ||
      ! -f "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" ||
      ! -f "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" ||
      ! -f "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" ]]; then
    echo 'Required corrected depth or instanced Flora shader artifact is missing' >&2
    exit 1
fi

mkdir -p "$OUT"
ditto "$BASE_RUNTIME" "$RT"
cp "$STAGE/profile_attribution.lua" "$OUT/profile_attribution.lua"
cp "$STAGE/run_attribution.sh" "$OUT/run_attribution.sh"
export XRAY_PROJECT_ROOT="$ROOT"
source "$ROOT/script/xray_metal_shader_env.sh"
unset MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR MTL_CAPTURE_ENABLED \
    XRAY_METAL_NATIVE_RETINA XRAY_METAL_E08_GPU_PASSES XRAY_METAL_LIGHT_HOM_DIAGNOSTIC \
    XRAY_METAL_E07_DIAGNOSTIC XRAY_METAL_E07_NPC_COUNT \
    XRAY_METAL_E01_FRAMES XRAY_METAL_E07_FRAMES \
    XRAY_METAL_DYNAMIC_DRAW_DIAGNOSTICS XRAY_METAL_E03_TRACE_PATH \
    XRAY_METAL_E03_CAPTURE_FLAG_PATH XRAY_METAL_E03_CENSUS_FLAG_PATH \
    METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR \
    XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE XRAY_METAL_LOCAL_LIGHT_FACE_CULL \
    XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR \
    XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY XRAY_METAL_E08_FLORA_INSTANCING_OFF \
    XRAY_METAL_E08_EARLY_DRAWABLE XRAY_METAL_E08_UPLOAD_BATCH XRAY_METAL_E08_DEDUP_RESOURCES \
    XRAY_METAL_E08_HEAP_BINDINGS XRAY_METAL_E08_FORCE_SUBMIT_RETRY \
    XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE \
    XRAY_METAL_COMPUTE_SKIN_ORACLE PERF_GAP_PHASES RESCUE_WARMUP_MS RESCUE_MEASURE_MS \
    RESCUE_SAVE_CONTROL DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES

export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADOW_DIR"
export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$FLORA_DIR"
export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1
export XRAY_METAL_COMPUTE_SKIN=1
export XRAY_METAL_E01_FRAMES="$OUT/e01-frames.csv"
export XRAY_METAL_E07_FRAMES="$OUT/e07-dynamic-world.csv"
export XRAY_ATTRIBUTION_MARKERS="$OUT/attribution-markers.csv"
export XRAY_PARITY_HARNESS=1
export XRAY_PARITY_HARNESS_SCRIPT="$OUT/profile_attribution.lua"
env | /usr/bin/awk -F= '$1 ~ /^(XRAY_|MTL_)/ {print}' | sort > "$OUT/environment.txt"
/usr/bin/shasum -a 256 \
    "$GAME_EXE" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" \
    "$STAGE/run_attribution.sh" "$STAGE/profile_attribution.lua" \
    "$SHADOW_DIR/shadow_direct_tree.vs.metallib" \
    "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" \
    "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" \
    "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" \
    "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json" \
    > "$OUT/input-sha256.txt"
stat -f '%N bytes=%z mtime=%m' "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" > "$OUT/input-metadata.txt"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"

GAME_PID=''
cleanup_owned_game()
{
    if [[ -n "$GAME_PID" ]] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null || true
        for ((grace=0; grace<3; ++grace)); do
            kill -0 "$GAME_PID" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$GAME_PID" 2>/dev/null; then
            kill -KILL "$GAME_PID" 2>/dev/null || true
        fi
    fi
}
trap cleanup_owned_game EXIT INT TERM

cd "$RT"
"$GAME_EXE" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE_NAME\"" \
    > "$OUT/stdout.log" 2> "$OUT/engine.log" &
GAME_PID=$!
echo "$GAME_PID" > "$OUT/owned-pid.txt"
LAUNCH_EPOCH="$(date +%s)"

TIMEOUT=0
while kill -0 "$GAME_PID" 2>/dev/null; do
    NOW="$(date +%s)"
    if (( NOW - LAUNCH_EPOCH >= MAX_OWNED_SECONDS - 5 )); then
        TIMEOUT=1
        echo 'timeout: attribution harness exceeded the 175-second runtime budget' > "$OUT/timeout.txt"
        cleanup_owned_game
        break
    fi
    sleep 1
done
STATUS=0
wait "$GAME_PID" || STATUS=$?
echo "$STATUS" > "$OUT/exit-status.txt"
/usr/bin/shasum -a 256 "$GAME_EXE" > "$OUT/post-run-app-sha256.txt"
trap - EXIT INT TERM

if [[ "$STATUS" != 0 || "$TIMEOUT" == 1 ]]; then
    echo "Attribution run ended unsuccessfully; exit=$STATUS timeout=$TIMEOUT" >&2
    exit 1
fi
if ! /usr/bin/grep -q '^attribution_window_start,' "$OUT/attribution-markers.csv" ||
   ! /usr/bin/grep -q '^attribution_window_end,' "$OUT/attribution-markers.csv"; then
    echo 'Attribution harness did not complete its bounded sample window' >&2
    exit 1
fi
if /usr/bin/grep -q '^FAILED_' "$OUT/attribution-markers.csv"; then
    echo 'Attribution harness recorded a failed state precondition' >&2
    exit 1
fi
echo "V5 stage-attribution run completed; output: $OUT"
