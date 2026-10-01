#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
RESCUE="$ROOT/scratch/gameplay-rescue-20260929"
FEATURE_MODE="${RESCUE_V7_MOTION_FEATURES:-combined}"
case "$FEATURE_MODE" in
    baseline|combined) ;;
    *) echo "RESCUE_V7_MOTION_FEATURES must be baseline or combined" >&2; exit 2 ;;
esac
COLOR_PROBE_MODE="${RESCUE_V7_COLOR_PROBE:-0}"
case "$COLOR_PROBE_MODE" in
    0|1) ;;
    *) echo "RESCUE_V7_COLOR_PROBE must be 0 or 1" >&2; exit 2 ;;
esac
if [[ "$COLOR_PROBE_MODE" == 1 && "$FEATURE_MODE" != combined ]]; then
    echo 'The native color probe is only authorized for the combined-motion diagnostic.' >&2
    exit 2
fi
COLOR_PROBE_EVERY=180
RUN_ID="${RESCUE_RUN_ID:-v7-motion-${FEATURE_MODE}-01}"
RUN="$RESCUE/runs/$RUN_ID"
BASE="$RESCUE/runs/v2-canonical-oracle-01/runtime"
APP="$RESCUE/candidate-v7/Anomaly.app"
GAME_EXE="$APP/Contents/MacOS/AnomalyDX11"
MANIFEST="$RESCUE/candidate-v7/source-manifest.json"
SHADER_ROOT="$ROOT/build/metal-runtime-shaders"
OUT="$RUN/output"
RT="$RUN/runtime"
COLOR_OUT="$OUT/color-probe"
SAVE="rescue-cordon-control"
EXPECTED_V7_SHA256="43c07e4d7a043d44f01d675554aa30a16d5bd09f76a9e99468317d5983fc2bdd"
EXPECTED_V7_MANIFEST_SHA256="8bc76f14f28d42d81695988e9b982ff725485083af310fa15394f5d7f98765e4"
EXPECTED_BASE_USER_SHA256="b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334"
EXPECTED_BASE_SAVE_SHA256="a21f09761ca6e7c3633e0b002b34d760ea30ece41f6542a1814eabba5a0ac3c4"
EXPECTED_SHADOW_RECEIPT_SHA256="02625fdf8b75a8167cd447506cf783809ac139a1853eae6a92f71430e75dc70b"
EXPECTED_FLORA_RECEIPT_SHA256="b2e8753cb653d564188984142367bd14e0eff1b71385e24afa0f5e50f3733350"
SHADOW_DIR="$SHADER_ROOT/shadow-casters-raster-depth-v1"
FLORA_DIR="$SHADER_ROOT/flora-shadow-instanced-e08"


if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second game process' >&2
    exit 1
fi
if [[ -e "$RUN" ]]; then
    echo "Run path already exists: $RUN (evidence is preserved; choose a fresh RESCUE_RUN_ID)" >&2
    exit 1
fi
if [[ "${RESCUE_ROOT_APPROVED:-}" != "1" ]]; then
    echo 'Refusing launch: this prepared motion run still requires root authorization.' >&2
    exit 2
fi
if [[ -z "${RESCUE_EXPECTED_APP_SHA256:-}" || "$RESCUE_EXPECTED_APP_SHA256" != "$EXPECTED_V7_SHA256" ]]; then
    echo 'Set RESCUE_EXPECTED_APP_SHA256 to the reviewed V7 executable hash.' >&2
    exit 2
fi
[[ -x "$GAME_EXE" && -f "$MANIFEST" ]] || { echo "Missing V7 candidate or manifest: $APP" >&2; exit 1; }
[[ -f "$BASE/appdata/user.ltx" && -f "$BASE/appdata/savedgames/$SAVE.scop" ]] || { echo "Missing canonical runtime/save: $BASE" >&2; exit 1; }
[[ -f "$SHADOW_DIR/shadow-casters-msc-receipt.txt" && -f "$FLORA_DIR/flora-instanced-shadow-receipt.json" ]] || { echo 'Required corrected-depth or flora-instancing bundle receipt is missing' >&2; exit 1; }

sha256_file() {
    local digest rest
    read -r digest rest < <(/usr/bin/shasum -a 256 "$1")
    printf '%s' "$digest"
}
ACTUAL_GAME_SHA256="$(sha256_file "$GAME_EXE")"
[[ "$ACTUAL_GAME_SHA256" == "$EXPECTED_V7_SHA256" && "$ACTUAL_GAME_SHA256" == "$RESCUE_EXPECTED_APP_SHA256" ]] || {
    echo "V7 executable hash mismatch: $ACTUAL_GAME_SHA256" >&2; exit 1;
}
[[ "$(sha256_file "$MANIFEST")" == "$EXPECTED_V7_MANIFEST_SHA256" ]] || {
    echo 'V7 source manifest hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE/appdata/user.ltx")" == "$EXPECTED_BASE_USER_SHA256" ]] || {
    echo 'Canonical base user.ltx hash changed; refusing to clone.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE/appdata/savedgames/$SAVE.scop")" == "$EXPECTED_BASE_SAVE_SHA256" ]] || {
    echo 'Canonical base save hash changed; refusing to clone.' >&2; exit 1;
}
[[ "$(sha256_file "$SHADOW_DIR/shadow-casters-msc-receipt.txt")" == "$EXPECTED_SHADOW_RECEIPT_SHA256" ]] || {
    echo 'Corrected raster-depth shader receipt hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$FLORA_DIR/flora-instanced-shadow-receipt.json")" == "$EXPECTED_FLORA_RECEIPT_SHA256" ]] || {
    echo 'Qualified flora-instancing shader receipt hash changed.' >&2; exit 1;
}
check_shader_hash() {
    local path="$1" expected="$2" actual
    actual="$(sha256_file "$path")"
    [[ "$actual" == "$expected" ]] || { echo "Shader artifact hash mismatch: $path" >&2; exit 1; }
}
check_shader_hash "$SHADOW_DIR/shadow_direct_tree.vs.metallib" 18448ada281c9d651da08414516d2e4481dc3114c84687db31cae3b16ffb4502
check_shader_hash "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" a3988e3246ee69170e6f583d2f524fc70a4f4d1c7e1c75955179c258f9150ee0
check_shader_hash "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" e6498441be07820d0540e5d5a69a801ab8fcb9efb3467962f0091306c6fb6679
check_shader_hash "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" 442f2fd5c9e3dfa9eec34694b4cc9a02c6d7c0d8af22cf85c01c0b45b142524f
check_shader_hash "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" ec8f02fc821d24f09811c63a613a70d75e507a5d6470cf54f2a4e1da585a037e
check_shader_hash "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" e7aa6b36843a163c0d3a0a3a06cd4de40483c57abd2b931c8a659590d3215d5d
/usr/bin/python3 - "$ROOT" "$MANIFEST" "$ACTUAL_GAME_SHA256" <<'PYV7'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
manifest_path = pathlib.Path(sys.argv[2])
actual = sys.argv[3]
with open(manifest_path, 'r', encoding='utf-8') as f:
    manifest = json.load(f)
if manifest.get('candidate') != 'V7':
    raise SystemExit('source manifest is not candidate V7')
if manifest.get('candidate_executable_sha256') != actual:
    raise SystemExit('manifest executable hash does not match V7')
if manifest.get('validation', {}).get('build_exit') != 0:
    raise SystemExit('V7 source manifest does not record a successful build')
source_hashes = manifest.get('source_hashes')
if not isinstance(source_hashes, dict) or not source_hashes:
    raise SystemExit('V7 manifest has no source_hashes map')
for rel, expected in source_hashes.items():
    source = root / rel
    if not source.is_file():
        raise SystemExit('V7 manifest source is missing: ' + rel)
    got = hashlib.sha256(source.read_bytes()).hexdigest()
    if got != expected:
        raise SystemExit(f'V7 source hash mismatch: {rel}: {got} != {expected}')
PYV7

mkdir -p "$OUT"
ditto "$BASE" "$RT"
cp "$(dirname "$0")/motion.lua" "$OUT/motion.lua"
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
    XRAY_METAL_NATIVE_RETINA XRAY_METAL_E08_GPU_PASSES XRAY_METAL_LIGHT_HOM_DIAGNOSTIC \
    XRAY_METAL_LOCAL_LIGHT_DIAGNOSTICS XRAY_METAL_E07_DIAGNOSTIC XRAY_METAL_E07_NPC_COUNT \
    XRAY_METAL_E01_FRAMES XRAY_METAL_E07_FRAMES XRAY_METAL_DYNAMIC_DRAW_DIAGNOSTICS \
    XRAY_METAL_E03_TRACE_PATH XRAY_METAL_E03_CAPTURE_FLAG_PATH XRAY_METAL_E03_CENSUS_FLAG_PATH \
    METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR \
    XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE XRAY_METAL_LOCAL_LIGHT_FACE_CULL \
    XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR \
    XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY XRAY_METAL_E08_FLORA_INSTANCING_OFF \
    XRAY_METAL_E08_EARLY_DRAWABLE XRAY_METAL_E08_UPLOAD_BATCH XRAY_METAL_E08_DEDUP_RESOURCES \
    XRAY_METAL_E08_HEAP_BINDINGS XRAY_METAL_E08_FORCE_SUBMIT_RETRY \
    XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE \
    XRAY_METAL_COMPUTE_SKIN_ORACLE XRAY_METAL_ACTIVE_OCCLUSION XRAY_METAL_WHOLE_LIGHT_VISIBILITY \
    XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME XRAY_WHOLE_LIGHT_VISIBILITY_HARNESS_LOG \
    XRAY_METAL_E08_FLORA_LINEAR XRAY_METAL_E08_FLORA_ORACLE XRAY_METAL_E08_FLORA_REUSE_GLOBALS \
    XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF XRAY_METAL_E08_STATIC_LINEAR XRAY_METAL_E08_STATIC_ORACLE \
    XRAY_METAL_E08_STATIC_MERGE_PIXEL_ORACLE_DIR XRAY_METAL_E08_SUN_LINEAR XRAY_METAL_E08_SUN_ORACLE \
    XRAY_METAL_E08_SUN_PIXEL_ORACLE_DIR XRAY_METAL_E08_PARTICLE_LEGACY XRAY_METAL_E08_SHADOW_SEPARATE_TILES \
    XRAY_METAL_E08_TEST_LARGE_TLAB XRAY_METAL_COLOR_PROBE_DIR XRAY_METAL_COLOR_PROBE_EVERY \
    XRAY_METAL_MOTION_PROBE_DIR XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE \
    XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_DIR XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_EVERY \
    XRAY_METAL_SHADOW_LOCAL_PROBE XRAY_METAL_SHADOW_PROBE_DIR XRAY_METAL_SHADOW_PROBE_EVERY \
    XRAY_METAL_TEMPORAL_DIAGNOSTICS XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY \
    XRAY_METAL_MAX_FRAMES_IN_FLIGHT XRAY_METAL_EXPERIMENTAL_HUD_SKIN4 \
    XRAY_METAL_SHADOW_FIXTURE_LIGHT_POSITION XRAY_METAL_FIXED_SCENE_CAMERA XRAY_METAL_LEVEL_DIR \
    XRAY_METAL_VISUAL_ID XRAY_METAL_LOG_DISPLAY_CAPS_JSON XRAY_BENCHMARK_LOGICAL_SIZE \
    XRAY_METAL_COMPUTE_SKIN XRAY_METAL_DYNAMIC_SHADOW_INDEX \
    XRAY_METAL_DYNAMIC_BUMP_PS_METALLIB XRAY_METAL_DYNAMIC_BUMP_VS_METALLIB \
    XRAY_METAL_DYNAMIC_MODEL_BUMP_VS_METALLIB XRAY_METAL_DYNAMIC_SHADOW_PS_METALLIB \
    XRAY_METAL_DYNAMIC_SHADOW_VS_METALLIB XRAY_METAL_LOCAL_LIGHT_FACE_CULL \
    XRAY_PARITY_HARNESS XRAY_PARITY_HARNESS_SCRIPT PERF_GAP_PHASES RESCUE_WARMUP_MS \
    RESCUE_MEASURE_MS RESCUE_SAVE_CONTROL RESCUE_BASE_RUNTIME RESCUE_APP DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES
export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADOW_DIR"
export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$FLORA_DIR"
if [[ "$FEATURE_MODE" == combined ]]; then
    export XRAY_METAL_COMPUTE_SKIN=1
    export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1
else
    export XRAY_METAL_E08_FLORA_INSTANCING_OFF=1
fi
if [[ "$COLOR_PROBE_MODE" == 1 ]]; then
    export XRAY_METAL_COLOR_PROBE_DIR="$COLOR_OUT"
    export XRAY_METAL_COLOR_PROBE_EVERY="$COLOR_PROBE_EVERY"
fi
export RESCUE_MOTION_OUT="$OUT"
export XRAY_PARITY_HARNESS=1
export XRAY_PARITY_HARNESS_SCRIPT="$OUT/motion.lua"
export RESCUE_V7_MOTION_FEATURES="$FEATURE_MODE"
env | /usr/bin/awk -F= '$1 ~ /^(XRAY_|RESCUE_|PERF_GAP_|MTL_)/ {print}' | sort > "$OUT/environment.txt"
printf 'features=%s\ncolor_probe=%s\ncolor_probe_every=%s\n' \
    "$FEATURE_MODE" "$COLOR_PROBE_MODE" "$COLOR_PROBE_EVERY" > "$OUT/run-mode.txt"
printf '%s\n' 'COLOR probe artifacts are PPM HDR previews named r4_temporal_<phase>_<label>_frame_<engineFrame>.ppm. Join engineFrame to motion.csv stage markers; scene_before_postprocess is before presentation and is not the final screen. Captures are diagnostic, not exact six-stage captures or FPS evidence.' > "$OUT/color-probe-scope.txt"
if [[ "$COLOR_PROBE_MODE" == 1 ]]; then mkdir -p "$COLOR_OUT"; fi
/usr/bin/shasum -a 256 "$GAME_EXE" "$MANIFEST" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE.scop" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$SHADOW_DIR/shadow_direct_tree.vs.metallib" "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" \
    "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json" \
    "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" \
    "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" > "$OUT/input-sha256.txt"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"
cd "$RT"
"$GAME_EXE" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE\"" > "$OUT/stdout.log" 2> "$OUT/engine.log" &
GAME_PID=$!
echo "$GAME_PID" > "$OUT/owned-pid.txt"
MARKER_PID=''
WATCHDOG_PID=''
NORMAL_EXIT=0
cleanup() {
    if [[ "$NORMAL_EXIT" != 1 ]]; then kill "$GAME_PID" 2>/dev/null || true; fi
    [[ -z "$MARKER_PID" ]] || kill "$MARKER_PID" 2>/dev/null || true
    [[ -z "$WATCHDOG_PID" ]] || kill "$WATCHDOG_PID" 2>/dev/null || true
    [[ -z "$MARKER_PID" ]] || wait "$MARKER_PID" 2>/dev/null || true
    [[ -z "$WATCHDOG_PID" ]] || wait "$WATCHDOG_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
sleep 1
if ! kill -0 "$GAME_PID" 2>/dev/null; then
    echo 'Game exited before runtime motion began' >&2
    exit 1
fi
echo "RUN=$RUN" > "$OUT/live-owner.txt"
echo "GAME_PID=$GAME_PID" >> "$OUT/live-owner.txt"
echo "APP_SHA=$(sha256_file "$GAME_EXE")" >> "$OUT/live-owner.txt"
echo "FEATURE_MODE=$FEATURE_MODE" >> "$OUT/live-owner.txt"
echo "Started bounded V7 camera motion (features=$FEATURE_MODE); run=$RUN pid=$GAME_PID"
printf '%s\n' 'Stage markers are recorded in stage-cua-markers.csv for root CUA snapshots; no shell UI capture is used.' > "$OUT/capture-method.txt"

(
    LAST_STAGE=''
    for ((i=0; i<180; i++)); do
        kill -0 "$GAME_PID" 2>/dev/null || exit 0
        if [[ "$COLOR_PROBE_MODE" == 1 && -d "$COLOR_OUT" ]]; then
            COLOR_USAGE_KB="$(/usr/bin/du -sk "$COLOR_OUT" | /usr/bin/awk '{print $1 + 0}')"
            if (( COLOR_USAGE_KB >= 786432 )); then
                {
                    printf 'reason=proactive_color_probe_output_stop\n'
                    printf 'observed_kib=%s\nstop_kib=786432\nplanning_ceiling_kib=1048576\n' "$COLOR_USAGE_KB"
                    printf 'The 1GiB planning ceiling is not a hard filesystem cap; this stop reserves about 256MiB for an in-flight capture.\n'
                    date -u '+stopped_utc=%Y-%m-%dT%H:%M:%SZ'
                } > "$OUT/color-probe-budget-stop.txt"
                kill -TERM "$GAME_PID" 2>/dev/null || true
                for ((grace=0; grace<5; ++grace)); do
                    kill -0 "$GAME_PID" 2>/dev/null || exit 0
                    sleep 1
                done
                if kill -0 "$GAME_PID" 2>/dev/null; then
                    kill -KILL "$GAME_PID" 2>/dev/null || true
                fi
                exit 0
            fi
        fi
        STAGE_ROW="$(tail -n 1 "$OUT/motion.csv" 2>/dev/null || true)"
        STAGE="${STAGE_ROW%%,*}"
        if [[ "$STAGE" != "$LAST_STAGE" && "$STAGE" == stage_* ]]; then
            LAST_STAGE="$STAGE"
            printf '%s,%s,%s\n' "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$STAGE" "$STAGE_ROW" >> "$OUT/stage-cua-markers.csv"
        fi
        sleep 1
    done
) &
MARKER_PID=$!
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
wait "$MARKER_PID" || true
MARKER_PID=''
if [[ -f "$OUT/color-probe-budget-stop.txt" ]]; then STATUS=125; fi
echo "$STATUS" > "$OUT/exit-status.txt"
if [[ "$COLOR_PROBE_MODE" == 1 && -d "$COLOR_OUT" ]]; then
    printf 'ppm_files=%s\n' "$(find "$COLOR_OUT" -type f -name '*.ppm' | wc -l | tr -d ' ')" > "$OUT/color-probe-output-summary.txt"
    printf 'soft_stop_kib=786432 planning_ceiling_kib=1048576\n' >> "$OUT/color-probe-output-summary.txt"
    /usr/bin/du -sk "$COLOR_OUT" >> "$OUT/color-probe-output-summary.txt"
fi
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/exit-time.txt"
NORMAL_EXIT=1
trap - EXIT INT TERM
echo "Motion run exited with status $STATUS; run=$RUN"
exit "$STATUS"
