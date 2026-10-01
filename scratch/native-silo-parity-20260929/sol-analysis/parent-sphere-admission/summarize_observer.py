#!/usr/bin/env python3
"""Validate source/accepted-view provenance before reporting parent-sphere ROI."""
from pathlib import Path
import argparse
import csv
import json
import math
import re
import struct

p = argparse.ArgumentParser()
p.add_argument("engine_log", type=Path)
p.add_argument("--frame", type=int, default=700)
p.add_argument("--e01", type=Path, required=True)
p.add_argument("--device-phases", type=Path, required=True)
p.add_argument("--require-corrected-r1", action="store_true")
p.add_argument("--output", type=Path)
a = p.parse_args()

def reject(reason):
    raise SystemExit("REJECT: " + reason)

prefixes = {
    "METAL_PARENT_SPHERE_CAPTURE ": "capture",
    "METAL_PARENT_SPHERE_PARENT ": "parents",
    "METAL_PARENT_SPHERE_CAPTURE_DONE ": "done",
    "METAL_PARENT_SPHERE_VIEW ": "view",
    "METAL_PARENT_SPHERE_FACE ": "faces",
}
data = {key: [] for key in prefixes.values()}
for line in a.engine_log.read_text(errors="replace").splitlines():
    for prefix, destination in prefixes.items():
        if prefix in line:
            row = dict(re.findall(r"([a-z][a-z0-9_]+)=([^\s]+)", line))
            if row.get("frame") != str(a.frame):
                reject("observer emitted a different frame; check the requested frame/environment")
            data[destination].append(row)
            break
if any(len(data[key]) != 1 for key in ("capture", "done", "view")):
    reject("require one capture, one done and one accepted view; duplicates/retries are unusable")
capture, done, view = (data[key][0] for key in ("capture", "done", "view"))

def integer(row, key):
    return int(row[key])

def bits(row, key, count):
    values = [int(value, 16) for value in row[key].split(".")]
    if len(values) != count or any(not 0 <= value <= 0xffffffff for value in values):
        reject("invalid " + key)
    floats = [struct.unpack("<f", struct.pack("<I", value))[0] for value in values]
    if not all(math.isfinite(value) for value in floats):
        reject("nonfinite " + key)
    return values

if integer(capture, "source_view_matched") != 1 or integer(view, "valid") != 1:
    reject("Device VP did not match captured SetCacheXform view or accepted view is invalid")
if any(integer(row, key) != a.frame for row, key in (
        (capture, "device_frame"), (capture, "cache_frame"),
        (view, "snapshot_frame"), (view, "environment_frame"))):
    reject("source/cache/scene/snapshot/environment frame mismatch")
if integer(view, "environment_valid") != 1:
    reject("invalid current environment")
bits(capture, "device_vp_bits", 16)
for key, count in (("view_bits", 16), ("projection_bits", 16), ("eye_bits", 3)):
    if bits(capture, key, count) != bits(view, key, count):
        reject("accepted EngineScene " + key + " differs from source capture; stale/alternate view")
if a.require_corrected_r1 and (integer(capture, "r1_policy") != 1 or integer(capture, "allow_r1") != 0):
    reject("require R1 policy enabled with source R1 setting off for remaining-dynamic ROI")

parents = []
for row in data["parents"]:
    parent = {key: float(value) if key.startswith("sphere_") and key != "sphere_valid" else int(value)
              for key, value in row.items()}
    if any(parent[key] not in (0, 1) for key in
           ("source_static", "shadowed", "hud", "volumetric", "sphere_valid")):
        reject("invalid parent source/consumer boolean")
    if not parent["parent"] or parent["captured_records"] < 1 or parent["source_outside"] not in (-1, 0, 1):
        reject("invalid parent identity/count/classification")
    if parent["sphere_valid"]:
        if parent["source_outside"] == -1 or parent["sphere_radius"] <= 0 or not all(
                math.isfinite(parent[key]) for key in ("sphere_x", "sphere_y", "sphere_z", "sphere_radius")):
            reject("valid sphere has no usable finite classification")
    elif parent["source_outside"] != -1:
        reject("unqualified source sphere was classified")
    parents.append(parent)
by_parent = {row["parent"]: row for row in parents}
if len(by_parent) != len(parents) or len(parents) != integer(done, "parents"):
    reject("duplicate or unreconciled source parents")
if sum(row["captured_records"] for row in parents) != integer(done, "records"):
    reject("captured record count does not reconcile")
if a.require_corrected_r1 and any(row["source_static"] for row in parents):
    reject("corrected R1-off snapshot unexpectedly contains source-static parents")

faces = [{key: int(value) for key, value in row.items()} for row in data["faces"]]
if len({row["light"] for row in faces}) != len(faces) or len({(row["batch"], row["tile"]) for row in faces}) != len(faces):
    reject("duplicate light/tile rows; inspect retry/discard")
per_parent = {}
counter_names = ("material_rows", "static_geometry_draws", "dynamic_geometry_casters", "flora_shadow_draws", "transform_bytes")
for face in faces:
    parent = by_parent.get(face["parent"])
    if not parent or face["source_static"] != parent["source_static"] or face["type"] != parent["type"] or not parent["shadowed"]:
        reject("final shadow face has no matching immutable captured parent")
    if face["transform_bytes"] != 256 * face["material_rows"] or any(face[key] < 0 for key in counter_names):
        reject("negative/unreconciled existing work proxy")
    per_parent.setdefault(face["parent"], []).append(face)
for identity, group in per_parent.items():
    parent = by_parent[identity]
    ordinals = {row["face"] for row in group}
    if (parent["type"] == 2 and (len(group) != 6 or ordinals != set(range(6)))) or (
            parent["type"] == 1 and (len(group) != 1 or ordinals != {0})) or parent["type"] not in (1, 2):
        reject("require complete ordinary point/spot packages; disable face-cull/reordering experiments")
    if len(group) > parent["captured_records"]:
        reject("rendered group exceeds captured records")

e01 = [row for row in csv.DictReader(a.e01.open()) if row["engine_frame"] == str(a.frame)]
phases = [row for row in csv.DictReader(a.device_phases.open()) if row["engine_frame"] == str(a.frame)]
if len(e01) != 1 or e01[0]["status"] != "completed" or len(phases) != 1 or phases[0]["phase"] != "1":
    reject("require exactly one completed acquisition and one measured phase1 device frame")

outside_ids = {row["parent"] for row in parents if row["source_outside"] == 1}
ordinary_ids = {row["parent"] for row in parents if not row["source_static"] and
                not row["hud"] and not row["volumetric"] and not integer(view, "scope_receivers")}
outside_faces = [row for row in faces if row["parent"] in outside_ids]
ordinary_faces = [row for row in outside_faces if row["parent"] in ordinary_ids]
unknown_faces = [row for row in faces if by_parent[row["parent"]]["source_outside"] == -1]
inside_faces = [row for row in faces if by_parent[row["parent"]]["source_outside"] == 0]

def workload(rows):
    return {"parents": len({row["parent"] for row in rows}), "faces": len(rows),
            **{key: sum(row[key] for row in rows) for key in counter_names}}

out = {"schema": "parent-sphere-admission-observer-summary/v1", "frame": a.frame,
       "provenance": "PASS: Device VP exactly recomposed from current cached matrices; accepted scene view/projection/eye bits identical",
       "note": "Source sphere classification and existing work counts only. No draw suppression, CPU/GPU/FPS saving or full consumer/visual acceptance.",
       "settings": {key: integer(capture, key) for key in ("r1_policy", "allow_r1")},
       "scene_consumers": {key: integer(view, key) for key in ("scope_receivers", "hud_receivers", "volume_frame")},
       "captured_parents": len(parents), "captured_records": integer(done, "records"),
       "captured_unknown_sphere_parents": sum(row["source_outside"] == -1 for row in parents),
       "captured_source_outside_parents": len(outside_ids),
       "actual_rendered": workload(faces), "source_outside_actual_rendered": workload(outside_faces),
       "source_inside_actual_rendered": workload(inside_faces),
       "unknown_source_sphere_actual_rendered": workload(unknown_faces),
       "ordinary_dynamic_source_outside_actual_rendered": workload(ordinary_faces),
       "completed_e01": {key: int(e01[0][key]) for key in ("serial", "engine_frame", "indexed_draws", "pso_binds", "updated_bytes")},
       "device_phase": phases[0], "capture": capture, "accepted_view": view,
       "parents": parents, "faces": faces}
if out["actual_rendered"]["static_geometry_draws"] + out["actual_rendered"]["flora_shadow_draws"] > out["completed_e01"]["indexed_draws"]:
    reject("selected draw proxies exceed completed frame draws")
serialized = json.dumps(out, indent=2) + "\n"
if a.output:
    a.output.write_text(serialized)
print(serialized)
