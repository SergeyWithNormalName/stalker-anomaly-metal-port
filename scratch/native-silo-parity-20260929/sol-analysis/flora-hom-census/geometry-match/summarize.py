#!/usr/bin/env python3
"""Hash exact HOM/level/parser inputs and validate the triangle-screen output."""
import csv, hashlib, json, struct
from pathlib import Path

STAGE=Path(__file__).resolve().parent
REPO=STAGE.parents[4]
RUN=REPO/"scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/runtime/gamedata/levels/l01_escape"
HOM=REPO/"scratch/release_runs/20260928-light-hom-assets/extracted/level_l01_escape__004__level.hom"
PARSER=REPO/"scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/draft/src/Layers/xrRenderMetal"
SOURCES=[STAGE/"screen.cpp",STAGE/"run.sh",PARSER/"R4LevelGeometry.cpp",PARSER/"R4LevelGeometry.h",REPO/"src/Layers/xrRender/HOM.cpp",REPO/"src/xrCDB/xrCDB_Collector.cpp",REPO/"src/Layers/xrRender/occRasterizer_core.cpp",REPO/"src/Layers/xrRenderMetal/MetalR4EngineScene.mm",REPO/"src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp",REPO/"build/metal-shader-sources/r3/deffer_terrain_high_flat_d.ps"]
def digest(p):
 b=p.read_bytes();return {"path":str(p),"size_bytes":len(b),"sha256":hashlib.sha256(b).hexdigest()}

hom=HOM.read_bytes()
assert len(hom)==56580 and hashlib.sha256(hom).hexdigest()=="9eee6174792357c0c18276f6240da662a57930b83117ad95857058fb785bd4e8"
assert struct.unpack_from("<II",hom,12)==(1,56560)
flags=[struct.unpack_from("<I",hom,20+i*40+36)[0] for i in range(1414)]
assert len(flags)==1414 and set(flags)=={1}
rows=list(csv.DictReader((STAGE/"matches.csv").open(newline="")))
screen=(STAGE/"screen-summary.txt").read_text().strip()
assert "hom_triangles=1414" in screen and "raw_matches=0" in screen and "unique_matches=0" in screen
assert not rows
inputs=[digest(RUN/"level"),digest(RUN/"level.geom"),digest(HOM)]+[digest(p) for p in SOURCES]
summary={"schema":"r4-hom-triangle-geometry-screen/v1","result":"zero_matches",
 "hom":{"triangles":1414,"flags_histogram":{"1":1414},"double_sided_raster_flag":True},
 "active_level_geometry":{"all_indexed_triangles":8303157,"mt_normal_triangles":1242810,"local_mt_normal_triangles_with_centroid_within_150_02m_of_baseline_camera":266226,"malformed_triangles":0},
 "match_test":{"raw_float32_triangle_identity":"all HOM versus all valid MT_NORMAL indexed triangles; vertex order ignored for geometric identity and winding retained in output","collector_packed_equivalence":"HOM centroid query radius 150m; active triangle coverage radius 150.02m to include the source 0.01m VPack match margin; exact CHOM VPack component rule (abs delta < 0.01 on each axis); 0.1m spatial grid with adjacent-cell search","baseline_camera_xyz":[-221.082565,-18.284866,-168.182037],"matching_rows":len(rows)},
 "interpretation":{"geometry_verified":False,"candidate_occluder_count":0,"suppression":False,"fps_claim":False,"opacity_claim":False,"reason":"No raw or source-VPack-equivalent triangle match was found in MT_NORMAL indexed geometry. MT_NORMAL+shader names do not establish opaque depth-writing material behavior; HOM flags are retained, and all 1414 are nonzero (double-sided HOM eligibility)."},
 "inputs":inputs,"output":{"matches_csv":digest(STAGE/"matches.csv"),"screen_summary":digest(STAGE/"screen-summary.txt")}}
(STAGE/"manifest.json").write_text(json.dumps(summary,indent=2)+"\n")
print(json.dumps({"validated":True,"matches":0,"hom_triangles":1414,"mt_normal_triangles":1242810,"local_mt_normal_triangles":266226,"manifest":str(STAGE/"manifest.json")},indent=2))
