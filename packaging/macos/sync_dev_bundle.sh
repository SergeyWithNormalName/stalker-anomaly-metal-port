#!/usr/bin/env bash
set -euo pipefail

TARGET_FILE=""
BUNDLE_DIR=""
PLIST_FILE=""
OUTPUT_NAME=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target-file)
            TARGET_FILE="$2"
            shift 2
            ;;
        --bundle-dir)
            BUNDLE_DIR="$2"
            shift 2
            ;;
        --plist-file)
            PLIST_FILE="$2"
            shift 2
            ;;
        --output-name)
            OUTPUT_NAME="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

if [[ -z "$TARGET_FILE" || -z "$BUNDLE_DIR" || -z "$PLIST_FILE" || -z "$OUTPUT_NAME" ]]; then
    echo "Missing required arguments for sync_dev_bundle.sh" >&2
    exit 1
fi

if [[ ! -f "$TARGET_FILE" ]]; then
    echo "Target executable not found: $TARGET_FILE" >&2
    exit 1
fi

MACOS_DIR="$BUNDLE_DIR/Contents/MacOS"
RESOURCES_DIR="$BUNDLE_DIR/Contents/Resources"
FRAMEWORKS_DIR="$BUNDLE_DIR/Contents/Frameworks"

mkdir -p "$MACOS_DIR" "$RESOURCES_DIR" "$FRAMEWORKS_DIR"

# Synchronize Info.plist only when content changes
DEST_PLIST="$BUNDLE_DIR/Contents/Info.plist"
if [[ ! -f "$DEST_PLIST" ]] || ! cmp -s "$PLIST_FILE" "$DEST_PLIST"; then
    cp -pf "$PLIST_FILE" "$DEST_PLIST"
fi

# Ensure standard PkgInfo is present
DEST_PKGINFO="$BUNDLE_DIR/Contents/PkgInfo"
if [[ ! -f "$DEST_PKGINFO" ]]; then
    printf 'APPL????' > "$DEST_PKGINFO"
fi

# DEV-ONLY HARD LINK RULE:
# Hard links are strictly for the local DEV .app bundle to achieve zero-copy (<1ms) sync.
# NEVER use hard links for RELEASE staging or codesigning (Phase E10):
# In RELEASE staging, the executable MUST be an independent copy (distinct inode)
# so that install_name_tool modifications and Developer ID code signatures do not alter
# the local raw development binary.
DEST_BIN="$MACOS_DIR/$OUTPUT_NAME"
if [[ ! -f "$DEST_BIN" ]] || [[ "$TARGET_FILE" -nt "$DEST_BIN" ]] || [[ "$(stat -f '%i' "$TARGET_FILE" 2>/dev/null || true)" != "$(stat -f '%i' "$DEST_BIN" 2>/dev/null || true)" ]]; then
    ln -f "$TARGET_FILE" "$DEST_BIN" 2>/dev/null || cp -pf "$TARGET_FILE" "$DEST_BIN"
fi

touch "$BUNDLE_DIR"
