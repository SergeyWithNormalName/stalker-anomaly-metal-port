#!/usr/bin/env python3
"""Reconcile frame-700 main Flora IDs, active level geometry, and native fetches."""
from __future__ import annotations

import collections
import contextlib
import csv
import hashlib
import io
import json
from pathlib import Path
import runpy
import sys


REPO = Path(__file__).resolve().parents[4]
ANALYSIS = REPO / "scratch/native-silo-parity-20260929/sol-analysis"
RUN = REPO / "scratch/native-silo-parity-20260929/runs/flora-fetch-observe-02/native/output"
SOURCE_ORDER = ANALYSIS / "visible-flora-adjacent"
MANIFEST = SOURCE_ORDER / "source-order-census.json"
IDS = RUN / "main-flora-ids/main-flora-ids-frame-700.csv"
FETCH = RUN / "flora-fetch.tsv"
OUT = Path(__file__).with_name("visible-main-group-census.json")


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


manifest = json.loads(MANIFEST.read_text())
level = Path(manifest["level_source"])
if not level.is_file() or digest(level) != manifest["level_sha256"]:
    raise SystemExit("active source level missing or does not match pinned census hash")

# Reuse the existing X-Ray chunk parser and its validated source-key census.
sys.argv = [str(SOURCE_ORDER / "census_source_order.py"), "--level", str(level)]
with contextlib.redirect_stdout(io.StringIO()):
    parsed = runpy.run_path(sys.argv[0])
source_by_id = {
    int(visual_id): (key, material, lighting)
    for visual_id, key, material, lighting in zip(
        parsed["model_ids"], parsed["keys"], parsed["materials"], parsed["tree_lighting"])
}

header_keys = {
    "schema", "engine_frame", "extent", "admitted", "submitted", "culled",
    "view_bits", "projection_bits", "eye_bits", "ssa_settings_bits", "visual_id",
}
visible: list[tuple[int, str]] = []
with IDS.open() as stream:
    for line in stream:
        if line.split(",", 1)[0] in header_keys or line.startswith("encode_complete,"):
            continue
        visual_id, material = line.rstrip("\n").split(",", 1)
        visible.append((int(visual_id), material))
if len(visible) != 5316 or len({visual_id for visual_id, _ in visible}) != 5316:
    raise SystemExit("frame-700 main Flora ID fixture is not 5,316 unique IDs")
if any(visual_id not in source_by_id or source_by_id[visual_id][1] != material
       for visual_id, material in visible):
    raise SystemExit("frame-700 ID/material sequence disagrees with pinned source parser")

lines = FETCH.read_text().splitlines()
if len(lines) < 8 or lines[0] != "schema\tflora-main-vertex-fetch-observer/v1":
    raise SystemExit("unexpected fetch-observer schema")
field_line = lines[6].split("\t", 1)
if len(field_line) != 2 or field_line[0] != "draw_fields":
    raise SystemExit("fetch-observer field header missing")
fields = field_line[1].split()
rows = [dict(zip(fields, line.split("\t"))) for line in lines[7:] if line.strip()]
if len(rows) != len(visible):
    raise SystemExit("fetch draw count differs from frame-700 ID count")

source_resources = []
live_signatures = []
tree_lighting_by_resource: dict[tuple, set[bytes]] = collections.defaultdict(set)
for ordinal, ((visual_id, material), row) in enumerate(zip(visible, rows)):
    source_key, source_material, lighting = source_by_id[visual_id]
    if source_material != material:
        raise SystemExit(f"material mismatch at draw ordinal {ordinal}")
    expected_ps = "leaf" if material == "leaf" else "trunk"
    if expected_ps not in row["pso_label"]:
        raise SystemExit(f"PSO/material mismatch at draw ordinal {ordinal}")
    source_resources.append(source_key)
    tree_lighting_by_resource[source_key].add(lighting)
    live_signatures.append((
        row["pso_uid"], row["vertex_version_uid"], row["index_version_uid"],
        row["index_offset"], row["index_count"], row["index_type"], row["base_vertex"],
    ))

resource_to_live: dict[tuple, tuple] = {}
live_to_resource: dict[tuple, tuple] = {}
for source_key, live_key in zip(source_resources, live_signatures):
    if source_key in resource_to_live and resource_to_live[source_key] != live_key:
        raise SystemExit("one source geometry/material key maps to multiple live fetch signatures")
    if live_key in live_to_resource and live_to_resource[live_key] != source_key:
        raise SystemExit("one live fetch signature maps to multiple source geometry/material keys")
    resource_to_live[source_key] = live_key
    live_to_resource[live_key] = source_key
if len(resource_to_live) != len(live_to_resource):
    raise SystemExit("source/live grouping is not a bijection")

sizes = collections.Counter(collections.Counter(source_resources).values())
lighting_variant_groups = sum(len(values) > 1 for values in tree_lighting_by_resource.values())
group_members: dict[tuple, list[int]] = collections.defaultdict(list)
group_live: dict[tuple, tuple] = {}
for (visual_id, _), source_key, live_key in zip(visible, source_resources, live_signatures):
    group_members[source_key].append(visual_id)
    group_live[source_key] = live_key
GROUPS_CSV = Path(__file__).with_name("visible-main-group-members.csv")
with GROUPS_CSV.open("w", newline="") as stream:
    writer = csv.writer(stream)
    writer.writerow(["group_index", "draw_count", "visual_ids", "flora_material", "base_texture",
        "vertex_buffer_id", "vertex_base", "vertex_count", "index_buffer_id", "index_base", "index_count",
        "live_pso_uid", "live_vertex_version_uid", "live_index_version_uid", "live_index_offset",
        "live_index_count", "live_index_type", "live_base_vertex", "tree_lighting_payload_variants"])
    for group_index, (key, visual_ids) in enumerate(group_members.items()):
        vb_id, vb_base, vb_count, ib_id, ib_base, ib_count, material, base_texture = key
        live_key = group_live[key]
        pso_uid, vertex_version, index_version, index_offset, live_index_count, index_type, base_vertex = live_key
        writer.writerow([group_index, len(visual_ids), ";".join(map(str, visual_ids)),
            material.decode("ascii"), base_texture.decode("ascii"), vb_id, vb_base, vb_count,
            ib_id, ib_base, ib_count, pso_uid, vertex_version, index_version, index_offset,
            live_index_count, index_type, base_vertex,
            len(tree_lighting_by_resource[key])])
receipt = {
    "schema": "frame700-visible-main-flora-group-census/v1",
    "frame": 700,
    "scope": "Visible main Flora draws only; source/material grouping reconciled to native fetch records by ordinal.",
    "inputs": {
        "active_level": str(level),
        "active_level_sha256": digest(level),
        "source_order_script": str(SOURCE_ORDER / "census_source_order.py"),
        "frame700_ids": str(IDS),
        "frame700_ids_sha256": digest(IDS),
        "frame700_fetch": str(FETCH),
        "frame700_fetch_sha256": digest(FETCH),
        "group_members_csv": str(GROUPS_CSV),
        "group_members_csv_sha256": digest(GROUPS_CSV),
    },
    "checks": {
        "visible_ids_unique": len(visible) == len({visual_id for visual_id, _ in visible}),
        "id_materials_match_source": True,
        "ordinal_materials_match_live_psos": True,
        "source_resource_to_live_fetch_partition_bijective": True,
    },
    "counts": {
        "visible_draws": len(visible),
        "unique_source_geometry_material_base_texture_groups": len(resource_to_live),
        "unique_live_geometry_index_pso_signatures": len(live_to_resource),
        "distinct_vertex_buffer_versions": len({row["vertex_version_uid"] for row in rows}),
        "distinct_index_buffer_versions": len({row["index_version_uid"] for row in rows}),
        "draws_reducible_if_every_group_were_combined": len(visible) - len(resource_to_live),
        "theoretical_submit_reduction_fraction": round(1.0 - len(resource_to_live) / len(visible), 8),
        "group_size_histogram": {str(size): count for size, count in sorted(sizes.items())},
        "groups_with_multiple_tree_lighting_payloads": lighting_variant_groups,
        "groups_total": len(tree_lighting_by_resource),
        "draws_by_material": dict(collections.Counter(material for _, material in visible)),
        "draws_by_pso": dict(collections.Counter(row["pso_label"] for row in rows)),
    },
    "limitations": [
        "The submit reduction is a combinatorial upper bound, not a measured frame-time/FPS result.",
        "The source resource key includes mesh ranges, flora shader material, and base texture; the frame fetch adds actual PSO and buffer-version/range identity.",
        "No adapter batching was implemented; per-instance transforms, leaf fragment WV, and authored tree lighting remain distinct inputs.",
        "Any nonadjacent regrouping/reordering can change strict-depth/stencil winners at overlapping pixels and requires the main G-buffer oracle.",
    ],
}
OUT.write_text(json.dumps(receipt, indent=2) + "\n")
print(json.dumps(receipt["counts"], indent=2))
