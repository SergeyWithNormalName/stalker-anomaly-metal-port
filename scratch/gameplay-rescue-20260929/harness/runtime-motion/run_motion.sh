#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
RESCUE="$ROOT/scratch/gameplay-rescue-20260929"
RUN_ID="${RESCUE_RUN_ID:-v4-motion-01}"
RUN="$RESCUE/runs/$RUN_ID"
BASE="${RESCUE_BASE_RUNTIME:-$RESCUE/runs/v4-combined-on-01/runtime}"
APP="${RESCUE_APP:-$RESCUE/candidate-v4/Anomaly.app}"
SHADER_ROOT="$ROOT/build/metal-runtime-shaders"
OUT="$RUN/output"
RT="$RUN/runtime"
SAVE="rescue-cordon-control"

if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second game process' >&2
    exit 1
fi
if [[ -e "$RUN" ]]; then
    echo "Run path already exists: $RUN (evidence is preserved; choose a fresh RESCUE_RUN_ID)" >&2
    exit 1
fi
[[ -x "$APP/Contents/MacOS/AnomalyDX11" ]] || { echo "Missing executable: $APP" >&2; exit 1; }
[[ -f "$BASE/appdata/savedgames/$SAVE.scop" ]] || { echo "Missing save in base runtime: $SAVE" >&2; exit 1; }
[[ -d "$SHADER_ROOT/shadow-casters-raster-depth-v1" && -d "$SHADER_ROOT/flora-shadow-instanced-e08" ]] || { echo 'Required qualified shader bundle is missing' >&2; exit 1; }

mkdir -p "$OUT"
ditto "$BASE" "$RT"
cp "$(dirname "$0")/motion.lua" "$OUT/motion.lua"
cp "$(dirname "$0")/owned_window.swift" "$OUT/owned_window.swift"
LOADER="$RT/gamedata/scripts/axr_main.script"
if [[ ! -f "$LOADER" ]] || ! /usr/bin/grep -q 'if not parity_harness_register then' "$LOADER" || \
   ! /usr/bin/grep -q '^[[:space:]]*parity_harness_register()' "$LOADER" || \
   ! /usr/bin/grep -q '^function parity_harness_register()' "$OUT/motion.lua"; then
    echo 'Parity harness loader contract failed: cloned axr_main must call parity_harness_register exported by motion.lua' >&2
    exit 1
fi
{
    shasum -a 256 "$LOADER" "$OUT/motion.lua"
    /usr/bin/grep -n -E 'parity_harness_register' "$LOADER" "$OUT/motion.lua"
} > "$OUT/loader-contract.txt"
export XRAY_PROJECT_ROOT="$ROOT"
source "$ROOT/script/xray_metal_shader_env.sh"
unset MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR MTL_CAPTURE_ENABLED \
    XRAY_METAL_E01_FRAMES XRAY_METAL_E07_FRAMES XRAY_METAL_E07_DIAGNOSTIC \
    XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE \
    XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY \
    XRAY_METAL_E08_FLORA_INSTANCING_OFF XRAY_METAL_COMPUTE_SKIN \
    XRAY_METAL_LOCAL_LIGHT_FACE_CULL XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR \
    XRAY_METAL_E08_GPU_PASSES METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS
export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADER_ROOT/shadow-casters-raster-depth-v1"
export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$SHADER_ROOT/flora-shadow-instanced-e08"
export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1
export RESCUE_MOTION_OUT="$OUT"
export XRAY_PARITY_HARNESS=1
export XRAY_PARITY_HARNESS_SCRIPT="$OUT/motion.lua"
env | /usr/bin/awk -F= '$1 ~ /^(XRAY_|RESCUE_|PERF_GAP_|MTL_)/ {print}' | sort > "$OUT/environment.txt"
shasum -a 256 "$APP/Contents/MacOS/AnomalyDX11" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE.scop" > "$OUT/input-sha256.txt"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"
cd "$RT"
"$APP/Contents/MacOS/AnomalyDX11" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE\"" > "$OUT/stdout.log" 2> "$OUT/engine.log" &
GAME_PID=$!
echo "$GAME_PID" > "$OUT/owned-pid.txt"
CAPTURE_PID=''
WATCHDOG_PID=''
NORMAL_EXIT=0
cleanup() {
    if [[ "$NORMAL_EXIT" != 1 ]]; then kill "$GAME_PID" 2>/dev/null || true; fi
    [[ -z "$CAPTURE_PID" ]] || kill "$CAPTURE_PID" 2>/dev/null || true
    [[ -z "$WATCHDOG_PID" ]] || kill "$WATCHDOG_PID" 2>/dev/null || true
    [[ -z "$CAPTURE_PID" ]] || wait "$CAPTURE_PID" 2>/dev/null || true
    [[ -z "$WATCHDOG_PID" ]] || wait "$WATCHDOG_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
for ((i=0; i<45; i++)); do
    kill -0 "$GAME_PID" 2>/dev/null || break
    PID="$(pgrep -x AnomalyDX11 | head -1 || true)"
    [[ -n "$PID" ]] && { GAME_PID="$PID"; break; }
    sleep 1
done
echo "$GAME_PID" > "$OUT/owned-pid.txt"
if ! kill -0 "$GAME_PID" 2>/dev/null; then
    echo 'Game exited before runtime motion began' >&2
    exit 1
fi
echo "RUN=$RUN" > "$OUT/live-owner.txt"
echo "GAME_PID=$GAME_PID" >> "$OUT/live-owner.txt"
echo "APP_SHA=$(shasum -a 256 "$APP/Contents/MacOS/AnomalyDX11" | awk '{print $1}')" >> "$OUT/live-owner.txt"
echo "Started bounded V4 camera motion; run=$RUN pid=$GAME_PID"

(
    LAST_STAGE=''
    for ((i=0; i<80; i++)); do
        kill -0 "$GAME_PID" 2>/dev/null || exit 0
        STAGE="$(tail -n 1 "$OUT/motion.csv" 2>/dev/null | cut -d, -f1 || true)"
        if [[ "$STAGE" != "$LAST_STAGE" && "$STAGE" == stage_* ]]; then
            META="$(/usr/bin/swift "$OUT/owned_window.swift" "$GAME_PID" 2>>"$OUT/window-diagnostics.log" || true)"
            if [[ -n "$META" ]]; then
                printf '%s\n' "$META" > "$OUT/window-metadata.json"
                WINDOW_ID="$(/usr/bin/plutil -extract id raw -o - - <<<"$META" 2>/dev/null || true)"
                if [[ -n "$WINDOW_ID" ]]; then
                    LAST_STAGE="$STAGE"
                    /usr/sbin/screencapture -x -l "$WINDOW_ID" "$OUT/$STAGE.png" >>"$OUT/window-capture.log" 2>&1 || echo "$STAGE window capture failed" >> "$OUT/screencapture-error.txt"
                fi
            fi
        fi
        sleep 1
    done
) &
CAPTURE_PID=$!
(
    for ((i=0; i<180; i++)); do
        kill -0 "$GAME_PID" 2>/dev/null || exit 0
        sleep 1
    done
    if kill -0 "$GAME_PID" 2>/dev/null; then
        date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/watchdog-timeout.txt"
        echo "Owned game PID $GAME_PID exceeded the 180-second limit; sent TERM" >> "$OUT/watchdog-timeout.txt"
        kill "$GAME_PID" 2>/dev/null || true
        for ((i=0; i<5; i++)); do
            kill -0 "$GAME_PID" 2>/dev/null || exit 0
            sleep 1
        done
        if kill -0 "$GAME_PID" 2>/dev/null; then
            echo 'Process remained after 5-second TERM grace; sent KILL' >> "$OUT/watchdog-timeout.txt"
            kill -KILL "$GAME_PID" 2>/dev/null || true
        fi
    fi
) &
WATCHDOG_PID=$!
echo "$WATCHDOG_PID" > "$OUT/watchdog-pid.txt"
STATUS=0
wait "$GAME_PID" || STATUS=$?
if [[ -f "$OUT/watchdog-timeout.txt" ]]; then STATUS=124; fi
kill "$WATCHDOG_PID" 2>/dev/null || true
wait "$WATCHDOG_PID" 2>/dev/null || true
WATCHDOG_PID=''
wait "$CAPTURE_PID" || true
CAPTURE_PID=''
echo "$STATUS" > "$OUT/exit-status.txt"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/exit-time.txt"
NORMAL_EXIT=1
trap - EXIT INT TERM
echo "Motion run exited with status $STATUS; run=$RUN"
exit "$STATUS"
