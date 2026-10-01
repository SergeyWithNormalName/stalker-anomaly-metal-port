#!/usr/bin/env python3
"""Value-owned, HOM-off CPU mirror of X-Ray R4 sector portal traversal.

All parsed data is copied from level bytes. No engine objects, renderer pointers,
CDB ownership, filesystem writes outside this scratch directory, or culling API.
"""
from __future__ import annotations
from dataclasses import dataclass, field
import collections
import hashlib
import math
import struct
from pathlib import Path

# Per source anchors: xrRenderPC_R4/r4_loader.cpp::b_portal/LoadSectors;
# xrRender/r__sector.cpp::CPortal::Setup, CSector::load, CreateFrustumFromPortal,
# CSector::traverse; xrCDB/Frustum.cpp::CreateFromMatrix, CreateFromPoints,
# ClipPoly, testSphere_dirty, testSAABB.
EPS_S = 1.0e-7
EPS_L = 1.0e-3
GUARD = 2.0e-4                 # outward only: uncertainty retains geometry
F32_EPS_NORM = 1.192092896e-05 # xrCore/_vector3d.h::exact_normalize


def f(x: float) -> float:
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]

def addf(a,b): return f(f(a)+f(b))
def subf(a,b): return f(f(a)-f(b))
def mulf(a,b): return f(f(a)*f(b))
def divf(a,b): return f(f(a)/f(b))
def sqrtf(a): return f(math.sqrt(max(0.0, f(a))))
def dot(a,b): return addf(addf(mulf(a[0],b[0]),mulf(a[1],b[1])),mulf(a[2],b[2]))
def vsub(a,b): return tuple(subf(a[i],b[i]) for i in range(3))
def vmad(a,d,t): return tuple(addf(a[i],mulf(d[i],t)) for i in range(3))
def cross(a,b):
    return (subf(mulf(a[1],b[2]),mulf(a[2],b[1])),
            subf(mulf(a[2],b[0]),mulf(a[0],b[2])),
            subf(mulf(a[0],b[1]),mulf(a[1],b[0])))
def mag(v): return sqrtf(dot(v,v))
def dist(a,b): return mag(vsub(a,b))
def plane_class(p, v): return addf(dot(p[:3],v),p[3])
def norm_precise(v):
    # exact_normalize uses double accumulation/rsqrt, then stores each component as float.
    sq=sum(float(f(x))*float(f(x)) for x in v)
    if not math.isfinite(sq) or sq <= F32_EPS_NORM:
        return None
    inv=1.0/math.sqrt(sq)
    n=tuple(f(float(x)*inv) for x in v)
    return n if all(math.isfinite(x) for x in n) else None

def norm_fast(v):
    # Fvector::normalize(v): float square magnitude, reciprocal sqrt, float stores.
    sq=dot(v,v)
    if not math.isfinite(sq) or sq<=0: return None
    inv=sqrtf(divf(1.0,sq))
    return tuple(mulf(x,inv) for x in v)

def precise_plane(a,b,c):
    n=norm_precise(cross(vsub(a,b),vsub(a,c)))
    if n is None: return None
    return (*n, f(-dot(n,a)))

def normalize_plane(p):
    length=mag(p[:3])
    if length == 0 or not math.isfinite(length): return None
    scale=divf(1.0,length)
    q=(*tuple(mulf(x,scale) for x in p[:3]),mulf(p[3],scale))
    return q if all(math.isfinite(x) for x in q) else None

@dataclass(frozen=True)
class Portal:
    id: int
    front: int
    back: int
    polygon: tuple[tuple[float,float,float],...]
    sphere: tuple[float,float,float,float]
    plane: tuple[float,float,float,float]

@dataclass(frozen=True)
class Sector:
    id: int
    portal_ids: tuple[int,...]
    root_id: int

@dataclass(frozen=True)
class Visual:
    id: int
    kind: int
    box: tuple[float,...]
    sphere: tuple[float,...]
    children: tuple[int,...]

@dataclass
class Topology:
    valid: bool
    reason: str
    sectors: dict[int,Sector] = field(default_factory=dict)
    portals: dict[int,Portal] = field(default_factory=dict)
    visuals: dict[int,Visual] = field(default_factory=dict)
    owners: dict[int,set[int]] = field(default_factory=dict)

@dataclass(frozen=True)
class Frustum:
    planes: tuple[tuple[float,float,float,float],...]

@dataclass
class WalkResult:
    valid: bool
    reason: str
    frusta: dict[int,list[Frustum]] = field(default_factory=dict)
    counts: collections.Counter = field(default_factory=collections.Counter)
    uncertain: list[str] = field(default_factory=list)


def fail(reason):
    return Topology(False, reason)

def chunks(data: bytes):
    out={}; pos=0
    while pos < len(data):
        if pos+8 > len(data): raise ValueError('truncated chunk header')
        cid,size=struct.unpack_from('<II',data,pos); end=pos+8+size
        if end>len(data): raise ValueError(f'chunk {cid} exceeds parent bounds')
        if cid in out: raise ValueError(f'duplicate chunk {cid}')
        out[cid]=data[pos+8:end]; pos=end
    return out

def parse_level_bytes(raw: bytes) -> Topology:
    try:
        top=chunks(raw)
        for required in (3,4,8):
            if required not in top: raise ValueError(f'missing level chunk {required}')
        vchunks=chunks(top[3]); schunks=chunks(top[8])
        visuals={}
        for vid,blob in vchunks.items():
            oc=chunks(blob); hdr=oc.get(1)
            if hdr is None or len(hdr)<44: raise ValueError(f'visual {vid} missing OGF header')
            version,kind,shader=struct.unpack_from('<BBH',hdr)
            if version != 4: raise ValueError(f'visual {vid} unsupported OGF version {version}')
            box=tuple(f(x) for x in struct.unpack_from('<6f',hdr,4))
            sphere=tuple(f(x) for x in struct.unpack_from('<4f',hdr,28))
            if not all(math.isfinite(x) for x in box+sphere) or sphere[3] < 0:
                raise ValueError(f'visual {vid} nonfinite/negative bounds')
            child_ids=()
            if 10 in oc:
                cb=oc[10]
                if len(cb)<4: raise ValueError(f'visual {vid} child header truncated')
                n=struct.unpack_from('<I',cb)[0]
                if len(cb)!=4+n*4: raise ValueError(f'visual {vid} child count mismatch')
                child_ids=struct.unpack_from('<'+'I'*n,cb,4) if n else ()
            visuals[vid]=Visual(vid,kind,box,sphere,tuple(child_ids))

        pdata=top[4]
        if len(pdata)%80: raise ValueError('portal chunk size is not 80-byte records')
        portals={}
        for pid in range(len(pdata)//80):
            rec=pdata[pid*80:(pid+1)*80]
            front,back=struct.unpack_from('<HH',rec)
            verts=struct.unpack_from('<18f',rec,4); n=struct.unpack_from('<I',rec,76)[0]
            if not 3<=n<=6: raise ValueError(f'portal {pid} vertex count out of range')
            poly=tuple(tuple(f(verts[i*3+j]) for j in range(3)) for i in range(n))
            if not all(math.isfinite(x) for p in poly for x in p): raise ValueError(f'portal {pid} nonfinite vertex')
            # CPortal::Setup: BB getsphere; averaged normalized fan normals.
            lo=tuple(min(p[k] for p in poly) for k in range(3)); hi=tuple(max(p[k] for p in poly) for k in range(3))
            center=tuple(mulf(addf(lo[k],hi[k]),0.5) for k in range(3)); radius=dist(center,hi)
            normal_sum=[f(0.0),f(0.0),f(0.0)]; normal_count=0
            for i in range(2,n):
                raw_n=cross(vsub(poly[i-1],poly[0]),vsub(poly[i],poly[0]))
                m=mag(raw_n)
                if m>f(1.0e-7):
                    sq=dot(raw_n,raw_n)
                    inv=sqrtf(divf(1.0,sq))
                    unit=tuple(mulf(x,inv) for x in raw_n) # Fvector::normalize_safe
                    normal_sum=[addf(normal_sum[k],unit[k]) for k in range(3)]
                    normal_count+=1
            if not normal_count: raise ValueError(f'portal {pid} degenerate')
            avg=tuple(divf(x,f(normal_count)) for x in normal_sum) # CPortal::Setup N.div(_cnt)
            normal=norm_fast(avg)
            if normal is None: raise ValueError(f'portal {pid} averaged normal degenerate')
            pplane=(*normal,f(-dot(normal,poly[0])))
            portals[pid]=Portal(pid,front,back,poly,(*center,radius),pplane)

        sectors={}
        for sid,blob in schunks.items():
            sc=chunks(blob)
            if 1 not in sc or 2 not in sc or len(sc[2])!=4 or len(sc[1])%2:
                raise ValueError(f'sector {sid} malformed portal/root chunks')
            pids=struct.unpack('<'+'H'*(len(sc[1])//2),sc[1]) if sc[1] else ()
            root=struct.unpack('<I',sc[2])[0]
            if root not in visuals: raise ValueError(f'sector {sid} missing root visual {root}')
            if any(p not in portals for p in pids): raise ValueError(f'sector {sid} references missing portal')
            sectors[sid]=Sector(sid,tuple(pids),root)
        if not sectors: raise ValueError('level contains no sectors')
        for p in portals.values():
            if p.front not in sectors or p.back not in sectors or p.front==p.back:
                raise ValueError(f'portal {p.id} references missing/identical endpoint sectors')
            if p.id not in sectors[p.front].portal_ids or p.id not in sectors[p.back].portal_ids:
                raise ValueError(f'portal {p.id} endpoint/sector adjacency is inconsistent')
        for sid,s in sectors.items():
            if len(set(s.portal_ids))!=len(s.portal_ids): raise ValueError(f'sector {sid} duplicates a portal reference')

        # Preserve multi-root/multi-sector ownership; reject hierarchy cycles or missing children.
        owners=collections.defaultdict(set)
        for sid,s in sectors.items():
            state={}; stack=[(s.root_id,False)]
            while stack:
                vid,leaving=stack.pop()
                if vid not in visuals: raise ValueError(f'sector {sid} child missing visual {vid}')
                if leaving:
                    state[vid]=2; continue
                if state.get(vid)==1: raise ValueError(f'visual hierarchy cycle in sector {sid} at {vid}')
                if state.get(vid)==2:
                    owners[vid].add(sid); continue
                state[vid]=1; owners[vid].add(sid); stack.append((vid,True))
                for child in reversed(visuals[vid].children):
                    if child not in visuals: raise ValueError(f'visual {vid} references missing child {child}')
                    if state.get(child)==1: raise ValueError(f'visual hierarchy cycle in sector {sid}: {vid}->{child}')
                    if state.get(child)!=2: stack.append((child,False))
        return Topology(True,'',sectors,portals,visuals,dict(owners))
    except (ValueError,struct.error,OverflowError) as e:
        return fail(str(e))


def initial_frustum(full):
    # Source CFrustum::CreateFromMatrix(FRUSTUM_P_LRTB | FRUSTUM_P_FAR).
    terms=[(-(full[3]+full[0]),-(full[7]+full[4]),-(full[11]+full[8]),-(full[15]+full[12])),
           (-(full[3]-full[0]),-(full[7]-full[4]),-(full[11]-full[8]),-(full[15]-full[12])),
           (-(full[3]-full[1]),-(full[7]-full[5]),-(full[11]-full[9]),-(full[15]-full[13])),
           (-(full[3]+full[1]),-(full[7]+full[5]),-(full[11]+full[9]),-(full[15]+full[13])),
           (-(full[3]-full[2]),-(full[7]-full[6]),-(full[11]-full[10]),-(full[15]-full[14]))]
    planes=[]
    for p in terms:
        q=normalize_plane(tuple(f(x) for x in p))
        if q is None: return None
        planes.append(q)
    return Frustum(tuple(planes))

def sphere_maybe(fr, sphere):
    return all(plane_class(p,sphere[:3]) <= f(sphere[3]+GUARD) for p in fr.planes)

def clip_poly(fr, poly):
    # Source ClipPoly strict negative/positive tests, float32 classify/D/denom/t/mad.
    cur=list(poly)
    for plane in fr.planes:
        cls=[plane_class(plane,p) for p in cur]
        if any(not math.isfinite(c) for c in cls): return None,'nonfinite clip classification'
        if any(abs(c)<=GUARD for c in cls):
            # Preserve the incoming whole frustum if a source side decision is ambiguous.
            return None,'clip boundary requires fail-open'
        out=[]; wrapped=cur+[cur[0]]; vals=cls+[cls[0]]
        for i in range(len(cur)):
            a,b,da,db=wrapped[i],wrapped[i+1],vals[i],vals[i+1]
            if all(abs(a[k]-b[k])<EPS_S for k in range(3)): continue
            if da<0:
                out.append(a)
                if db>0:
                    d=vsub(b,a); den=dot(plane[:3],d)
                    if den!=0: out.append(vmad(a,d,divf(-da,den)))
            elif db<0:
                d=vsub(b,a); den=dot(plane[:3],d)
                if den!=0: out.append(vmad(a,d,divf(-da,den)))
        if len(out)<3: return None,None
        if len(out)>48: return None,'clipped polygon exceeds FRUSTUM_SAFE'
        cur=out
    return cur,''

def from_portal(poly,eye,full):
    if len(poly)>6: return None,'source SimplifyPoly_AABB path left unimplemented'
    near=precise_plane(poly[0],poly[1],poly[2])
    if near is None: return None,'degenerate clipped portal plane'
    # Source reverses only for classify<0; ambiguity is widened to the incoming frustum.
    cls=plane_class(near,eye)
    if abs(cls)<=GUARD: return None,'portal orientation boundary'
    p=list(poly)
    if cls<0:
        p.reverse(); near=precise_plane(p[0],p[1],p[2])
        if near is None: return None,'degenerate reversed portal plane'
    planes=[]
    for i in range(1,len(p)):
        q=precise_plane(eye,p[i-1],p[i])
        if q is None: return None,'degenerate side plane'
        planes.append(q)
    q=precise_plane(eye,p[-1],p[0])
    if q is None: return None,'degenerate final side plane'
    planes.append(q); planes.append(near)
    far=normalize_plane(tuple(f(x) for x in (-(full[3]-full[2]),-(full[7]-full[6]),-(full[11]-full[10]),-(full[15]-full[14]))))
    if far is None: return None,'degenerate far plane'
    planes.append(far)
    return Frustum(tuple(planes)),''

def walk(top: Topology,start: int,base: Frustum,eye,full,viewport_near,options=6,draw_portals=False,discard=1.68038387e-5,lod_b=8.7791486294e-5,saved_eye=None):
    if not top.valid: return WalkResult(False,top.reason)
    if base is None: return WalkResult(False,'missing base frustum')
    if (len(eye)!=3 or len(full)!=16 or
        not all(math.isfinite(float(x)) for x in tuple(eye)+tuple(full)) or
        (saved_eye is not None and (len(saved_eye)!=3 or not all(math.isfinite(float(x)) for x in saved_eye))) or
        not math.isfinite(float(viewport_near)) or viewport_near<=0 or
        not math.isfinite(float(discard)) or discard<0 or
        not math.isfinite(float(lod_b)) or lod_b<0):
        return WalkResult(False,'nonfinite or semantically invalid camera/portal-threshold input')
    if not base.planes or any(len(p)!=4 or not all(math.isfinite(float(x)) for x in p) for p in base.planes):
        return WalkResult(False,'invalid base frustum planes')
    # Validate all value-owned references before traversal so malformed links
    # cannot pick a seemingly valid neighbor and create a false negative.
    for key,s in top.sectors.items():
        if key!=s.id or s.root_id not in top.visuals:
            return WalkResult(False,f'invalid sector/root identity {key}')
        for pid in s.portal_ids:
            p=top.portals.get(pid)
            if p is None or key not in (p.front,p.back):
                return WalkResult(False,f'inconsistent sector/portal adjacency {key}/{pid}')
    for key,p in top.portals.items():
        if key!=p.id or p.front not in top.sectors or p.back not in top.sectors:
            return WalkResult(False,f'invalid portal endpoint identity {key}')
        if key not in top.sectors[p.front].portal_ids or key not in top.sectors[p.back].portal_ids:
            return WalkResult(False,f'missing endpoint portal adjacency {key}')
    for key,v in top.visuals.items():
        if key!=v.id or any(child not in top.visuals for child in v.children):
            return WalkResult(False,f'invalid visual descendant identity {key}')
    for vid,owners in top.owners.items():
        if vid not in top.visuals or any(sid not in top.sectors for sid in owners):
            return WalkResult(False,f'invalid visual owner identity {vid}')
    if start not in top.sectors:
        return WalkResult(False,'invalid externally validated start sector or base frustum')
    result=WalkResult(True,''); visited=collections.defaultdict(set); active=set()
    def visit(sid,fr,depth=0):
        if depth>128:
            result.valid=False; result.reason='portal recursion depth exceeded'; return
        result.frusta.setdefault(sid,[]).append(fr); result.counts['sector_frusta']+=1
        if (sid,depth) in active:
            result.valid=False; result.reason='sector traversal recursion cycle'; return
        active.add((sid,depth))
        for pid in top.sectors[sid].portal_ids:
            result.counts['portals_checked']+=1
            result.counts[f'portal_checks_sector_{sid}']+=1
            if pid in visited[sid]: result.counts['already_visited']+=1; continue
            p=top.portals[pid]
            if not sphere_maybe(fr,p.sphere): result.counts['sphere_reject']+=1; continue
            center=p.sphere[:3]
            base_plane_class=plane_class(p.plane,eye)
            selection_eye=eye if saved_eye is None else saved_eye
            saved_distance=dist(center,selection_eye)
            saved_plane_class=plane_class(p.plane,selection_eye)
            if saved_distance < p.sphere[3]: result.counts['near_portal_sphere_candidates']+=1
            near=(saved_distance < p.sphere[3] and abs(saved_plane_class)<=f(viewport_near+EPS_L))
            if near:
                target=p.back if sid==p.front else p.front
            else:
                target=p.back if base_plane_class>0 else p.front
            if target==sid or target==start or target not in top.sectors:
                result.counts['sector_reject']+=1; continue
            # Exact active source expression: ((options & VQ_FADE) | VQ_SSA) && draw_portals.
            if (((options & 4) | 2) and draw_portals):
                pass
            else:
                if options & 2:
                    delta=vsub(center,eye); d2=dot(delta,delta)
                    if d2<=0 or not math.isfinite(d2):
                        result.valid=False; result.reason=f'portal {pid} SSA distance invalid'; return
                    direction=tuple(divf(x,sqrtf(d2)) for x in delta)
                    ssa=mul_f(divf(mulf(p.sphere[3],p.sphere[3]),d2),abs(dot(p.plane[:3],direction)))
                    if ssa < discard:
                        result.counts['ssa_discard_reject']+=1; continue
                    if options & 4 and ssa < lod_b:
                        result.counts['ssa_lod_reject']+=1; continue
            clipped,err=clip_poly(fr,p.polygon)
            if clipped is None:
                if err is None:
                    result.counts['clip_reject']+=1; continue
                # Ambiguous or bounded-out clip path: keep target reachable via incoming frustum.
                result.uncertain.append(f'portal {pid}: {err}'); next_fr=fr
            else:
                if len(clipped)>6:
                    result.uncertain.append(f'portal {pid}: clipped polygon >6'); next_fr=fr
                else:
                    next_fr,err=from_portal(clipped,eye,full)
                    if next_fr is None:
                        result.uncertain.append(f'portal {pid}: {err}'); next_fr=fr
            visited[sid].add(pid)
            result.counts['recurse']+=1
            visit(target,next_fr,depth+1)
            if not result.valid: return
        active.remove((sid,depth))
    visit(start,base)
    if not result.valid:
        # Invalid traversal means no exclusion is trustworthy.
        result.frusta={sid:[base] for sid in top.sectors}
    return result

def mul_f(a,b): return mulf(a,b)

def sphere_aabb_maybe(fr,sphere,box):
    # Source CFrustum::testSAABB rejection shape with outward guard at boundaries.
    center=sphere[:3]; r=sphere[3]
    for p in fr.planes:
        cls=plane_class(p,center)
        if cls>r+GUARD: return False
        if abs(cls)+GUARD>=r: continue
        neg=tuple(box[k] if p[k]>=0 else box[k+3] for k in range(3))
        if plane_class(p,neg)>GUARD: return False
    return True

def visible_id(top,result,vid):
    if not result.valid or vid not in top.visuals: return True,'fail-open-invalid'
    owners=top.owners.get(vid)
    if not owners: return True,'fail-open-no-owner'
    for sid in owners:
        if sid not in result.frusta: continue
        vis=top.visuals[vid]
        if any(sphere_aabb_maybe(fr,vis.sphere,vis.box) for fr in result.frusta[sid]):
            return True,'intersects'
    return False,'outside-all-owner-frusta'


def read_float_words(words): return struct.unpack('<'+'f'*len(words),struct.pack('<'+'I'*len(words),*words))
