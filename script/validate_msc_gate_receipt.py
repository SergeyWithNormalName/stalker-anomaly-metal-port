#!/usr/bin/env python3
"""Verify metallib/reflection files against the MSC gate receipt and manifest."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_manifest(path: Path) -> list[tuple[str, str, str, str]]:
    entries: list[tuple[str, str, str, str]] = []
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        if not raw_line or raw_line.startswith("#"):
            continue
        fields = raw_line.split("\t")
        if len(fields) != 4:
            raise ValueError(f"{path}:{line_number}: expected four tab-separated fields")
        entries.append(tuple(fields))
    return entries


def read_receipt(path: Path) -> tuple[dict[str, str], dict[str, dict[str, str]]]:
    metadata: dict[str, str] = {}
    shaders: dict[str, dict[str, str]] = {}
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        if raw_line.startswith("shader="):
            fields = dict(field.split("=", 1) for field in raw_line.split("\t"))
            shader = fields["shader"]
            if shader in shaders:
                raise ValueError(f"{path}:{line_number}: duplicate shader {shader}")
            shaders[shader] = fields
        elif "=" in raw_line:
            key, value = raw_line.split("=", 1)
            metadata[key] = value
    return metadata, shaders


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--metal-dir", required=True, type=Path)
    parser.add_argument("--metallib", action="append", default=[], type=Path)
    args = parser.parse_args()

    receipt_path = args.metal_dir / "msc-gate-receipt.txt"
    if not args.manifest.is_file() or not receipt_path.is_file():
        raise SystemExit("manifest or msc-gate-receipt.txt is missing")

    entries = read_manifest(args.manifest)
    metadata, shaders = read_receipt(receipt_path)
    expected_names = {source for _, _, source, _ in entries}

    if args.metallib:
        expected_metallibs = sorted(f"{source}.metallib" for source in expected_names)
        provided_metallibs = sorted(path.name for path in args.metallib)
        if provided_metallibs != expected_metallibs:
            raise SystemExit("provided metallib arguments do not exactly match the manifest")

    if metadata.get("manifest_sha256") != sha256(args.manifest):
        raise SystemExit("MSC receipt manifest hash does not match the current manifest")
    if metadata.get("dxc_receipt_validation") != "complete":
        raise SystemExit("MSC receipt lacks a completed DXIL receipt validation")
    if metadata.get("shader_count") != str(len(entries)):
        raise SystemExit("MSC receipt shader_count does not match the manifest")
    if set(shaders) != expected_names:
        raise SystemExit("MSC receipt shader set does not match the manifest")

    for shader_class, profile, source, permutation in entries:
        record = shaders[source]
        metallib_path = args.metal_dir / f"{source}.metallib"
        reflection_path = args.metal_dir / f"{source}.json"
        if record.get("profile") != profile or record.get("permutation") != permutation:
            raise SystemExit(f"manifest metadata mismatch for {source}")
        if not metallib_path.is_file() or not reflection_path.is_file():
            raise SystemExit(f"missing MSC output for {shader_class}: {source}")
        metallib_hash = sha256(metallib_path)
        reflection_hash = sha256(reflection_path)
        if record.get("metallib_sha256") != metallib_hash:
            raise SystemExit(f"metallib hash mismatch for {source}")
        if record.get("reflection_sha256") != reflection_hash:
            raise SystemExit(f"reflection hash mismatch for {source}")
        print(
            f"MSC_RECEIPT_READY class={shader_class} source={source} "
            f"metallib_sha256={metallib_hash}"
        )

    print(f"MSC_RECEIPT_COMPLETE shaders={len(entries)} receipt={receipt_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
