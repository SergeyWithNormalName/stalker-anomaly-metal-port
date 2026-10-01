#!/usr/bin/env python3
"""Join immutable parent policy labels with actual completed frame360 faces."""
from pathlib import Path
import argparse
import csv
import json
import re

p = argparse.ArgumentParser()
p.add_argument("engine_log", type=Path)
p.add_argument("--e01", type=Path, required=True)
p.add_argument("--device-phases", type=Path, required=True)
p.add_argument("--output", type=Path)
a = p.parse_args()
parents, faces, done = [], [], []
for line in a.engine_log.read_text(errors="replace").splitlines():
    destination = (parents if "METAL_R1_STATIC_PARENT " in line else
                   faces if "METAL_R1_STATIC_SHADOW_FACE " in line else
                   done if "METAL_R1_STATIC_CAPTURE_DONE " in line else None)
    if destination is not None:
        destination.append({k: int(v) for k, v in re.findall(r"([a-z][a-z0-9_]+)=([^\s]+)", line)})
if not parents or not faces or len(done) != 1 or any(r["frame"] != 360 for r in parents + faces + done):
    raise SystemExit("REJECT: require one frame360 capture and actual shadow-face rows")
by_parent = {r["parent"]: r for r in parents}
if len(by_parent) != len(parents) or len({r["light"] for r in faces}) != len(faces):
    raise SystemExit("REJECT: duplicate capture/face rows; inspect retries")
if len({(r["batch"], r["tile"]) for r in faces}) != len(faces):
    raise SystemExit("REJECT: duplicated final shadow tile")
if done[0]["parents"] != len(parents) or done[0]["records"] != sum(r["captured_records"] for r in parents):
    raise SystemExit("REJECT: captured counts do not reconcile")
for r in parents:
    if not r["parent"] or not r["captured_records"] or r["allow_r1"] not in (0, 1):
        raise SystemExit("REJECT: invalid captured policy parent")
    if any(r[k] not in (0, 1) for k in ("source_static", "policy_reject", "shadowed", "hud", "volumetric")):
        raise SystemExit("REJECT: invalid captured boolean label")
    if r["allow_r1"] != done[0]["allow_r1"] or r["policy_reject"] != int(bool(r["source_static"]) and not r["allow_r1"]):
        raise SystemExit("REJECT: source static-switch predicate does not reconcile")
final_counts = {}
for r in faces:
    parent = by_parent.get(r["parent"])
    if not parent or r["source_static"] != parent["source_static"] or r["type"] != parent["type"] or not parent["shadowed"]:
        raise SystemExit("REJECT: final face has no matching captured parent/source flags")
    if (r["type"] == 2 and not 0 <= r["face"] < 6) or (r["type"] == 1 and r["face"] != 0):
        raise SystemExit("REJECT: invalid spot/OmniFace ordinal")
    if r["transform_bytes"] != 256 * r["material_rows"]:
        raise SystemExit("REJECT: transform payload does not reconcile")
    final_counts[r["parent"]] = final_counts.get(r["parent"], 0) + 1
if any(n > by_parent[parent]["captured_records"] for parent, n in final_counts.items()):
    raise SystemExit("REJECT: final face count exceeds captured records")
completed = [r for r in csv.DictReader(a.e01.open()) if r["engine_frame"] == "360" and r["status"] == "completed"]
phase = [r for r in csv.DictReader(a.device_phases.open()) if r["engine_frame"] == "360"]
if len(completed) != 1 or len(phase) != 1 or phase[0]["phase"] != "1":
    raise SystemExit("REJECT: require one completed E01 frame and measured device phase")
rejected = [r for r in faces if by_parent[r["parent"]]["policy_reject"]]
kept = [r for r in faces if not by_parent[r["parent"]]["policy_reject"]]
counter_names = ("material_rows", "static_geometry_draws", "dynamic_geometry_casters", "flora_shadow_draws", "transform_bytes")
totals = {name: {key: sum(r[key] for r in rows) for key in counter_names}
          for name, rows in (("all_rendered_faces", faces), ("policy_rejected_faces", rejected), ("policy_kept_faces", kept))}
if totals["all_rendered_faces"]["static_geometry_draws"] + totals["all_rendered_faces"]["flora_shadow_draws"] > int(completed[0]["indexed_draws"]):
    raise SystemExit("REJECT: measured draw proxies exceed completed frame indexed draws")
out = {"schema": "r1-static-policy-observer-summary/v1", "engine_frame": 360,
       "note": "Observed package/work counts only; no CPU/GPU/FPS saving. Dynamic caster counts are a proxy; DefVertex/LOD/detail work is excluded. Reject render error/discard/retry at this frame.",
       "allow_r1": done[0]["allow_r1"], "captured_parents": len(parents),
       "captured_records": done[0]["records"],
       "captured_source_static_parents": sum(bool(r["source_static"]) for r in parents),
       "captured_policy_rejected_parents": sum(r["policy_reject"] for r in parents),
       "captured_policy_rejected_records": sum(r["captured_records"] for r in parents if r["policy_reject"]),
       "rendered_parent_count": len(final_counts), "rendered_face_count": len(faces),
       "rendered_policy_rejected_parent_count": len({r["parent"] for r in rejected}),
       "rendered_policy_rejected_face_count": len(rejected),
       "rendered_policy_kept_face_count": len(kept), "totals": totals,
       "completed_e01": {k: int(completed[0][k]) for k in ("serial", "engine_frame", "indexed_draws", "pso_binds", "updated_bytes")},
       "device_phase": phase[0], "parents": parents, "faces": faces}
serialized = json.dumps(out, indent=2) + "\n"
if a.output:
    a.output.write_text(serialized)
print(serialized)
