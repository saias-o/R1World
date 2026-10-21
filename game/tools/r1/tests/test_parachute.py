"""The arrival's parachute: a Creative Commons canopy, entered normalised.

Plan §4 and CLAUDE.md rule 2: the canopy the project owner chose (Poly by
Google, CC BY 3.0) enters in measured albedos, not in its painted texture,
its lines end on the wearer's shoulders, and its licence travels with it.
"""
import json
import struct
import unittest
from pathlib import Path

from r1 import parachute

GAME = Path(__file__).resolve().parents[3]
MODEL = GAME / "assets/models/external/parachute/parachute.glb"


def document():
    data = MODEL.read_bytes()
    length = struct.unpack_from("<I", data, 12)[0]
    return json.loads(data[20:20 + length]), data[20 + length + 8:]


class Parachute(unittest.TestCase):
    def test_it_enters_in_albedos_not_in_paint(self):
        doc, _ = document()
        self.assertNotIn("images", doc)
        self.assertNotIn("textures", doc)
        names = {m["name"] for m in doc["materials"]}
        self.assertEqual(names, set(parachute.CLASSES))
        for material in doc["materials"]:
            pbr = material["pbrMetallicRoughness"]
            self.assertLessEqual(max(pbr["baseColorFactor"][:3]), 0.35, material["name"])
            self.assertEqual(pbr["metallicFactor"], 0.0)

    def test_its_lines_end_on_the_shoulders(self):
        doc, binary = document()
        lines = next(p for p in doc["meshes"][0]["primitives"]
                     if doc["materials"][p["material"]]["name"] == "lines")
        accessor = doc["accessors"][lines["attributes"]["POSITION"]]
        view = doc["bufferViews"][accessor["bufferView"]]
        points = [struct.unpack_from("<3f", binary, view.get("byteOffset", 0) + 12 * k)
                  for k in range(accessor["count"])]
        feet = [p for p in points if p[1] < 1.0]
        self.assertTrue(feet)
        for x, y, z in feet:
            self.assertAlmostEqual(abs(x), parachute.SHOULDERS / 2, places=5)
            self.assertEqual((y, z), (0.0, 0.0))
        # Nothing of the kite's bar is left below the canopy but the lines.
        for primitive in doc["meshes"][0]["primitives"]:
            if primitive is lines:
                continue
            low = doc["accessors"][primitive["attributes"]["POSITION"]]["min"][1]
            self.assertGreater(low, 3.5, doc["materials"][primitive["material"]]["name"])

    def test_its_licence_and_provenance_are_said(self):
        registry = json.loads((GAME / "assets/THIRD_PARTY_ASSETS.json").read_text(encoding="utf-8"))
        entry = next(a for a in registry["assets"] if a["name"] == parachute.RECORD_NAME)
        self.assertEqual(entry["license"], "CC BY 3.0")
        self.assertEqual(entry["files"], ["assets/models/external/parachute/parachute.glb"])
        self.assertTrue((GAME / entry["licenseFile"]).is_file())
        self.assertIn("Poly by Google", (GAME / entry["licenseFile"]).read_text(encoding="utf-8"))
        # The credits the player reads name it too (CC BY requires it).
        self.assertIn("Poly by Google", (GAME / "ui/world.html").read_text(encoding="utf-8"))
