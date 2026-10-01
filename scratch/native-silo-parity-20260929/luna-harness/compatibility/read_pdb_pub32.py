#!/usr/bin/env python3
"""Read-only bounded PE/PDB identity + CodeView S_PUB32 extractor (stdlib only)."""
from __future__ import annotations
import argparse, math, struct, uuid
from pathlib import Path

MSF_MAGIC = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\x00\x00\x00"
S_PUB32 = 0x110E

def need(ok: bool, msg: str) -> None:
    if not ok:
        raise ValueError(msg)

def u16(b: bytes, o: int) -> int:
    need(0 <= o <= len(b)-2, f"u16 outside buffer @{o}")
    return struct.unpack_from('<H', b, o)[0]

def u32(b: bytes, o: int) -> int:
    need(0 <= o <= len(b)-4, f"u32 outside buffer @{o}")
    return struct.unpack_from('<I', b, o)[0]

def pe_debug_identity(pe: bytes):
    peoff = u32(pe, 0x3c)
    need(pe[peoff:peoff+4] == b'PE\0\0', 'missing PE signature')
    coff = peoff + 4
    nsec, optsize = u16(pe, coff+2), u16(pe, coff+16)
    opt = coff+20
    magic = u16(pe, opt)
    need(magic in (0x10b,0x20b), f'unsupported PE optional header {magic:#x}')
    imagebase = struct.unpack_from('<Q' if magic == 0x20b else '<I', pe, opt+24 if magic == 0x20b else opt+28)[0]
    dd = opt + (112 if magic == 0x20b else 96)
    debug_rva, debug_size = struct.unpack_from('<II', pe, dd+6*8)
    sections=[]; sh=opt+optsize
    for i in range(nsec):
        o=sh+i*40
        name=pe[o:o+8].split(b'\0',1)[0].decode('ascii','replace')
        vsize, va, rawsize, rawptr=struct.unpack_from('<IIII',pe,o+8)
        sections.append((name,va,max(vsize,rawsize),rawptr,rawsize))
    def rva_to_file(rva, size=1):
        for name,va,vspan,rawptr,rawsize in sections:
            if va <= rva and rva+size <= va+vspan:
                delta=rva-va
                need(delta+size <= rawsize, f'RVA {rva:#x} has no raw data in {name}')
                return rawptr+delta
        raise ValueError(f'RVA {rva:#x} not in PE sections')
    need(debug_rva and debug_size >= 28, 'no PE debug directory')
    dfo=rva_to_file(debug_rva,debug_size)
    for o in range(dfo,dfo+debug_size,28):
        typ=u32(pe,o+12); size=u32(pe,o+16); ptr=u32(pe,o+24)
        if typ==2 and size>=24 and pe[ptr:ptr+4]==b'RSDS':
            return uuid.UUID(bytes_le=pe[ptr+4:ptr+20]), u32(pe,ptr+20), imagebase, sections
    raise ValueError('no RSDS CodeView record')

def msf_streams(pdb: bytes):
    need(pdb[:32] == MSF_MAGIC, 'not MSF 7.00')
    block, _fpm, nblocks, dirbytes, _unknown, blockmap = struct.unpack_from('<6I',pdb,32)
    need(block in (512,1024,2048,4096), f'unsupported MSF block size {block}')
    need(nblocks*block == len(pdb), 'MSF block count does not match file length')
    n_dir_blocks=math.ceil(dirbytes/block)
    need(0 < n_dir_blocks <= block//4, 'invalid MSF directory block count')
    need((blockmap+1)*block <= len(pdb), 'MSF block-map page out of range')
    dirpages=struct.unpack_from('<'+'I'*n_dir_blocks,pdb,blockmap*block)
    directory=b''.join(pdb[p*block:(p+1)*block] for p in dirpages)[:dirbytes]
    nstreams=u32(directory,0); need(nstreams<100000,'implausible stream count')
    need(4+4*nstreams <= len(directory),'truncated stream-size table')
    sizes=struct.unpack_from('<'+'I'*nstreams,directory,4)
    pos=4+4*nstreams; streams=[]
    for size in sizes:
        if size==0xffffffff:
            streams.append(None); continue
        count=math.ceil(size/block)
        need(pos+4*count <= len(directory),'truncated stream block list')
        pages=struct.unpack_from('<'+'I'*count,directory,pos) if count else ()
        pos+=4*count
        need(all(p<nblocks for p in pages),'stream page out of range')
        data=b''.join(pdb[p*block:(p+1)*block] for p in pages)[:size]
        streams.append(data)
    need(pos==len(directory),'directory trailing bytes or malformed stream list')
    return streams

def parse(path_pe: Path, path_pdb: Path):
    pe=path_pe.read_bytes(); pdb=path_pdb.read_bytes()
    pe_guid,pe_age,imagebase,sections=pe_debug_identity(pe)
    streams=msf_streams(pdb)
    need(len(streams)>3 and streams[1] and streams[3],'missing PDB info/DBI streams')
    info=streams[1]; need(len(info)>=28,'short PDB info stream')
    version,signature,age=struct.unpack_from('<III',info,0)
    guid=uuid.UUID(bytes_le=info[12:28])
    need((guid,age)==(pe_guid,pe_age),f'PE/PDB mismatch PE={pe_guid}/{pe_age} PDB={guid}/{age}')
    dbi=streams[3]; need(len(dbi)>=64,'short DBI stream')
    public_idx=u16(dbi,16); symrec_idx=u16(dbi,20)
    need(public_idx<len(streams) and symrec_idx<len(streams),'DBI stream index out of range')
    public=streams[public_idx]; symrec=streams[symrec_idx]
    need(public and symrec,'public/symbol-record stream absent')
    pubs=[]; pos=0
    while pos+4 <= len(symrec):
        reclen=u16(symrec,pos); need(reclen>=2 and pos+2+reclen<=len(symrec),f'bad CodeView record at {pos:#x}')
        kind=u16(symrec,pos+2)
        if kind==S_PUB32:
            need(reclen>=13,f'short S_PUB32 at {pos:#x}')
            flags=u32(symrec,pos+4); off=u32(symrec,pos+8); seg=u16(symrec,pos+12)
            raw=symrec[pos+14:pos+2+reclen]
            name=raw.split(b'\0',1)[0].decode('utf-8','replace')
            pubs.append((name,flags,seg,off,pos))
        pos+=2+reclen
    need(pos==len(symrec),'symbol record stream ended mid-record')
    print(f'PE/PDB match GUID={guid} age={age}; PDB version={version} signature={signature:#x}')
    print(f'DBI: public stream={public_idx} bytes={len(public)}; symbol-record stream={symrec_idx} bytes={len(symrec)}; S_PUB32={len(pubs)}')
    for target in ('CModelPool::Exists','CModelPool::Create','CLevel::g_sv_Spawn','CLevel::cl_Process_Spawn','CGameObject::net_Spawn','CHangingLamp::net_Spawn'):
        found=[x for x in pubs if target.replace('::','@') in x[0] or target.split('::')[-1] in x[0] and target.split('::')[0] in x[0]]
        print(f'[{target}] matches={len(found)}')
        for name,flags,seg,off,record in found[:12]:
            section=sections[seg-1][0] if 0<seg<=len(sections) else '<invalid-segment>'
            sec_va=sections[seg-1][1] if 0<seg<=len(sections) else 0
            print(f'  name={name} flags={flags:#x} seg={seg} offset={off:#x} section={section} RVA={sec_va+off:#x} VA={imagebase+sec_va+off:#x} record={record:#x}')
    exists=next((x for x in pubs if x[0]=='?Exists@CModelPool@@QEAA_NPEBD@Z'),None)
    if exists:
        following=sorted((x for x in pubs if x[2]==exists[2] and x[3]>exists[3] and x[1]&2),key=lambda x:x[3])[:5]
        print('[function-boundary candidates after Exists]')
        for name,flags,seg,off,_ in following:
            print(f'  name={name} seg={seg} offset={off:#x} delta={off-exists[3]:#x}')

if __name__=='__main__':
    ap=argparse.ArgumentParser()
    ap.add_argument('exe',type=Path); ap.add_argument('pdb',type=Path)
    a=ap.parse_args(); parse(a.exe,a.pdb)
