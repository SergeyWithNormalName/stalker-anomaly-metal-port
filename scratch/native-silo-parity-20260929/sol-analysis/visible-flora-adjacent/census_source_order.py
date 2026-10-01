#!/usr/bin/env python3
"""Count adjacent exact Flora resources in one saved, archive-winning level file.

This does not load level.geom or classify the camera-visible subsequence.
"""
from pathlib import Path
from collections import Counter, defaultdict
import argparse
import hashlib
import json
import struct

p = argparse.ArgumentParser()
p.add_argument("--level", type=Path, required=True)
p.add_argument("--output", type=Path)
a = p.parse_args()
source = a.level.read_bytes()
view = memoryview(source)

def u16(offset):
    return struct.unpack_from("<H", view, offset)[0]

def u32(offset):
    return struct.unpack_from("<I", view, offset)[0]

def chunks(begin, end):
    found = []
    at = begin
    while at < end:
        if at + 8 > end:
            raise ValueError("truncated X-Ray chunk header")
        ident, length = struct.unpack_from("<II", view, at)
        if ident & 0x80000000 or at + 8 + length > end:
            raise ValueError("compressed or overrun X-Ray chunk")
        found.append((ident, at + 8, at + 8 + length))
        at += 8 + length
    if at != end:
        raise ValueError("unreconciled X-Ray chunk stream")
    return found

def one(items, ident):
    matched = [value for value in items if value[0] == ident]
    if len(matched) != 1:
        raise ValueError(f"expected one chunk {ident}, found {len(matched)}")
    return matched[0]

top = chunks(0, len(view))
_, shader_start, shader_end = one(top, 2)
shader_count = u32(shader_start)
shaders = []
at = shader_start + 4
for _ in range(shader_count):
    nul = source.find(b"\0", at, shader_end)
    if nul < 0:
        raise ValueError("unterminated shader entry")
    entry = source[at:nul]
    at = nul + 1
    if entry:
        if b"/" not in entry:
            raise ValueError("invalid shader entry")
        material, textures = entry.split(b"/", 1)
        shaders.append((material, textures.split(b",", 1)[0]))
    else:
        shaders.append((b"", b""))
if at != shader_end:
    raise ValueError("trailing shader bytes")

_, visual_start, visual_end = one(top, 3)
visual_chunks = chunks(visual_start, visual_end)
keys = []
model_ids = []
tree_lighting = []
materials = []
emitted = set()
for ident, start, end in visual_chunks:
    sub = chunks(start, end)
    _, hb, he = one(sub, 1)
    if he - hb < 44 or view[hb] != 4:
        raise ValueError("invalid visual header")
    visual_type = view[hb + 1]
    shader_id = u16(hb + 2)
    if shader_id >= len(shaders):
        raise ValueError("invalid shader index")
    material, base = shaders[shader_id]
    if material not in (b"flora\\leaf_wave", b"flora\\trunk_wave"):
        continue
    if ident in emitted:
        continue
    emitted.add(ident)
    if visual_type != 7 or not base:
        raise ValueError("matched Flora source is not an admitted regular-tree visual")
    _, gb, ge = one(sub, 21)
    _, tb, te = one(sub, 12)
    if ge - gb != 24 or te - tb < 104:
        raise ValueError("invalid Flora geometry/tree constant payload")
    vb_id, vb_base, vb_count, ib_id, ib_base, ib_count = struct.unpack_from("<6I", view, gb)
    if vb_count == 0 or ib_count == 0 or ib_count % 3:
        raise ValueError("invalid indexed Flora geometry")
    # These six fields are exactly geometryCacheKey(); source base texture and
    # material add the exact shadow-group resource identity. Secondary leaf
    # bump paths derive deterministically from this base and active XRFS state.
    keys.append((vb_id, vb_base, vb_count, ib_id, ib_base, ib_count, material, base))
    tree_lighting.append(bytes(view[tb + 64:tb + 104]))
    model_ids.append(ident)
    materials.append("leaf" if material == b"flora\\leaf_wave" else "trunk")

if len(keys) != 11663:
    raise ValueError(f"source candidate count {len(keys)} disagrees with admitted runtime 11663")
unique = len(set(keys))
if unique != 487:
    raise ValueError(f"source resource-group count {unique} disagrees with runtime shadow groups 487")
geometry_groups = len({key[:6] for key in keys})
if geometry_groups != 487:
    raise ValueError(f"source geometry group count {geometry_groups} disagrees with runtime 487")
runs = []
at = 0
while at < len(keys):
    end = at + 1
    while end < len(keys) and keys[end] == keys[at]:
        end += 1
    runs.append((at, end))
    at = end
transitions = Counter(f"{a}->{b}" for a, b in zip(materials, materials[1:]))
lighting_variants = defaultdict(set)
for key, lighting in zip(keys, tree_lighting):
    lighting_variants[key].add(lighting)
out = {
    "schema": "flora-main-source-order-adjacency/v1",
    "level_source": str(a.level.resolve()),
    "level_sha256": hashlib.sha256(source).hexdigest(),
    "level_bytes": len(source),
    "scope": "All admitted source records in original R4LevelGeometry.visuals order. No main-camera culling and no production draw replay.",
    "admitted_source_records": len(keys),
    "materials": dict(Counter(materials)),
    "geometry_groups": geometry_groups,
    "exact_resource_groups": unique,
    "adjacent_runs_all_records": len(runs),
    "adjacent_draws_saved_all_records_if_instanced": len(keys) - len(runs),
    "multi_instance_runs_all_records": sum(end - start > 1 for start, end in runs),
    "instances_in_multi_runs_all_records": sum(end - start for start, end in runs if end - start > 1),
    "multi_runs_with_distinct_tree_lighting": sum(
        len(set(tree_lighting[start:end])) > 1 for start, end in runs if end - start > 1),
    "max_adjacent_run_all_records": max(end - start for start, end in runs),
    "material_transitions_all_records": dict(transitions),
    "groups_with_distinct_tree_lighting": sum(len(values) > 1 for values in lighting_variants.values()),
    "groups_total": len(lighting_variants),
    "example_multiruns": [{"first_visual": model_ids[start], "last_visual": model_ids[end - 1],
                           "instances": end - start, "material": materials[start],
                           "distinct_tree_lighting": len(set(tree_lighting[start:end])) > 1}
                          for start, end in runs if end - start > 1][:12],
}
serialized = json.dumps(out, indent=2) + "\n"
if a.output:
    a.output.write_text(serialized)
print(serialized)
