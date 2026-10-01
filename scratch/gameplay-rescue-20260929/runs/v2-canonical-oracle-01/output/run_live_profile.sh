#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
RESCUE_ROOT="$ROOT/scratch/gameplay-rescue-20260929"
RUN_ID="${RESCUE_RUN_ID:-baseline}"
RUN_ROOT="${RESCUE_RUN_ROOT:-$RESCUE_ROOT/runs/$RUN_ID}"
BASE_RUNTIME="${RESCUE_BASE_RUNTIME:-$ROOT/scratch/windows-metal-gap-20260929/newgame-native/runtime}"
SAVE_NAME="${RESCUE_SAVE_NAME:-player - autosave}"
LAUNCH_MODE="${RESCUE_LAUNCH_MODE:-direct}"
APP="${RESCUE_APP:-$RESCUE_ROOT/prestate/validation/Anomaly-baseline.app}"
OUT="$RUN_ROOT/output"
RT="$RUN_ROOT/runtime"

if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second owned process' >&2
    exit 1
fi
if [[ -e "$RUN_ROOT" ]]; then
    echo "Run directory already exists: $RUN_ROOT" >&2
    echo 'Choose a fresh RESCUE_RUN_ID; existing run evidence is preserved.' >&2
    exit 1
fi
if [[ ! -x "$APP/Contents/MacOS/AnomalyDX11" ]]; then
    echo "App executable not found: $APP/Contents/MacOS/AnomalyDX11" >&2
    exit 1
fi
if [[ ! -f "$BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop" ]]; then
    echo "Selected save is missing from $BASE_RUNTIME: $SAVE_NAME" >&2
    exit 1
fi

mkdir -p "$OUT"
ditto "$BASE_RUNTIME" "$RT"
cp "$RESCUE_ROOT/harness/validation/live_profile.lua" "$OUT/live_profile.lua"
cp "$RESCUE_ROOT/harness/validation/run_live_profile.sh" "$OUT/run_live_profile.sh"
export XRAY_PROJECT_ROOT="$ROOT"
source "$ROOT/script/xray_metal_shader_env.sh"
unset MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR MTL_CAPTURE_ENABLED \
    XRAY_METAL_NATIVE_RETINA XRAY_METAL_E08_GPU_PASSES XRAY_METAL_LIGHT_HOM_DIAGNOSTIC \
    XRAY_METAL_E07_DIAGNOSTIC XRAY_METAL_E07_NPC_COUNT XRAY_METAL_E01_FRAMES XRAY_METAL_E07_FRAMES \
    METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR \
    XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE XRAY_METAL_LOCAL_LIGHT_FACE_CULL \
    XRAY_METAL_DYNAMIC_SHADOW_INDEX XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE \
    XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY \
    XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR XRAY_METAL_E08_FLORA_INSTANCING_OFF \
    DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES

export PERF_GAP_PHASES="$OUT/phases.csv"
export XRAY_PARITY_HARNESS=1
export XRAY_PARITY_HARNESS_SCRIPT="$RESCUE_ROOT/harness/validation/live_profile.lua"
export RESCUE_WARMUP_MS="${RESCUE_WARMUP_MS:-30000}"
export RESCUE_MEASURE_MS="${RESCUE_MEASURE_MS:-60000}"
export RESCUE_SAVE_CONTROL="${RESCUE_SAVE_CONTROL:-0}"
OPEN_ENV_ARGS=()
while IFS= read -r ENV_NAME; do
    OPEN_ENV_ARGS+=(--env "$ENV_NAME=${!ENV_NAME}")
done < <(compgen -e | /usr/bin/grep -E '^(XRAY_PROJECT_ROOT|XRAY_METAL_MSC_|XRAY_METAL_MATERIALS$|XRAY_METAL_TERRAIN_VISUALS$)')
if [[ -n "${RESCUE_ENV_ASSIGNMENTS:-}" ]]; then
    IFS=';' read -r -a ASSIGNMENTS <<< "$RESCUE_ENV_ASSIGNMENTS"
    for ASSIGNMENT in "${ASSIGNMENTS[@]}"; do
        if [[ ! "$ASSIGNMENT" =~ ^[A-Za-z_][A-Za-z0-9_]*=.*$ ]]; then
            echo "Invalid RESCUE_ENV_ASSIGNMENTS entry: $ASSIGNMENT" >&2
            exit 1
        fi
        export "$ASSIGNMENT"
        OPEN_ENV_ARGS+=(--env "$ASSIGNMENT")
    done
fi
env | /usr/bin/awk -F= '$1 ~ /^(XRAY_|RESCUE_|PERF_GAP_|MTL_)/ {print}' | sort > "$OUT/environment.txt"

shasum -a 256 "$APP/Contents/MacOS/AnomalyDX11" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" > "$OUT/input-sha256.txt"
stat -f '%N bytes=%z mtime=%m' "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" > "$OUT/input-metadata.txt"
pmset -g batt > "$OUT/power-before.txt"
pmset -g therm > "$OUT/thermal-before.txt"
vm_stat > "$OUT/vm-stat-before.txt"
sysctl vm.swapusage > "$OUT/swap-before.txt"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"
cd "$RT"

if [[ "$LAUNCH_MODE" == direct ]]; then
    "$APP/Contents/MacOS/AnomalyDX11" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE_NAME\"" > "$OUT/stdout.log" 2> "$OUT/engine.log" &
else
    open -n -W -a "$APP" --stdout "$OUT/stdout.log" --stderr "$OUT/engine.log" \
    --env "XRAY_PARITY_HARNESS=1" \
    --env "XRAY_PARITY_HARNESS_SCRIPT=$XRAY_PARITY_HARNESS_SCRIPT" \
    --env "PERF_GAP_PHASES=$PERF_GAP_PHASES" \
    --env "RESCUE_WARMUP_MS=$RESCUE_WARMUP_MS" \
    --env "RESCUE_MEASURE_MS=$RESCUE_MEASURE_MS" \
    --env "RESCUE_SAVE_CONTROL=$RESCUE_SAVE_CONTROL" \
    "${OPEN_ENV_ARGS[@]}" \
    --args -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE_NAME\"" &
fi
OPEN_PID=$!
echo "$OPEN_PID" > "$OUT/open-pid.txt"
GAME_PID=''
for ((i=0; i<45; i++)); do
    GAME_PID="$(pgrep -x AnomalyDX11 | head -1 || true)"
    if [[ -n "$GAME_PID" ]]; then break; fi
    sleep 1
done
if [[ -z "$GAME_PID" ]]; then
    echo 'AnomalyDX11 did not start' >&2
    exit 1
fi
echo "$GAME_PID" > "$OUT/owned-pid.txt"
trap 'kill "$GAME_PID" 2>/dev/null || true' EXIT INT TERM

# Capture the actual display after the timed window; the in-game `screenshot`
# command only records a console command on this build and is not visual proof.
(
    for ((i=0; i<170; i++)); do
        if ! kill -0 "$GAME_PID" 2>/dev/null; then
            echo 'game exited before display capture' > "$OUT/screencapture-error.txt"
            exit 0
        fi
        if [[ -f "$PERF_GAP_PHASES" && ! -f "$OUT/vm-stat-measure-start.txt" ]] && /usr/bin/grep -q '^measure_start,' "$PERF_GAP_PHASES"; then
            date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/memory-measure-start-time.txt"
            vm_stat > "$OUT/vm-stat-measure-start.txt"
            sysctl vm.swapusage > "$OUT/swap-measure-start.txt"
        fi
        if [[ -f "$PERF_GAP_PHASES" ]] && /usr/bin/grep -q '^measure_end,' "$PERF_GAP_PHASES"; then
            date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/memory-measure-end-time.txt"
            vm_stat > "$OUT/vm-stat-measure-end.txt"
            sysctl vm.swapusage > "$OUT/swap-measure-end.txt"
            sleep 2
            /usr/sbin/screencapture -x "$OUT/display.png" || echo 'display capture failed' > "$OUT/screencapture-error.txt"
            if /usr/bin/swift "$RESCUE_ROOT/harness/validation/window_id.swift" "$GAME_PID" > "$OUT/window-id.txt" 2> "$OUT/window-id-error.txt"; then
                /usr/sbin/screencapture -x -l "$(cat "$OUT/window-id.txt")" "$OUT/game-window.png" || echo 'window capture failed' >> "$OUT/screencapture-error.txt"
            fi
            exit 0
        fi
        sleep 1
    done
    echo 'measure_end was not observed before display capture timeout' > "$OUT/screencapture-error.txt"
) &
CAPTURE_PID=$!

for ((i=0; i<180; i++)); do
    if ! kill -0 "$GAME_PID" 2>/dev/null; then break; fi
    sleep 1
done
if kill -0 "$GAME_PID" 2>/dev/null; then
    echo 'timeout: harness did not reach its scripted quit' > "$OUT/timeout.txt"
    kill "$GAME_PID"
fi
STATUS=0
wait "$OPEN_PID" || STATUS=$?
wait "$CAPTURE_PID" || true
echo "$STATUS" > "$OUT/exit-status.txt"
pmset -g batt > "$OUT/power-after.txt"
pmset -g therm > "$OUT/thermal-after.txt"
vm_stat > "$OUT/vm-stat-after.txt"
sysctl vm.swapusage > "$OUT/swap-after.txt"
trap - EXIT INT TERM
echo "Live profile completed with exit status $STATUS; output: $OUT"
