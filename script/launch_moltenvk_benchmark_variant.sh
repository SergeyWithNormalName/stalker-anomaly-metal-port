#!/usr/bin/env bash
# Launch exactly one native MoltenVK benchmark variant. It intentionally does
# not record FPS: frame statistics must be taken from the fixed live scene.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNTIME_ROOT="$PROJECT_ROOT/runtime/macos-gamma"

usage() {
    cat <<'EOF'
Usage: launch_moltenvk_benchmark_variant.sh {safe|no-live-check|stock} [launcher arguments]

safe          Custom MoltenVK plus LIVE_CHECK_ALL_RESOURCES=1 (default path).
no-live-check Same custom MoltenVK, only LIVE_CHECK_ALL_RESOURCES=0.
stock         Homebrew's stock MoltenVK 1.4.2, LIVE_CHECK_ALL_RESOURCES=0.

Examples:
  ./script/launch_moltenvk_benchmark_variant.sh safe --load 'stalker - autosave'
  ./script/launch_moltenvk_benchmark_variant.sh no-live-check --load 'stalker - autosave'
EOF
}

variant="${1:-}"
if [[ -z "$variant" ]]; then
    usage >&2
    exit 2
fi
shift

unset MTL_CAPTURE_ENABLED MTL_DEBUG_LAYER MTL_SHADER_VALIDATION
unset VK_LAYER_PATH VK_INSTANCE_LAYERS VK_DEVICE_LAYERS
# This is intentionally scoped to benchmark variants. The normal launcher
# retains its existing Retina/borderless behavior.
export XRAY_BENCHMARK_LOGICAL_SIZE="${XRAY_BENCHMARK_LOGICAL_SIZE:-960x540}"
case "$variant" in
    safe)
        export MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=1
        unset MOLTENVK_ICD_JSON
        ;;
    no-live-check)
        export MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=0
        unset MOLTENVK_ICD_JSON
        ;;
    stock)
        "$SCRIPT_DIR/prepare_moltenvk_variants.sh"
        export MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=0
        export MOLTENVK_ICD_JSON="$RUNTIME_ROOT/moltenvk-variants/stock-1.4.2/lib/MoltenVK_icd.json"
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

exec "$PROJECT_ROOT/run-gamma-macos.command" "$@"
