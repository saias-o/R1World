"""Relightable Far retains source colours and spatial canopy information."""
import hashlib
import io
import json
import unittest

try:
    import numpy as np
    from PIL import Image
    from r1.tree_impostor import (NATURE, OCCLUSION_DIRECTIONS, OCCLUSION_RESOLUTION,
                                  ambient_visibility, sphere_directions)
except ImportError as error:
    raise unittest.SkipTest(f'Tree asset verification needs NumPy and Pillow: {error}')
from r1.tree_lod import read_glb


class TreeImpostorTests(unittest.TestCase):
    def test_visibility_distinguishes_open_and_occluded_surfaces(self):
        points = np.zeros((1, 2, 3), dtype=np.float32)
        normals = np.array([[[0, 1, 0], [0, 1, 0]]], dtype=np.float32)
        projection = np.eye(4, dtype=np.float32)
        size = (OCCLUSION_RESOLUTION, OCCLUSION_RESOLUTION)
        clear = [(d, projection, np.ones(size)) for d in sphere_directions(OCCLUSION_DIRECTIONS)]
        blocked = [(d, projection, np.zeros(size)) for d in sphere_directions(OCCLUSION_DIRECTIONS)]
        np.testing.assert_allclose(ambient_visibility(points, normals, clear), 1)
        np.testing.assert_allclose(ambient_visibility(points, normals, blocked), 0)

    def test_shipped_far_preserves_albedo_and_contains_relief(self):
        for name in ('urban_tree', 'sakura', 'pine_tall'):
            with self.subTest(tree=name):
                self.check_shipped_far(name)

    def check_shipped_far(self, name):
        doc, binary = read_glb(NATURE / (name + '_far.glb'))
        mat = doc['materials'][0]
        report = json.loads((NATURE / (name + '_far.source.json')).read_text(encoding='utf8'))
        self.assertEqual(report['sourceSha256'], hashlib.sha256((NATURE / report['derivedFrom']).read_bytes()).hexdigest())
        self.assertEqual(report['vertices'], 4)
        self.assertEqual(report['triangles'], 2)
        def pixels(texture):
            image = doc['images'][doc['textures'][texture['index']]['source']]
            view = doc['bufferViews'][image['bufferView']]
            offset = view.get('byteOffset', 0)
            return np.asarray(Image.open(io.BytesIO(binary[offset:offset+view['byteLength']])).convert('RGBA'))
        albedo = pixels(mat['pbrMetallicRoughness']['baseColorTexture'])
        surface = pixels(mat['normalTexture'])
        self.assertEqual(albedo.shape, surface.shape)
        covered = albedo[:, :, 3] > 127
        # All directions and occlusion ranges must survive quantization and packing.
        self.assertTrue(np.all(np.ptp(surface[:, :, :3][covered], axis=0) > 127))
        self.assertGreater(np.ptp(surface[:, :, 3][covered]), 127)
        self.assertEqual(mat.get('pbrMetallicRoughness', {}).get('baseColorFactor', [1]*4), [1]*4)


if __name__ == '__main__':
    unittest.main()
