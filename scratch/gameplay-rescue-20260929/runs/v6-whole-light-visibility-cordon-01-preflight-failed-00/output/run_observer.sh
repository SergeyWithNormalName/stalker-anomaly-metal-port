#!/bin/bash
set -euo pipefail

# Prepared runner only. It deliberately refuses to launch without a reviewed V6
# hash and an explicit root authorization environment token.
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
STAGE="$ROOT/scratch/gameplay-rescue-20260929/whole-light-visibility-stage/observer-harness"
RUN_ID="${RESCUE_RUN_ID:-v6-whole-light-visibility-cordon-01}"
RUN_ROOT="$ROOT/scratch/gameplay-rescue-20260929/runs/$RUN_ID"
BASE_RUNTIME="$ROOT/scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime"
SAVE_NAME="rescue-cordon-control"
APP="$ROOT/scratch/gameplay-rescue-20260929/candidate-v6/Anomaly.app"
GAME_EXE="$APP/Contents/MacOS/AnomalyDX11"
MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v6/source-manifest.json"
ORIGINAL_EXE="$ROOT/build/Binaries/RelWithDebInfo/Anomaly.app/Contents/MacOS/AnomalyDX11"
V4_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v4/Anomaly.app/Contents/MacOS/AnomalyDX11"
V5_EXE="$ROOT/scratch/gameplay-rescue-20260929/candidate-v5/Anomaly.app/Contents/MacOS/AnomalyDX11"
SHADER_ROOT="$ROOT/build/metal-runtime-shaders"
SHADOW_DIR="$SHADER_ROOT/shadow-casters-raster-depth-v1"
FLORA_DIR="$SHADER_ROOT/flora-shadow-instanced-e08"
OUT="$RUN_ROOT/output"
RT="$RUN_ROOT/runtime"
MAX_OWNED_SECONDS=180

EXPECTED_ORIGINAL_SHA256="b0bcf586c65fdb8bad8034202b3df96f0786d7fefaf4f8d9e1b411c29fc323cc"
EXPECTED_V4_SHA256="2bb3dc6b4b35a805a991792864c98f40fb7df128f8431da306d731a88d517f71"
EXPECTED_V5_SHA256="5fd0bde4a9d1662fd51ec332e90957caa7984726d20415966de5679412afc44f"
EXPECTED_BASE_USER_SHA256="b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334"
EXPECTED_BASE_SAVE_SHA256="a21f09761ca6e7c3633e0b002b34d760ea30ece41f6542a1814eabba5a0ac3c4"

if [[ "${RESCUE_ROOT_APPROVED:-}" != "1" ]]; then
    echo 'Refusing launch: root has not authorized this numeric observer run.' >&2
    exit 2
fi
if [[ -z "${RESCUE_EXPECTED_APP_SHA256:-}" ]]; then
    echo 'Set RESCUE_EXPECTED_APP_SHA256 from the reviewed candidate-v6 manifest.' >&2
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
for required in "$GAME_EXE" "$MANIFEST" "$ORIGINAL_EXE" "$V4_EXE" "$V5_EXE" \
    "$BASE_RUNTIME/appdata/user.ltx" "$BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json"; do
    [[ -e "$required" ]] || { echo "Required file missing: $required" >&2; exit 1; }
done

sha256_file() {
    local digest rest
    read -r digest rest < <(/usr/bin/shasum -a 256 "$1")
    printf '%s' "$digest"
}
ACTUAL_GAME_SHA256="$(sha256_file "$GAME_EXE")"
[[ "$ACTUAL_GAME_SHA256" == "$RESCUE_EXPECTED_APP_SHA256" ]] || {
    echo "V6 executable hash mismatch: $ACTUAL_GAME_SHA256" >&2; exit 1;
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
[[ "$(sha256_file "$BASE_RUNTIME/appdata/user.ltx")" == "$EXPECTED_BASE_USER_SHA256" ]] || {
    echo 'Canonical base user.ltx hash changed; refusing to clone.' >&2; exit 1;
}
[[ "$(sha256_file "$BASE_RUNTIME/appdata/savedgames/$SAVE_NAME.scop")" == "$EXPECTED_BASE_SAVE_SHA256" ]] || {
    echo 'Canonical base save hash changed; refusing to clone.' >&2; exit 1;
}
V5_MANIFEST="$ROOT/scratch/gameplay-rescue-20260929/candidate-v5/source-manifest.json"
/usr/bin/python3 - "$ROOT" "$MANIFEST" "$V5_MANIFEST" "$ACTUAL_GAME_SHA256" <<'PY'
import hashlib, json, pathlib, sys
root = pathlib.Path(sys.argv[1])
path = pathlib.Path(sys.argv[2])
v5_path = pathlib.Path(sys.argv[3])
actual = sys.argv[4]
with open(path, 'r', encoding='utf-8') as f:
    manifest = json.load(f)
if manifest.get('candidate') != 'V6':
    raise SystemExit('source manifest is not candidate V6')
if manifest.get('candidate_executable_sha256') != actual:
    raise SystemExit('manifest executable hash does not match candidate V6')
if manifest.get('validation', {}).get('build_exit') != 0:
    raise SystemExit('candidate V6 source manifest does not record a successful build')
source_hashes = manifest.get('source_hashes')
if not isinstance(source_hashes, dict) or not source_hashes:
    raise SystemExit('candidate V6 manifest has no source_hashes map')
required_v5 = json.loads(v5_path.read_text(encoding='utf-8')).get('source_hashes', {})
required_observer = {
    'src/Layers/xrRenderMetal/IR4Backend.h',
    'src/Layers/xrRenderMetal/MetalR4Backend.mm',
    'src/Layers/xrRenderMetal/CMakeLists.txt',
    'src/Layers/xrRenderMetal/MetalR4EngineScene.mm',
    'src/Layers/xrRenderMetal/MetalR4LocalLightAdapter.cpp',
    'src/Layers/xrRenderMetal/MetalR4LocalLightAdapter.h',
    'src/Layers/xrRenderMetal/R4WholeLightVisibilityLedger.h',
    'src/Layers/xrRenderMetal/R4WholeLightVisibilitySelection.h',
    'src/Layers/xrRenderMetal/R4WholeLightVisibilitySelection.cpp',
    'src/Layers/xrRenderMetal/tests/R4WholeLightVisibilityLedgerProbe.cpp',
    'src/Layers/xrRenderMetal/tests/R4WholeLightVisibilitySelectionProbe.cpp',
    'src/Layers/xrRenderMetal/tests/R4WholeLightVisibilityBackendGpuProbe.cpp',
}
missing = sorted((set(required_v5) | required_observer) - set(source_hashes))
if missing:
    raise SystemExit('candidate V6 source manifest omits required V5/observer files: ' + ', '.join(missing))
for rel, expected in source_hashes.items():
    source = root / rel
    if not source.is_file():
        raise SystemExit('candidate V6 source manifest path is missing: ' + rel)
    got = hashlib.sha256(source.read_bytes()).hexdigest()
    if got != expected:
        raise SystemExit(f'candidate V6 source hash mismatch: {rel}: {got} != {expected}')
PY

mkdir -p "$OUT"
ditto "$BASE_RUNTIME" "$RT"
cp "$STAGE/observer_window.lua" "$OUT/observer_window.lua"
cp "$STAGE/summarize_observation.py" "$OUT/summarize_observation.py"
cp "$STAGE/run_observer.sh" "$OUT/run_observer.sh"
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
    XRAY_METAL_COMPUTE_SKIN_ORACLE XRAY_METAL_WHOLE_LIGHT_VISIBILITY \
    XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME XRAY_WHOLE_LIGHT_VISIBILITY_HARNESS_LOG \
    XRAY_PARITY_HARNESS XRAY_PARITY_HARNESS_SCRIPT PERF_GAP_PHASES RESCUE_WARMUP_MS \
    RESCUE_MEASURE_MS RESCUE_SAVE_CONTROL DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES

export XRAY_METAL_MSC_FLORA_SHADOW_DIR="$SHADOW_DIR"
export XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR="$FLORA_DIR"
export XRAY_METAL_DYNAMIC_SHADOW_INDEX=1
export XRAY_METAL_COMPUTE_SKIN=1
export XRAY_METAL_WHOLE_LIGHT_VISIBILITY=1
export XRAY_METAL_WHOLE_LIGHT_VISIBILITY_START_FRAME=300
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
    "$GAME_EXE" "$MANIFEST" "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" \
    "$STAGE/run_observer.sh" "$STAGE/observer_window.lua" "$STAGE/summarize_observation.py" \
    "$SHADOW_DIR/shadow-casters-msc-receipt.txt" \
    "$SHADOW_DIR/shadow_direct_tree.vs.metallib" "$SHADOW_DIR/shadow_direct_tree_aref.vs.metallib" \
    "$SHADOW_DIR/shadow_direct_base_color.ps.metallib" "$SHADOW_DIR/shadow_direct_base_aref_color.ps.metallib" \
    "$FLORA_DIR/flora-instanced-shadow-receipt.json" \
    "$FLORA_DIR/shadow_direct_tree_instanced.vs.metallib" \
    "$FLORA_DIR/shadow_direct_tree_instanced_aref.vs.metallib" \
    > "$OUT/input-sha256.txt"
printf '%s\n' "$(sha256_file "$ORIGINAL_EXE")  $ORIGINAL_EXE" "$(sha256_file "$V4_EXE")  $V4_EXE" "$(sha256_file "$V5_EXE")  $V5_EXE" > "$OUT/preserved-app-hashes-before.txt"
stat -f '%N bytes=%z mtime=%m' "$RT/appdata/user.ltx" "$RT/appdata/savedgames/$SAVE_NAME.scop" > "$OUT/input-metadata.txt"
/usr/bin/date -u '+%Y-%m-%dT%H:%M:%SZ' > "$OUT/launch-time.txt"
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
printf '%s\n' "$(sha256_file "$ORIGINAL_EXE")  $ORIGINAL_EXE" "$(sha256_file "$V4_EXE")  $V4_EXE" "$(sha256_file "$V5_EXE")  $V5_EXE" > "$OUT/preserved-app-hashes-after.txt"
if ! /usr/bin/cmp -s "$OUT/preserved-app-hashes-before.txt" "$OUT/preserved-app-hashes-after.txt"; then
    printf '%s\n' 'preserved app candidate hash changed during observer run' > "$OUT/preserved-app-hash-mismatch.txt"
    STATUS=1
fi
printf '%s\n' "$STATUS" > "$OUT/exit-status.txt"
printf '%s\n' "$TIMEOUT" > "$OUT/timeout-flag.txt"
trap - EXIT INT TERM
set +e
LC_ALL=C /usr/bin/python3 "$STAGE/summarize_observation.py" "$OUT" > "$OUT/summary.stdout.txt" 2> "$OUT/summary.stderr.txt"
SUMMARY_STATUS=$?
set -e
printf '%s\n' "$SUMMARY_STATUS" > "$OUT/summary-exit-status.txt"
if [[ "$STATUS" != 0 || "$TIMEOUT" == 1 || "$SUMMARY_STATUS" != 0 ]]; then
    echo "Observer execution integrity failed; game_exit=$STATUS timeout=$TIMEOUT summary_exit=$SUMMARY_STATUS" >&2
    exit 1
fi
echo "Whole-light observation run completed; output: $OUT"
