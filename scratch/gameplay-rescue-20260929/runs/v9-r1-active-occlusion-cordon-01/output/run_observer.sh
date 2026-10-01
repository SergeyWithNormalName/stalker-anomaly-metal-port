#!/bin/bash
set -euo pipefail

# Prepared V9 revision 1 runner; launch remains pin- and approval-gated.
# hash and an explicit root authorization environment token.
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
STAGE="$ROOT/scratch/gameplay-rescue-20260929/active-occlusion-stage/observer-harness-v9-r1"
RUN_ID="${RESCUE_RUN_ID:-v9-r1-active-occlusion-cordon-01}"
RUN_ROOT="$ROOT/scratch/gameplay-rescue-20260929/runs/$RUN_ID"
BASE_RUNTIME="$ROOT/scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime"
SAVE_NAME="rescue-cordon-control"
APP="$ROOT/scratch/gameplay-rescue-20260929/candidate-v9-r1/Anomaly.app"
GAME_EXE="$APP/Contents/MacOS/AnomalyDX11"
MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v9-r1/source-manifest.json"
PARSER_SCHEMA="$STAGE/catalog-schema-v9r1.json"
HARNESS_MANIFEST="$STAGE/harness-sha256.json"
ORIGINAL_EXE="$ROOT/build/Binaries/RelWithDebInfo/Anomaly.app/Contents/MacOS/AnomalyDX11"
V4_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v4/Anomaly.app/Contents/MacOS/AnomalyDX11"
V5_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v5/Anomaly.app/Contents/MacOS/AnomalyDX11"
V6_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v6/Anomaly.app/Contents/MacOS/AnomalyDX11"
V7_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v7/Anomaly.app/Contents/MacOS/AnomalyDX11"
V8_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v8/Anomaly.app/Contents/MacOS/AnomalyDX11"
V8_MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v8/source-manifest.json"
V9_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v9/Anomaly.app/Contents/MacOS/AnomalyDX11"
V9_MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v9/source-manifest.json"
SHADER_ROOT="$ROOT/build/metal-runtime-shaders"
SHADOW_DIR="$SHADER_ROOT/shadow-casters-raster-depth-v1"
FLORA_DIR="$SHADER_ROOT/flora-shadow-instanced-e08"
OUT="$RUN_ROOT/output"
RT="$RUN_ROOT/runtime"
MAX_OWNED_SECONDS=180

EXPECTED_ORIGINAL_SHA256="b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc"
EXPECTED_V4_SHA256="2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71"
EXPECTED_V5_SHA256="5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f"
EXPECTED_V6_SHA256="238db09155f62caf726f53917cf82876aa12b06ee60c8ea03d157b24731c26dd"
EXPECTED_BASE_USER_SHA256="b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334"
EXPECTED_BASE_SAVE_SHA256="a21f09761ca6e7c3633e0b002b34d760ea30ece41f6542a1814eabba5a0ac3c4"
EXPECTED_V8_SHA256="73bb23f7c7596da5a184522f7c12871ed299cca4892b7b4887a8ac1864ab5539"
EXPECTED_V8_MANIFEST_SHA256="514c416942971283c42ec5d2d91f2402503be4b22c82a6383f48129f0938cf15"
EXPECTED_V9_BASE_SHA256="1c2cfc02958b3ea6ad3b175490b6eb5c755e42175a6bf85ee89af00b954d9f2c"
EXPECTED_V9_BASE_MANIFEST_SHA256="dd21b562ebe6e7e2604d716921f80c7174d8da3677fa4da5cdd9f889b3c62210"
EXPECTED_V9R1_SHA256="f868820558752e6bf97f42eba0c9e7b2ed01b6c6114cf6a2ca030954ff4588ba"
EXPECTED_V9R1_MANIFEST_SHA256="e1b3c9d037ac3d4662dd5a52ca4a0041629aa5e0afa291ab5b7262b53e4323fb"

V9_SANITIZE_ENV=(
    MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR MTL_CAPTURE_ENABLED
    XRAY_METAL_NATIVE_RETINA XRAY_METAL_E08_GPU_PASSES XRAY_METAL_LIGHT_HOM_DIAGNOSTIC
    XRAY_METAL_LOCAL_LIGHT_DIAGNOSTICS XRAY_METAL_E07_DIAGNOSTIC XRAY_METAL_E07_NPC_COUNT
    XRAY_METAL_E01_FRAMES XRAY_METAL_E07_FRAMES XRAY_METAL_DYNAMIC_DRAW_DIAGNOSTICS
    XRAY_METAL_E03_TRACE_PATH XRAY_METAL_E03_CAPTURE_FLAG_PATH XRAY_METAL_E03_CENSUS_FLAG_PATH
    METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR
    XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE XRAY_METAL_LOCAL_LIGHT_FACE_CULL
    XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR
    XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY XRAY_METAL_E08_FLORA_INSTANCING_OFF
    XRAY_METAL_E08_EARLY_DRAWABLE XRAY_METAL_E08_UPLOAD_BATCH XRAY_METAL_E08_DEDUP_RESOURCES
    XRAY_METAL_E08_HEAP_BINDINGS XRAY_METAL_E08_FORCE_SUBMIT_RETRY XRAY_METAL_E08_FLORA_LINEAR
    XRAY_METAL_E08_FLORA_ORACLE XRAY_METAL_E08_FLORA_REUSE_GLOBALS XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF
    XRAY_METAL_E08_STATIC_LINEAR XRAY_METAL_E08_STATIC_ORACLE XRAY_METAL_E08_STATIC_MERGE_PIXEL_ORACLE_DIR
    XRAY_METAL_E08_SUN_LINEAR XRAY_METAL_E08_SUN_ORACLE XRAY_METAL_E08_SUN_PIXEL_ORACLE_DIR
    XRAY_METAL_E08_PARTICLE_LEGACY XRAY_METAL_E08_SHADOW_SEPARATE_TILES
    XRAY_METAL_E08_TEST_LARGE_TLAB XRAY_METAL_COLOR_PROBE_DIR XRAY_METAL_COLOR_PROBE_EVERY
    XRAY_METAL_MOTION_PROBE_DIR XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE
    XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_DIR XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_EVERY
    XRAY_METAL_SHADOW_LOCAL_PROBE XRAY_METAL_SHADOW_PROBE_DIR XRAY_METAL_SHADOW_PROBE_EVERY
    XRAY_METAL_TEMPORAL_DIAGNOSTICS XRAY_METAL_TEMPORAL_FORCE_ACQUIRE_RETRY
    XRAY_METAL_TEMPORAL_COLD_UPLOAD_PROBE XRAY_METAL_TEMPORAL_LIFECYCLE_PROBE
    XRAY_METAL_MAX_FRAMES_IN_FLIGHT XRAY_METAL_EXPERIMENTAL_HUD_SKIN4
    XRAY_METAL_SHADOW_FIXTURE_LIGHT_POSITION XRAY_METAL_FIXED_SCENE_CAMERA XRAY_METAL_LEVEL_DIR
    XRAY_METAL_VISUAL_ID XRAY_METAL_LOG_DISPLAY_CAPS_JSON XRAY_BENCHMARK_LOGICAL_SIZE
    XRAY_METAL_COMPUTE_SKIN XRAY_METAL_COMPUTE_SKIN_ORACLE XRAY_METAL_DYNAMIC_SHADOW_INDEX
    XRAY_METAL_DYNAMIC_BUMP_PS_METALLIB XRAY_METAL_DYNAMIC_BUMP_VS_METALLIB
    XRAY_METAL_DYNAMIC_MODEL_BUMP_VS_METALLIB XRAY_METAL_DYNAMIC_SHADOW_PS_METALLIB
    XRAY_METAL_DYNAMIC_SHADOW_VS_METALLIB XRAY_METAL_ACTIVE_OCCLUSION XRAY_METAL_STATIC_CONSTANT_GROUPS
    XRAY_METAL_WHOLE_LIGHT_VISIBILITY XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME
    XRAY_WHOLE_LIGHT_VISIBILITY_HARNESS_LOG XRAY_METAL_MSC_FLORA_SHADOW_DIR
    XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR XRAY_PARITY_HARNESS XRAY_PARITY_HARNESS_SCRIPT
    PERF_GAP_PHASES RESCUE_WARMUP_MS RESCUE_MEASURE_MS RESCUE_SAVE_CONTROL RESCUE_BASE_RUNTIME
    RESCUE_APP RESCUE_ENV_ASSIGNMENTS DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES
)

sha256_file() {
    local digest rest
    read -r digest rest < <(/usr/bin/shasum -a 256 "$1")
    printf '%s' "$digest"
}

configure_v9_environment() {
    export XRAY_PROJECT_ROOT="$ROOT"
    source "$ROOT/script/xray_metal_shader_env.sh"
    unset "${V9_SANITIZE_ENV[@]}"
    export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADOW_DIR"
    export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$FLORA_DIR"
    export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1
    export XRAY_METAL_COMPUTE_SKIN=1
    export XRAY_METAL_ACTIVE_OCCLUSION=1
    export XRAY_METAL_WHOLE_LIGHT_VISIBILITY=1
    unset XRAY_METAL_STATIC_CONSTANT_GROUPS
    export XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME=300
}

if [[ "${RESCUE_PREFLIGHT_ENV:-}" == "1" ]]; then
    configure_v9_environment
    for name in "${V9_SANITIZE_ENV[@]}"; do
        case "$name" in
            XRAY_METAL_COMPUTE_SKIN|XRAY_METAL_DYNAMIC_SHADOW_INDEX|XRAY_METAL_ACTIVE_OCCLUSION|XRAY_METAL_WHOLE_LIGHT_VISIBILITY|XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME|XRAY_METAL_MSC_FLORA_SHADOW_DIR|XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR)
                continue ;;
        esac
        if printenv "$name" >/dev/null 2>&1; then
            echo "Environment sanitizer left forbidden variable set: $name" >&2
            exit 1
        fi
    done
    [[ "$XRAY_METAL_COMPUTE_SKIN" == 1 && "$XRAY_METAL_DYNAMIC_SHADOW_INDEX" == 1 && \
       "$XRAY_METAL_ACTIVE_OCCLUSION" == 1 && "$XRAY_METAL_WHOLE_LIGHT_VISIBILITY" == 1 && \
       "$XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME" == 300 && \
       "$XRAY_METAL_MSC_FLORA_SHADOW_DIR" == "$SHADOW_DIR" && \
       "$XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR" == "$FLORA_DIR" ]] || {
        echo 'Intended V9 observation features were not configured exactly.' >&2; exit 1;
    }
    [[ -z "${XRAY_METAL_STATIC_CONSTANT_GROUPS:-}" ]] || {
        echo 'Static constant grouping leaked into V9.' >&2; exit 1;
    }
    echo "sanitizer_preflight=PASS cleared=${#V9_SANITIZE_ENV[@]} static_groups=off compute=1 dynamic_index=1 active_observer=1 gpu_observer=1"
    exit 0
fi
configure_v9_environment

if [[ "${RESCUE_ROOT_APPROVED:-}" != "1" ]]; then
    echo 'Refusing launch: root has not authorized this hybrid CPU/GPU observer run.' >&2
    exit 2
fi
if [[ "$EXPECTED_V9R1_SHA256" == "V9R1_NOT_BUILT" || "$EXPECTED_V9R1_MANIFEST_SHA256" == "V9R1_MANIFEST_NOT_BUILT" ]]; then
    echo 'Refusing launch: candidate V9r1 build and source-manifest pins are not frozen.' >&2
    exit 2
fi
if [[ ! -f "$PARSER_SCHEMA" ]]; then
    echo 'Refusing launch: V9r1 catalog/selector parser schema has not been reviewed and frozen.' >&2
    exit 2
fi
if [[ ! -f "$HARNESS_MANIFEST" ]]; then
    echo 'V9r1 harness hash manifest is missing.' >&2
    exit 2
fi
if [[ -z "${RESCUE_EXPECTED_APP_SHA256:-}" ]]; then
    echo 'Set RESCUE_EXPECTED_APP_SHA256 from the reviewed candidate-v9-r1 manifest.' >&2
    exit 2
fi
if pgrep -x AnomalyDX11 >/dev/null; then
    echo 'AnomalyDX11 is already running; refusing a second owned process.' >&2
    exit 1
fi
if [[ -e "$RUN_ROOT" ]]; then
    echo "Run directory already exists: $RUN_ROOT" >&2
    echo 'Choose a fresh RESCUE_RUN_ID; existing evidence is preserved.' >&2
    exit 1
fi
for required in "$GAME_EXE" "$MANIFEST" "$ORIGINAL_EXE" "$V4_EXE" "$V5_EXE" "$V6_EXE" "$V7_EXE" "$V8_EXE" "$V8_MANIFEST" "$V9_EXE" "$V9_MANIFEST" "$PARSER_SCHEMA" \
    "$BASE_RUNTIME/appdata/user.ltx" "$BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json"; do
    [[ -e "$required" ]] || { echo "Required file missing: $required" >&2; exit 1; }
done

ACTUAL_GAME_SHA256="$(sha256_file "$GAME_EXE")"
[[ "$ACTUAL_GAME_SHA256" == "$EXPECTED_V9R1_SHA256" ]] || {
    echo "Candidate V9r1 does not match the pinned executable hash: $ACTUAL_GAME_SHA256" >&2; exit 1;
}
[[ "$ACTUAL_GAME_SHA256" == "$RESCUE_EXPECTED_APP_SHA256" ]] || {
    echo "V9r1 executable hash mismatch: $ACTUAL_GAME_SHA256" >&2; exit 1;
}
[[ "$(sha256_file "$ORIGINAL_EXE")" == "$EXPECTED_ORIGINAL_SHA256" ]] || {
    echo 'Original production executable is not at its verified restored hash.' >&2; exit 1;
}
[[ "$(sha256_file "$V4_EXE")" == "$EXPECTED_V4_SHA256" ]] || {
    echo 'V4 candidate hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V5_EXE")" == "$EXPECTED_V5_SHA256" ]] || {
    echo 'V5 candidate hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V6_EXE")" == "$EXPECTED_V6_SHA256" ]] || {
    echo 'V6 candidate hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V7_EXE")" == "43c07e4d7a043d44f01d675554aa30a16d5bd09f76a9e99468317d5983fc2bdd" ]] || {
    echo 'V7 candidate hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V8_EXE")" == "$EXPECTED_V8_SHA256" ]] || {
    echo 'V8 candidate hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V8_MANIFEST")" == "$EXPECTED_V8_MANIFEST_SHA256" ]] || {
    echo 'V8 candidate manifest hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V9_EXE")" == "$EXPECTED_V9_BASE_SHA256" ]] || {
    echo 'V9 base candidate hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$V9_MANIFEST")" == "$EXPECTED_V9_BASE_MANIFEST_SHA256" ]] || {
    echo 'V9 base candidate manifest hash changed.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE_RUNTIME/appdata/user.ltx")" == "$EXPECTED_BASE_USER_SHA256" ]] || {
    echo 'Canonical base user.ltx hash changed; refusing to clone.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop")" == "$EXPECTED_BASE_SAVE_SHA256" ]] || {
    echo 'Canonical base save hash changed; refusing to clone.' >&2; exit 1;
}
V6_MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v6/source-manifest.json"
V5_MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v5/source-manifest.json"
/usr/bin/python3 - "$ROOT" "$MANIFEST" "$V9_MANIFEST" "$ACTUAL_GAME_SHA256" "$EXPECTED_V9R1_MANIFEST_SHA256" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
path = pathlib.Path(sys.argv[2])
v9_path = pathlib.Path(sys.argv[3])
actual = sys.argv[4]
expected_manifest_sha = sys.argv[5]
if hashlib.sha256(path.read_bytes()).hexdigest() != expected_manifest_sha:
    raise SystemExit('candidate V9r1 source-manifest hash mismatch')
manifest = json.loads(path.read_text(encoding='utf-8'))
v9 = json.loads(v9_path.read_text(encoding='utf-8'))
if manifest.get('candidate') != 'V9r1':
    raise SystemExit('source manifest is not candidate V9r1')
if manifest.get('candidate_executable_sha256') != actual:
    raise SystemExit('manifest executable hash does not match candidate V9r1')
if manifest.get('validation', {}).get('build_exit') != 0:
    raise SystemExit('candidate V9r1 manifest does not record a successful build')
source_hashes = manifest.get('source_hashes')
if not isinstance(source_hashes, dict) or not source_hashes:
    raise SystemExit('candidate V9r1 manifest has no source_hashes map')
missing = sorted(set(v9.get('source_hashes', {})) - set(source_hashes))
if missing:
    raise SystemExit('candidate V9r1 manifest omits V9 lineage sources: ' + ', '.join(missing))
for rel, expected in source_hashes.items():
    source = root / rel
    if not source.is_file():
        raise SystemExit('candidate V9r1 source manifest path is missing: ' + rel)
    got = hashlib.sha256(source.read_bytes()).hexdigest()
    if got != expected:
        raise SystemExit(f'candidate V9r1 source hash mismatch: {rel}: {got} != {expected}')
PY

/usr/bin/python3 - "$STAGE" "$HARNESS_MANIFEST" <<'PY'
import hashlib, json, pathlib, sys
stage = pathlib.Path(sys.argv[1])
manifest = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding='utf-8'))
files = manifest.get('files')
if not isinstance(files, dict) or not files:
    raise SystemExit('V9r1 harness manifest has no file hashes')
for rel, expected in files.items():
    path = stage / rel
    if not path.is_file():
        raise SystemExit('V9r1 harness file missing: ' + rel)
    actual = hashlib.sha256(path.read_bytes()).hexdigest()
    if actual != expected:
        raise SystemExit(f'V9r1 harness hash mismatch: {rel}: {actual} != {expected}')
PY

BASE_LOADER="$BASE_RUNTIME/gamedata/scripts/axr_main.script"
if [[ ! -f "$BASE_LOADER" ]] || ! /usr/bin/grep -q 'if not parity_harness_register then' "$BASE_LOADER" || \
   ! /usr/bin/grep -q '^[[:space:]]*parity_harness_register()' "$BASE_LOADER" || \
   ! /usr/bin/grep -q '^function parity_harness_register()' "$STAGE/observer_window_v9r1.lua"; then
    echo 'V9r1 base-runtime parity harness loader contract failed.' >&2
    exit 1
fi
if [[ "${RESCUE_PREFLIGHT_ONLY:-}" == "1" ]]; then
    echo "static_preflight=PASS app_sha256=$ACTUAL_GAME_SHA256 manifest_sha256=$EXPECTED_V9R1_MANIFEST_SHA256 source_files=47 loader_contract=PASS run_root_absent=1"
    exit 0
fi

mkdir -p "$OUT"
ditto "$BASE_RUNTIME" "$RT"
cp "$STAGE/observer_window_v9r1.lua" "$OUT/observer_window.lua"
cp "$STAGE/summarize_observation_v9r1.py" "$OUT/summarize_observation.py"
cp "$STAGE/run_observer_v9r1.sh" "$OUT/run_observer.sh"
LOADER="$RT/gamedata/scripts/axr_main.script"
if [[ ! -f "$LOADER" ]] || ! /usr/bin/grep -q 'if not parity_harness_register then' "$LOADER" || \
   ! /usr/bin/grep -q '^[[:space:]]*parity_harness_register()' "$LOADER" || \
   ! /usr/bin/grep -q '^function parity_harness_register()' "$OUT/observer_window.lua"; then
    echo 'V9r1 parity harness loader contract failed.' >&2
    exit 1
fi
{
    /usr/bin/shasum -a 256 "$LOADER" "$OUT/observer_window.lua"
    /usr/bin/grep -n -E 'parity_harness_register' "$LOADER" "$OUT/observer_window.lua"
} > "$OUT/loader-contract.txt"
export XRAY_WHOLE_LIGHT_VISIBILITY_HARNESS_LOG="$OUT/observer-markers.csv"
export XRAY_PARITY_HARNESS=1
export XRAY_PARITY_HARNESS_SCRIPT="$OUT/observer_window.lua"

/usr/bin/python3 - "$OUT/environment.json" <<'PY'
import json, os, sys
wanted = {k: v for k, v in os.environ.items() if k.startswith(('XRAY_', 'MTL_'))}
with open(sys.argv[1], 'w', encoding='ascii') as f:
    json.dump(dict(sorted(wanted.items())), f, indent=2)
    f.write('\n')
PY
/usr/bin/shasum -a 256 \
    "$GAME_EXE" "$MANIFEST" "$V8_EXE" "$V8_MANIFEST" "$V9_EXE" "$V9_MANIFEST" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" \
    "$STAGE/run_observer_v9r1.sh" "$STAGE/observer_window_v9r1.lua" "$STAGE/summarize_observation_v9r1.py" "$STAGE/test_summarize_observation_v9r1.py" "$PARSER_SCHEMA" \
    "$HARNESS_MANIFEST" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$SHADOW_DIR/shadow_direct_tree.vs.metallib" "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" \
    "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json" \
    "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" \
    "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" \
    > "$OUT/input-sha256.txt"
printf '%s\n' "$(sha256_file "$ORIGINAL_EXE")  $ORIGINAL_EXE" "$(sha256_file "$V4_EXE")  $V4_EXE" "$(sha256_file "$V5_EXE")  $V5_EXE" "$(sha256_file "$V6_EXE")  $V6_EXE" "$(sha256_file "$V7_EXE")  $V7_EXE" "$(sha256_file "$V8_EXE")  $V8_EXE" "$(sha256_file "$V9_EXE")  $V9_EXE" > "$OUT/preserved-app-hashes-before.txt"
stat -f '%N bytes=%z mtime=%m' "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" > "$OUT/input-metadata.txt"
/bin/date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"
printf '%s\n' "$GAME_EXE" '-fsltx' "$RT/fsgame.ltx" '-render_backend' 'metal' '-nointro' '-skiplogo' '-load' "\"$SAVE_NAME\"" > "$OUT/argv.txt"

GAME_PID=''
cleanup_owned_game() {
    if [[ -n "$GAME_PID" ]] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null || true
        for ((grace=0; grace<3; ++grace)); do
            kill -0 "$GAME_PID" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$GAME_PID" 2>/dev/null; then kill -KILL "$GAME_PID" 2>/dev/null || true; fi
    fi
}
trap cleanup_owned_game EXIT INT TERM
cd "$RT"
"$GAME_EXE" -fsltx "$RT/fsgame.ltx" -render_backend metal -nointro -skiplogo -load "\"$SAVE_NAME\"" \
    > "$OUT/stdout.log" 2> "$OUT/engine.log" &
GAME_PID=$!
printf '%s\n' "$GAME_PID" > "$OUT/owned-pid.txt"
LAUNCH_EPOCH="$(/bin/date +%s)"
TIMEOUT=0
while kill -0 "$GAME_PID" 2>/dev/null; do
    NOW="$(/bin/date +%s)"
    if (( NOW - LAUNCH_EPOCH >= MAX_OWNED_SECONDS - 5 )); then
        TIMEOUT=1
        printf '%s\n' 'timeout: observer exceeded the 175-second owned-process budget' > "$OUT/timeout.txt"
        cleanup_owned_game
        break
    fi
    sleep 1
done
STATUS=0
wait "$GAME_PID" || STATUS=$?
printf '%s\n' "$STATUS" > "$OUT/exit-status.txt"
printf '%s  %s\n' "$(sha256_file "$GAME_EXE")" "$GAME_EXE" > "$OUT/post-run-app-sha256.txt"
printf '%s\n' "$(sha256_file "$ORIGINAL_EXE")  $ORIGINAL_EXE" "$(sha256_file "$V4_EXE")  $V4_EXE" "$(sha256_file "$V5_EXE")  $V5_EXE" "$(sha256_file "$V6_EXE")  $V6_EXE" "$(sha256_file "$V7_EXE")  $V7_EXE" "$(sha256_file "$V8_EXE")  $V8_EXE" "$(sha256_file "$V9_EXE")  $V9_EXE" > "$OUT/preserved-app-hashes-after.txt"
if ! /usr/bin/cmp -s "$OUT/preserved-app-hashes-before.txt" "$OUT/preserved-app-hashes-after.txt"; then
    printf '%s\n' 'preserved app candidate hash changed during observer run' > "$OUT/preserved-app-hash-mismatch.txt"
    STATUS=1
fi
printf '%s\n' "$STATUS" > "$OUT/exit-status.txt"
printf '%s\n' "$TIMEOUT" > "$OUT/timeout-flag.txt"
trap - EXIT INT TERM
set +e
LC_ALL=C /usr/bin/python3 "$STAGE/summarize_observation_v9r1.py" "$OUT" > "$OUT/summary.stdout.txt" 2> "$OUT/summary.stderr.txt"
SUMMARY_STATUS=$?
set -e
printf '%s\n' "$SUMMARY_STATUS" > "$OUT/summary-exit-status.txt"
if [[ "$STATUS" != 0 || "$TIMEOUT" == 1 || "$SUMMARY_STATUS" == 1 ]]; then
    echo "Observer execution integrity failed; game_exit=$STATUS timeout=$TIMEOUT summary_exit=$SUMMARY_STATUS" >&2
    exit 1
fi
if [[ "$SUMMARY_STATUS" == 3 ]]; then
    echo 'Observer recorded a CPU-behind / GPU-nonzero false-hidden finding; inspect the preserved summary.' >&2
    exit 3
fi
if [[ "$SUMMARY_STATUS" == 2 ]]; then
    echo 'Observer execution completed, but catalog/selector/GPU correlation is inconclusive; inspect the preserved summary.' >&2
    exit 2
fi
if [[ "$SUMMARY_STATUS" != 0 ]]; then
    echo "Observer parser returned unexpected status $SUMMARY_STATUS; inspect preserved summary." >&2
    exit 1
fi
echo "Hybrid active-occlusion observation run completed; output: $OUT"
