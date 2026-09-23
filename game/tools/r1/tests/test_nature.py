import json
import struct
import unittest
from pathlib import Path
from dataclasses import replace
from r1.nature import plan_nature, species, MODEL_DIR
from r1.sources import OsmData, OsmNode, OsmWay, normalize_osm
from r1.world_tiles import tile_at
from r1.geodesy import Anchor

GAME = Path(__file__).resolve().parents[3]


class NatureTests(unittest.TestCase):
    def setUp(self):
        self.tile = tile_at(2.3522, 48.8566)
        self.lon, self.lat = self.tile.center
        self.anchor = Anchor.at(self.lon, self.lat, 0)
        self.osm = OsmData((), (), (), (), (), (), ())

    def point(self, x, z):
        lo, la, _ = self.anchor.engine_to_geodetic(x, 0, z)
        return lo, la

    def ground(self, lo, la):
        return self.anchor.geodetic_to_engine(lo, la, 0)

    def area(self, tags):
        return OsmWay(9, tuple(self.point(x, z) for x, z in
                              ((-100,-100),(100,-100),(100,100),(-100,100),(-100,-100))), tags)

    def plan(self, osm, budget=320):
        return plan_nature(osm, self.tile, self.anchor, self.ground, GAME, budget)

    def test_empty_and_lawn_do_not_invent_trees(self):
        self.assertEqual(self.plan(self.osm)[0], [])
        nodes, stats = self.plan(replace(self.osm, vegetation_areas=(self.area({'landuse':'grass'}),)))
        self.assertTrue(nodes)
        self.assertEqual(stats['trees'], 0)
        self.assertLessEqual(stats['grassTufts'], 160)

    def test_surveyed_height_and_position_win(self):
        node = OsmNode(4, self.lon, self.lat, {'natural':'tree','height':'14','genus':'Platanus'})
        nodes, stats = self.plan(replace(self.osm, features=(node,)))
        self.assertEqual(nodes[0]['transform']['scale'], [14]*3)
        self.assertEqual(nodes[0]['transform']['position'], list(self.ground(self.lon,self.lat)))
        self.assertEqual(stats['heightsMeasured'], 1)
        self.assertEqual(stats['bySource']['osm-point'], 1)

    def test_tree_rows_are_normalized_and_placed(self):
        osm = normalize_osm({'elements':[
            {'type':'node','id':1,'lon':self.lon,'lat':self.lat},
            {'type':'node','id':2,'lon':self.lon+.001,'lat':self.lat},
            {'type':'way','id':3,'nodes':[1,2],'tags':{'natural':'tree_row','tree_count':'4'}}]})
        self.assertEqual(len(osm.tree_rows), 1)
        self.assertEqual(self.plan(osm)[1]['bySource']['osm-row-inferred-spacing'], 4)

    def test_inferred_forest_avoids_roads_and_buildings(self):
        forest = self.area({'natural':'wood'})
        self.assertEqual(self.plan(replace(self.osm,vegetation_areas=(forest,),buildings=(self.area({'building':'yes'}),)))[0], [])
        road = OsmWay(7, (self.point(-120,0),self.point(120,0)), {'highway':'residential','width':'12'})
        nodes, _ = self.plan(replace(self.osm,vegetation_areas=(forest,),roads=(road,)))
        self.assertTrue(nodes)
        for node in nodes:
            self.assertGreater(abs(node['transform']['position'][2]), 6.7)

    def test_budget_and_order_are_deterministic(self):
        features = tuple(OsmNode(i,*self.point(i*5-90,0),{'natural':'tree'}) for i in range(35))
        first = self.plan(replace(self.osm,features=features),10)
        self.assertEqual(first,self.plan(replace(self.osm,features=tuple(reversed(features))),10))
        self.assertEqual(first[1]['trees'],10)
        self.assertEqual(first[1]['droppedForBudget'],25)

    def test_tile_boundary_has_one_owner(self):
        node = OsmNode(5,self.tile.bounds.east,self.lat,{'natural':'tree'})
        from r1.world_tiles import Tile
        osm = replace(self.osm,features=(node,))
        neighbor = Tile(self.tile.row,self.tile.col+1)
        count = len(self.plan(osm)[0]) + len(plan_nature(osm,neighbor,self.anchor,self.ground,GAME)[0])
        self.assertEqual(count,1)

    def test_botanical_tags_override_region(self):
        self.assertEqual(species({'genus':'Pinus'},2,48,1)[0],'pine_sapling')
        self.assertEqual(species({'leaf_type':'broadleaved'},10,65,1,True)[0],'broadleaf')
        self.assertNotEqual(species({},77,17,1)[0],'quiver_tree')
        self.assertEqual(species({},18,-25,1)[0],'quiver_tree')

    def test_unique_asset_geometry_fits_arena_margin(self):
        count = 0
        paths = list((GAME/MODEL_DIR).glob('*.glb')) + [GAME/'assets/models/external/nature_selected'/f'{name}.glb' for name in ('urban_tree','grass_fresh','grass_dry','grass_tall')]
        for path in paths:
            raw = path.read_bytes()
            doc = json.loads(raw[20:20+struct.unpack_from('<I',raw,12)[0]])
            count += sum(doc['accessors'][p['attributes']['POSITION']]['count'] for m in doc['meshes'] for p in m['primitives'])
            self.assertFalse(any('uri' in i for i in doc.get('images',[])),path.name)
        self.assertLess(count+900000+9390+15000,1048576)

    def test_extracted_grass_has_no_display_ground_or_man(self):
        manifest = json.loads((GAME/'assets/models/external/nature_selected/SOURCES.json').read_text(encoding='utf-8'))
        for item in manifest:
            if item['output'].startswith('grass'):
                self.assertTrue(all(name.startswith('Grass') for name in item['selectedChildren']))
                self.assertTrue(item['attribution']['author'])
