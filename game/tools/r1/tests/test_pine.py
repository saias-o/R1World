"""The adult pine stays small, normalized, shaded and consistent across LODs."""
import json
import unittest

try:
    import numpy as np
    from r1.tree_lod import NATURE, read_glb
    from r1.tree_impostor import transformed_parts
except ImportError as error:
    raise unittest.SkipTest(f"Pine validation needs NumPy and Pillow: {error}")


class AdultPineTests(unittest.TestCase):
    def test_pine_lods_fit_the_arena_and_retain_the_exact_lod0_crown(self):
        near, near_binary = read_glb(NATURE / "pine_tall.glb")
        mid, mid_binary = read_glb(NATURE / "pine_tall_mid.glb")
        detailed = transformed_parts(near, near_binary)
        reduced = transformed_parts(mid, mid_binary)
        near_count = sum(len(v) for v, _, _, _ in detailed)
        mid_count = sum(len(v) for v, _, _, _ in reduced)
        self.assertLess(near_count, 8000)
        self.assertLess(mid_count, near_count)
        self.assertLessEqual(sum(len(i)//3 for _, i, _, _ in reduced), 4028)
        self.assertLess(near_count + mid_count + 4, 1048576 * .015)
        for parts in (detailed, reduced):
            positions = np.concatenate([v[:, :3] for v, _, _, _ in parts])
            self.assertAlmostEqual(float(positions[:, 1].min()), 0, places=6)
            self.assertAlmostEqual(float(positions[:, 1].max()), 1, places=6)
            for vertices, _, _, has_tangents in parts:
                self.assertTrue(np.isfinite(vertices).all())
                self.assertTrue(has_tangents)
                np.testing.assert_allclose(np.sum(vertices[:, 3:6] * vertices[:, 8:11], axis=1), 0, atol=1e-5)
        # Every needle card and its UV survives: a sparse Mid cannot pass.
        np.testing.assert_array_equal(detailed[1][0], reduced[1][0])
        np.testing.assert_array_equal(detailed[1][1], reduced[1][1])
        def leaf_image(doc,binary):
            material=doc['materials'][doc['meshes'][1]['primitives'][0]['material']]
            ref=material['pbrMetallicRoughness']['baseColorTexture']
            image=doc['images'][doc['textures'][ref['index']]['source']]
            view=doc['bufferViews'][image['bufferView']];offset=view.get('byteOffset',0)
            return material['alphaCutoff'],material['pbrMetallicRoughness']['baseColorFactor'],binary[offset:offset+view['byteLength']]
        self.assertEqual(leaf_image(near,near_binary),leaf_image(mid,mid_binary))
        far, far_binary = read_glb(NATURE / "pine_tall_far.glb")
        self.assertEqual(far['accessors'][far['meshes'][0]['primitives'][0]['indices']]['count'], 6)
        self.assertEqual(far['accessors'][far['meshes'][0]['primitives'][0]['attributes']['POSITION']]['count'], 4)
        self.assertEqual(far['materials'][0]['extras']['SAIDA_billboard'], {'columns': 32, 'rows': 5})
        self.assertEqual(far['materials'][0]['alphaCutoff'], .5)
        report=json.loads((NATURE/'pine_tall_far.source.json').read_text(encoding='utf-8'))
        self.assertEqual(report['derivedFrom'], 'pine_tall.glb')

    def test_redistributable_pine_retains_pbr_and_valid_embedded_pngs(self):
        reports = json.loads((NATURE / "SOURCES.json").read_text(encoding="utf-8"))
        for name in ("pine_tall.glb", "pine_tall_mid.glb", "pine_tall_far.glb"):
            doc, binary = read_glb(NATURE / name)
            report = next(r for r in reports if r.get("output") == name)
            self.assertEqual(report["attribution"]["license"], "CC0-1.0")
            self.assertTrue(report["attribution"]["source"].startswith("https://terraintale.com/"))
            self.assertTrue(any(m.get("alphaMode") == "MASK" for m in doc["materials"]))
            for image in doc["images"]:
                self.assertEqual(image["mimeType"], "image/png")
                view = doc["bufferViews"][image["bufferView"]]
                offset = view.get("byteOffset", 0)
                self.assertEqual(binary[offset:offset + 8], b"\x89PNG\r\n\x1a\n")


if __name__ == "__main__":
    unittest.main()
