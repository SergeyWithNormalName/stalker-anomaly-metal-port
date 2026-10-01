#!/usr/bin/env python3
"""CPU-only exact-identity and binary-layout validator for the frame-700 census."""
import csv, hashlib, json, math, struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[3]
CAPTURE = REPO / "scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv"
BIN = ROOT / "main-flora-hom-frame-700.bin"
MAGIC = b"R4HOM1\0\0"
HEADER = 8 + 4 + 16*4 + 16*4 + 3*4
RECORD = 8 + 6*4

def sha(p):
    b=p.read_bytes(); return {"path":str(p),"size_bytes":len(b),"sha256":hashlib.sha256(b).hexdigest()}

rows=list(csv.reader(CAPTURE.open(newline="")))
assert rows[0]==["schema","main-flora-id-observer/v1"] and rows[1]==["engine_frame","700"]
meta={r[0]:r[1:] for r in rows[1:10]}
assert rows[10]==["visual_id","material"] and rows[-1]==["encode_complete","5316"]
expected=[]
for r in rows[11:-1]:
    assert len(r)==2 and r[1] in ("leaf","trunk")
    expected.append((int(r[0]),1 if r[1]=="leaf" else 2))
assert len(expected)==len(set(i for i,_ in expected))==5316
data=BIN.read_bytes()
assert len(data)==HEADER+RECORD*5316, (len(data),HEADER+RECORD*5316)
assert data[:8]==MAGIC and struct.unpack_from("<I",data,8)[0]==5316
cursor=12
for key,n in (("view_bits",16),("projection_bits",16),("eye_bits",3)):
    want=[int(x,16) for x in meta[key]]
    got=list(struct.unpack_from("<"+"I"*n,data,cursor)); assert got==want,(key,got,want)
    cursor+=n*4
actual=[]
for j in range(5316):
    ident,mat=struct.unpack_from("<II",data,cursor); cursor+=8
    vals=struct.unpack_from("<6f",data,cursor); cursor+=24
    assert all(math.isfinite(x) for x in vals)
    assert all(vals[a]<=vals[a+3] for a in range(3)),(ident,vals)
    actual.append((ident,mat))
assert actual==sorted(expected), "record ID/material sequence differs from capture"
assert cursor==len(data)

draft=REPO/"scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/draft/src/Layers/xrRenderMetal/R4LevelGeometry.cpp"
header= draft.with_suffix(".h")
level=REPO/"scratch/native-silo-parity-20260929/runs/ssa-main-ids-observe-01/native/runtime/gamedata/levels/l01_escape/level"
geom=level.with_name("level.geom")
manifest={"schema":"r4-flora-hom-census-manifest/v1","frame":700,"record_count":5316,
 "binary":{"path":BIN.name,"size_bytes":len(data),"sha256":hashlib.sha256(data).hexdigest(),"header_bytes":HEADER,"record_bytes":RECORD,"byte_order":"little-endian"},
 "inputs":[sha(CAPTURE),sha(level),sha(geom),sha(draft),sha(header)],
 "validation":{"cpu_only":True,"capture_ids_and_materials_exact":True,"capture_view_projection_eye_bits_exact":True,"all_bounds_finite_and_ordered":True,"duplicate_or_missing_ids":0,"record_order":"ascending visual_id"},
 "scope":{"geometry_verified":False,"suppression_enabled":False,"shipping_list":False,"fps_or_gpu_saving_claim":False,"bounds":"referenced indexed POSITION0 local AABB transformed at all eight corners by parsed modelTransform","pose":"rest pose; wind deformation unqualified","sphere_bounds_used":False}}
(ROOT/"manifest.json").write_text(json.dumps(manifest,indent=2)+"\n")
print(json.dumps({"validated":True,"header_bytes":HEADER,"record_bytes":RECORD,"records":len(actual),"binary_sha256":manifest["binary"]["sha256"],"manifest":str(ROOT/"manifest.json")},indent=2))
