#!/usr/bin/env python3
"""Small CPU-only selftest for parser/traversal fail-open contracts."""
import struct
import unittest
from portal_graph import Portal, Sector, Visual, Topology, Frustum, parse_level_bytes, walk, visible_id, f, vsub, cross, dot, mag

def ck(cid,payload): return struct.pack('<II',cid,len(payload))+payload
def ogf(children=()):
    h=struct.pack('<BBH10f',4,7,0,-1,-1,-1,1,1,1,0,0,0,1)
    out=ck(1,h)
    if children: out+=ck(10,struct.pack('<I'+'I'*len(children),len(children),*children))
    return out

def minimal_level(children=(), endpoint=0):
    visuals=ck(0,ogf(children))
    pids=struct.pack('<H',0) if endpoint is not None else b''
    sectors=ck(0,ck(1,pids)+ck(2,struct.pack('<I',0)))
    if endpoint is None: return ck(3,visuals)+ck(4,b'')+ck(8,sectors)
    pts=((3,-1,-1),(3,1,-1),(3,1,1),(3,-1,1))
    floats=[v for p in pts for v in p]+[0.0]*6
    rec=struct.pack('<HH18fI',0,endpoint,*floats,4)
    return ck(3,visuals)+ck(4,rec)+ck(8,sectors)

def square_portal(pid,front,back,x):
    poly=((f(x),-1.,-1.),(f(x),1.,-1.),(f(x),1.,1.),(f(x),-1.,1.))
    raw=cross(vsub(poly[1],poly[0]),vsub(poly[2],poly[1])); normal=tuple(f(q/mag(raw)) for q in raw)
    center=(f(x),0.,0.); radius=f(2.0**0.5); plane=(*normal,f(-dot(normal,poly[0])))
    return Portal(pid,front,back,poly,(*center,radius),plane)

def square_frustum():
    return Frustum(((1.,0.,0.,-100.),(-1.,0.,0.,-100.),(0.,1.,0.,-100.),(0.,-1.,0.,-100.),(0.,0.,1.,-100.),(0.,0.,-1.,-100.)))

class PortalGraphTests(unittest.TestCase):
    def test_malformed_sector_portal_ref_fails_open(self):
        top=parse_level_bytes(minimal_level(endpoint=9)); self.assertFalse(top.valid)
        result=walk(top,0,square_frustum(),(0.,)*3,(0.,)*16,0.2)
        self.assertFalse(result.valid); self.assertTrue(visible_id(top,result,77)[0])

    def test_hierarchy_cycle_rejects_topology(self):
        top=parse_level_bytes(minimal_level(children=(0,),endpoint=None))
        self.assertFalse(top.valid); self.assertIn('cycle',top.reason)
        self.assertTrue(visible_id(top,walk(top,0,square_frustum(),(0.,)*3,(0.,)*16,0.2),0)[0])

    def test_two_paths_keep_two_frusta_and_multiownership(self):
        # Diamond paths 0->1->3 and 0->2->3, plus a 3->1 cycle edge.
        sectors={0:Sector(0,(0,1),0),1:Sector(1,(0,2,4),1),
                 2:Sector(2,(1,3),2),3:Sector(3,(2,3,4),3)}
        portals={0:square_portal(0,1,0,3),1:square_portal(1,2,0,3),
                 2:square_portal(2,3,1,5),3:square_portal(3,3,2,5),
                 4:square_portal(4,1,3,7)}
        visuals={i:Visual(i,7,(-.1,-.1,-.1,.1,.1,.1),(0.,0.,0.,.1),()) for i in range(4)}
        owners={i:{i} for i in range(4)}; owners[3]={1,3}
        top=Topology(True,'',sectors,portals,visuals,owners)
        result=walk(top,0,square_frustum(),(0.,)*3,(0.,)*16,0.2,options=0)
        self.assertTrue(result.valid,result.reason)
        self.assertEqual(len(result.frusta.get(3,[])),2)
        self.assertGreaterEqual(result.counts['already_visited'],1)
        # Portal 0 was visited from sector 0, but is independently checked in sector 1.
        self.assertGreaterEqual(result.counts['portal_checks_sector_1'],3)
        self.assertEqual(top.owners[3],{1,3})

    def test_missing_visual_owner_never_excludes(self):
        top=Topology(True,'',{0:Sector(0,(),0)},{},
            {0:Visual(0,7,(-1,-1,-1,1,1,1),(0,0,0,1),())},{})
        result=walk(top,0,square_frustum(),(0.,)*3,(0.,)*16,0.2)
        self.assertTrue(result.valid)
        self.assertEqual(visible_id(top,result,0),(True,'fail-open-no-owner'))

    def test_nonfinite_view_input_disables_all_exclusions(self):
        top=Topology(True,'',{0:Sector(0,(),0)},{},
            {0:Visual(0,7,(-1,-1,-1,1,1,1),(0,0,0,1),())},{0:{0}})
        result=walk(top,0,square_frustum(),(float('nan'),0.,0.),(0.,)*16,0.2)
        self.assertFalse(result.valid)
        self.assertTrue(visible_id(top,result,0)[0])

if __name__=='__main__': unittest.main(verbosity=2)
