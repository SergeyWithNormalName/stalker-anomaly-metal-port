#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
RESCUE="$ROOT/scratch/gameplay-rescue-20260929"
STAGE="$RESCUE/harness/runtime-motion/v8-static-constant-groups"
PROFILE_LUA="$RESCUE/harness/runtime-motion/v7-performance-ab/live_profile_no_capture.lua"
ARM="${RESCUE_V8_STATIC_GROUPS_ARM:-}"
case "$ARM" in
    off|on) ;;
    *) echo 'Set RESCUE_V8_STATIC_GROUPS_ARM to off or on.' >&2; exit 2 ;;
esac
RUN_ID="${RESCUE_RUN_ID:-v8-static-groups-${ARM}-01}"
RUN="$RESCUE/runs/$RUN_ID"
BASE="$RESCUE/runs/v2-canonical-oracle-01/runtime"
APP="$RESCUE/candidate-v8/Anomaly.app"
GAME_EXE="$APP/Contents/MacOS/AnomalyDX11"
MANIFEST="$RESCUE/candidate-v8/source-manifest.json"
SHADER_ROOT="$ROOT/build/metal-runtime-shaders"
SHADOW_DIR="$SHADER_ROOT/shadow-casters-raster-depth-v1"
FLORA_DIR="$SHADER_ROOT/flora-shadow-instanced-e08"
OUT="$RUN/output"
RT="$RUN/runtime"
SAVE="rescue-cordon-control"
EXPECTED_APP_SHA256="73bb23f7c7596da5a184522f7c12871ed299cca4892b7b4887a8ac1864ab5539"
EXPECTED_MANIFEST_SHA256="514c416942971283c42ec5d2d91f2402503be4b22c82a6383f48129f0938cf15"
EXPECTED_USER_SHA256="b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334"
EXPECTED_SAVE_SHA256="a21f09761ca6e7c3633e0b002b34d760ea30ece41f6542a1814eabba5a0ac3c4"
EXPECTED_SHADOW_RECEIPT_SHA256="02625fdf8b75a8167cd447506cf783809ac139a1853eae6a92f71430e75dc70b"
EXPECTED_FLORA_RECEIPT_SHA256="b2e8753cb653d564188984142367bd14e0eff1b71385e24afa0f5e50f3733350"

if [[ "${RESCUE_ROOT_APPROVED:-}" != 1 ]]; then
    echo 'Refusing launch: root has not authorized this V8 performance arm.' >&2
    exit 2
fi
if [[ -z "${RESCUE_EXPECTED_APP_SHA256:-}" || "$RESCUE_EXPECTED_APP_SHA256" != "$EXPECTED_APP_SHA256" ]]; then
    echo 'Set RESCUE_EXPECTED_APP_SHA256 to the reviewed V8 executable SHA-256.' >&2
    exit 2
fi
if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second game process.' >&2
    exit 1
fi
if [[ -e "$RUN" ]]; then
    echo "Run path already exists: $RUN; evidence is preserved." >&2
    exit 1
fi
for required in "$GAME_EXE" "$MANIFEST" "$BASE/appdata/user.ltx" \
    "$BASE/appdata/savedgames/$SAVE.scop" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json"; do
    [[ -f "$required" ]] || { echo "Required file missing: $required" >&2; exit 1; }
done
[[ -x "$GAME_EXE" ]] || { echo "V8 executable is not executable: $GAME_EXE" >&2; exit 1; }

sha256_file() {
    local digest rest
    read -r digest rest < <(/usr/bin/shasum -a 256 "$1")
    printf '%s' "$digest"
}
ACTUAL_APP_SHA256="$(sha256_file "$GAME_EXE")"
[[ "$ACTUAL_APP_SHA256" == "$EXPECTED_APP_SHA256" && "$ACTUAL_APP_SHA256" == "$RESCUE_EXPECTED_APP_SHA256" ]] || {
    echo "V8 executable hash mismatch: $ACTUAL_APP_SHA256" >&2; exit 1;
}
[[ "$(sha256_file "$MANIFEST")" == "$EXPECTED_MANIFEST_SHA256" ]] || {
    echo 'V8 source manifest hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE/appdata/user.ltx")" == "$EXPECTED_USER_SHA256" ]] || {
    echo 'Canonical user.ltx hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE/appdata/savedgames/$SAVE.scop")" == "$EXPECTED_SAVE_SHA256" ]] || {
    echo 'Canonical Cordon save hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$SHADOW_DIR/shadow-casters-msc-receipt.txt")" == "$EXPECTED_SHADOW_RECEIPT_SHA256" ]] || {
    echo 'Corrected depth bundle receipt hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$FLORA_DIR/flora-instanced-shadow-receipt.json")" == "$EXPECTED_FLORA_RECEIPT_SHA256" ]] || {
    echo 'Flora bundle receipt hash changed.' >&2; exit 1;
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
/usr/bin/python3 - "$ROOT" "$MANIFEST" "$ACTUAL_APP_SHA256" <<'PYV8'
import hashlib, json, pathlib, sys
root, manifest_path, actual = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
if manifest.get('candidate') != 'V8':
    raise SystemExit('source manifest is not candidate V8')
if manifest.get('candidate_executable_sha256') != actual:
    raise SystemExit('manifest executable hash does not match V8')
if manifest.get('validation', {}).get('build_exit') != 0:
    raise SystemExit('V8 manifest does not record a successful build')
source_hashes = manifest.get('source_hashes')
if not isinstance(source_hashes, dict) or not source_hashes:
    raise SystemExit('V8 manifest has no source_hashes map')
for rel, expected in source_hashes.items():
    source = root / rel
    if not source.is_file():
        raise SystemExit('V8 manifest source is missing: ' + rel)
    actual_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    if actual_hash != expected:
        raise SystemExit(f'V8 source hash mismatch: {rel}: {actual_hash} != {expected}')
PYV8

mkdir -p "$OUT"
ditto "$BASE" "$RT"
cp "$PROFILE_LUA" "$OUT/live_profile.lua"
LOADER="$RT/gamedata/scripts/axr_main.script"
if [[ ! -f "$LOADER" ]] || ! /usr/bin/grep -q 'if not parity_harness_register then' "$LOADER" || \
   ! /usr/bin/grep -q '^[[:space:]]*parity_harness_register()' "$LOADER" || \
   ! /usr/bin/grep -q '^function parity_harness_register()' "$OUT/live_profile.lua"; then
    echo 'Parity harness loader contract failed.' >&2
    exit 1
fi
{
    /usr/bin/shasum -a 256 "$LOADER" "$OUT/live_profile.lua"
    /usr/bin/grep -n -E 'parity_harness_register' "$LOADER" "$OUT/live_profile.lua"
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
    XRAY_METAL_COMPUTE_SKIN XRAY_METAL_COMPUTE_SKIN_ORACLE XRAY_METAL_DYNAMIC_SHADOW_INDEX \
    XRAY_METAL_STATIC_CONSTANT_GROUPS \
    XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE XRAY_METAL_DYNAMIC_BUMP_PS_METALLIB \
    XRAY_METAL_DYNAMIC_BUMP_VS_METALLIB XRAY_METAL_DYNAMIC_MODEL_BUMP_VS_METALLIB \
    XRAY_METAL_DYNAMIC_SHADOW_PS_METALLIB XRAY_METAL_DYNAMIC_SHADOW_VS_METALLIB \
    XRAY_METAL_LOCAL_LIGHT_FACE_CULL XRAY_PARITY_HARNESS XRAY_PARITY_HARNESS_SCRIPT \
    PERF_GAP_PHASES RESCUE_WARMUP_MS RESCUE_MEASURE_MS RESCUE_SAVE_CONTROL RESCUE_BASE_RUNTIME \
    RESCUE_APP RESCUE_ENV_ASSIGNMENTS DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES

export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADOW_DIR"
export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$FLORA_DIR"
export XRAY_METAL_COMPUTE_SKIN=1
export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1
if [[ "$ARM" == on ]]; then
    export XRAY_METAL_STATIC_CONSTANT_GROUPS=1
fi
export PERF_GAP_PHASES="$OUT/phases.csv"
export XRAY_PARITY_HARNESS=1
export XRAY_PARITY_HARNESS_SCRIPT="$OUT/live_profile.lua"
export RESCUE_WARMUP_MS=30000
export RESCUE_MEASURE_MS=60000
export RESCUE_SAVE_CONTROL=0
export RESCUE_V8_STATIC_GROUPS_ARM="$ARM"
env | /usr/bin/awk -F= '$1 ~ /^(XRAY_|RESCUE_|PERF_GAP_|MTL_)/ {print}' | sort > "$OUT/environment.txt"
printf 'candidate_feature=static_constant_groups\narm=%s\nbase_runtime=%s\nbase_save=%s\nwarmup_ms=30000\nmeasure_ms=60000\npost_measure_ms=15000\n' \
    "$ARM" "$BASE" "$SAVE" > "$OUT/run-mode.txt"
printf '%s\n' 'Metric semantics: phase=1 Lua callback interval throughput only; it is not presented/display FPS. Both arms keep compute skin, dynamic shadow indexing, flora instancing, corrected depth and flora bundles enabled; only XRAY_METAL_STATIC_CONSTANT_GROUPS differs. No color, screenshot, oracle, GPU timing, or observer diagnostics are enabled.' > "$OUT/metric-scope.txt"
/usr/bin/shasum -a 256 "$GAME_EXE" "$MANIFEST" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE.scop" \
    "$OUT/live_profile.lua" "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$SHADOW_DIR/shadow_direct_tree.vs.metallib" "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" \
    "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json" \
    "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" \
    "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" > "$OUT/input-sha256.txt"
pmset -g batt > "$OUT/power-before.txt"
pmset -g therm > "$OUT/thermal-before.txt"
vm_stat > "$OUT/vm-stat-before.txt"
sysctl vm.swapusage > "$OUT/swap-before.txt"
date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"

GAME_PID=''
cleanup_game() {
    if [[ -n "$GAME_PID" ]] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null || true
        for ((grace=0; grace<5; ++grace)); do
            kill -0 "$GAME_PID" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$GAME_PID" 2>/dev/null; then kill -KILL "$GAME_PID" 2>/dev/null || true; fi
    fi
}
trap cleanup_game EXIT INT TERM
cd "$RT"
"$GAME_EXE" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE\"" \
    > "$OUT/stdout.log" 2> "$OUT/engine.log" &
GAME_PID=$!
printf '%s\n' "$GAME_PID" > "$OUT/owned-pid.txt"
printf 'pid=%s\narm=%s\napp_sha256=%s\n' "$GAME_PID" "$ARM" "$ACTUAL_APP_SHA256" > "$OUT/live-owner.txt"

(
    for ((i=0; i<180; ++i)); do
        kill -0 "$GAME_PID" 2>/dev/null || exit 0
        if [[ -f "$OUT/phases.csv" ]]; then
            if /usr/bin/grep -q '^measure_start,' "$OUT/phases.csv" && [[ ! -f "$OUT/vm-stat-measure-start.txt" ]]; then
                date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/measure-start-time.txt"
                vm_stat > "$OUT/vm-stat-measure-start.txt"
                sysctl vm.swapusage > "$OUT/swap-measure-start.txt"
            fi
            if /usr/bin/grep -q '^measure_end,' "$OUT/phases.csv" && [[ ! -f "$OUT/vm-stat-measure-end.txt" ]]; then
                date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/measure-end-time.txt"
                vm_stat > "$OUT/vm-stat-measure-end.txt"
                sysctl vm.swapusage > "$OUT/swap-measure-end.txt"
            fi
        fi
        sleep 1
    done
    if kill -0 "$GAME_PID" 2>/dev/null; then
        date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/watchdog-timeout.txt"
        kill -TERM "$GAME_PID" 2>/dev/null || true
        for ((grace=0; grace<5; ++grace)); do
            kill -0 "$GAME_PID" 2>/dev/null || exit 0
            sleep 1
        done
        if kill -0 "$GAME_PID" 2>/dev/null; then kill -KILL "$GAME_PID" 2>/dev/null || true; fi
    fi
) &
WATCHDOG_PID=$!
printf '%s\n' "$WATCHDOG_PID" > "$OUT/watchdog-pid.txt"
STATUS=0
wait "$GAME_PID" || STATUS=$?
kill "$WATCHDOG_PID" 2>/dev/null || true
wait "$WATCHDOG_PID" 2>/dev/null || true
printf '%s\n' "$STATUS" > "$OUT/exit-status.txt"
pmset -g batt > "$OUT/power-after.txt"
pmset -g therm > "$OUT/thermal-after.txt"
vm_stat > "$OUT/vm-stat-after.txt"
sysctl vm.swapusage > "$OUT/swap-after.txt"
printf '%s\n' "$(sha256_file "$GAME_EXE")  $GAME_EXE" > "$OUT/post-run-app-sha256.txt"
if [[ -f "$OUT/phases.csv.device.csv" ]]; then
    printf '%s\n' 'Lua phase markers are in phases.csv; measured per-callback rows are phase=1 in phases.csv.device.csv.' > "$OUT/phase-contract.txt"
fi
NORMAL_EXIT=1
trap - EXIT INT TERM
echo "V8 static constant groups arm $ARM exited with status $STATUS; run=$RUN"
exit "$STATUS"
