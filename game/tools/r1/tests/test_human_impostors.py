"""The player's impostors (r1.human_impostor): what the engine's loader accepts,
from the scan they say they come from, registered. Not drawn by the game
until the user validates them on the sheets (docs/PLAN.md)."""
import hashlib
import io
import json
import unittest
from pathlib import Path

from PIL import Image

from r1 import humans  # not r1.human_impostor: baking needs numpy, checking does not
from r1.tree_lod import read_glb

GAME = Path(__file__).resolve().parents[3]
SOURCES = GAME / "assets/models/humans/impostors/SOURCES.json"


class HumanImpostors(unittest.TestCase):
    def setUp(self):
        self.reports = json.loads(SOURCES.read_text(encoding="utf-8"))

    def test_the_player_walks_and_waits(self):
        player = json.loads(humans.MANIFEST.read_text(encoding="utf-8"))["player"]
        self.assertEqual({(r["person"], r["clip"]) for r in self.reports}, {(player["name"], "walk"), (player["name"], "idle")})
        for r in self.reports:
            self.assertEqual(r["derivedFrom"], player["model"])
            self.assertEqual(r["sourceSha256"], hashlib.sha256((GAME / r["derivedFrom"]).read_bytes()).hexdigest(),
                             "the scan changed since the bake: python -m r1.human_impostor")

    def test_the_engine_loader_accepts_every_atlas(self):
        # GLTFLoader.cpp: 1..32 columns, 1..17 rows, 1..64 frames, 128 rows in
        # all, loopSeconds > 0, stride 0..20 m.
        for r in self.reports:
            doc, binary = read_glb(GAME / r["output"])
            material = doc["materials"][0]
            atlas = material["extras"]["SAIDA_billboard"]
            self.assertTrue(1 <= atlas["columns"] <= 32 and 1 <= atlas.get("rows", 1) <= 17, r["output"])
            self.assertTrue(1 <= atlas["frames"] <= 64 and atlas.get("rows", 1) * atlas["frames"] <= 128, r["output"])
            self.assertGreater(atlas.get("loopSeconds", 1.0), 0)
            self.assertTrue(0 <= atlas.get("stride", 0.0) <= 20, r["output"])
            self.assertEqual("stride" in atlas, r["clip"] == "walk")
            self.assertEqual(material["alphaMode"], "MASK")
            primitive = doc["meshes"][0]["primitives"][0]
            self.assertEqual(set(primitive["attributes"]), {"POSITION", "NORMAL", "TANGENT", "TEXCOORD_0"})
            for image, mode in zip(doc["images"], ("RGBA", "RGBA")):
                view = doc["bufferViews"][image["bufferView"]]
                start = view.get("byteOffset", 0)
                picture = Image.open(io.BytesIO(binary[start:start + view["byteLength"]]))
                self.assertEqual(picture.mode, mode)
                self.assertEqual(picture.size, (atlas["columns"] * r["resolution"], atlas["frames"] * r["resolution"]))

    def test_the_walk_loop_is_the_clip_s_stride(self):
        player = json.loads(humans.MANIFEST.read_text(encoding="utf-8"))["player"]
        walk = next(r for r in self.reports if r["clip"] == "walk")
        self.assertAlmostEqual(walk["stride"], player["clips"]["walk"]["speed"] * player["clips"]["walk"]["seconds"], places=3)
        self.assertTrue(8 <= walk["frames"] <= 12, "eight to twelve poses of the walk (docs/PLAN.md)")

    def test_they_are_registered(self):
        doc = json.loads((GAME / "assets/THIRD_PARTY_ASSETS.json").read_text(encoding="utf-8"))
        entry = next(a for a in doc["assets"] if a.get("generatedBy") == "game/tools/r1/human_impostor.py")
        self.assertEqual(entry["license"], "MIT")
        for r in self.reports:
            self.assertIn(r["output"], entry["files"])
        self.assertIn("not drawn yet", entry["modifications"])


if __name__ == "__main__":
    unittest.main()
