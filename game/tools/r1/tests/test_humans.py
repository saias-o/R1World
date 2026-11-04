"""The people: one grade for the player and the crowd, and every file said.

CLAUDE.md rule 1: the player is a scanned-and-rigged Rocketbox avatar, and
nothing brings back the Kenney block character it replaced. The crowd is the
same library, within its share of the arena, with the clips the game plays.
"""
import json
import unittest
from pathlib import Path

from r1 import humans

GAME = Path(__file__).resolve().parents[3]
MANIFEST = json.loads((GAME / "assets/models/humans/humans.json").read_text(encoding="utf-8"))


def animations(path: Path) -> set:
    import struct
    data = path.read_bytes()
    length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + length])
    return {a["name"] for a in doc.get("animations", [])}, {m["name"] for m in doc.get("meshes", [])}, len(doc.get("skins", []))


class Humans(unittest.TestCase):
    def test_every_person_is_rocketbox_and_registered(self):
        self.assertEqual(MANIFEST["license"], "MIT")
        self.assertIn("Microsoft-Rocketbox", MANIFEST["source"])
        self.assertTrue((GAME / MANIFEST["licenseFile"]).is_file())
        registry = json.loads((GAME / "assets/THIRD_PARTY_ASSETS.json").read_text(encoding="utf-8"))
        entry = next(a for a in registry["assets"] if "Rocketbox" in a["name"])
        for person in [MANIFEST["player"]] + MANIFEST["crowd"]:
            self.assertTrue((GAME / person["model"]).is_file(), person["model"])
            self.assertIn(person["model"], entry["files"])

    def test_the_block_character_is_gone(self):
        self.assertFalse((GAME / "assets/models/characters/player.glb").exists())
        scene = (GAME / "scenes/earth.scene").read_text(encoding="utf-8")
        self.assertIn(MANIFEST["player"]["model"], scene)

    def test_the_player_has_what_the_game_plays(self):
        clips, meshes, skins = animations(GAME / MANIFEST["player"]["model"])
        self.assertTrue({"idle", "run", "sprint", "jump"} <= clips, clips)
        self.assertEqual(skins, 1)
        # The whole scan, not a decimation: the player is always close.
        self.assertGreater(MANIFEST["player"]["lods"][0]["triangles"], 7000)

    def test_every_passer_by_has_two_levels_and_every_clip(self):
        for person in MANIFEST["crowd"]:
            clips, meshes, skins = animations(GAME / person["model"])
            self.assertEqual(meshes, {"Near", "Far"}, person["name"])
            self.assertTrue({"idle", "walk", "wait", "phone", "talk", "sit", "run"} <= clips, person["name"])
            self.assertIn("pelvisHeight", person["seat"])
            self.assertGreater(person["clips"]["walk"]["speed"], 0.8)

    def test_every_passer_by_can_look_and_answer_a_bump(self):
        # native/world.cpp turns the eyes toward the player (GazeModifier) and
        # plays these after a bump (gen/crowd.cpp); the face rig folds, not them.
        for person in MANIFEST["crowd"]:
            clips, _, _ = animations(GAME / person["model"])
            self.assertTrue({"shrug", "angry", "dust"} <= clips, person["name"])
            self.assertEqual(person["eyes"], ["Bip01 LEye", "Bip01 REye"], person["name"])
            for clip, seconds in humans.CLIP_SECONDS.items():
                self.assertLessEqual(person["clips"][clip]["seconds"], seconds + 0.05, (person["name"], clip))

    def test_the_crowd_fits_its_share_of_the_arena(self):
        # More varied scans still use under 55k vertices, well inside the
        # shared models' 15% of the 1 048 576-vertex arena (CLAUDE.md §5).
        self.assertLessEqual(MANIFEST["sharedVertices"], 55000)
        self.assertEqual(MANIFEST["sharedVertices"],
                         sum(l["exportedVertices"] for p in [MANIFEST["player"]] + MANIFEST["crowd"]
                             for l in p["lods"] + p.get("autoLods", [])))

    def test_the_player_is_drawn_in_levels_of_detail(self):
        # Every other player is a copy of his scan (native/world/online.cpp):
        # AutoLOD's three levels, each coarser, the skin carried, switched at
        # decreasing screen coverage (python -m r1.humans --lods).
        model = GAME / MANIFEST["player"]["model"]
        levels = humans.autolod_levels(model)
        self.assertEqual(levels, MANIFEST["player"]["autoLods"])
        self.assertEqual(len(levels), 3)
        counts = [MANIFEST["player"]["lods"][0]["triangles"]] + [l["triangles"] for l in levels]
        self.assertEqual(counts, sorted(counts, reverse=True))
        doc = humans.glb_document(model)
        node = next(n for n in doc["nodes"] if "MSFT_lod" in n.get("extensions", {}))
        coverage = node["extras"]["MSFT_screencoverage"]
        self.assertEqual(coverage[1:], sorted(coverage[1:], reverse=True))
        self.assertEqual(["--coverages", ",".join(f"{c:g}" for c in [round(v, 6) for v in coverage[1:]])], humans.PLAYER_AUTOLOD)
        for i in node["extensions"]["MSFT_lod"]["ids"]:
            self.assertEqual(doc["nodes"][i].get("skin"), node.get("skin"))

    def test_skin_and_cloth_are_albedos(self):
        for person in [MANIFEST["player"]] + MANIFEST["crowd"]:
            for kind, value in person["albedo"].items():
                self.assertLessEqual(value, humans.MAX_MEAN_ALBEDO, (person["name"], kind))

    def test_each_gender_has_darker_scans(self):
        for sex in ("m", "f"):
            tones = {p["skinTone"] for p in MANIFEST["crowd"] if p["sex"] == sex}
            self.assertIn("light", tones)
            self.assertIn("dark", tones)

    def test_people_are_drawn_at_the_scale_the_clips_were_timed_for(self):
        self.assertEqual(MANIFEST["scale"], humans.SCALE)
        # The player's run is retimed so 2.8 m/s at this scale keeps the feet on the ground.
        self.assertAlmostEqual(MANIFEST["player"]["clips"]["run"]["speed"] * MANIFEST["scale"], 2.8, places=2)
        self.assertAlmostEqual(MANIFEST["player"]["clips"]["sprint"]["speed"] * MANIFEST["scale"], 7.0, places=2)


if __name__ == "__main__":
    unittest.main()
