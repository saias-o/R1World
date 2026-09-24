"""Shipped fleet contracts: dimensions, pivots, PBR separation and arena cost."""
from __future__ import annotations
import json
import math
import struct
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


def car_node():
    scene=json.loads((GAME/'scenes/earth.scene').read_text(encoding='utf-8'))
    return next(n for n in scene['scene']['children'] if 'vehicle' in n.get('groups',[]))


class Fleet(unittest.TestCase):
    def setUp(self):
        self.manifest=json.loads((GAME/'assets/models/vehicles/fleet.json').read_text())

    def test_all_categories_and_two_lods_are_shipped(self):
        self.assertEqual({v['name'] for v in self.manifest['vehicles']},
                         {'city','sedan','suv','offroad','sport','truck','bus'})
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                doc=document(v[lod]['path'])
                self.assertFalse(doc.get('images'), 'PBR fleet requires no external textures')
                self.assertEqual({n['name'] for n in doc['nodes'] if n['name'].startswith('wheel-')},
                    {'wheel-front-left','wheel-front-right','wheel-back-left','wheel-back-right'})

    def test_budget_counts_exported_seams_and_shared_wheels(self):
        total=0
        for v in self.manifest['vehicles']:
            for lod,limit in (('near',4500),('far',1600)):
                doc=document(v[lod]['path'])
                verts=sum(doc['accessors'][p['attributes']['POSITION']]['count']
                          for m in doc['meshes'] for p in m['primitives'])
                self.assertEqual(verts,v[lod]['vertices'])
                self.assertLessEqual(verts,limit,v['name']+' '+lod)
                total+=verts
                wheel_meshes=[doc['nodes'][c]['mesh'] for n in doc['nodes']
                              if n['name'].startswith('wheel-') for c in n['children']]
                self.assertEqual(len(set(wheel_meshes))*4,len(wheel_meshes))
            self.assertLess(v['far']['drawTriangles'],v['near']['drawTriangles']*.55)
        self.assertEqual(total,self.manifest['totalVertices'])
        self.assertLessEqual(total,40000,'Both resident LODs must fit the shared asset reserve')

    def test_paint_does_not_include_glass_rubber_or_lights(self):
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                doc=document(v[lod]['path'])
                self.assertEqual({m['name'] for m in doc['materials']},
                                 {'paint','rubber','glass','alloy','headlamp','taillamp','indicator'})
                for node in doc['nodes']:
                    if 'mesh' not in node:continue
                    for primitive in doc['meshes'][node['mesh']]['primitives']:
                        mat=doc['materials'][primitive['material']]
                        self.assertEqual(node['name'].startswith('paint-'),mat['name']=='paint')
                        pbr=mat['pbrMetallicRoughness']
                        self.assertGreaterEqual(pbr['roughnessFactor'],.1)
                        self.assertEqual(pbr['baseColorFactor'][3],1)

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
                self.assertAlmostEqual(low[1],0,places=5)
                self.assertAlmostEqual(high[2]-low[2],v['length'],delta=.12)
                self.assertAlmostEqual(high[1],v['height'],delta=.10)
                wheels=[n for n in doc['nodes'] if n['name'].startswith('wheel-')]
                self.assertAlmostEqual(max(n['translation'][2] for n in wheels)-min(n['translation'][2] for n in wheels),v['wheelbase'])
                self.assertTrue(all(abs(n['translation'][1]-v['wheelRadius'])<1e-6 for n in wheels))

    def test_provenance_covers_every_asset(self):
        record=json.loads((GAME/'assets/THIRD_PARTY_ASSETS.json').read_text(encoding='utf-8'))
        for v in self.manifest['vehicles']:
            for lod in ('near','far'):
                rows=[a for a in record['assets'] if v[lod]['path'] in a.get('files',[])]
                self.assertEqual(len(rows),1)
                self.assertEqual(rows[0]['license'],'CC0 1.0')
                self.assertTrue((GAME.parent/rows[0]['generatedBy']).is_file())


class InTheEntryScene(unittest.TestCase):
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
