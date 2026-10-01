#!/usr/bin/env bash
# Prepare read-only benchmark libraries next to, never in place of, the
# known-good runtime. This script has no effect on the default launcher.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNTIME_ROOT="$PROJECT_ROOT/runtime/macos-gamma"
STOCK_ROOT="/opt/homebrew/Cellar/molten-vk/1.4.2/lib"
VARIANT_ROOT="$RUNTIME_ROOT/moltenvk-variants/stock-1.4.2"

if [[ ! -f "$STOCK_ROOT/libMoltenVK.dylib" ]]; then
    echo "Stock MoltenVK 1.4.2 was not found at $STOCK_ROOT." >&2
    exit 1
fi

mkdir -p "$VARIANT_ROOT/lib"
cp -f "$STOCK_ROOT/libMoltenVK.dylib" "$VARIANT_ROOT/lib/libMoltenVK.dylib"
cat > "$VARIANT_ROOT/lib/MoltenVK_icd.json" <<'EOF'
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "./libMoltenVK.dylib",
    "api_version": "1.4.0",
    "is_portability_driver": true
  }
}
EOF

echo "Prepared isolated stock variant: $VARIANT_ROOT"
shasum -a 256 "$VARIANT_ROOT/lib/libMoltenVK.dylib"
