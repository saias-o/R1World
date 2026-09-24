"""The aircraft fleet's contracts: what the generator parks and the game flies.

- every colour is an albedo no brighter than 0.35 (CLAUDE.md rule 2);
- the models are the size the manifest says, nose to +Z, wheels on Y = 0,
  because the generator fits them on stands by those numbers and the game
  turns them about their centre of gravity;
- the named nodes the game animates exist in both levels of detail;
- they widen coverage and displace nothing (rule 1): the project had no
  aircraft of any grade, and the trees still point at the photoscans.
"""
from __future__ import annotations

import json
import math
import struct
import unittest

from r1 import aircraft_fleet
from r1.external_assets import GAME_ROOT, tree_model

GAME = GAME_ROOT


def document(path):
    payload = (GAME/path).read_bytes()
    length, kind = struct.unpack_from('<II', payload, 12)
    assert kind == 0x4e4f534a
    return json.loads(payload[20:20+length])


def extent(doc, only=None):
    """The model's box, or with `only` the box of the node of that name."""
    low, high = [math.inf]*3, [-math.inf]*3

    def visit(index, offset=(0, 0, 0), inside=False):
        node = doc['nodes'][index]
        offset = tuple(a+b for a, b in zip(offset, node.get('translation', (0, 0, 0))))
        inside = inside or only is None or node.get('name') == only
        if 'mesh' in node and inside:
            for p in doc['meshes'][node['mesh']]['primitives']:
                a = doc['accessors'][p['attributes']['POSITION']]
                for i in range(3):
                    low[i] = min(low[i], a['min'][i]+offset[i])
                    high[i] = max(high[i], a['max'][i]+offset[i])
        for child in node.get('children', []):
            visit(child, offset, inside)
    for root in doc['scenes'][0]['nodes']:
        visit(root)
    return low, high


class Fleet(unittest.TestCase):
    def setUp(self):
        self.manifest = json.loads((GAME/'assets/models/aircraft/fleet.json').read_text(encoding='utf-8'))
        self.fleet = {a['name']: a for a in self.manifest['aircraft']}

    def test_the_three_kinds_asked_for_are_there(self):
        self.assertEqual({a['class'] for a in self.fleet.values()}, {'airliner', 'jet', 'helicopter'})
        self.assertIn('widebody', self.fleet)  # "type gros airbus"

    def test_every_colour_is_an_albedo(self):
        for name, a in self.fleet.items():
            for lod in ('near', 'far'):
                doc = document(a[lod]['path'])
                self.assertFalse(doc.get('images'), 'no textures to bring paint in at another level')
                for m in doc['materials']:
                    colour = m['pbrMetallicRoughness']['baseColorFactor']
                    self.assertLessEqual(max(colour[:3]), .35, f"{name} {m['name']}")

    def test_the_models_are_the_size_the_manifest_says(self):
        for name, a in self.fleet.items():
            for lod in ('near', 'far'):
                low, high = extent(document(a[lod]['path']))
                self.assertAlmostEqual(high[2]-low[2], a['length'], delta=a['length']*.06, msg=f'{name} {lod} length')
                if a['class'] == 'helicopter':
                    # The span is the rotor's diameter; its four blades rest at 45 degrees.
                    rl, rh = extent(document(a[lod]['path']), 'rotor-main')
                    self.assertAlmostEqual((rh[0]-rl[0])/math.cos(math.pi/4), a['span'], delta=a['span']*.06,
                                           msg=f'{name} {lod} rotor')
                else:
                    self.assertAlmostEqual(high[0]-low[0], a['span'], delta=a['span']*.06, msg=f'{name} {lod} span')
                self.assertAlmostEqual(high[1], a['height'], delta=a['height']*.12, msg=f'{name} {lod} height')
                self.assertAlmostEqual(low[1], 0, delta=.02, msg=f'{name} {lod} stands on Y = 0')
                self.assertLess(abs(low[2]+high[2])/2, a['length']*.1, f'{name} {lod}: Z = 0 is mid-length')

    def test_the_animated_nodes_are_named_in_both_levels(self):
        for name, a in self.fleet.items():
            for lod in ('near', 'far'):
                names = {n.get('name') for n in document(a[lod]['path'])['nodes']}
                if a['class'] == 'helicopter':
                    self.assertTrue({'rotor-main', 'rotor-tail'} <= names, f'{name} {lod}')
                else:
                    self.assertIn('gear', names, f'{name} {lod}')

    def test_the_manifest_is_what_the_authoring_tool_writes(self):
        self.assertEqual(set(self.fleet), set(aircraft_fleet.FLEET))
        for name, spec in aircraft_fleet.FLEET.items():
            a = self.fleet[name]
            self.assertEqual(a['handling'], spec['handling'])
            for lod, far in (('near', False), ('far', True)):
                self.assertEqual(a[lod]['path'], aircraft_fleet.model(name, far))
                self.assertEqual(spec['build'](far).vertices, a[lod]['vertices'], f'{name} {lod}: rebuild the fleet')
        self.assertEqual(self.manifest['totalVertices'],
                         sum(a[lod]['vertices'] for a in self.fleet.values() for lod in ('near', 'far')))

    def test_handling_is_its_own_for_each_class(self):
        wide, jet, heli = self.fleet['widebody']['handling'], self.fleet['bizjet']['handling'], self.fleet['helicopter']['handling']
        self.assertGreater(jet['rollRate'], 2*wide['rollRate'])
        self.assertGreater(jet['spool'], 2*wide['spool'])
        self.assertLess(jet['rotate'], wide['rotate'])
        self.assertGreater(heli['climb'], 0)
        for a in self.fleet.values():
            h = a['handling']
            if a['class'] != 'helicopter':
                self.assertLess(h['stall'], h['rotate'])
                self.assertLess(h['rotate'], h['top'])

    def test_the_trees_did_not_move(self):
        # Rule 1, from the other side: nothing about aircraft touches vegetation.
        for species in ('fir_sapling', 'pine_sapling', 'quiver_tree', 'broadleaf'):
            path = tree_model(species)
            self.assertIn('trees_lod', path)
            self.assertTrue((GAME/path).is_file())


if __name__ == '__main__':
    unittest.main()
