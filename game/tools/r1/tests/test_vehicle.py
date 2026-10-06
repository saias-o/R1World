"""Shipped fleet contracts: dimensions, pivots, PBR separation and arena cost."""
from __future__ import annotations
import json
import math
import struct
import hashlib
import unittest
from pathlib import Path
from r1 import prepare_world
from r1.external_assets import GAME_ROOT, tree_model
from r1.vehicle_fleet import model
GAME = GAME_ROOT
CAR = model("city")


def document(path):
    payload=(GAME/path).read_bytes()
    length,kind=struct.unpack_from('<II',payload,12)
    assert kind==0x4e4f534a
    return json.loads(payload[20:20+length])


def mesh_data(path):
    payload=(GAME/path).read_bytes()
    length=struct.unpack_from('<I',payload,12)[0]
    doc=json.loads(payload[20:20+length])
    binary=payload[28+length:]
    def values(index):
        a=doc['accessors'][index];v=doc['bufferViews'][a['bufferView']]
        count=a['count']*(3 if a['type']=='VEC3' else 1)
        return struct.unpack_from('<'+('f' if a['componentType']==5126 else 'I')*count,
                                  binary,v.get('byteOffset',0)+a.get('byteOffset',0))
    result={}
    for n in doc['nodes']:
        if 'mesh' not in n:continue
        p=doc['meshes'][n['mesh']]['primitives'][0]
        flat=values(p['attributes']['POSITION']);indices=values(p['indices'])
        result[n['name']]=([flat[i:i+3] for i in range(0,len(flat),3)],indices)
    return result


def surface_x(mesh,y,z,side):
    """Intersections with the exported triangles along the mirror arm axis."""
    points,indices=mesh;hits=[]
    for i in range(0,len(indices),3):
        a,b,c=(points[k] for k in indices[i:i+3])
        by,bz=b[1]-a[1],b[2]-a[2];cy,cz=c[1]-a[1],c[2]-a[2]
        det=by*cz-bz*cy
        if abs(det)<1e-10:continue
        u=((y-a[1])*cz-(z-a[2])*cy)/det
        v=(by*(z-a[2])-bz*(y-a[1]))/det
        if u>=-1e-6 and v>=-1e-6 and u+v<=1+1e-6:
            x=side*(a[0]+u*(b[0]-a[0])+v*(c[0]-a[0]))
            if x>0:hits.append(x)
    return hits


def triangle_distance(p,a,b,c):
    dot=lambda x,y:sum(i*j for i,j in zip(x,y))
    sub=lambda x,y:tuple(i-j for i,j in zip(x,y))
    u,v,w=sub(b,a),sub(c,a),sub(p,a)
    uu,uv,vv,wu,wv=dot(u,u),dot(u,v),dot(v,v),dot(w,u),dot(w,v)
    det=uu*vv-uv*uv
    if det>1e-15:
        s=(wu*vv-wv*uv)/det;t=(wv*uu-wu*uv)/det
        if s>=0 and t>=0 and s+t<=1:
            return math.dist(p,tuple(a[i]+s*u[i]+t*v[i] for i in range(3)))
    distances=[]
    for start,end in ((a,b),(b,c),(c,a)):
        edge=sub(end,start);length=dot(edge,edge)
        t=max(0,min(1,dot(sub(p,start),edge)/length)) if length else 0
        distances.append(math.dist(p,tuple(start[i]+t*edge[i] for i in range(3))))
    return min(distances)


def car_node():
    scene=json.loads((GAME/'scenes/earth.scene').read_text(encoding='utf-8'))
    return next(n for n in scene['scene']['children'] if 'vehicle' in n.get('groups',[]))


class Fleet(unittest.TestCase):
    def setUp(self):
        self.manifest=json.loads((GAME/'assets/models/vehicles/fleet.json').read_text())

    def test_all_categories_and_two_lods_are_shipped(self):
        self.assertEqual({v['name'] for v in self.manifest['vehicles']},
                         {'city','sedan','suv','offroad','sport','truck','bus'})
        compact=next(v for v in self.manifest['vehicles'] if v['name']=='city')
        # The full source compact is a ~46k-triangle mesh. Close-up geometry
        # must never silently return to the reduced or procedural version.
        self.assertGreater(compact['near']['drawTriangles'],40000)
        self.assertGreater(compact['near']['vertices'],25000)
        self.assertLess(compact['far']['drawTriangles'],2500)
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                doc=document(v[lod]['path'])
                for image in doc.get('images',[]):
                    self.assertIn('bufferView',image,'Every detail texture must ship inside the GLB')
                    self.assertNotIn('uri',image)
                self.assertLessEqual(len(doc.get('images',[])),2)
                self.assertEqual({n['name'] for n in doc['nodes'] if n['name'].startswith('wheel-')},
                    {'wheel-front-left','wheel-front-right','wheel-back-left','wheel-back-right'})

    def test_budget_counts_every_exported_seam_and_resident_lod(self):
        total=0
        for v in self.manifest['vehicles']:
            for lod,limit in (('near',30000),('far',3000)):
                doc=document(v[lod]['path'])
                verts=sum(doc['accessors'][p['attributes']['POSITION']]['count']
                          for m in doc['meshes'] for p in m['primitives'])
                self.assertEqual(verts,v[lod]['vertices'])
                self.assertLessEqual(verts,limit,v['name']+' '+lod)
                total+=verts
                self.assertTrue(all(n.get('children') for n in doc['nodes']
                                    if n['name'].startswith('wheel-')))
            self.assertLess(v['far']['drawTriangles'],v['near']['drawTriangles']*.55)
        self.assertEqual(total,self.manifest['totalVertices'])
        self.assertLessEqual(total,80000,'Full source meshes and resident distance LODs must fit the arena')

    def test_paint_does_not_include_glass_rubber_or_lights(self):
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                doc=document(v[lod]['path'])
                roles={m['name'].split('-')[0] for m in doc['materials']}
                self.assertTrue({'paint','rubber','glass','mirror'}<=roles)
                if lod=='near':self.assertIn('alloy',roles)
                for node in doc['nodes']:
                    if 'mesh' not in node:continue
                    for primitive in doc['meshes'][node['mesh']]['primitives']:
                        mat=doc['materials'][primitive['material']]
                        self.assertEqual(node['name'].startswith('paint-'),mat['name'].startswith('paint-'))
                        pbr=mat['pbrMetallicRoughness']
                        self.assertGreaterEqual(pbr['roughnessFactor'],.04)
                        self.assertEqual(pbr['baseColorFactor'][3],1)
                        self.assertEqual(mat.get('alphaMode','OPAQUE'),'OPAQUE')

    def test_gloss_glazing_mirrors_and_rubber_have_distinct_pbr_responses(self):
        for vehicle in self.manifest['vehicles']:
            for lod in ('near','far'):
                for m in document(vehicle[lod]['path'])['materials']:
                    role=m['name'].split('-')[0];p=m['pbrMetallicRoughness']
                    if role=='paint':
                        self.assertLess(p['roughnessFactor'],.22)
                        self.assertGreater(p['metallicFactor'],.3)
                    if role=='glass':
                        self.assertLess(p['roughnessFactor'],.12)
                        self.assertEqual(p['metallicFactor'],0)
                    if role in ('alloy','mirror'):self.assertGreater(p['metallicFactor'],.9)
                    if role=='mirror':self.assertLess(p['roughnessFactor'],.1)
                    if role=='rubber':
                        self.assertEqual(p['metallicFactor'],0)
                        self.assertGreater(p['roughnessFactor'],.7)

    def test_mirror_lenses_are_inside_the_authored_housings(self):
        for vehicle in self.manifest['vehicles']:
            meshes=mesh_data(vehicle['near']['path'])
            lenses=[p for name,(pts,_) in meshes.items() if name.startswith('mirror-') for p in pts]
            housing=[mesh for name,mesh in meshes.items() if name.endswith('-body')
                     and name.startswith(('paint-','rubber-','alloy-','trim-'))]
            self.assertTrue(lenses,vehicle['name'])
            self.assertTrue(any(p[0]<0 for p in lenses) and any(p[0]>0 for p in lenses))
            for p in lenses:
                distances=[]
                for pts,indices in housing:
                    for i in range(0,len(indices),3):
                        a,b,c=(pts[k] for k in indices[i:i+3])
                        if any(p[k]<min(a[k],b[k],c[k])-.06 or p[k]>max(a[k],b[k],c[k])+.06 for k in range(3)):continue
                        distances.append(triangle_distance(p,a,b,c))
                self.assertTrue(distances,vehicle['name']+' missing mirror housing')
                self.assertLess(min(distances),.06,vehicle['name']+' detached mirror lens')

    def test_real_dimensions_and_wheel_contact_survive_export(self):
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                doc=document(v[lod]['path'])
                low=[math.inf]*3;high=[-math.inf]*3
                def visit(index,offset=(0,0,0)):
                    n=doc['nodes'][index]
                    offset=tuple(a+b for a,b in zip(offset,n.get('translation',(0,0,0))))
                    if 'mesh' in n:
                        for p in doc['meshes'][n['mesh']]['primitives']:
                            a=doc['accessors'][p['attributes']['POSITION']]
                            for i in range(3):
                                low[i]=min(low[i],a['min'][i]+offset[i])
                                high[i]=max(high[i],a['max'][i]+offset[i])
                    for c in n.get('children',[]):visit(c,offset)
                for n in doc['scenes'][0]['nodes']:visit(n)
                self.assertAlmostEqual(low[1],0,delta=.035)
                self.assertAlmostEqual(high[2]-low[2],v['length'],delta=.12)
                self.assertAlmostEqual(high[1],v['height'],delta=.10)
                wheels=[n for n in doc['nodes'] if n['name'].startswith('wheel-')]
                self.assertAlmostEqual(max(n['translation'][2] for n in wheels)-min(n['translation'][2] for n in wheels),v['wheelbase'],delta=.04)
                self.assertTrue(all(abs(n['translation'][1]-v['wheelRadius'])<.04 for n in wheels))

    def test_provenance_covers_every_asset(self):
        record=json.loads((GAME/'assets/THIRD_PARTY_ASSETS.json').read_text(encoding='utf-8'))
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                rows=[a for a in record['assets'] if v[lod]['path'] in a.get('files',[])]
                self.assertEqual(len(rows),1)
                self.assertEqual(rows[0]['license'],'CC BY 3.0')
                self.assertTrue((GAME/rows[0]['licenseFile']).is_file())
                self.assertTrue((GAME.parent/rows[0]['generatedBy']).is_file())
            sources=json.loads((GAME.parent/'data/source-assets/vehicles/sources.json').read_text())
            source=next(s for s in sources.values() if s['url']==v['sourceUrl'])
            archive=next(n for n,s in sources.items() if s==source)
            self.assertEqual(hashlib.sha256((GAME.parent/f'data/source-assets/vehicles/{archive}.zip').read_bytes()).hexdigest(),v['sourceSha256'])


class InTheEntryScene(unittest.TestCase):
    def test_hdr_specular_reflections_are_enabled_without_duplicate_ambient(self):
        settings=json.loads((GAME/'scenes/earth.scene').read_text(encoding='utf-8'))['scene']['settings']
        self.assertTrue(settings['iblEnabled'])
        self.assertEqual(settings['iblDiffuseIntensity'],0)
        self.assertGreater(settings['iblSpecularIntensity'],0)
        self.assertTrue((GAME/settings['skyboxTexture']).is_file())

    def test_the_car_rides_with_the_player_and_not_with_the_tile(self) -> None:
        node = car_node()
        self.assertEqual(node["children"][0]["importedFrom"], CAR)
        # Disabled until a spawn finds it a kerb: a car enabled at load stands
        # at the scene origin, which is the middle of the Atlantic.
        self.assertFalse(node["enabled"])

    def test_regenerating_the_world_keeps_the_car(self) -> None:
        import tempfile
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "scenes").mkdir()
            with patch.object(prepare_world, "GAME", root), \
                    patch.object(prepare_world, "basemap"):
                prepare_world.main()
            scene = json.loads((root / "scenes" / "earth.scene").read_text(encoding="utf-8"))
        car = next(n for n in scene["scene"]["children"] if "vehicle" in n.get("groups", []))
        self.assertEqual(car["children"][0]["importedFrom"], CAR)


class PreservedAssets(unittest.TestCase):
    def test_photoscanned_trees_remain(self):
        for species in ('fir_sapling','pine_sapling','quiver_tree','broadleaf'):
            path=tree_model(species)
            self.assertIn('trees_lod',path)
            self.assertTrue((GAME/path).is_file())


if __name__=='__main__':unittest.main()
