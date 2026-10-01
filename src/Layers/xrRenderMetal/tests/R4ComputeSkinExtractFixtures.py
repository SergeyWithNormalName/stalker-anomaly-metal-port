#!/usr/bin/env python3
"""Extract bounded raw installed OGF leaves for the independent compute oracle.

Diagnostic-only chunk walk follows FBasicVisual.cpp captureCpuGeometry: embedded
OGF_CHILDREN recursively, OGF_HEADER v4, OGF_VERTICES/INDICES verbatim. Compressed
chunks or linked children remain unsupported rather than substituting geometry.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

FORMATS = {1: (1, 60), 2: (2, 64), 3: (3, 70), 4: (4, 76),
           0x12071980: (1, 60), 0x240E3300: (2, 64),
           0x481C6600: (3, 70), 0x5A237F80: (4, 76)}


def chunks(data):
    result = {}
    offset = 0
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError("truncated OGF chunk header")
        chunk, size = struct.unpack_from("<II", data, offset)
        offset += 8
        if chunk & 0x80000000 or size > len(data) - offset or chunk in result:
            raise ValueError("compressed, overflowing or repeated OGF chunk")
        result[chunk] = data[offset:offset + size]
        offset += size
    return result


def leaves(data, label="root", depth=0):
    if depth > 32:
        raise ValueError("OGF hierarchy exceeds diagnostic depth limit")
    tree = chunks(data)
    if 10 in tree:
        raise ValueError("OGF has unresolved linked geometry")
    if 9 in tree:
        for child, child_data in sorted(chunks(tree[9]).items()):
            yield from leaves(child_data, f"{label}-{child}", depth + 1)
        return
    header = tree.get(1, b"")
    if len(header) < 44 or header[0] != 4 or header[1] not in (4, 5):
        return
    vertices = tree.get(3, b"")
    indices = tree.get(4, b"")
    if len(vertices) < 8 or len(indices) < 4:
        raise ValueError("skinned OGF leaf has missing vertex/index data")
    vertex_format, vertex_count = struct.unpack_from("<II", vertices)
    if vertex_format not in FORMATS:
        raise ValueError(f"unsupported skinned format {vertex_format:#x}")
    influences, stride = FORMATS[vertex_format]
    index_count, = struct.unpack_from("<I", indices)
    raw = vertices[8:8 + vertex_count * stride]
    raw_indices = indices[4:4 + index_count * 2]
    if len(raw) != vertex_count * stride or len(raw_indices) != index_count * 2:
        raise ValueError("truncated skinned OGF spans")
    maximum_bone = 0
    for vertex in range(vertex_count):
        if influences == 1:
            bone, = struct.unpack_from("<I", raw, vertex * stride + 56)
            maximum_bone = max(maximum_bone, bone)
        else:
            maximum_bone = max(maximum_bone, *struct.unpack_from(
                f"<{influences}H", raw, vertex * stride))
    if maximum_bone >= 256:
        raise ValueError("source bone ordinal exceeds live Metal palette cap")
    envelope = b"R4SKIN01" + struct.pack("<5I", vertex_format, stride, vertex_count,
                                        index_count, maximum_bone + 1) + raw + raw_indices
    yield label, envelope, {"format": vertex_format, "influences": influences,
                            "vertices": vertex_count, "indices": index_count,
                            "bones": maximum_bone + 1}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("models", type=Path, nargs="+")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = []
    total_bytes = 0
    leaf_count = 0
    for model_index, model in enumerate(args.models):
        row = {"source": str(model), "resolved_source": str(model.resolve()), "leaves": []}
        try:
            data = model.read_bytes()
            if len(data) > 16 * 1024 * 1024:
                raise ValueError("model exceeds diagnostic 16 MiB limit")
            row["source_sha256"] = hashlib.sha256(data).hexdigest()
            for label, envelope, info in leaves(data):
                if leaf_count >= 32 or total_bytes + len(envelope) > 32 * 1024 * 1024:
                    raise ValueError("fixture bundle exceeds diagnostic 32 MiB limit")
                target = args.output / f"model-{model_index:02d}-{label}.r4skin"
                target.write_bytes(envelope)
                total_bytes += len(envelope)
                leaf_count += 1
                info.update(path=str(target.resolve()), sha256=hashlib.sha256(envelope).hexdigest())
                row["leaves"].append(info)
        except (OSError, ValueError, struct.error) as error:
            row["unavailable"] = str(error)
        manifest.append(row)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"models": len(manifest), "leaves": sum(len(x["leaves"]) for x in manifest),
                      "raw_fixture_bytes": total_bytes, "manifest": str((args.output / "manifest.json").resolve())}))


if __name__ == "__main__":
    main()
