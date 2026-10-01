#!/usr/bin/env python3
"""Summarize frozen one-frame Flora shadow state rows; never start the game."""
from pathlib import Path
from collections import defaultdict
import argparse
import csv
import json
import re

p = argparse.ArgumentParser()
p.add_argument("engine_log", type=Path)
p.add_argument("--e01", type=Path)
p.add_argument("--device-phases", type=Path)
p.add_argument("--engine-frame", type=int)
p.add_argument("--output", type=Path)
a = p.parse_args()
text = a.engine_log.read_text(errors="replace")
if "METAL_FLORA_SHADOW_STATE_CENSUS_TRUNCATED" in text:
    raise SystemExit("REJECT: census was truncated")
rows = []
done = []
for line in text.splitlines():
    if "METAL_FLORA_SHADOW_STATE_CENSUS " in line:
        fields = dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
        rows.append({k: v if k == "family" else int(v) for k, v in fields.items()})
    elif "METAL_FLORA_SHADOW_STATE_CENSUS_DONE " in line:
        done.append({k: int(v) for k, v in re.findall(r"([a-z_]+)=([^\s]+)", line)})
if not rows:
    raise SystemExit("REJECT: no census rows")
frames = {row["frame"] for row in rows}
if len(frames) != 1 or len(done) != 1 or done[0]["frame"] not in frames:
    raise SystemExit("REJECT: expected exactly one finished census frame")
frame = next(iter(frames))
if a.engine_frame is not None and frame != a.engine_frame:
    raise SystemExit("REJECT: census engine frame differs from requested frame")
if done[0]["unique_views"] != len(rows) or {row["view"] for row in rows} != set(range(1, len(rows) + 1)):
    raise SystemExit("REJECT: census rows are missing or duplicated")
for row in rows:
    groups = row["selected_groups"]
    if groups > row["total_groups"] or row["instances"] > row["broad_candidates"]:
        raise SystemExit("REJECT: caster/group counts violate selection contract")
    if not (0 <= row["current_pso_binds"] <= groups and
            0 <= row["grouped_pso_binds"] <= min(2, row["current_pso_binds"])):
        raise SystemExit("REJECT: exact-PSO partition count is inconsistent")
    if any(not 0 <= row[name] <= groups for name in ("current_vb_binds", "grouped_vb_binds")):
        raise SystemExit("REJECT: VB bind count is inconsistent")

totals = defaultdict(lambda: defaultdict(int))
for row in rows:
    if row["family"] not in {"local", "sun"}:
        raise SystemExit("REJECT: unknown census family")
    for family in (row["family"], "all"):
        t = totals[family]
        t["views"] += 1
        t["nonempty_views"] += row["selected_groups"] != 0
        for name in ("selected_groups", "broad_candidates", "instances", "current_pso_binds",
                     "grouped_pso_binds", "current_vb_binds", "grouped_vb_binds"):
            t[name] += row[name]
for t in totals.values():
    for prefix in ("current", "grouped"):
        for state in ("pso", "vb"):
            t[f"{prefix}_{state}_transitions"] = t[f"{prefix}_{state}_binds"] - t["nonempty_views"]
    t["avoided_pso_binds"] = t["current_pso_binds"] - t["grouped_pso_binds"]
    t["extra_vb_binds"] = t["grouped_vb_binds"] - t["current_vb_binds"]
out = {"schema": "flora-shadow-state-census-summary/v1", "engine_frame": frame,
       "note": "Counts only. No CPU/GPU time or FPS saving is inferred.", "totals": dict(totals),
       "completed_e01": None, "device_phase": None, "rows": rows}
if a.e01:
    completed = [row for row in csv.DictReader(a.e01.open())
                 if int(row["engine_frame"]) == frame and row["status"] == "completed"]
    if len(completed) != 1:
        raise SystemExit("REJECT: require exactly one completed E01 acquisition at census frame")
    r = completed[0]
    out["completed_e01"] = {name: int(r[name]) for name in
                             ("serial", "engine_frame", "indexed_draws", "pso_binds", "updated_bytes")}
    if totals["all"]["current_pso_binds"] > int(r["pso_binds"]):
        raise SystemExit("REJECT: census PSO count exceeds E01 total")
    if totals["all"]["selected_groups"] > int(r["indexed_draws"]):
        raise SystemExit("REJECT: census selected groups exceed E01 indexed draws")
    out["pso_request_fraction_of_frame"] = totals["all"]["current_pso_binds"] / max(1, int(r["pso_binds"]))
    out["avoided_pso_request_fraction_of_frame"] = totals["all"]["avoided_pso_binds"] / max(1, int(r["pso_binds"]))
if a.device_phases:
    matches = [r for r in csv.DictReader(a.device_phases.open()) if int(r["engine_frame"]) == frame]
    if len(matches) != 1 or int(matches[0]["phase"]) != 1:
        raise SystemExit("REJECT: require exactly one measured device-phase row at census frame")
    out["device_phase"] = matches[0]
serialized = json.dumps(out, indent=2) + "\n"
if a.output:
    a.output.write_text(serialized)
print(serialized)
