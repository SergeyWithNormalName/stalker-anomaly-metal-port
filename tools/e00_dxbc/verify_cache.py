#!/usr/bin/env python3
"""Verify the E00 cache ABI and reproduce X-Ray ShaderSourceCRC.cpp exactly."""

from __future__ import annotations

import argparse
import re
import struct
import zlib
from pathlib import Path


def xray_crc32(data: bytes, starting_crc: int = 0) -> int:
    # src/xrCore/crc32.cpp: initial state is ~starting_crc; zlib exposes the
    # same continued IEEE CRC semantics when seeded with the previous result.
    return zlib.crc32(data, starting_crc) & 0xFFFFFFFF


def source_crc(path: Path, source_root: Path, seen: list[Path] | None = None) -> int:
    if seen is None:
        seen = []
    if path in seen:
        raise SystemExit(f"include cycle is not a valid ShaderSourceCRC input: {path}")
    seen.append(path)
    data = path.read_bytes()
    crc = xray_crc32(data)
    for line in data.splitlines():
        match = re.match(rb"^\s*#include\s+\"([^\"]+)\"", line)
        if not match:
            continue
        include_name = match.group(1).decode("ascii").lower().replace("\\", "/")
        # ShaderSourceCRC.cpp uses the renderer's r3 root for every include,
        # then falls back to the same root without a current-file directory.
        include_path = source_root / include_name
        if not include_path.is_file():
            raise SystemExit(f"ShaderSourceCRC include missing: {include_name}")
        crc = xray_crc32(include_path.read_bytes(), crc)
        crc = source_crc_includes(include_path, source_root, crc, seen)
    seen.pop()
    return crc


def source_crc_includes(path: Path, source_root: Path, crc: int, seen: list[Path]) -> int:
    data = path.read_bytes()
    for line in data.splitlines():
        match = re.match(rb"^\s*#include\s+\"([^\"]+)\"", line)
        if not match:
            continue
        include_name = match.group(1).decode("ascii").lower().replace("\\", "/")
        include_path = source_root / include_name
        if not include_path.is_file():
            raise SystemExit(f"ShaderSourceCRC include missing: {include_name}")
        if include_path in seen:
            raise SystemExit(f"include cycle is not a valid ShaderSourceCRC input: {include_path}")
        seen.append(include_path)
        include_data = include_path.read_bytes()
        crc = xray_crc32(include_data, crc)
        crc = source_crc_includes(include_path, source_root, crc, seen)
        seen.pop()
    return crc


def check_dxbc(dxbc: bytes) -> list[str]:
    if dxbc[:4] != b"DXBC":
        raise SystemExit("raw compiler payload has no DXBC magic")
    if len(dxbc) < 32:
        raise SystemExit("DXBC payload is too short")
    total_size, chunk_count = struct.unpack_from("<II", dxbc, 24)
    if total_size != len(dxbc):
        raise SystemExit(f"DXBC total-size mismatch: header={total_size} actual={len(dxbc)}")
    offsets_end = 32 + chunk_count * 4
    if offsets_end > len(dxbc):
        raise SystemExit("DXBC chunk offset table is truncated")
    chunks: list[str] = []
    for offset in struct.unpack_from(f"<{chunk_count}I", dxbc, 32):
        if offset + 8 > len(dxbc):
            raise SystemExit(f"DXBC chunk offset outside container: {offset}")
        name = dxbc[offset:offset + 4].decode("ascii", "replace")
        size = struct.unpack_from("<I", dxbc, offset + 4)[0]
        if offset + 8 + size > len(dxbc):
            raise SystemExit(f"DXBC chunk {name} is truncated")
        chunks.append(name)
    if "RDEF" not in chunks:
        raise SystemExit(f"DXBC has no RDEF reflection chunk: {chunks}")
    if not any(name in chunks for name in ("OSGN", "OSG5")):
        raise SystemExit(f"DXBC has no output-signature chunk: {chunks}")
    return chunks


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--raw-dxbc", type=Path, required=True)
    parser.add_argument("--cache-output", type=Path, required=True)
    parser.add_argument("--expected-key", required=True)
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9]{59}", args.expected_key):
        raise SystemExit(f"invalid R4 shader cache key: {args.expected_key}")
    dxbc = args.raw_dxbc.read_bytes()
    chunks = check_dxbc(dxbc)
    crc = source_crc(args.source, args.source_root)
    args.cache_output.parent.mkdir(parents=True, exist_ok=True)
    args.cache_output.write_bytes(struct.pack("<I", crc) + dxbc)
    cache = args.cache_output.read_bytes()
    if cache[4:8] != b"DXBC" or struct.unpack_from("<I", cache)[0] != crc:
        raise SystemExit("cache ABI verification failed")
    print(f"E00_CACHE_READY key={args.expected_key} source_crc=0x{crc:08x} "
          f"raw_dxbc_bytes={len(dxbc)} cache_bytes={len(cache)} chunks={','.join(chunks)}")


if __name__ == "__main__":
    main()
