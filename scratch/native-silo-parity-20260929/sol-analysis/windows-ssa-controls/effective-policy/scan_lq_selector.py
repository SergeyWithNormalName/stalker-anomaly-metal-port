#!/usr/bin/env python3
"""Bounded, read-only scan of the pinned stock EXE's matching PDB publics."""
import importlib.util, json, re, sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[5]
COMPAT=ROOT/"scratch/native-silo-parity-20260929/luna-harness/compatibility/read_pdb_pub32.py"
spec=importlib.util.spec_from_file_location("pdbreader",COMPAT);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
EXE=ROOT/"scratch/native-silo-parity-20260929/runs/r1-final-weather-v3-01/silo/output/AnomalyDX11.stock.exe"
PDB=ROOT/"scratch/native-silo-parity-20260929/runs/r1-final-weather-v3-01/silo/runtime/bin/AnomalyDX11.pdb"
pe=EXE.read_bytes();pdb=PDB.read_bytes();guid,age,imagebase,sections=mod.pe_debug_identity(pe)
streams=mod.msf_streams(pdb);info=streams[1];dbi=streams[3]
import uuid
pdb_guid=uuid.UUID(bytes_le=info[12:28]);pdb_age=mod.u32(info,8)
assert (guid,age)==(pdb_guid,pdb_age),f"PE/PDB identity mismatch {(guid,age)} vs {(pdb_guid,pdb_age)}"
public_idx=mod.u16(dbi,16);symidx=mod.u16(dbi,20);symrec=streams[symidx]
pubs=[];pos=0
while pos+4<=len(symrec):
 n=mod.u16(symrec,pos);assert n>=2 and pos+2+n<=len(symrec)
 if mod.u16(symrec,pos+2)==mod.S_PUB32:
  flags=mod.u32(symrec,pos+4);off=mod.u32(symrec,pos+8);seg=mod.u16(symrec,pos+12)
  name=symrec[pos+14:pos+2+n].split(b"\0",1)[0].decode("utf-8","replace")
  section=sections[seg-1] if 0<seg<=len(sections) else ("<invalid>",0,0,0,0)
  pubs.append({"name":name,"flags":flags,"segment":seg,"offset":off,"section":section[0],"rva":section[1]+off,"va":imagebase+section[1]+off})
 pos+=2+n
assert pos==len(symrec)
terms=re.compile(r"rimp_select_sh_static|r_dtex_range|ps_r2_df_parallax_range|Calculate@CRender",re.I)
matches=[p for p in pubs if terms.search(p["name"])]
result={"exe":str(EXE),"pdb":str(PDB),"pe_pdb_match":True,"guid":str(guid),"age":age,"pdb_version":mod.u32(info,0),"public_stream":public_idx,"symbol_record_stream":symidx,"public_symbol_count":len(pubs),"matches":matches}
out=Path(__file__).resolve().parent/"leaf-lq-selector-publics.json";out.write_text(json.dumps(result,indent=2)+"\n")
print(json.dumps({"pe_pdb_match":True,"public_symbol_count":len(pubs),"matching_publics":len(matches),"output":str(out)},indent=2))
