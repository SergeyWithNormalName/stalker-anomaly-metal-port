#!/usr/bin/env python3
"""Source-family-only LMH census against the captured indoor07 C++ portal frusta.

The geometry/owner rows come from the stage10 portable R4LevelGeometry parser.
Portal plane values come from the exact frame700 source-topology receipt. This
script is a conservative census, not runtime material-admission proof.
"""
from __future__ import annotations
import csv, hashlib, json, math, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
RUN = ROOT / "scratch/native-silo-parity-20260929/runs/portal-pixel-oracle-indoor-07/native/output"
ANALYSIS = Path(__file__).resolve().parent
GEOM = RUN.parent / "runtime/gamedata/levels/l01_escape/level.geom"
ACTIVE = RUN / "active-engine-level.bin"
TOPOLOGY = RUN / "source-topology.json"
VALIDATION = ROOT / "scratch/native-silo-parity-20260929/sol-analysis/source-topology-wind-census/portal-architecture/general-context/runtime-validation/portal-pixel-oracle-indoor-07/source-observer-validation.json"
INPUTS = ANALYSIS / "lmh-source-upperbound.csv"
OUTCSV = ANALYSIS / "lmh-indoor07-opportunity-upperbound.csv"
SUMMARY = ANALYSIS / "lmh-indoor07-opportunity-summary.json"
EXPECTED_ACTIVE = "1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7"
EXPECTED_GEOM = "a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a"
EPS = 2.0e-5

def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""): h.update(block)
    return h.hexdigest()

def f32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", int(bits)))[0]

def matrix(bits: list[int]) -> list[float]:
    return [f32(x) for x in bits]

def mul(a: list[float], b: list[float]) -> list[float]:
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4))
            for c in range(4) for r in range(4)]

def point(m: list[float], p: tuple[float, float, float]) -> tuple[float, float, float, float]:
    x, y, z = p
    return (x*m[0]+y*m[4]+z*m[8]+m[12],
            x*m[1]+y*m[5]+z*m[9]+m[13],
            x*m[2]+y*m[6]+z*m[10]+m[14],
            x*m[3]+y*m[7]+z*m[11]+m[15])

def corners(lo, hi):
    return [(x,y,z) for x in (lo[0],hi[0]) for y in (lo[1],hi[1]) for z in (lo[2],hi[2])]

def camera_visible(row, full):
    try:
        lo = [float(row[f"min_{a}"]) for a in "xyz"]
        hi = [float(row[f"max_{a}"]) for a in "xyz"]
        model = [float(x) for x in row["model16"].split(";")]
        if len(model) != 16 or any(not math.isfinite(v) for v in lo+hi+model+full): return True
        wvp = mul(full, model)
        clips = [point(wvp, c) for c in corners(lo, hi)]
        if any(not all(math.isfinite(v) for v in c) for c in clips): return True
        planes = [all(c[0] < -c[3]-EPS for c in clips),
                  all(c[0] > c[3]+EPS for c in clips),
                  all(c[1] < -c[3]-EPS for c in clips),
                  all(c[1] > c[3]+EPS for c in clips),
                  all(c[2] < -EPS for c in clips),
                  all(c[2] > c[3]+EPS for c in clips)]
        return not any(planes)
    except (KeyError, ValueError, OverflowError):
        return True

def world_aabb(row):
    lo = [float(row[f"min_{a}"]) for a in "xyz"]
    hi = [float(row[f"max_{a}"]) for a in "xyz"]
    model = [float(x) for x in row["model16"].split(";")]
    transformed = [point(model, c)[:3] for c in corners(lo, hi)]
    if any(not all(math.isfinite(v) for v in c) for c in transformed): return None
    return tuple(min(c[i] for c in transformed) for i in range(3)), tuple(max(c[i] for c in transformed) for i in range(3))

def box_hits_frustum(box, frustum):
    lo, hi = box
    for plane in frustum["planes"]:
        nx, ny, nz, d = plane
        # CFrustum::AABB_OverlapPlane uses the negative support point. Add a
        # tiny fail-open tolerance around its strict >0 reject test.
        neg = (lo[0] if nx > 0 else hi[0],
               lo[1] if ny > 0 else hi[1],
               lo[2] if nz > 0 else hi[2])
        cls = nx*neg[0] + ny*neg[1] + nz*neg[2] + d
        if cls > EPS: return False
    return True

def portal_visible(row, owners, reached):
    if not owners or any(o < 0 or o >= len(reached) for o in owners): return True
    box = world_aabb(row)
    if box is None: return True
    for owner in owners:
        for frustum in reached[owner]["frusta"]:
            if box_hits_frustum(box, frustum): return True
    # A fully known owner list with no reached frusta has no visible portal path.
    return False

def main():
    topology = json.loads(TOPOLOGY.read_text())
    validation = json.loads(VALIDATION.read_text())
    active_sha, geom_sha = sha(ACTIVE), sha(GEOM)
    gates = (topology.get("complete") is True and topology.get("suppressed") == 0 and
             topology.get("frame") == 700 and topology.get("start_sector") == 25 and
             topology.get("observer_valid") is True and topology.get("view_match") is True and
             topology.get("full_transform_source_order_match") is True and
             topology.get("active_level_sha256") == EXPECTED_ACTIVE and
             topology.get("level_geom_sha256") == EXPECTED_GEOM and
             validation.get("passed") is True and validation.get("errors") == [] and
             all(validation.get("checks", {}).values()) and
             active_sha == EXPECTED_ACTIVE and geom_sha == EXPECTED_GEOM)
    if not gates: raise SystemExit("indoor07 input receipt gate failed")
    reached = topology["reached"]
    if len(reached) != 32 or sum(bool(x["frusta"]) for x in reached) != 6 or sum(len(x["frusta"]) for x in reached) != 8:
        raise SystemExit("unexpected sector/frustum topology")
    full = matrix(topology["full_transform_bits"])
    rows = list(csv.DictReader(INPUTS.open(newline="")))
    ids = [int(r["visual_id"]) for r in rows]
    if len(ids) != len(set(ids)) or any(r["texture_slot2"].lower().startswith("lmap") is False for r in rows):
        raise SystemExit("source-family rows are duplicated or outside the lmap prefix family")
    output = []
    camera_survivors = portal_exclusions = portal_visible_count = 0
    unknown_owner = invalid_bounds = 0
    excluded_ids = []
    for row in rows:
        vid = int(row["visual_id"])
        owners = [int(x) for x in row["owner_sector_ids"].split(";") if x]
        valid_bounds = row["bounds_available"] == "1" and row["ogf_bounds_valid"] == "1"
        camera = camera_visible(row, full) if valid_bounds else True
        vis = portal_visible(row, owners, reached) if valid_bounds else True
        if not owners: unknown_owner += 1
        if not valid_bounds: invalid_bounds += 1
        if camera:
            camera_survivors += 1
            if vis: portal_visible_count += 1
            else:
                portal_exclusions += 1
                excluded_ids.append(vid)
        output.append({"visual_id": vid, "shader_id": int(row["shader_id"]),
            "shader": row["shader_name"], "texture_slot2": row["texture_slot2"],
            "owners": owners, "indexed_bounds_valid": valid_bounds,
            "camera_survives_conservative_clip": camera,
            "reached_owner_frustum_intersects": vis,
            "upper_bound_portal_exclusion": bool(camera and valid_bounds and not vis)})
    with OUTCSV.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(output[0].keys())); w.writeheader(); w.writerows(output)
    result = {
      "schema":"indoor07-lmh-source-upperbound-v1", "frame":700,
      "interpretation":"parser-valid shader-slot-2 lmap-prefix source-family upper bound; runtime material/pipeline admission is not proven",
      "valid":True, "source_family_classifier":"geometry shader textures[2] begins with lmap (same conservative source classifier as existing R4LevelGeometryProbe --lmapped)",
      "active_level_sha256":active_sha, "level_geom_sha256":geom_sha,
      "topology_sha256":sha(TOPOLOGY), "validator_sha256":sha(VALIDATION), "parser_rows_sha256":sha(INPUTS),
      "active_eye_view_projection_full_transform_ssa":"pinned by source-topology.json and runtime validator",
      "start_sector":25, "reached_sectors":[i for i,x in enumerate(reached) if x["frusta"]],
      "reached_frusta":sum(len(x["frusta"]) for x in reached),
      "source_lmap_rows":len(rows), "unique_visual_ids":len(set(ids)),
      "valid_indexed_bounds":sum(x["indexed_bounds_valid"] for x in output),
      "unknown_owner_rows":unknown_owner, "invalid_bounds_rows":invalid_bounds,
      "ordinary_camera_survivors_conservative":camera_survivors,
      "portal_intersecting_among_camera_survivors":portal_visible_count,
      "portal_excluded_among_camera_survivors_upper_bound":portal_exclusions,
      "portal_excluded_visual_ids":excluded_ids,
      "method":"ordinary frustum uses source conservative eight-corner clip test with EPS fail-open; portal visibility uses captured C++ frustum planes and source CFrustum::AABB_OverlapPlane negative-support reject with EPS fail-open; all owners/frusta must reject before marking a candidate",
      "bounds":"all indexed POSITION0 bounds from staged stage10 R4LevelGeometry parser, transformed by OGF model matrix; unknown/malformed stays visible",
      "caveats":["not the exact runtime textureMaterialByVisual/pipeline-admitted LMH list", "no D3D/Windows count or FPS inference", "source candidate exclusions are not shipping cull admission or pixel-equivalence proof"],
      "result_csv_sha256":None
    }
    result["result_csv_sha256"] = sha(OUTCSV)
    SUMMARY.write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps({k:v for k,v in result.items() if k != "portal_excluded_visual_ids"}, indent=2))

if __name__ == "__main__": main()
