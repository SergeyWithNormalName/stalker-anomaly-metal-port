#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOCK_FILE="$PROJECT_ROOT/src/Layers/xrRenderMetal/shaders/toolchain/metal-shaderconverter-4.0.1.lock"
PACKAGE=""
OUTPUT_DIR="$PROJECT_ROOT/build/direct-metal-toolchain/metal-shaderconverter-4.0.1"
ACCEPT_UNVERIFIED=0

usage() {
    cat <<'EOF'
Usage:
  provision_metal_shaderconverter.sh --package FILE \
    --accept-unverified-package-signature [--output DIR]

Expands the pinned Apple package into an isolated project-local toolchain.
It never invokes Installer or writes to /usr/local. The acceptance flag is
required because macOS does not validate this package's outer signature.
EOF
}

lock_value() {
    awk -F= -v key="$1" '$1 == key { print substr($0, index($0, "=") + 1); exit }' "$LOCK_FILE"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --package)
            PACKAGE="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --accept-unverified-package-signature)
            ACCEPT_UNVERIFIED=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ -z "$PACKAGE" || ! -f "$PACKAGE" || ! -f "$LOCK_FILE" ]]; then
    usage >&2
    exit 2
fi
if [[ "$ACCEPT_UNVERIFIED" -ne 1 ]]; then
    echo "Refusing to use a package whose outer signature is not validated." >&2
    echo "Inspect the provenance lock, then pass --accept-unverified-package-signature." >&2
    exit 1
fi

EXPECTED_PACKAGE_SHA="$(lock_value package_sha256)"
EXPECTED_CLI_SHA="$(lock_value cli_sha256)"
EXPECTED_LIBRARY_SHA="$(lock_value library_sha256)"
ACTUAL_PACKAGE_SHA="$(shasum -a 256 "$PACKAGE" | awk '{print $1}')"
if [[ "$ACTUAL_PACKAGE_SHA" != "$EXPECTED_PACKAGE_SHA" ]]; then
    echo "Package SHA-256 mismatch: $ACTUAL_PACKAGE_SHA" >&2
    exit 1
fi

OUTPUT_PARENT="$(cd "$(dirname "$OUTPUT_DIR")" && pwd -P)"
OUTPUT_ABS="$OUTPUT_PARENT/$(basename "$OUTPUT_DIR")"
if [[ -L "$OUTPUT_ABS" ]]; then
    echo "Refusing a symlink as the toolchain output: $OUTPUT_ABS" >&2
    exit 1
fi
case "$OUTPUT_ABS/" in
    "$PROJECT_ROOT/runtime/"*)
        echo "Refusing to provision tools inside the live runtime" >&2
        exit 1
        ;;
esac
if [[ -e "$OUTPUT_ABS" ]]; then
    echo "Output path must not already exist: $OUTPUT_ABS" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d /private/tmp/xray-msc-provision.XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT
EXPANDED="$WORK_DIR/expanded"
pkgutil --expand-full "$PACKAGE" "$EXPANDED"
PAYLOAD="$EXPANDED/MetalShaderConverter.pkg/Payload"
CLI="$PAYLOAD/usr/local/bin/metal-shaderconverter"
LIBRARY="$PAYLOAD/usr/local/lib/libmetalirconverter.dylib"

if [[ "$(shasum -a 256 "$CLI" | awk '{print $1}')" != "$EXPECTED_CLI_SHA" ]]; then
    echo "Payload CLI hash does not match the provenance lock" >&2
    exit 1
fi
if [[ "$(shasum -a 256 "$LIBRARY" | awk '{print $1}')" != "$EXPECTED_LIBRARY_SHA" ]]; then
    echo "Payload library hash does not match the provenance lock" >&2
    exit 1
fi

mkdir -p "$OUTPUT_ABS/bin" "$OUTPUT_ABS/lib" "$OUTPUT_ABS/include" \
    "$OUTPUT_ABS/share/man" "$OUTPUT_ABS/docs"
cp "$CLI" "$OUTPUT_ABS/bin/metal-shaderconverter"
cp "$LIBRARY" "$OUTPUT_ABS/lib/libmetalirconverter.dylib"
cp -R "$PAYLOAD/usr/local/include/." "$OUTPUT_ABS/include/"
cp -R "$PAYLOAD/usr/local/share/man/." "$OUTPUT_ABS/share/man/"
cp "$PAYLOAD/opt/metal-shaderconverter/docs/UserManual.md" "$OUTPUT_ABS/docs/UserManual.md"

SIGNATURE_OUTPUT="$(pkgutil --check-signature "$PACKAGE" 2>&1 || true)"
RECEIPT="$OUTPUT_ABS/provision-receipt.txt"
{
    echo "evidence_scope=isolated_payload_provision_with_unverified_outer_package_signature"
    echo "source_url=$(lock_value source_url)"
    echo "package_path=$(realpath "$PACKAGE")"
    echo "package_sha256=$ACTUAL_PACKAGE_SHA"
    echo "package_signature_accepted_unverified=true"
    printf '%s\n' "$SIGNATURE_OUTPUT" | sed 's/^/pkgutil_signature_output=/'
    echo "cli_sha256=$(shasum -a 256 "$OUTPUT_ABS/bin/metal-shaderconverter" | awk '{print $1}')"
    echo "library_sha256=$(shasum -a 256 "$OUTPUT_ABS/lib/libmetalirconverter.dylib" | awk '{print $1}')"
    "$OUTPUT_ABS/bin/metal-shaderconverter" --version | sed 's/^/msc_version=/'
    echo "provenance_lock_sha256=$(shasum -a 256 "$LOCK_FILE" | awk '{print $1}')"
} > "$RECEIPT"

echo "MSC_PROVISION_READY output=$OUTPUT_ABS receipt=$RECEIPT"
