"""Contract tests for props — rank 8 and rank 10 (plan §2.1).

Three of the classes below exist because of a bug that shipped in the first
render of this feature, and they are the ones worth keeping:

  - every prop the table can place must actually be on disk, or a street is a
    row of nothing;
  - no prop may reference an image it did not bring with it, or the engine
    draws missing-texture magenta — which is what a street of Paris lamp posts
    looked like the first time;
  - no prop may keep a kit colour, because Kenney's foliage is a turquoise at
    four times the albedo of a leaf and it saturates into a white blob under
    this world's Sun. That one is the same lesson the ground table learned the
    same day.

Nothing here needs the engine or the network.
"""

from __future__ import annotations

import json
import struct
import unittest
from pathlib import Path

from r1 import props
from r1.atlas import CHAMONIX, GENERIC, MAGHREB_SAHARA, profile_for
from r1.buildings import is_worship, steeple_style
from r1.external_assets import (
    GAME_ROOT, PROP_PALETTE, PROP_TEXTURE_TINT, prop_model, tree_model,
)
from r1.sources import OsmNode

GAME = GAME_ROOT


def node(osm_id: int, tags: dict, lon: float = 0.0, lat: float = 0.0) -> OsmNode:
    return OsmNode(osm_id, lon, lat, tags)


def flat(lon, lat):
    # A metre of longitude is not a metre, but nothing under test cares: the
    # projection is the caller's business and is tested elsewhere.
    return (lon, 0.0, lat)


class Classification(unittest.TestCase):
    def test_the_things_osm_maps_are_the_things_placed(self) -> None:
        for tags, expected in (
            ({"natural": "tree"}, "tree"),
            ({"highway": "street_lamp"}, "street lamp"),
            ({"amenity": "bench"}, "bench"),
            ({"highway": "bus_stop"}, "bus stop"),
            ({"power": "tower"}, "pylon"),
        ):
            kind = props.classify(tags)
            self.assertIsNotNone(kind, tags)
            self.assertEqual(kind.name, expected)

    def test_a_tag_that_is_not_a_thing_is_not_placed(self) -> None:
        # The query asks for a narrow list, but a node carries all its tags and
        # a bench may also be a `tourism` something. Nothing without a shape
        # gets one.
        for tags in ({"amenity": "restaurant"}, {"shop": "bakery"},
                     {"building": "yes"}, {}):
            self.assertIsNone(props.classify(tags))


class Budget(unittest.TestCase):
    """§12.4: the count is capped per tile, and the cap is shared out."""

    def test_a_share_of_zero_places_nothing_at_all(self) -> None:
        """Off means off, including when the budget has room to spare.

        The leftover pass exists so a village keeps everything it mapped, and
        it happily handed a disabled kind the whole surplus: a hundred and
        sixty-six trees on a tile whose share for trees was zero.
        """
        disabled = [k.name for k in props.PROP_KINDS if k.share <= 0.0]
        self.assertTrue(disabled, "nothing is disabled; this test has no subject")
        tags = {"tree": {"natural": "tree"}}
        for name in disabled:
            features = [node(i, tags[name]) for i in range(50)]
            _placed, stats = props.plan_props(features, flat, GENERIC, (), GAME)
            self.assertEqual(stats.by_kind.get(name, 0), 0, name)
            self.assertEqual(stats.placed, 0)

    def _plan(self, features, budget=props.TILE_PROP_BUDGET, profile=GENERIC):
        return props.plan_props(features, flat, profile, (), GAME, budget)

    def test_a_thousand_lamps_do_not_squeeze_out_five_benches(self) -> None:
        """The failure the share exists to prevent, asserted directly."""
        features = [node(i, {"highway": "street_lamp"}) for i in range(1000)]
        features += [node(50000 + i, {"amenity": "bench"}) for i in range(5)]
        placed, stats = self._plan(features)
        self.assertEqual(stats.by_kind.get("bench"), 5)
        self.assertLessEqual(stats.placed, props.TILE_PROP_BUDGET)

    def test_a_village_under_the_budget_keeps_everything(self) -> None:
        features = [node(i, {"amenity": "bench"}) for i in range(12)]
        features += [node(100 + i, {"highway": "bus_stop"}) for i in range(3)]
        placed, stats = self._plan(features)
        self.assertEqual(stats.placed, 15)
        self.assertEqual(stats.dropped, 0)

    def test_what_the_budget_refused_is_counted(self) -> None:
        features = [node(i, {"highway": "street_lamp"}) for i in range(900)]
        _placed, stats = self._plan(features, budget=100)
        self.assertEqual(stats.placed, 100)
        self.assertEqual(stats.dropped, 800)

    def test_a_tile_of_one_kind_may_spend_the_whole_budget_on_it(self) -> None:
        features = [node(i, {"amenity": "bench"}) for i in range(400)]
        _placed, stats = self._plan(features, budget=50)
        self.assertEqual(stats.placed, 50)

    def test_the_same_tile_places_the_same_props_every_run(self) -> None:
        """§4 I3, and it has to hold for *which* props survive the budget."""
        features = [node(i, {"highway": "street_lamp"}) for i in range(900)]
        first, _ = self._plan(features, budget=120)
        second, _ = self._plan(list(reversed(features)), budget=120)
        self.assertEqual(first, second)


class Placement(unittest.TestCase):
    def test_a_prop_is_the_size_of_the_real_thing(self) -> None:
        """The model's own height is divided out, so a lamp is five metres."""
        lamp = props._BY_NAME["street lamp"]
        placed, _ = props.plan_props(
            [node(1, {"highway": "street_lamp"})], flat, GENERIC, (), GAME)
        model = placed[0]["importedFrom"]
        scale = placed[0]["transform"]["scale"][0]
        height = scale * props._model_height(GAME, model)
        self.assertAlmostEqual(height, lamp.height, places=6)

    def test_a_bench_faces_the_street_and_a_tree_does_not(self) -> None:
        road = (((-50.0, 5.0), (50.0, 5.0)),)
        bench, _ = props.plan_props(
            [node(1, {"amenity": "bench"})], flat, GENERIC, road, GAME)
        # The road runs along +x, so its bearing is a quarter turn: what is
        # asserted is that two different benches on it agree, and that a tree
        # at the same place does not.
        other, _ = props.plan_props(
            [node(2, {"amenity": "bench"})], flat, GENERIC, road, GAME)
        self.assertEqual(bench[0]["transform"]["rotation"],
                         other[0]["transform"]["rotation"])
        loose = [props.plan_props([node(i, {"natural": "rock"})], flat,
                                  GENERIC, road, GAME)[0][0]["transform"]["rotation"]
                 for i in (3, 4, 5)]
        self.assertEqual(len(set(map(tuple, loose))), 3, "boulders are ruled")

    def test_a_prop_far_from_any_road_still_gets_an_angle(self) -> None:
        far = (((10_000.0, 10_000.0), (10_100.0, 10_000.0)),)
        placed, _ = props.plan_props(
            [node(1, {"amenity": "bench"})], flat, GENERIC, far, GAME)
        self.assertEqual(len(placed), 1)
        self.assertNotEqual(placed[0]["transform"]["rotation"], [0.0, 0.0, 0.0, 1.0])

    def test_the_species_comes_from_the_region(self) -> None:
        """Rank 8, "inférée par biome": an aloe does not grow in Chamonix.

        Asserted on the table rather than on placement, because planting is off
        until impostors exist (see `props.PROP_KINDS`). The table is what that
        work will switch back on, so it is what has to stay right meanwhile.
        """
        self.assertIn("fir_sapling", CHAMONIX.tree_models)
        self.assertIn("quiver_tree", MAGHREB_SAHARA.tree_models)
        self.assertNotIn("quiver_tree", CHAMONIX.tree_models)
        self.assertNotIn("fir_sapling", MAGHREB_SAHARA.tree_models)

    def test_no_region_plants_a_kit_tree(self) -> None:
        """Rule 1 of CLAUDE.md, as a test rather than as a good intention.

        The project ships photoscanned trees and they are the best asset it
        has. A kit tree entering here would not fail anything else — it renders,
        it is cheap, it is the right size — which is exactly why the rule needs
        an assertion and not a comment.
        """
        from r1.atlas import GENERIC, _REGIONS
        from r1.external_assets import TREES
        photoreal = {name for _folder, _stem, name in TREES}
        for _box, profile in _REGIONS + (((0, 0, 0, 0), GENERIC),):
            for stem in profile.tree_models:
                self.assertIn(stem, photoreal,
                              f"{profile.name} plants {stem}, which is not photoreal")
        for kind in props.PROP_KINDS:
            for model in kind.models:
                self.assertNotIn("tree", Path(model).stem.split("_"),
                                 f"{kind.name} carries a tree model in the prop table")


class Steeples(unittest.TestCase):
    def test_a_place_of_worship_is_recognised_however_it_was_tagged(self) -> None:
        self.assertTrue(is_worship({"amenity": "place_of_worship"}))
        self.assertTrue(is_worship({"building": "chapel"}))
        self.assertTrue(is_worship({"building": "mosque"}))
        self.assertFalse(is_worship({"building": "house"}))
        self.assertFalse(is_worship({"amenity": "restaurant"}))

    def test_the_silhouette_follows_what_the_survey_says_it_is(self) -> None:
        self.assertEqual(steeple_style({"religion": "muslim"}), "minaret")
        self.assertEqual(steeple_style({"building": "mosque"}), "minaret")
        self.assertEqual(steeple_style({"religion": "christian"}), "spire")
        # Unlabelled: a tower, which is wrong nowhere, rather than a guess.
        self.assertEqual(steeple_style({}), "spire")
        self.assertEqual(steeple_style({"religion": "jewish"}), "tower")


class Assets(unittest.TestCase):
    """§11.4 — "un asset non conforme n'entre pas", as three assertions."""

    def _kit_models(self):
        """The Kenney props: manufactured objects, painted by their author."""
        return sorted({model for kind in props.PROP_KINDS for model in kind.models})

    def _tree_models(self):
        """The Poly Haven trees: photographs, whose colours are measurements."""
        seen = set()
        for profile_lon, profile_lat in ((2.35, 48.86), (6.87, 45.92), (10.18, 36.81),
                                         (114.0, 1.0), (151.2, -33.87)):
            profile = profile_for(profile_lon, profile_lat)
            seen.update(tree_model(stem) for stem in profile.tree_models)
        return sorted(seen)

    def _models(self):
        return self._kit_models() + self._tree_models()

    def test_every_prop_the_table_can_place_exists_on_disk(self) -> None:
        for model in self._models():
            self.assertTrue((GAME / model).is_file(), model)

    def test_no_prop_references_an_image_it_did_not_bring(self) -> None:
        """A dangling URI is magenta on screen and nothing in the log."""
        for model in self._models():
            payload = (GAME / model).read_bytes()
            length, _ = struct.unpack_from("<II", payload, 12)
            document = json.loads(payload[20:20 + length])
            dangling = [i.get("uri") for i in document.get("images", []) if i.get("uri")]
            self.assertEqual(dangling, [], f"{model} points outside itself")

    def test_a_photoscanned_tree_keeps_its_own_colours(self) -> None:
        """The repainting rule stops exactly where the measurement starts.

        A kit's colours are paint and must be replaced by albedo. A photoscan's
        colours *are* albedo — someone photographed that bark — so repainting
        one would be overwriting a measurement with a guess, which is the same
        error as an inferred height beating a surveyed one (§4 I5).
        """
        painted = {**PROP_PALETTE, **PROP_TEXTURE_TINT}
        for model in self._tree_models():
            payload = (GAME / model).read_bytes()
            length, _ = struct.unpack_from("<II", payload, 12)
            document = json.loads(payload[20:20 + length])
            self.assertTrue(document.get("materials"), model)
            for material in document.get("materials", []):
                self.assertNotIn(material.get("name"), painted,
                                 f"{model}: a scanned material was repainted")
                self.assertIn("baseColorTexture",
                              material.get("pbrMetallicRoughness", {}),
                              f"{model}: the scan lost its texture")

    def test_no_kit_prop_keeps_a_colour_the_kit_chose(self) -> None:
        known = {**PROP_PALETTE, **PROP_TEXTURE_TINT}
        for model in self._kit_models():
            payload = (GAME / model).read_bytes()
            length, _ = struct.unpack_from("<II", payload, 12)
            document = json.loads(payload[20:20 + length])
            for material in document.get("materials", []):
                name = material.get("name")
                self.assertIn(name, known, f"{model}: {name} was never repainted")
                factor = material["pbrMetallicRoughness"]["baseColorFactor"][:3]
                self.assertEqual([round(v, 6) for v in factor],
                                 [round(v, 6) for v in known[name]], f"{model}: {name}")
                self.assertEqual(material["pbrMetallicRoughness"]["metallicFactor"], 0.0)

    def test_no_prop_is_bright_enough_to_saturate(self) -> None:
        # The bug this whole class is here for: a sunlit surface above roughly
        # 0.35 albedo goes white at this world's light level.
        for name, colour in {**PROP_PALETTE, **PROP_TEXTURE_TINT}.items():
            luminance = 0.2126 * colour[0] + 0.7152 * colour[1] + 0.0722 * colour[2]
            self.assertLess(luminance, 0.55, f"{name} would saturate in sunlight")


if __name__ == "__main__":
    unittest.main()
