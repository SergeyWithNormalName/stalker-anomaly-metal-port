#!/usr/bin/env bash
# E00 fixture runner.  It is intentionally opt-in and never terminates another game.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME="$ROOT/runtime/macos-gamma"
BINARY="$ROOT/build/Binaries/RelWithDebInfo/AnomalyDX11"
FIXTURE=""
BACKEND="metal"
RUN_ID=""
EXECUTE=0
TIMING=0
TIMING_SECONDS=120
TIMING_WARMUP=60
TIMING_TELEMETRY=1
METAL_VALIDATION=0
GPU_CAPTURE=0
GPU_CAPTURE_TIMEOUT_SECONDS=120
GPU_CAPTURE_TIMEOUT_SET=0
FIXTURE_WAIT_TIMEOUT_SECONDS="${XRAY_PARITY_TIMEOUT_SECONDS:-360}"
FIXTURE_REGISTRY_OVERRIDE=""
SCENARIO_SCRIPT_OVERRIDE=""
FIXTURE_OVERLAY_OVERRIDE=""
E07_NPC_COUNT=""
E07_NPC_COUNT_SET=0
E07_TELEMETRY=1

usage() {
    echo "Usage: $0 [--preflight|--execute] --fixture S0 --backend dxvk|metal [--run-id ID] [--fixture-registry PATH] [--gpu-capture] [--gpu-capture-timeout SECONDS]"
    echo "--preflight records provenance only; --execute creates an isolated runtime and starts one owned process."
    echo "--timing --duration 120 --warmup 60 runs E01 without screenshot/video capture."
    echo "--metal-validation enables API and shader validation for a non-timing Direct Metal fixture."
    echo "XRAY_PARITY_TIMEOUT_SECONDS bounds fixture completion (default 360, maximum 1800)."
    echo "--fixture-registry PATH selects a validated fixture JSON registry."
    echo "--scenario-script PATH selects a copied diagnostic scenario; never E00-admitted."
    echo "--fixture-overlay DIR adds new fixture assets/configs to the isolated runtime; never overwrites profile files."
    echo "--gpu-capture records exactly one Direct Metal frame at READY_FOR_NO_HUD."
    echo "--gpu-capture-timeout SECONDS bounds the capture wait (default 120, maximum 600)."
    echo "--telemetry-off disables CSV instrumentation for E01 overhead comparison."
    echo "--e07-npc-count 0|1|4|8 runs an E07 fixed-count S2 diagnostic timing fixture."
    echo "--no-e07-telemetry keeps that scenario and E01 timings, without per-object profiling CSVs (E08)."
}
while [[ $# -gt 0 ]]; do
    case "$1" in
        --preflight) EXECUTE=0 ;;
        --execute) EXECUTE=1 ;;
        --timing) TIMING=1 ; EXECUTE=1 ;;
        --duration) TIMING_SECONDS="$2"; shift ;;
        --warmup) TIMING_WARMUP="$2"; shift ;;
        --telemetry-off) TIMING_TELEMETRY=0 ;;
        --metal-validation) METAL_VALIDATION=1 ;;
        --gpu-capture) GPU_CAPTURE=1 ;;
        --gpu-capture-timeout) GPU_CAPTURE_TIMEOUT_SECONDS="$2"; GPU_CAPTURE_TIMEOUT_SET=1; shift ;;
        --fixture-registry) FIXTURE_REGISTRY_OVERRIDE="$2"; shift ;;
        --scenario-script) SCENARIO_SCRIPT_OVERRIDE="$2"; shift ;;
        --fixture-overlay) FIXTURE_OVERLAY_OVERRIDE="$2"; shift ;;
        --no-e07-telemetry) E07_TELEMETRY=0 ;;
        --e07-npc-count) E07_NPC_COUNT="$2"; E07_NPC_COUNT_SET=1; shift ;;
        --fixture) FIXTURE="$2"; shift ;;
        --backend) BACKEND="$2"; shift ;;
        --run-id) RUN_ID="$2"; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
case "$FIXTURE" in S0|S1|S2|S3|S4|S5|S6|S7) ;; *) echo "fixture is required and must be S0 through S7" >&2; usage >&2; exit 2;; esac
case "$BACKEND" in dxvk|metal) ;; *) echo "unknown backend $BACKEND" >&2; exit 2;; esac
[[ "$FIXTURE_WAIT_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ && "$FIXTURE_WAIT_TIMEOUT_SECONDS" -le 1800 ]] || {
    echo "invalid fixture timeout (expected 1..1800 seconds)" >&2
    exit 2
}
if [[ "$TIMING" -eq 1 ]]; then
    [[ "$BACKEND" == metal ]] || { echo "E01 timing requires Direct Metal" >&2; exit 2; }
    [[ "$TIMING_SECONDS" =~ ^[1-9][0-9]*$ && "$TIMING_WARMUP" =~ ^[1-9][0-9]*$ ]] || { echo "invalid E01 timing duration" >&2; exit 2; }
fi
if [[ "$E07_NPC_COUNT_SET" -eq 1 ]]; then
    [[ "$E07_NPC_COUNT" == "0" || "$E07_NPC_COUNT" == "1" || "$E07_NPC_COUNT" == "4" || "$E07_NPC_COUNT" == "8" ]] || {
        echo "--e07-npc-count must be one of 0, 1, 4, or 8" >&2
        exit 2
    }
    [[ "$TIMING" -eq 1 && "$BACKEND" == metal && "$FIXTURE" == S2 ]] || {
        echo "--e07-npc-count requires --timing --backend metal --fixture S2" >&2
        exit 2
    }
    [[ "$TIMING_TELEMETRY" -eq 1 ]] || { echo "E07 timing requires frame telemetry; --telemetry-off is unsupported" >&2; exit 2; }
    [[ -z "$FIXTURE_REGISTRY_OVERRIDE" && -z "$SCENARIO_SCRIPT_OVERRIDE" && -z "$FIXTURE_OVERLAY_OVERRIDE" ]] || {
        echo "--e07-npc-count requires the registered S2 fixture without overrides" >&2
        exit 2
    }
fi
if [[ "$E07_TELEMETRY" -eq 0 && ( "$E07_NPC_COUNT_SET" -ne 1 || "$TIMING" -ne 1 ) ]]; then echo "--no-e07-telemetry requires --timing --e07-npc-count" >&2; exit 2; fi
if [[ "$TIMING_TELEMETRY" -eq 0 && "$TIMING" -ne 1 ]]; then echo "--telemetry-off requires --timing" >&2; exit 2; fi
if [[ "$METAL_VALIDATION" -eq 1 && ( "$TIMING" -eq 1 || "$BACKEND" != metal ) ]]; then
    echo "--metal-validation requires non-timing Direct Metal" >&2
    exit 2
fi
if [[ "$GPU_CAPTURE" -eq 1 && ( "$TIMING" -eq 1 || "$BACKEND" != metal || "$EXECUTE" -ne 1 ) ]]; then
    echo "--gpu-capture requires --execute, non-timing Direct Metal" >&2
    exit 2
fi
if [[ "$GPU_CAPTURE_TIMEOUT_SET" -eq 1 ]]; then
    [[ "$GPU_CAPTURE" -eq 1 ]] || { echo "--gpu-capture-timeout requires --gpu-capture" >&2; exit 2; }
fi
if [[ "$GPU_CAPTURE" -eq 1 ]]; then
    [[ "$GPU_CAPTURE_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ && "$GPU_CAPTURE_TIMEOUT_SECONDS" -le 600 ]] || {
        echo "invalid GPU capture timeout (expected 1..600 seconds)" >&2
        exit 2
    }
fi
# Metal shader validation cannot be captured by MTLCaptureManager. Keep the
# validation and resource-inspection runs separate instead of aborting in-game.
if [[ "$GPU_CAPTURE" -eq 1 && "$METAL_VALIDATION" -eq 1 ]]; then
    echo "--gpu-capture cannot be combined with --metal-validation; run them separately" >&2
    exit 2
fi
if [[ -n "$SCENARIO_SCRIPT_OVERRIDE" ]]; then
    [[ "$TIMING" -eq 0 ]] || { echo "--scenario-script requires a non-timing diagnostic run" >&2; exit 2; }
    test -s "$SCENARIO_SCRIPT_OVERRIDE" || { echo "diagnostic scenario missing or empty" >&2; exit 2; }
fi
if [[ -n "$FIXTURE_OVERLAY_OVERRIDE" ]]; then
    test -d "$FIXTURE_OVERLAY_OVERRIDE/gamedata" || { echo "fixture overlay gamedata directory missing" >&2; exit 2; }
    export XRAY_PARITY_FIXTURE_OVERLAY="$(cd "$FIXTURE_OVERLAY_OVERRIDE" && pwd -P)"
fi
if [[ -z "$RUN_ID" ]]; then RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-${BACKEND}-${FIXTURE}"; fi
case "$RUN_ID" in ""|*/*|*\\*) echo "run id must be a single directory name" >&2; exit 2;; esac

FIXTURE_REGISTRY="${FIXTURE_REGISTRY_OVERRIDE:-$ROOT/docs/direct-metal-release/FIXTURES.json}"
if [[ "$FIXTURE_REGISTRY" != /* ]]; then
    FIXTURE_REGISTRY="$(cd "$(dirname "$FIXTURE_REGISTRY")" && pwd)/$(basename "$FIXTURE_REGISTRY")"
fi
test -s "$FIXTURE_REGISTRY" || { echo "fixture registry missing or empty: $FIXTURE_REGISTRY" >&2; exit 2; }
FIXTURE_HELPER="$ROOT/script/e00_fixture_contract.py"
fixture_field() {
    python3 "$FIXTURE_HELPER" --registry "$FIXTURE_REGISTRY" --fixture "$FIXTURE" --field "$1"
}
FIXTURE_STATUS="$(fixture_field status)"
if [[ "$FIXTURE_STATUS" != "ready" ]]; then
    echo "fixture $FIXTURE is not prepared (status=$FIXTURE_STATUS); refusing to substitute S0 defaults" >&2
    exit 2
fi
FIXTURE_LEVEL="$(fixture_field level)"
FIXTURE_CAMERA_POS="$(fixture_field camera)"
FIXTURE_CAMERA_DIR="$(fixture_field direction)"
FIXTURE_WEATHER="$(fixture_field weather)"
FIXTURE_WEATHER_WEIGHT="$(fixture_field weather_weight)"
FIXTURE_TIME="$(fixture_field time)"
SAVE_NAME="$(fixture_field save_name)"
EXPECTED_SAVE_SHA256="$(fixture_field save_sha256)"
SCOPE_CACHE_REQUIRED="$(fixture_field scope_cache_required)"
WARMUP_SECONDS="$(fixture_field warmup_seconds)"
if [[ "$TIMING" -eq 1 ]]; then WARMUP_SECONDS="$TIMING_WARMUP"; fi
FIXTURE_SCENARIO="$(fixture_field scenario)"
MOTION_SECONDS="$(fixture_field capture_contract.motion_duration_seconds)"
NO_HUD_SETTLE_SECONDS="$(fixture_field capture_contract.no_hud_settle_seconds)"
CAMERA_POSITION_TOLERANCE="$(fixture_field capture_contract.camera_position_tolerance_m)"
CAMERA_ANGLE_TOLERANCE="$(fixture_field capture_contract.camera_angle_tolerance_rad)"
MATRIX_TOLERANCE="$(fixture_field capture_contract.matrix_tolerance)"
TIMELINE_UNIT="$(fixture_field capture_contract.timeline_unit)"
FIXTURE_REGISTRY_SHA256="$(python3 "$FIXTURE_HELPER" --registry "$FIXTURE_REGISTRY" --registry-sha256)"
SAVE_SOURCE="${XRAY_PARITY_SAVE_SOURCE_DIR:-$RUNTIME/appdata/savedgames}"
SAVE_PATH="$SAVE_SOURCE/$SAVE_NAME.scop"
test -f "$SAVE_PATH" || { echo "fixture save missing: $SAVE_PATH" >&2; exit 1; }
ACTUAL_SAVE_SHA256="$(shasum -a 256 "$SAVE_PATH" | awk '{print $1}')"
if [[ "$ACTUAL_SAVE_SHA256" != "$EXPECTED_SAVE_SHA256" ]]; then
    echo "fixture $FIXTURE save hash mismatch: expected $EXPECTED_SAVE_SHA256, got $ACTUAL_SAVE_SHA256" >&2
    exit 1
fi
# These diagnostic-only harness switches change the fixture and have no
# registry contract, so an inherited terminal environment may not enable them.
unset XRAY_PARITY_PLAY_ANM XRAY_PARITY_DUMP_GAME_GRAPH XRAY_PARITY_BOOTSTRAP_LEVEL

RUN_DIR="$ROOT/scratch/release_runs/$RUN_ID"
IPC_DIR="$RUN_DIR/ipc"
# Refuse an existing directory before creating any child: an old evidence
# package must never be mixed with a later invocation.
if [[ -e "$RUN_DIR" ]]; then echo "run directory already exists: $RUN_DIR" >&2; exit 2; fi
mkdir -p "$IPC_DIR"
FIXTURE_REGISTRY_COPY="$RUN_DIR/fixture_registry.json"
cp "$FIXTURE_REGISTRY" "$FIXTURE_REGISTRY_COPY"
FIXTURE_REGISTRY_COPY_SHA256="$(shasum -a 256 "$FIXTURE_REGISTRY_COPY" | awk '{print $1}')"
test "$FIXTURE_REGISTRY_COPY_SHA256" = "$FIXTURE_REGISTRY_SHA256" || { echo "copied fixture registry hash mismatch" >&2; exit 1; }

# These files preserve the starting WIP; no reset, checkout, or staging is used.
git -C "$ROOT" diff --binary > "$RUN_DIR/dirty.patch"
git -C "$ROOT" ls-files --others --exclude-standard \
    --exclude='scratch/release_runs/**' \
    --exclude='scratch/release_manifests/**' \
    --exclude='tools/e00_dxbc/out*/**' \
    --exclude='scratch/e00-save-thumbnails/**' | sort > "$RUN_DIR/untracked.txt"
export XRAY_PROJECT_ROOT="$ROOT"
if [[ "$BACKEND" == metal ]]; then source "$ROOT/script/xray_metal_shader_env.sh"; fi
export XRAY_PARITY_MATERIAL_SET_SHA256="$(shasum -a 256 "$RUNTIME/profile/profile-manifest.json" | awk '{print $1}')"
# Capture provenance sees the complete contract used by the child process,
# including the registry hash and verified source save.  Do not add an
# XRAY_PARITY_* export below this point.
export XRAY_PARITY_HARNESS=1 XRAY_PARITY_HARNESS_SCRIPT="$ROOT/scratch/zzzz_visual_parity_harness.script"
export XRAY_PARITY_SCENARIO_SCRIPT="$ROOT/scratch/e00_fixture_scenarios.script" XRAY_PARITY_SCENARIO="$FIXTURE_SCENARIO"
if [[ -n "$SCENARIO_SCRIPT_OVERRIDE" ]]; then
    cp "$SCENARIO_SCRIPT_OVERRIDE" "$RUN_DIR/diagnostic_scenario.script"
    export XRAY_PARITY_SCENARIO_SCRIPT="$RUN_DIR/diagnostic_scenario.script"
    export XRAY_PARITY_DIAGNOSTIC_SCENARIO_SHA256="$(shasum -a 256 "$XRAY_PARITY_SCENARIO_SCRIPT" | awk '{print $1}')"
else
    unset XRAY_PARITY_DIAGNOSTIC_SCENARIO_SHA256
fi
export XRAY_PARITY_FLAG_PATH="$IPC_DIR/stage.txt" XRAY_PARITY_TELEMETRY_PATH="$IPC_DIR/telemetry.json" XRAY_PARITY_CENSUS_PATH="$IPC_DIR/census.txt" XRAY_PARITY_BOOTSTRAP_MARKER="$IPC_DIR/bootstrap.txt"
unset XRAY_METAL_E07_NPC_COUNT XRAY_METAL_E07_DIAGNOSTIC
if [[ "$E07_NPC_COUNT_SET" -eq 1 ]]; then
    # Execute the bytes captured for this run even if the source is later edited.
    cp "$XRAY_PARITY_SCENARIO_SCRIPT" "$RUN_DIR/e07-scenario.script"
    export XRAY_PARITY_SCENARIO_SCRIPT="$RUN_DIR/e07-scenario.script"
    export XRAY_PARITY_E07_SCENARIO_SHA256="$(shasum -a 256 "$XRAY_PARITY_SCENARIO_SCRIPT" | awk '{print $1}')"
    export XRAY_METAL_E07_NPC_COUNT="$E07_NPC_COUNT" XRAY_METAL_E07_DIAGNOSTIC=1
    python3 - "$RUN_DIR/e07-diagnostic.json" "$E07_NPC_COUNT" <<'E07_JSON'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
path.write_text(json.dumps({
    "stage": "E07",
    "admission": "DIAGNOSTIC_ONLY_NOT_E00_ADMITTED",
    "requested_spawn_count": int(sys.argv[2]),
    "count_semantics": "requested fixture spawns; not verified visible count",
    "device_clock": "unfrozen for NPC simulation; weather/game clock remains fixture-paused; original Device factor restored after cleanup",
}, indent=2) + "\n")
E07_JSON
fi
export XRAY_PARITY_GIT_HEAD="$(git -C "$ROOT" rev-parse HEAD)" XRAY_PARITY_FIXTURE="$FIXTURE" XRAY_PARITY_FIXTURE_REGISTRY_SHA256="$FIXTURE_REGISTRY_SHA256" XRAY_PARITY_FIXTURE_REGISTRY_PATH="$FIXTURE_REGISTRY"
export XRAY_PARITY_SAVE_SOURCE="$SAVE_SOURCE" XRAY_PARITY_SAVE="$SAVE_NAME" XRAY_PARITY_SAVE_SHA256="$ACTUAL_SAVE_SHA256"
export XRAY_PARITY_EXPECTED_LEVEL="$FIXTURE_LEVEL" XRAY_PARITY_CAMERA_POS="$FIXTURE_CAMERA_POS" XRAY_PARITY_CAMERA_DIR="$FIXTURE_CAMERA_DIR" XRAY_PARITY_WEATHER="$FIXTURE_WEATHER" XRAY_PARITY_WEATHER_WEIGHT="$FIXTURE_WEATHER_WEIGHT" XRAY_PARITY_TIME="$FIXTURE_TIME"
export XRAY_PARITY_SCOPE_CACHE_REQUIRED="$SCOPE_CACHE_REQUIRED" XRAY_PARITY_WARMUP_SECONDS="$WARMUP_SECONDS" XRAY_PARITY_MOTION_SECONDS="$MOTION_SECONDS" XRAY_PARITY_CAPTURE_ACK_TIMEOUT_SECONDS=$((MOTION_SECONDS + 15))
export XRAY_PARITY_NO_HUD_SETTLE_SECONDS="$NO_HUD_SETTLE_SECONDS"
export XRAY_PARITY_CAMERA_POSITION_TOLERANCE_M="$CAMERA_POSITION_TOLERANCE" XRAY_PARITY_CAMERA_ANGLE_TOLERANCE_RAD="$CAMERA_ANGLE_TOLERANCE" XRAY_PARITY_MATRIX_TOLERANCE="$MATRIX_TOLERANCE" XRAY_PARITY_TIMELINE_UNIT="$TIMELINE_UNIT"
if [[ "$GPU_CAPTURE" -eq 1 ]]; then
    export XRAY_METAL_E03_TRACE_PATH="$RUN_DIR/e03-s0.gputrace" XRAY_METAL_E03_CAPTURE_FLAG_PATH="$XRAY_PARITY_FLAG_PATH"
else
    unset XRAY_METAL_E03_TRACE_PATH XRAY_METAL_E03_CAPTURE_FLAG_PATH
fi
export XRAY_PARITY_FIXTURE_REGISTRY_PATH="$FIXTURE_REGISTRY" XRAY_PARITY_FIXTURE_REGISTRY_RUN_COPY="$FIXTURE_REGISTRY_COPY"
unset XRAY_METAL_E01_SECONDS XRAY_METAL_E01_FRAMES XRAY_METAL_E01_WINDOW_PATH XRAY_METAL_E07_FRAMES
if [[ "$TIMING" -eq 1 ]]; then
    export XRAY_METAL_E01_SECONDS="$TIMING_SECONDS" XRAY_METAL_E01_WINDOW_PATH="$RUN_DIR/timing-window.json"
    if [[ "$TIMING_TELEMETRY" -eq 1 ]]; then export XRAY_METAL_E01_FRAMES="$RUN_DIR/frames.csv"; else unset XRAY_METAL_E01_FRAMES; fi
    # Performance runs never inherit instrumentation or validation switches.
    unset MTL_CAPTURE_ENABLED MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR MTL_ENABLE_DEBUG_INFO
fi
if [[ "$E07_NPC_COUNT_SET" -eq 1 && "$E07_TELEMETRY" -eq 1 ]]; then
    export XRAY_METAL_E07_FRAMES="$RUN_DIR/e07-frames.csv"
fi
# Both backends render a physical 1920x1080 target from the same 960x540
# logical macOS window.  Record this and every backend-specific diagnostic
# setting in provenance before a preflight can return.
export XRAY_BENCHMARK_LOGICAL_SIZE="${XRAY_BENCHMARK_LOGICAL_SIZE:-960x540}"
if [[ "$BACKEND" == metal ]]; then
    unset DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES MTL_CAPTURE_ENABLED MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR
    export XRAY_METAL_NATIVE_RETINA=1
    if [[ "$GPU_CAPTURE" -eq 1 ]]; then export MTL_CAPTURE_ENABLED=1; fi
    if [[ "$METAL_VALIDATION" -eq 1 ]]; then
        export MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1
    fi
else
    unset MTL_CAPTURE_ENABLED MTL_DEBUG_LAYER MTL_SHADER_VALIDATION MTL_SHADER_VALIDATION_REPORT_TO_STDERR DYLD_LIBRARY_PATH DXVK_WSI_DRIVER XRAY_METAL_NATIVE_RETINA
    export VK_ICD_FILENAMES="$RUNTIME/moltenvk/lib/MoltenVK_icd.json" MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1
fi
write_provenance() {
    local user_ltx="$1"
    python3 "$ROOT/script/capture_metal_release_provenance.py" --root "$ROOT" --run-dir "$RUN_DIR" --backend "$BACKEND" --fixture "$FIXTURE" --binary "$BINARY" --profile-manifest "$RUNTIME/profile/profile-manifest.json" --user-ltx "$user_ltx"
    if [[ -n "$FIXTURE_REGISTRY_OVERRIDE" || -n "$SCENARIO_SCRIPT_OVERRIDE" || -n "$FIXTURE_OVERLAY_OVERRIDE" || "$GPU_CAPTURE" -eq 1 ]]; then
        python3 - "$RUN_DIR/provenance.json" "$FIXTURE_REGISTRY" "$FIXTURE_REGISTRY_COPY" "$FIXTURE_REGISTRY_SHA256" <<'PY'
import json, pathlib, sys
manifest_path, source, copy_path, sha256 = sys.argv[1:]
path = pathlib.Path(manifest_path)
data = json.loads(path.read_text())
data["fixture_registry_selection"] = {
    "source_path": str(pathlib.Path(source).resolve()),
    "run_copy_path": str(pathlib.Path(copy_path).resolve()),
    "sha256": sha256,
    "admission": "DIAGNOSTIC_ONLY_NOT_E00_ADMITTED",
}
path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
PY
    fi
}
if [[ "$EXECUTE" -eq 0 ]]; then
    write_provenance "$RUNTIME/appdata/user.ltx"
    if [[ -n "$FIXTURE_REGISTRY_OVERRIDE" || -n "$SCENARIO_SCRIPT_OVERRIDE" || -n "$FIXTURE_OVERLAY_OVERRIDE" || "$GPU_CAPTURE" -eq 1 ]]; then
        echo "E03 diagnostic preflight (not E00-admitted): $RUN_DIR/provenance.json"
    else
        echo "E00 preflight: $RUN_DIR/provenance.json"
    fi
    exit 0
fi

# Do not invoke either normal launcher: both intentionally stop same-tree processes.
if pgrep -f "$BINARY" >/dev/null 2>&1; then
    echo "refusing fixture launch: an AnomalyDX11 from this checkout is already running" >&2
    exit 1
fi
WINDOW_HELPER="$ROOT/scratch/find_window"
if [[ ! -x "$WINDOW_HELPER" ]]; then
    WINDOW_HELPER="$RUN_DIR/find_window"
    swiftc -O "$ROOT/scratch/find_window.swift" -o "$WINDOW_HELPER"
fi

TEST_RUNTIME="$RUN_DIR/runtime"
mkdir -p "$TEST_RUNTIME/appdata" "$TEST_RUNTIME/appdata/savedgames" "$TEST_RUNTIME/appdata/logs" "$TEST_RUNTIME/appdata/screenshots"
cp "$RUNTIME/fsgame.ltx" "$TEST_RUNTIME/fsgame.ltx"
# The fixture gets its own settings/saves/cache.  APFS clone-copy avoids a
# bulk copy, while -P keeps foreign MO2 links as links until a file is
# explicitly materialized below for a write.
cp "$RUNTIME/appdata/user.ltx" "$TEST_RUNTIME/appdata/user.ltx"
# Automated evidence must enter the world without an unobservable keyboard
# acknowledgement.  This is an isolated copy; the player's user.ltx is never
# changed.  Provenance below hashes this effective fixture configuration.
perl -pi -e 's/^keypress_on_start\s+on\r?$/keypress_on_start off/; s/^discord_status\s+on\r?$/discord_status off/' "$TEST_RUNTIME/appdata/user.ltx"
if [[ "$TIMING" -eq 1 ]]; then perl -pi -e 's/^rs_stats\s+on\r?$/rs_stats off/' "$TEST_RUNTIME/appdata/user.ltx"; fi
grep -q '^keypress_on_start off' "$TEST_RUNTIME/appdata/user.ltx" || { echo "cannot disable fixture loading-screen key gate" >&2; exit 1; }
grep -q '^discord_status off' "$TEST_RUNTIME/appdata/user.ltx" || { echo "cannot disable fixture Discord integration" >&2; exit 1; }
if [[ "$TIMING" -eq 1 ]]; then grep -q '^rs_stats off' "$TEST_RUNTIME/appdata/user.ltx" || { echo "cannot disable rs_stats for timing" >&2; exit 1; }; fi
cp -cRP "$RUNTIME/appdata/shaders_cache" "$TEST_RUNTIME/appdata/shaders_cache"
cp -cRP "$RUNTIME/profile/gamedata" "$TEST_RUNTIME/gamedata"
if [[ -n "$FIXTURE_OVERLAY_OVERRIDE" ]]; then
    export XRAY_PARITY_FIXTURE_OVERLAY="$(cd "$FIXTURE_OVERLAY_OVERRIDE" && pwd -P)"
    python3 - "$XRAY_PARITY_FIXTURE_OVERLAY" "$TEST_RUNTIME" "$RUN_DIR/fixture_overlay.json" <<'OVERLAY_PY'
import hashlib,json,pathlib,shutil,sys
source,target,manifest=map(pathlib.Path,sys.argv[1:])
if not (source/'gamedata').is_dir():raise SystemExit('fixture overlay requires a gamedata directory')
records=[]
for item in sorted((source/'gamedata').rglob('*')):
    if item.is_symlink():raise SystemExit(f'fixture overlay rejects symlinks: {item}')
    if not item.is_file():continue
    relative=item.relative_to(source);destination=target/relative
    if destination.exists() or destination.is_symlink():raise SystemExit(f'fixture overlay cannot overwrite {relative}')
    destination.parent.mkdir(parents=True,exist_ok=True)
    try:destination.resolve().relative_to(target.resolve())
    except ValueError:raise SystemExit(f'fixture overlay destination escapes isolated runtime: {relative}')
    shutil.copy2(item,destination)
    records.append({'path':str(relative),'sha256':hashlib.sha256(item.read_bytes()).hexdigest()})
manifest.write_text(json.dumps({'source':str(source),'admission':'DIAGNOSTIC_ONLY_NOT_E00_ADMITTED','files':records},indent=2)+'\n')
OVERLAY_PY
fi
ln -s "$RUNTIME/db" "$TEST_RUNTIME/db"
for ext in scop scoc dds; do
    test -e "$SAVE_SOURCE/$SAVE_NAME.$ext" && cp "$SAVE_SOURCE/$SAVE_NAME.$ext" "$TEST_RUNTIME/appdata/savedgames/"
done
write_provenance "$TEST_RUNTIME/appdata/user.ltx"
test -f "$TEST_RUNTIME/appdata/savedgames/$SAVE_NAME.scop" || { echo "fixture save missing: $SAVE_SOURCE/$SAVE_NAME.scop" >&2; exit 1; }
if [[ "$SCOPE_CACHE_REQUIRED" == "true" ]]; then
    # Fixtures that need scoped reticle shaders are marked by the registry. Native macOS cannot
    # compile this missing D3D bytecode, so fail before launch rather than
    # accepting a lucky capture that aborts on a later repetition.
    REQUIRED_SCOPE_RECORDS=(
        "model_scope_lense.ps/20481111001100000000000001001011000100100801216200621000000"
        "models_reflex_reticle_simple_3db.ps/20481111001100000000000100001011000100100801216200621000000"
        "models_laserbeam.ps/20481111001100000000000100001011000100100801216200621000000"
        "models_scope_reticle.vs/20481111001100000000000100001011000100100801216200621000000"
    )
    for record in "${REQUIRED_SCOPE_RECORDS[@]}"; do
        cache_record="$TEST_RUNTIME/appdata/shaders_cache/r4/$record"
        if [[ ! -s "$cache_record" ]] || [[ "$(dd if="$cache_record" bs=1 skip=4 count=4 2>/dev/null)" != "DXBC" ]]; then
            echo "E00 fixture blocked: missing verified DXBC cache record $record" >&2
            exit 1
        fi
    done
fi
AXR="$TEST_RUNTIME/gamedata/scripts/axr_main.script"
# Only AXR is patched.  Its parent directories must belong to this run and a
# source symlink is copied to an adjacent regular file before it is replaced.
for dir in "$TEST_RUNTIME" "$TEST_RUNTIME/gamedata" "$TEST_RUNTIME/gamedata/scripts"; do
    test -d "$dir" && [[ ! -L "$dir" ]] || { echo "unsafe fixture write path: $dir" >&2; exit 1; }
done
if [[ -L "$AXR" ]]; then
    AXR_STAGE="$TEST_RUNTIME/gamedata/scripts/.axr_main.fixture.$$"
    cp -L "$AXR" "$AXR_STAGE"
    unlink "$AXR"
    mv "$AXR_STAGE" "$AXR"
fi
[[ ! -L "$AXR" ]] || { echo "unsafe fixture script symlink: $AXR" >&2; exit 1; }
patch --batch --forward --silent "$AXR" "$ROOT/scratch/axr_main_parity_overlay.patch" || { echo "cannot apply isolated parity hook" >&2; exit 1; }
if [[ "$BACKEND" == metal ]]; then
    ARGS=(-render_backend metal -nointro -skiplogo -load "\"$SAVE_NAME\"")
else
    ARGS=(-nointro -skiplogo -load "\"$SAVE_NAME\"")
fi
cd "$TEST_RUNTIME"
"$BINARY" "${ARGS[@]}" > "$RUN_DIR/engine.log" 2>&1 &
GAME_PID=$!
MOTION_PID=""
cleanup() {
    if [[ "$GPU_CAPTURE" -eq 1 ]] && grep -q '^METAL_E03_CAPTURE_STARTED ' "$RUN_DIR/engine.log" 2>/dev/null &&
        ! grep -q '^METAL_E03_CAPTURE_STOPPED ' "$RUN_DIR/engine.log" 2>/dev/null; then
        echo "incomplete: capture was interrupted before stopCapture" > "$RUN_DIR/capture_incomplete.txt"
    fi
    if [[ -n "$MOTION_PID" ]] && kill -0 "$MOTION_PID" 2>/dev/null; then kill "$MOTION_PID" 2>/dev/null || true; fi
    if kill -0 "$GAME_PID" 2>/dev/null; then kill "$GAME_PID" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

capture_one_gpu_frame() {
    local deadline=$((SECONDS + GPU_CAPTURE_TIMEOUT_SECONDS))
    while [[ $SECONDS -lt $deadline ]]; do
        if [[ -s "$RUN_DIR/e03-s0.gputrace" ]] &&
            grep -q '^METAL_E03_CAPTURE_STOPPED ' "$RUN_DIR/engine.log" 2>/dev/null; then
            return 0
        fi
        kill -0 "$GAME_PID" 2>/dev/null || break
        sleep 0.25
    done
    echo "programmatic Metal capture did not finalize within ${GPU_CAPTURE_TIMEOUT_SECONDS}s; see METAL_E03_CAPTURE in engine.log" >&2
    return 1
}

deadline=$((SECONDS + FIXTURE_WAIT_TIMEOUT_SECONDS)); window_id=""; scene_captured=0; hud_captured=0; dims=""; drawable_source=""; motion_started=0
capture_window_image() {
    local target="$1" attempt
    for attempt in {1..12}; do
        # SDL can replace its window while a saved level is loading.  A cached
        # window number then makes screencapture fail even though the game lives.
        window_id="$("$WINDOW_HELPER" "$GAME_PID" 2>/dev/null | awk 'NR==1 {print $1}' || true)"
        if [[ -n "$window_id" ]] && screencapture -l "$window_id" -o "$target" 2>"$RUN_DIR/window_capture_error.txt" && [[ -s "$target" ]]; then
            return 0
        fi
        rm -f "$target"
        kill -0 "$GAME_PID" 2>/dev/null || break
        sleep 0.25
    done
    echo "could not capture live game window; see $RUN_DIR/window_capture_error.txt" >&2
    return 1
}
while kill -0 "$GAME_PID" 2>/dev/null && [[ $SECONDS -lt $deadline ]]; do
    stage="$(head -n 1 "$XRAY_PARITY_FLAG_PATH" 2>/dev/null || true)"
    if [[ "$TIMING" -eq 1 ]]; then
        if [[ "$stage" == TIMING_FINISHED ]]; then break; fi
        sleep 0.25
        continue
    fi
    if [[ -z "$window_id" ]]; then window_id="$("$WINDOW_HELPER" "$GAME_PID" 2>/dev/null | awk 'NR==1 {print $1}' || true)"; fi
    if [[ "$stage" == READY_FOR_NO_HUD && $scene_captured -eq 0 ]]; then
        test -n "$window_id" || { sleep 1; continue; }
        if [[ "$GPU_CAPTURE" -eq 1 ]]; then capture_one_gpu_frame; fi
        capture_window_image "$RUN_DIR/window_no_hud.png"
        actual="$(grep -E 'METAL_DRAWABLE_SIZE.*render=|Buffer size:' "$RUN_DIR/engine.log" | tail -n 1 || true)"
        dims="$(echo "$actual" | sed -nE 's/.*(render=|Buffer size:[[:space:]]*)([0-9]+)x([0-9]+).*/\2 \3/p')"
        test -n "$dims" || { echo "missing actual drawable extent" >&2; exit 1; }
        python3 "$ROOT/script/crop_framebuffer_capture.py" "$RUN_DIR/window_no_hud.png" "$RUN_DIR/scene_no_hud.png" --width ${dims% *} --height ${dims#* }
        if [[ "$BACKEND" == metal ]]; then drawable_source="METAL_DRAWABLE_SIZE.render"; else drawable_source="DXVK Presenter Buffer size"; fi
        scene_captured=1
        echo CAPTURED_NO_HUD > "$XRAY_PARITY_FLAG_PATH"
    elif [[ "$stage" == READY_FOR_WITH_HUD ]]; then
        test -n "$window_id" || { sleep 1; continue; }
        if [[ $hud_captured -eq 0 ]]; then
            capture_window_image "$RUN_DIR/window_with_hud.png"
            python3 "$ROOT/script/crop_framebuffer_capture.py" "$RUN_DIR/window_with_hud.png" "$RUN_DIR/scene_with_hud.png" --width ${dims% *} --height ${dims#* }
            hud_captured=1
        fi
        if [[ $motion_started -eq 0 ]]; then
            # Dynamic fixtures exercise weapon/UI paths too, so motion is
            # recorded only after the HUD has been restored.
            # Validation belongs to the owned game, not the recording utility.
            env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u MTL_SHADER_VALIDATION_REPORT_TO_STDERR \
                screencapture -v -V "$MOTION_SECONDS" -l "$window_id" "$RUN_DIR/motion.mov" >"$RUN_DIR/motion_capture.log" 2>&1 &
            MOTION_PID=$!
            motion_started=1
            motion_deadline=$((SECONDS + MOTION_SECONDS))
            motion_finished=0
        else
            if [[ "$motion_finished" -eq 0 ]] && ! kill -0 "$MOTION_PID" 2>/dev/null; then
                motion_status=0
                wait "$MOTION_PID" || motion_status=$?
                echo "$motion_status" > "$RUN_DIR/motion_capture_exit_status.txt"
                motion_finished=1
            fi
            # An early recorder exit must not truncate the in-engine scenario.
            if [[ "$motion_finished" -eq 1 && $SECONDS -ge $motion_deadline ]]; then
                echo CAPTURED_WITH_HUD > "$XRAY_PARITY_FLAG_PATH"
            fi
        fi
    elif [[ "$stage" == ALL_CAPTURED || "$stage" == FINISHED ]]; then break; fi
    sleep 0.25
done
game_status=0
wait "$GAME_PID" || game_status=$?
echo "$game_status" > "$RUN_DIR/engine_exit_status.txt"
trap - EXIT INT TERM
cp "$XRAY_PARITY_TELEMETRY_PATH" "$RUN_DIR/telemetry.json"
if [[ "$TIMING" -eq 1 ]]; then
    test -s "$RUN_DIR/timing-window.json" -a -s "$RUN_DIR/telemetry.json" || { echo "incomplete E01 timing evidence" >&2; exit 1; }
    test "$game_status" -eq 0 || { echo "E01 timing engine exited nonzero: $game_status" >&2; exit 1; }
    if [[ "$TIMING_TELEMETRY" -eq 1 ]]; then
        test -s "$RUN_DIR/frames.csv" -a -s "$RUN_DIR/frames.csv.presented.csv" || { echo "missing E01 frame timings" >&2; exit 1; }
        python3 "$ROOT/script/analyze_metal_e01.py" --frames "$RUN_DIR/frames.csv" --presentation "$RUN_DIR/frames.csv.presented.csv" --window "$RUN_DIR/timing-window.json" --output "$RUN_DIR"
    fi
    if [[ "$E07_NPC_COUNT_SET" -eq 1 && "$E07_TELEMETRY" -eq 1 ]]; then
        test -s "$XRAY_METAL_E07_FRAMES" || { echo "missing E07 per-object frame telemetry" >&2; exit 1; }
    fi
    echo "E01 timing run: $RUN_DIR"
    exit 0
fi
test -f "$RUN_DIR/scene_no_hud.png" -a -f "$RUN_DIR/scene_with_hud.png" -a -f "$RUN_DIR/motion.mov" -a -n "$dims" || { echo "incomplete fixture capture" >&2; exit 1; }
if [[ "$GPU_CAPTURE" -eq 1 ]]; then
    test -s "$RUN_DIR/e03-s0.gputrace" || { echo "missing nonempty E03 GPU trace" >&2; exit 1; }
fi
python3 "$ROOT/script/annotate_fixture_telemetry.py" "$RUN_DIR/telemetry.json" --drawable "${dims% *}x${dims#* }" --source "$drawable_source"
if [[ "$BACKEND" == metal ]]; then
    python3 "$ROOT/script/summarize_metal_admission.py" "$RUN_DIR/engine.log" "$RUN_DIR/shader_admission.json"
fi
test -f "$RUN_DIR/telemetry.json" || { echo "missing fixture telemetry" >&2; exit 1; }
test "$game_status" -eq 0 || { echo "fixture engine exited nonzero: $game_status" >&2; exit 1; }
if [[ -n "$FIXTURE_REGISTRY_OVERRIDE" || -n "$SCENARIO_SCRIPT_OVERRIDE" || -n "$FIXTURE_OVERLAY_OVERRIDE" || "$GPU_CAPTURE" -eq 1 ]]; then
    echo "E03 diagnostic fixture capture (not E00-admitted): $RUN_DIR"
else
    echo "E00 fixture capture: $RUN_DIR"
fi
