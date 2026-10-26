"""The animals: placed by fauna.json, measured by r1/animals.py, never wrong-way round.

Every model a species places is measured (ground, head, gaits) with both its
levels on one skeleton; a clip refused for a defect plays no part; birds,
bats, fish and sea animals wait for their own update; indoor animals stay rare.
"""
import json
import math
import unittest
from pathlib import Path

from r1 import animals

GAME = Path(__file__).resolve().parents[3]
FAUNA = json.loads((GAME / "assets/animals/fauna.json").read_text(encoding="utf-8"))
MEASURED = json.loads((GAME / "assets/animals/animals.json").read_text(encoding="utf-8"))["models"]

# Not this update: birds and bats fly, fish and sea animals swim (PLAN).
NOT_YET = {"crow", "hawk", "hawk_coat2", "seagull", "eagle_bald", "owl_eagle", "parrot_macaw", "pigeon_rock",
           "robin", "shrike", "peacock", "turkey", "chicken_kelgar", "rooster", "bat", "dolphin", "fish_generic",
           "fish_single", "fish_kelgar", "pike", "shark", "shark_coat2", "tuna", "whale_fin", "turtle_sea",
           "horseshoe_crab"}


class Animals(unittest.TestCase):
    def test_every_placed_model_is_measured_on_one_skeleton(self):
        for model, override in animals.used_models(FAUNA).items():
            self.assertIn(model, MEASURED, f"{model}: run python -m r1.animals")
            m = MEASURED[model]
            self.assertTrue(m["sameRig"], f"{model}: its LOD1 has another skeleton")
            self.assertTrue((GAME / m["lod0"]).is_file() and (GAME / m["lod1"]).is_file(), model)
            self.assertTrue(math.isfinite(m["forward"]) and m["height"] > 0, model)
            self.assertTrue(m["roles"], f"{model}: no clip plays")

    def test_a_refused_clip_plays_no_part(self):
        for model, m in MEASURED.items():
            played = {c for clips in m["roles"].values() for c in clips}
            self.assertFalse(played & set(m.get("refusedClips", {})), model)

    def test_gaits_say_where_their_speed_comes_from(self):
        for model, m in MEASURED.items():
            for role, gait in m["gaits"].items():
                self.assertGreater(gait["speed"], 0, f"{model} {role}")
                self.assertTrue(gait["speedSource"].startswith(("measured", "inferred")), f"{model} {role}")
                self.assertEqual(m["roles"][role][0], gait["clip"], f"{model}: {role} plays its measured clip first")

    def test_no_bird_fish_or_sea_animal_yet(self):
        for model in animals.used_models(FAUNA):
            self.assertNotIn(model, NOT_YET)

    def test_indoor_animals_are_rare(self):
        for species in FAUNA["species"]:
            homes = species.get("indoor", {}).get("homes", 0)
            self.assertLessEqual(homes, 0.1, species["id"])

    def test_clip_roles_follow_their_names(self):
        roles = animals.assign_roles(["bovidae_idle_a", "bovidae_feeding", "bovidae_walk_relax", "bovidae_run",
                                      "bovidae_death_a", "bovidae_attack"])
        self.assertEqual(roles, {"idle": ["bovidae_idle_a"], "eat": ["bovidae_feeding"],
                                 "walk": ["bovidae_walk_relax"], "run": ["bovidae_run"]})
        self.assertEqual(animals.assign_roles(["Rig_Check_Head", "Rig_Check_Limbs"]), {"idle": ["Rig_Check_Head"]})
        self.assertEqual(animals.assign_roles(["peacock_walk_feeding", "peacock_feeding_transition"]), {})
        self.assertEqual(animals.assign_roles(["Flying"], {"fly": ["Flying"]}), {"fly": ["Flying"]})

    def test_registered_with_their_licences(self):
        registry = json.loads((GAME / "assets/THIRD_PARTY_ASSETS.json").read_text(encoding="utf-8"))
        files = {f for a in registry["assets"] if a["name"].startswith("Animals — ") for f in a["files"]}
        for model in animals.used_models(FAUNA):
            self.assertIn(f"assets/animals/{model}.glb", files)
            self.assertIn(f"assets/animals/{model}-lod1.glb", files)


if __name__ == "__main__":
    unittest.main()
