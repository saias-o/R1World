"""Shipped foliage budgets, provenance, cutouts and relightable depth data."""
import json
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from r1.understory_assets import OUTPUT, LOCK, SPECIES, slab_frames
from r1.tree_lod import read_glb
from r1.decimate import _read_accessor


class UnderstoryAssetsTests(unittest.TestCase):
    def test_every_shipped_plant_respects_the_complete_200_triangle_budget(self):
        for name in (*SPECIES, 'grass_dry'):
            for level, expected in (('near', 176), ('mid', 44)):
                with self.subTest(plant=name, level=level):
                    doc, binary = read_glb(OUTPUT / f'{name}_{level}.glb')
                    triangles = sum(doc['accessors'][p['indices']]['count'] // 3
                                    for mesh in doc['meshes'] for p in mesh['primitives'])
                    self.assertEqual(triangles, expected)
                    self.assertLessEqual(triangles, 200)
                    self.assertEqual(len(doc['meshes']), 1)
                    self.assertEqual(len(doc['materials']), 1)
                    self.assertFalse(doc['materials'][0]['doubleSided'])
                    for mesh in doc['meshes']:
                        for p in mesh['primitives']:
                            positions = np.array(_read_accessor(doc, [binary], p['attributes']['POSITION']))
                            self.assertTrue(np.all(np.isfinite(positions)))
                            self.assertLess(positions[:, 1].min(), .01)
                            self.assertGreater(positions[:, 1].max(), .99)
                            self.assertLess(positions[:, 1].max(), 1.12)

    def test_lods_share_external_textures_without_duplicate_residency(self):
        for name in (*SPECIES, 'grass_dry'):
            a, _ = read_glb(OUTPUT / f'{name}_near.glb')
            b, _ = read_glb(OUTPUT / f'{name}_mid.glb')
            self.assertEqual(a['images'], b['images'])
            for image in a['images']:
                self.assertTrue((OUTPUT / image['uri']).is_file())
                self.assertNotIn('bufferView', image)
            material = a['materials'][0]
            self.assertEqual(material['alphaMode'], 'MASK')
            self.assertEqual(material['extras']['SAIDA_foliage']['normalSpace'], 'object')
            self.assertGreater(material['extras']['SAIDA_foliage']['wind'], 0)
            self.assertGreater(material['extras']['SAIDA_foliage']['transmission'], 0)

    def test_atlases_contain_leaf_cutouts_directional_normals_and_visibility(self):
        for name in SPECIES:
            with self.subTest(plant=name):
                color = np.asarray(Image.open(OUTPUT / f'{name}_albedo.png').convert('RGBA'))
                normal = np.asarray(Image.open(OUTPUT / f'{name}_normal.png').convert('RGBA'))
                self.assertEqual(color.shape, (768, 1152, 4))
                self.assertEqual(color.shape, normal.shape)
                covered = color[:, :, 3] > 127
                self.assertGreater(covered.mean(), .015)
                self.assertLess(covered.mean(), .7)
                self.assertGreater((color[:, :, 3] == 0).mean(), .2)
                self.assertTrue(np.all(np.ptp(normal[:, :, :3][covered], axis=0) > 180))
                self.assertGreater(np.ptp(normal[:, :, 3][covered]), 120)
                self.assertGreater(np.std(color[:, :, :3][covered]), 15)

    def test_depth_layers_include_both_faces_and_overhead_views(self):
        frames = list(slab_frames())
        self.assertEqual(len(frames), 22)
        for i in range(0, 22, 2):
            np.testing.assert_allclose(frames[i][0], -frames[i+1][0])
        self.assertTrue(any(abs(f[0][1]) > .99 for f in frames))
        self.assertEqual(len(set(float(f[2]) for f in frames)), 5)

    def test_every_scan_has_a_cc0_author_and_pinned_download_digests(self):
        lock = json.loads(LOCK.read_text('utf8'))
        for name in SPECIES:
            origin = lock[name]
            report = json.loads((OUTPUT / f'{name}.source.json').read_text('utf8'))
            self.assertEqual(origin['license'], 'CC0-1.0')
            self.assertIn('Rico Cilliers', origin['authors'])
            self.assertEqual(report['source'], origin['source'])
            self.assertGreater(report['sourceTriangles'], 176)
            for path, file in origin['files'].items():
                self.assertEqual(len(file['sha256']), 64)
                self.assertEqual(report['sourceFiles'][path], file['sha256'])
            self.assertIn('textures/alpha.png', origin['files'])

    def test_dry_grass_retains_exact_coverage_and_normal_texture(self):
        fresh = np.asarray(Image.open(OUTPUT / 'grass_medium_02_albedo.png'))
        dry = np.asarray(Image.open(OUTPUT / 'grass_dry_albedo.png'))
        np.testing.assert_array_equal(fresh[:, :, 3], dry[:, :, 3])
        self.assertGreater(np.mean(np.abs(fresh[:, :, :3].astype(float)-dry[:, :, :3])), 1)
        doc, _ = read_glb(OUTPUT / 'grass_dry_near.glb')
        self.assertEqual(doc['images'][1]['uri'], 'grass_medium_02_normal.png')


if __name__ == '__main__':
    unittest.main()
