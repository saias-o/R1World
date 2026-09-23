"""Contract tests for the car — what "se déplacer en voiture" owes the world.

The driving itself lives in `native/world.cpp` and is held by the E2E driver
there: it walks to the car, gets in, drives, is refused the door at 76 km/h,
brakes, and gets out. What this file holds is everything that must be true
*before* the game starts, and each class below is one thing that would not be
caught by playing for five minutes:

  - the car must be a member of the entry scene, not a prop of a tile, or a
    teleport evicts it with the neighbourhood the player left;
  - it must be repainted like every other kit model, because Kenney's shared
    `colormap.png` is paint at roughly 0.8 where painted metal is 0.2 (§11.4);
  - it must be the right size in metres, and the scale must say which
    measurement it honours — the kit's saloon is 1.7 times longer than it is
    wide where a real one is 2.45, so one of the two is given up on purpose;
  - and it must not have displaced anything. Rule 1 of `CLAUDE.md` was broken
    twice by a kit model walking in over a better asset, so the test that
    matters most here is the one that says the trees did not move.

Nothing here needs the engine or the network.
"""

from __future__ import annotations

import json
import struct
import unittest
from pathlib import Path

from r1 import prepare_world
from r1.external_assets import (
    GAME_ROOT, PROP_KITS, PROP_PALETTE, PROP_TEXTURE_TINT, VEHICLE_LENGTH,
    VEHICLE_MODELS, tree_model, vehicle_model,
)

GAME = GAME_ROOT
CAR = vehicle_model("sedan")


def document(model: str) -> dict:
    payload = (GAME / model).read_bytes()
    length, _ = struct.unpack_from("<II", payload, 12)
    return json.loads(payload[20:20 + length])


def extent(model: str) -> tuple[float, float, float]:
    """The model's own bounding box in its own units, from its accessors."""
    doc = document(model)
    low = [float("inf")] * 3
    high = [float("-inf")] * 3
    for mesh in doc["meshes"]:
        for primitive in mesh["primitives"]:
            accessor = doc["accessors"][primitive["attributes"]["POSITION"]]
            low = [min(a, b) for a, b in zip(low, accessor["min"])]
            high = [max(a, b) for a, b in zip(high, accessor["max"])]
    return tuple(h - l for l, h in zip(low, high))


def car_node() -> dict:
    scene = json.loads((GAME / "scenes" / "earth.scene").read_text(encoding="utf-8"))
    return next(n for n in scene["scene"]["children"] if "vehicle" in n.get("groups", []))


class Shipped(unittest.TestCase):
    def test_the_car_is_on_disk(self) -> None:
        self.assertTrue((GAME / CAR).is_file(), CAR)

    def test_the_archive_it_comes_from_is_pinned(self) -> None:
        # Kenney publishes zips and no per-model URL, so the archive is what
        # can be pinned; a clean checkout must fetch exactly these bytes.
        archives = {kit.target.name: kit for kit in PROP_KITS}
        for archive_name, _stem, _target in VEHICLE_MODELS:
            self.assertIn(archive_name, archives)
            self.assertEqual(archives[archive_name].algorithm, "sha256")
            self.assertEqual(len(archives[archive_name].digest), 64)

    def test_provenance_names_it_with_its_licence(self) -> None:
        record = json.loads((GAME / "assets" / "THIRD_PARTY_ASSETS.json").read_text(encoding="utf-8"))
        entries = [a for a in record["assets"] if CAR in a.get("files", [])]
        self.assertEqual(len(entries), 1, "the car must be declared exactly once")
        self.assertEqual(entries[0]["license"], "CC0 1.0")
        self.assertIn("kenney_car-kit.zip", entries[0]["extractedFrom"])

    def test_it_brought_its_texture_with_it(self) -> None:
        # A dangling image URI is how a street of lamp posts came out magenta.
        dangling = [i.get("uri") for i in document(CAR).get("images", []) if i.get("uri")]
        self.assertEqual(dangling, [], f"{CAR} points outside itself")


class Normalised(unittest.TestCase):
    """§11.4: nothing enters in the state it was downloaded in."""

    def test_every_material_was_repainted(self) -> None:
        known = {**PROP_PALETTE, **PROP_TEXTURE_TINT}
        materials = document(CAR).get("materials", [])
        self.assertTrue(materials, CAR)
        for material in materials:
            name = material.get("name")
            self.assertIn(name, known, f"{CAR}: {name} was never repainted")
            factor = material["pbrMetallicRoughness"]["baseColorFactor"][:3]
            self.assertEqual([round(v, 6) for v in factor],
                             [round(v, 6) for v in known[name]], f"{CAR}: {name}")
            # A kit that leaves metallicFactor at its default of 1 turns a car
            # into a mirror under IBL, which is what the props learned first.
            self.assertEqual(material["pbrMetallicRoughness"]["metallicFactor"], 0.0)


class Size(unittest.TestCase):
    """A car is 1.80 m wide because cars are, not because it looked right."""

    def test_the_scene_scales_it_to_a_real_width(self) -> None:
        scale = car_node()["children"][0]["transform"]["scale"]
        self.assertEqual(scale[0], scale[1], "a non-uniform scale restyles the model")
        self.assertEqual(scale[1], scale[2])
        width = extent(CAR)[0] * scale[0]
        self.assertAlmostEqual(width, 1.80, places=2)

    def test_the_length_that_comes_out_is_said_out_loud(self) -> None:
        # The trade-off this scale makes: honouring the width gives up the
        # length, and the number it gives up must stay a real car's rather than
        # drift into a bus or a toy while nobody is looking.
        length = extent(CAR)[2] * car_node()["children"][0]["transform"]["scale"][2]
        self.assertLess(length, VEHICLE_LENGTH, "the kit saloon is shorter than a real one")
        self.assertGreater(length, 2.9, "shorter than a city car is not a car")

    def test_it_has_four_named_wheels(self) -> None:
        # `native/world.cpp` turns them by name. A re-export that renamed them
        # would leave a car sliding on frozen wheels.
        names = {n.get("name") for n in document(CAR)["nodes"]}
        self.assertEqual(
            {n for n in names if n and n.startswith("wheel-")},
            {"wheel-front-left", "wheel-front-right",
             "wheel-back-left", "wheel-back-right"})


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


class DisplacedNothing(unittest.TestCase):
    """Rule 1 of CLAUDE.md, held from the other side.

    The car is a kit model entering a repository whose best assets are
    photoscans, and that is exactly the shape of the substitution the rule was
    written for. It is allowed here because nothing it covers existed — there
    was no vehicle of any grade — and this test is what keeps that true.
    """

    def test_the_car_kit_ships_no_vegetation(self) -> None:
        stems = {stem for _archive, stem, _name in VEHICLE_MODELS}
        for stem in stems:
            self.assertNotIn("tree", stem)
            self.assertNotIn("plant", stem)

    def test_the_trees_are_still_the_photoscanned_ones(self) -> None:
        for species in ("fir_sapling", "pine_sapling", "quiver_tree", "broadleaf"):
            model = tree_model(species)
            self.assertIn("trees_lod", model, species)
            self.assertTrue((GAME / model).is_file(), model)


if __name__ == "__main__":
    unittest.main()
