#!/usr/bin/env python3
"""Join the exact frame-700 no-HOM portal frusta to all Flora motion envelopes."""
from __future__ import annotations
import collections
import csv
import hashlib
import importlib.util
import json
import math
import struct
from pathlib import Path

ROOT=Path(__file__).resolve().parents[5]
HERE=Path(__file__).resolve().parent
PORTAL_TOOL=HERE/"portal_frusta_roi.py"
ENVELOPES=HERE/"all-geometry-envelopes.csv"
ENVELOPE_SUMMARY=HERE/"all-geometry-envelopes.summary.json"
FRAME=ROOT/"scratch/native-silo-parity-20260929/runs/source-topology-wind-census-01/native/output/source-topology.json"
CAPTURE=ROOT/"scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv"
MESH=ROOT/"scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/wind-bounds/mesh-extrema/flora-mesh-extrema.csv"
OUT_CSV=HERE/"flora-portal-envelope-roi.csv"
OUT_JSON=HERE/"flora-portal-envelope-roi.summary.json"
EXPECTED_MESH_SHA="3d15d90548265cb9be3ec9b01c4e45ecfae4f240dada0a157346dee858f83605"
EXPECTED_CAPTURE_SHA="dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc"
EXPECTED_SECTOR_SOURCE_SHA="bdb7fdbd7b3be3dc333a252744af8f0e0f6fc50e877db793640e64abf06823b5"


def need(ok,msg):
    if not ok: raise SystemExit(msg)

def digest(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def f32(x): return struct.unpack("<f",struct.pack("<f",float(x)))[0]
def float_bits(x): return struct.unpack("<I",struct.pack("<f",f32(x)))[0]
def load_module(path,name):
    spec=importlib.util.spec_from_file_location(name,path)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module

def main():
    p=load_module(PORTAL_TOOL,"portal_frusta_roi_module")
    need(digest(MESH)==EXPECTED_MESH_SHA,"indexed mesh-extrema hash mismatch")
    need(digest(CAPTURE)==EXPECTED_CAPTURE_SHA,"exact main-Flora capture hash mismatch")
    need(digest(ROOT/"src/Layers/xrRender/r__sector.cpp")==EXPECTED_SECTOR_SOURCE_SHA,"live portal source hash changed")
    frame,a,b,lod_a,lod_b=p.compute_ssa_settings()
    visuals,sectors,portals,level_sha=p.parse_level()
    need(level_sha==p.EXPECTED_LEVEL,"active level hash mismatch")
    eye=p.bits_floats(frame["eye_bits"])
    full=p.bits_floats(frame["full_transform_bits"])
    initial=p.Frustum.from_matrix(full)
    owners=p.owner_sector_map(visuals,sectors)
    ids=p.load_capture_ids(visuals)
    # This is the separately established unique nearest frame-700 CFORM result.
    start=25
    viewport=p.re_search_float_setting(p.ENGINE_LOG.read_text(errors="replace"),"viewport_near")
    draw_portals=(p.re_search_setting(p.USER_LTX.read_text(errors="replace"),"rs_render_portals")=="on")
    frusta,walk_counts,walk_uncertain=p.traverse(start,sectors,portals,initial,eye,full,6,draw_portals,
        float(frame["r_ssaDISCARD"]),lod_a,lod_b,viewport)

    rows={}
    with ENVELOPES.open(newline="",encoding="utf-8") as f:
        for row in csv.DictReader(f):
            vid=int(row["visual_id"])
            need(vid not in rows,f"duplicate wind-envelope ID {vid}")
            rows[vid]=row
    need(len(rows)==5316 and set(rows)=={vid for vid,_ in ids},"wind envelope IDs do not exactly cover captured IDs")
    ssa_rejected=sum(row["source_rejected"]=="1" for row in rows.values())
    need(ssa_rejected==1199,"wind envelope source-SSA flags changed")
    env_summary=json.loads(ENVELOPE_SUMMARY.read_text())
    need(env_summary["processed_visuals"]==5316 and env_summary["mesh_rows"]==5316,
        "wind envelope summary is not all-ID")
    need(env_summary["unknown_displacement_bounds"]>=0,"invalid unknown-bound summary")

    raw_by=collections.Counter()
    env_by=collections.Counter()
    detail=[]
    unknown_ids=[]
    for vid,material in ids:
        owner_set=owners.get(vid,set())
        need(len(owner_set)==1,f"Flora visual {vid} has nonunique sector owner {owner_set}")
        owner=next(iter(owner_set))
        need(owner in (3,30),f"unexpected flora sector {owner}")
        visual=visuals[vid]
        sphere=visual["sphere"]
        box=visual["box"]
        env=rows[vid]
        need(env["shader"]==("flora\\leaf_wave" if material=="leaf" else "flora\\trunk_wave"),
            f"source material mismatch for {vid}")
        for key,axis in (("sphere_x",0),("sphere_y",1),("sphere_z",2),("source_radius",3)):
            need(float_bits(env[key])==float_bits(sphere[axis]),f"mesh/envelope OGF sphere mismatch {vid}:{key}")
        owner_frusta=frusta.get(owner,[])
        raw_tests=[p.sphere_and_aabb_hits(f,sphere,box) for f in owner_frusta]
        raw_hit=any(hit for hit,_ in raw_tests)
        raw_uncertain=any(unc for _,unc in raw_tests)
        raw_by[(owner,"raw_visible" if raw_hit else "raw_excluded")]+=1
        status=env["status"]
        if status!="bounded":
            unknown_ids.append(vid)
            envelope_hit=True # Unknown is fail-open: never claim excluded.
            radius=None
        else:
            bounded_radius=float(env["adjusted_radius"])
            rest=float(env["rest_max_distance"])
            source_radius=float(env["source_radius"])
            need(math.isfinite(bounded_radius) and bounded_radius>=rest,
                f"wind envelope radius does not contain indexed rest mesh for {vid}")
            # Preserve the exact OGF bound too if it is slightly larger than
            # indexed mesh extrema after CSV rounding.
            radius=max(bounded_radius,source_radius)
            # The indexed world mesh lies inside rest_max_distance of the exact
            # source sphere center; shader displacement is added outward. Test
            # that enclosing sphere against each retained sector frustum.
            envelope_hit=any(f.sphere_maybe(sphere[:3],radius) for f in owner_frusta)
        excluded=(not envelope_hit and status=="bounded")
        env_by[(owner,"envelope_visible_or_unknown" if not excluded else "envelope_excluded")]+=1
        if raw_hit and not envelope_hit:
            # This should be impossible for a valid rest+motion envelope; retain
            # explicit identity proof and fail rather than hiding disagreement.
            need(False,f"expanded envelope excluded raw-visible candidate {vid}")
        detail.append({
            "visual_id":vid,"material":material,"owner_sector":owner,
            "raw_ogf_bounds_intersect_any_frustum":int(raw_hit),
            "raw_boundary_guard_retained":int(raw_uncertain),
            "wind_envelope_status":status,
            "rest_max_distance":env["rest_max_distance"],
            "wind_displacement_bound":env["displacement_bound"],
            "envelope_radius":("" if radius is None else format(radius,".9g")),
            "envelope_intersects_any_frustum_or_unknown":int(envelope_hit),
            "excluded_by_all_nohom_owner_frusta":int(excluded),
            "reason":env["reason"],
        })

    raw_excluded=sum(v for (sector,key),v in raw_by.items() if key=="raw_excluded")
    safe_envelope_excluded=sum(v for (sector,key),v in env_by.items() if key=="envelope_excluded")
    raw_excluded_requalified=sum(1 for row in detail if row["raw_ogf_bounds_intersect_any_frustum"]==0 and row["excluded_by_all_nohom_owner_frusta"]==1)
    reopened_by_wind=sum(1 for row in detail if row["raw_ogf_bounds_intersect_any_frustum"]==0 and row["wind_envelope_status"]=="bounded" and row["envelope_intersects_any_frustum_or_unknown"]==1)
    raw_excluded_unknown=sum(1 for row in detail if row["raw_ogf_bounds_intersect_any_frustum"]==0 and row["wind_envelope_status"]!="bounded")
    raw_visible_unknown=sum(1 for row in detail if row["raw_ogf_bounds_intersect_any_frustum"]==1 and row["wind_envelope_status"]!="bounded")
    need(raw_excluded==4427,"corrected raw OGF test no longer matches corrected portal screen")
    need(raw_excluded_requalified+reopened_by_wind+raw_excluded_unknown==raw_excluded,"raw exclusion partition does not reconcile")
    with OUT_CSV.open("w",newline="",encoding="utf-8") as f:
        writer=csv.DictWriter(f,fieldnames=list(detail[0].keys()))
        writer.writeheader(); writer.writerows(detail)
    summary={
        "schema":"offline-portal-wind-envelope-roi/v1",
        "frame":700,"start_sector":start,"explicit_traversal_options":6,"captured_traversal_options":frame["traversal_options"],
        "hom_enabled":False,"rs_render_portals":("on" if draw_portals else "off"),"viewport_near_from_user_ltx_and_launch_log":viewport,
        "source_expression":"((options & VQ_FADE) | VQ_SSA) && rsDrawPortals; VQ_SSA is an unmasked source constant",
        "ssa_settings":{"discard":frame["r_ssaDISCARD"],"lod_a":lod_a,"lod_b":lod_b,"r2_ssa_lod_a":a,"r2_ssa_lod_b":b},
        "reached_sector_frusta":{str(k):len(v) for k,v in sorted(frusta.items())},"portal_walk_counts":dict(walk_counts),
        "walk_uncertain_fail_open":walk_uncertain,
        "captured_visual_ids":len(detail),"unique_owner_sector_ids":len({r["visual_id"] for r in detail}),
        "owner_counts":{"3":sum(1 for r in detail if r["owner_sector"]==3),"30":sum(1 for r in detail if r["owner_sector"]==30)},
        "raw_ogf_bounds_excluded":raw_excluded,"raw_ogf_bounds_visible":len(detail)-raw_excluded,
        "wind_envelope_unknown":len(unknown_ids),"wind_envelope_excluded_by_all_nohom_owner_frusta":safe_envelope_excluded,
        "raw_exclusions_still_excluded_after_wind_envelope":raw_excluded_requalified,
        "raw_exclusions_reopened_by_bounded_wind_envelope":reopened_by_wind,
        "raw_exclusions_fail_open_unknown_envelope":raw_excluded_unknown,
        "raw_visible_with_unknown_envelope":raw_visible_unknown,
        "raw_exclusions_reopened_by_wind_or_unknown":reopened_by_wind+raw_excluded_unknown,
        "near_portal_sphere_candidates":walk_counts.get("near_portal_sphere_candidates",0),
        "near_portal_branch_selected":walk_counts.get("near_portal_branch_selected",0),
        "clipped_portal_sizes_over_six":walk_counts.get("source_simplify_fallbacks_over6",0),
        "mesh_extrema_sha256":digest(MESH),"wind_envelope_csv_sha256":digest(ENVELOPES),
        "capture_sha256":digest(CAPTURE),"active_level_sha256":level_sha,"r__sector_cpp_sha256":digest(ROOT/"src/Layers/xrRender/r__sector.cpp"),
        "meaning":"Conservative per-indexed-mesh world sphere plus same-frame SSFX motion envelope against offline no-HOM portal frusta. Candidate geometry exclusions only; not source draw admission, visual correctness, savings, or suppression authorization.",
        "limitations":[
            "Portal frusta are a standalone float64 mirror with fail-open guards, not engine CFrustum/OPCODE execution.",
            "No HOM portal or static visual tests; options6 is the requested visibility superset of captured options7.",
            "No source add_Static ancestor/full-visible shortcuts or MT_LOD behavior is applied to per-ID candidates.",
            "The frame record does not itself include live ViewportNear; the same-run user.ltx and launch log both report 0.2.",
            "Wind bounds reuse the pinned all-indexed-vertex frame-700 analytic screen and fail open unknown visuals; same-frame only.",
        ],
    }
    OUT_JSON.write_text(json.dumps(summary,indent=2,allow_nan=False)+"\n")
    print(json.dumps({k:summary[k] for k in ("reached_sector_frusta","portal_walk_counts","captured_visual_ids","owner_counts","raw_ogf_bounds_excluded","wind_envelope_unknown","wind_envelope_excluded_by_all_nohom_owner_frusta","raw_exclusions_still_excluded_after_wind_envelope","raw_exclusions_reopened_by_wind_or_unknown")},indent=2))

if __name__=="__main__":
    main()
