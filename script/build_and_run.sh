#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DXVK_ROOT="$PROJECT_ROOT/../dxvk-native"
RUNTIME_ROOT="$PROJECT_ROOT/runtime/macos-gamma"
BINARY="$PROJECT_ROOT/build/Binaries/RelWithDebInfo/AnomalyDX11"
# Defaults preserve the known-good custom MoltenVK. Benchmark wrappers may
# override this path for an isolated runtime variant; they never replace it.
ICD_JSON="${MOLTENVK_ICD_JSON:-$RUNTIME_ROOT/moltenvk/lib/MoltenVK_icd.json}"
MO2_ROOT="${XRAY_MO2_ROOT:-$HOME/gamma/gamma}"
GAME_ROOT="${XRAY_GAME_ROOT:-$HOME/gamma/anomaly}"
PROFILE_ROOT="$RUNTIME_ROOT/profile"
MATERIALIZER="$PROJECT_ROOT/script/materialize_mo2_profile.py"
COMPAT_PDA_SCRIPT="$PROJECT_ROOT/gamedata/scripts/pda_dynamic_tabs.script"
GAMMA_SHADER_CACHE="$GAME_ROOT/appdata/shaders_cache"
STAGING_SHADER_CACHE="$RUNTIME_ROOT/appdata/shaders_cache"

usage() {
    cat <<'EOF'
Usage:
  run-gamma-macos.command [--new]
  run-gamma-macos.command --refresh-profile [--new]
  run-gamma-macos.command --dev-new
  run-gamma-macos.command --load <save name>

The default starts the normal GAMMA main-menu flow. --new is an alias for the
same normal launch. --dev-new retains the direct technical fresh-world command
used for engine bring-up; it intentionally bypasses the GAMMA New Game UI.

--refresh-profile rematerializes the active MO2 profile as project-local
symlinks before launching. The launcher uses runtime/macos-gamma/appdata for
settings, logs, shader cache, screenshots, and saves.
EOF
}

REFRESH_PROFILE=0
if [[ "${1:-}" == "--refresh-profile" ]]; then
    REFRESH_PROFILE=1
    shift
fi

case "${1:---new}" in
    --new|new)
        GAME_ARGS=()
        ;;
    --dev-new|dev-new)
        GAME_ARGS=(-start 'server(all/single/alife/new)' 'client(localhost)')
        ;;
    --load|load)
        if [[ $# -lt 2 || -z "${2:-}" ]]; then
            echo "A save name is required after --load." >&2
            usage >&2
            exit 2
        fi
        GAME_ARGS=(-load "$2")
        ;;
    --help|-h|help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown option: $1" >&2
        usage >&2
        exit 2
        ;;
esac

# Stop only earlier instances launched from this exact build tree and wait for
# them to exit before creating the next device/window.
while IFS= read -r pid; do
    kill "$pid" 2>/dev/null || true
done < <(pgrep -f "$BINARY" || true)

for _ in 1 2 3; do
    if ! pgrep -f "$BINARY" >/dev/null 2>&1; then
        break
    fi
    sleep 1
done

if pgrep -f "$BINARY" >/dev/null 2>&1; then
    echo "An existing AnomalyDX11 process did not exit; close it and retry." >&2
    exit 1
fi

if [[ "$REFRESH_PROFILE" -eq 1 ]] || ! python3 "$MATERIALIZER" --check-current \
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

# Preserve the Windows/GAMMA-generated DXBC cache in the writable runtime.
# Earlier bring-up builds used a symlink into the reference installation; do
# not let normal native launches write there.
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
    "$PROFILE_ROOT/profile-manifest.json" \
    "$ICD_JSON" \
    "$RUNTIME_ROOT/moltenvk/lib/libMoltenVK.dylib"; do
    if [[ ! -e "$required_path" ]]; then
        echo "Missing runtime component: $required_path" >&2
        exit 1
    fi
done

# Keep the project-local DXVK Native dylibs synchronized with their source.
# AnomalyDX11 loads these directly through its development rpaths.
ninja -C "$DXVK_ROOT/build" -j 4
ninja -C "$PROJECT_ROOT/build" -j 4 Anomaly.DX11

# The working Vulkan workaround is intentionally limited to this process. It
# uses the project-local custom MoltenVK and its retained-resource safety mode.
unset MTL_CAPTURE_ENABLED MTL_DEBUG_LAYER MTL_SHADER_VALIDATION
unset DYLD_LIBRARY_PATH DXVK_WSI_DRIVER
export VK_ICD_FILENAMES="$ICD_JSON"
export MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES="${MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES:-1}"

cd "$RUNTIME_ROOT"
if [[ -z "${GAME_ARGS[*]-}" ]]; then
    exec "$BINARY"
fi
exec "$BINARY" "${GAME_ARGS[@]}"
