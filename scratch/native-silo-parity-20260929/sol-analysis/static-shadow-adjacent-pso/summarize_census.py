#!/usr/bin/env python3
"""Read one frame-360 static-PSO census. No game/build/GPU actions."""
from pathlib import Path
import argparse
import csv
import json
import re

p = argparse.ArgumentParser()
p.add_argument("engine_log", type=Path)
p.add_argument("--e01", type=Path, required=True)
p.add_argument("--device-phases", type=Path)
p.add_argument("--output", type=Path)
p.add_argument("--reuse-on", action="store_true", help="Admit the candidate's actual cached PSO count")
a = p.parse_args()
rows = []
for line in a.engine_log.read_text(errors="replace").splitlines():
    if "METAL_LOCAL_STATIC_PSO_CENSUS " in line:
        rows.append({k: int(v) for k, v in re.findall(r"([a-z_]+)=([^\s]+)", line)})
if not rows or any(r["frame"] != 360 for r in rows):
    raise SystemExit("REJECT: expected frame-360 census rows")
keys = {(r["batch"], r["tile"]) for r in rows}
faces = {(r["light"], r["face"]) for r in rows}
if len(keys) != len(rows) or len(faces) != len(rows):
    raise SystemExit("REJECT: duplicated tile/face observations; inspect retry/oracle provenance")
for r in rows:
    if r["old_pso_binds"] != r["draws"] or not 0 <= r["cached_pso_binds"] <= r["draws"]:
        raise SystemExit("REJECT: inconsistent old/cached bind counts")
    if bool(r["draws"]) != bool(r["cached_pso_binds"]):
        raise SystemExit("REJECT: first bind must occur in every nonempty tile call")
completed = [r for r in csv.DictReader(a.e01.open()) if int(r["engine_frame"]) == 360 and r["status"] == "completed"]
if len(completed) != 1:
    raise SystemExit("REJECT: require exactly one completed E01 acquisition at frame360")
r = completed[0]
old = sum(x["old_pso_binds"] for x in rows)
cached = sum(x["cached_pso_binds"] for x in rows)
actual = cached if a.reuse_on else old
if actual > int(r["pso_binds"]) or sum(x["draws"] for x in rows) > int(r["indexed_draws"]):
    raise SystemExit("REJECT: census exceeds completed E01 work")
out = {"schema": "local-static-pso-census-summary/v1", "engine_frame": 360,
       "note": "Counts only. No time/FPS saving is inferred. Require no discard/error/retry at observed frame.",
       "tiles": len(rows), "merged_tiles": sum(x["merged"] != 0 for x in rows),
       "nonempty_tiles": sum(x["draws"] != 0 for x in rows),
       "old_pso_binds": old, "cached_pso_binds": cached, "avoided_pso_binds": old - cached,
       "reuse_on": a.reuse_on, "actual_static_pso_binds": actual,
       "indexed_draws_unchanged": old, "completed_e01": {k: int(r[k]) for k in
           ("serial", "engine_frame", "indexed_draws", "pso_binds", "updated_bytes")},
       "baseline_pso_request_total": int(r["pso_binds"]) + (old - cached if a.reuse_on else 0),
       "avoided_pso_request_fraction_of_frame": (old - cached) / max(1, int(r["pso_binds"]) + (old - cached if a.reuse_on else 0)),
       "device_phase": None, "rows": rows}
if a.device_phases:
    phases = [r for r in csv.DictReader(a.device_phases.open()) if int(r["engine_frame"]) == 360]
    if len(phases) != 1 or int(phases[0]["phase"]) != 1:
        raise SystemExit("REJECT: require measured phase1 at frame360")
    out["device_phase"] = phases[0]
serialized = json.dumps(out, indent=2) + "\n"
if a.output:
    a.output.write_text(serialized)
print(serialized)
