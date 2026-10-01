#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUNTIME_ROOT="$PROJECT_ROOT/runtime/macos-gamma"
BINARY="$PROJECT_ROOT/build/Binaries/RelWithDebInfo/AnomalyDX11"
APP_BUNDLE="$PROJECT_ROOT/build/Binaries/RelWithDebInfo/Anomaly.app"
MO2_ROOT="${XRAY_MO2_ROOT:-$HOME/gamma/gamma}"
GAME_ROOT="${XRAY_GAME_ROOT:-$HOME/gamma/anomaly}"
PROFILE_ROOT="$RUNTIME_ROOT/profile"
MATERIALIZER="$PROJECT_ROOT/script/materialize_mo2_profile.py"
COMPAT_PDA_SCRIPT="$PROJECT_ROOT/gamedata/scripts/pda_dynamic_tabs.script"
GAMMA_SHADER_CACHE="$GAME_ROOT/appdata/shaders_cache"
STAGING_SHADER_CACHE="$RUNTIME_ROOT/appdata/shaders_cache"
RAW_LAUNCH=0

usage() {
    cat <<'EOF'
Usage:
  run-gamma-metal.command [--raw] [--capture-ready] [--load <save name>]
  run-gamma-metal.command --help

Default (no arguments): launches DEV Anomaly.app via macOS LaunchServices
(enabling normal application lifecycle and Game Mode eligibility).

Options:
  --raw, --raw-binary  Launch raw AnomalyDX11 executable directly in foreground
                       (for debugger/lldb, CLI probes, live terminal output).
  --load <save name>   Quick-load a specific save directly into gameplay.
  --capture-ready      Enable Metal capture boundaries for gpucapture. This
                       does not capture a loading screen; wait for the
                       METAL_LIGHTING_CAPTURE_READY log marker first.
EOF
}

# Stop earlier instances launched from this tree
while IFS= read -r pid; do
    kill "$pid" 2>/dev/null || true
done < <(pgrep -f "AnomalyDX11" || true)

for _ in 1 2 3; do
    if ! pgrep -f "AnomalyDX11" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

if pgrep -f "AnomalyDX11" >/dev/null 2>&1; then
    echo "An existing AnomalyDX11 process did not exit; close it and retry." >&2
    exit 1
fi

# Ensure MO2 profile is materialized
if ! python3 "$MATERIALIZER" --check-current \
    --mo2-root "$MO2_ROOT" \
    --game-root "$GAME_ROOT" \
    --output "$PROFILE_ROOT" \
    --compat-pda-script "$COMPAT_PDA_SCRIPT"; then
    python3 "$MATERIALIZER" \
        --mo2-root "$MO2_ROOT" \
        --game-root "$GAME_ROOT" \
        --output "$PROFILE_ROOT" \
        --compat-pda-script "$COMPAT_PDA_SCRIPT"
fi

# Preserve shader cache
if [[ -L "$STAGING_SHADER_CACHE" ]]; then
    unlink "$STAGING_SHADER_CACHE"
fi
if [[ ! -d "$STAGING_SHADER_CACHE" ]]; then
    if [[ ! -d "$GAMMA_SHADER_CACHE" ]]; then
        echo "Missing GAMMA shader cache: $GAMMA_SHADER_CACHE" >&2
        exit 1
    fi
    cp -R "$GAMMA_SHADER_CACHE" "$STAGING_SHADER_CACHE"
fi

for required_path in \
    "$RUNTIME_ROOT/fsgame.ltx" \
    "$RUNTIME_ROOT/db" \
    "$RUNTIME_ROOT/gamedata/configs" \
    "$RUNTIME_ROOT/appdata" \
    "$PROFILE_ROOT/profile-manifest.json"; do
    if [[ ! -e "$required_path" ]]; then
        echo "Missing runtime component: $required_path" >&2
        exit 1
    fi
done

# Synchronize build if needed
ninja -C "$PROJECT_ROOT/build" -j 4 Anomaly.DX11

# Direct Metal runtime environment.  E00 uses the exact same source of paths.
export XRAY_PROJECT_ROOT="$PROJECT_ROOT"
source "$PROJECT_ROOT/script/xray_metal_shader_env.sh"

# Performance defaults for play.  Capture is opt-in because it changes
# Metal's runtime overhead, but a capture-enabled process is required before
# gpucapture can arm a world-frame boundary.
if [[ "${XRAY_METAL_CAPTURE:-0}" == "1" ]]; then
    export MTL_CAPTURE_ENABLED=1
else
    unset MTL_CAPTURE_ENABLED
fi
unset DYLD_LIBRARY_PATH DXVK_WSI_DRIVER VK_ICD_FILENAMES

GAME_ARGS=(-fsltx "$RUNTIME_ROOT/fsgame.ltx" -render_backend metal -nointro -skiplogo)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --raw|--raw-binary)
            RAW_LAUNCH=1
            shift
            ;;
        --capture-ready)
            export XRAY_METAL_CAPTURE=1
            export MTL_CAPTURE_ENABLED=1
            shift
            ;;
        --menu|--new)
            shift
            ;;
        --load)
            if [[ $# -lt 2 || -z "${2:-}" ]]; then
                echo "A save name is required after --load." >&2
                usage >&2
                exit 2
            fi
            GAME_ARGS+=(-load "\"$2\"")
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            if [[ "$1" == -* ]]; then
                GAME_ARGS+=("$1")
                shift
            else
                GAME_ARGS+=(-load "\"$1\"")
                shift
            fi
            ;;
    esac
done

cd "$RUNTIME_ROOT"
echo "==============================================="
echo "   S.T.A.L.K.E.R. GAMMA (Direct Apple Metal)   "
echo "==============================================="
echo "Arguments: ${GAME_ARGS[*]}"
if [[ "${MTL_CAPTURE_ENABLED:-0}" == "1" ]]; then
    echo "Metal capture boundaries: enabled (wait for METAL_LIGHTING_CAPTURE_READY)."
fi
echo ""
echo "Note: Launching directly into the Main Menu."
echo "You can start a New Game, load any save, or adjust"
echo "settings directly from the game's menu interface."

if [[ "$RAW_LAUNCH" -eq 1 ]]; then
    echo "Mode: Raw executable (direct terminal execution, no .app bundle wrapper)"
    echo "Binary: $BINARY"
    echo "==============================================="
    exec "$BINARY" "${GAME_ARGS[@]}"
else
    echo "Mode: DEV macOS Application Bundle (.app via LaunchServices)"
    echo "Bundle: $APP_BUNDLE"
    echo "Game Mode: Eligible (when switched to Fullscreen)"
    LOG_DIR="$RUNTIME_ROOT/appdata/logs"
    mkdir -p "$LOG_DIR"
    STDOUT_LOG="$LOG_DIR/launcher_stdout.log"
    STDERR_LOG="$LOG_DIR/launcher_stderr.log"
    echo "Stdout log: $STDOUT_LOG"
    echo "Stderr log: $STDERR_LOG"
    echo "==============================================="
    if [[ ! -d "$APP_BUNDLE" ]]; then
        echo "Error: DEV bundle not found at $APP_BUNDLE" >&2
        echo "Run 'ninja -C $PROJECT_ROOT/build Anomaly.DX11' to build." >&2
        exit 1
    fi
    open -n -W -a "$APP_BUNDLE" --stdout "$STDOUT_LOG" --stderr "$STDERR_LOG" --args "${GAME_ARGS[@]}"
fi
