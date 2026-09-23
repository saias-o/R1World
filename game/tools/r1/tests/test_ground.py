"""Contract tests for ground materials (plan §2.1 rank 9).

Almost nothing here has a right answer to check against: there is no true colour
for a field. What can be checked, and is what the plan actually asks for, is the
structure — that a lake beats the wood it sits in, that the numbers in the table
are albedos and not paint, that partitioning the terrain costs nothing, and that
the collision bitmap and the picture cannot disagree about where the bank is.

Nothing here touches a projection, an elevation service or the network: the
polygons are written as literal longitudes and latitudes and the ground is flat.
"""

from __future__ import annotations

import unittest

from r1 import ground
from r1.atlas import MAGHREB_SAHARA, profile_for
from r1.terrain import TERRAIN_MESH_SIZE, build_terrain
from r1.sources import Bounds, ElevationGrid, OsmWay
from r1.geodesy import Anchor
from r1.world_service import WATER_GRID, _water_grid


def ring(west, south, east, north):
    return ((west, south), (east, south), (east, north), (west, north), (west, south))


def area(osm_id, tags, box=(0.2, 0.2, 0.8, 0.8)):
    return OsmWay(osm_id, ring(*box), tags)


FLAT = ElevationGrid(Bounds(0.0, 0.0, 1.0, 1.0), 2, ((0.0, 0.0), (0.0, 0.0)))


class Classification(unittest.TestCase):
    def test_water_beats_everything_it_sits_inside(self) -> None:
        """A lake in a forest is a lake, whatever order OSM returned them in."""
        forest = area(1, {"landuse": "forest"}, (0.0, 0.0, 1.0, 1.0))
        lake = area(2, {"natural": "water"}, (0.4, 0.4, 0.6, 0.6))
        for ways in ((forest, lake), (lake, forest)):
            cover = ground.Landcover(ways)
            self.assertEqual(cover.at(0.5, 0.5), "water")
            self.assertEqual(cover.at(0.1, 0.1), "forest")

    def test_a_park_inside_a_district_is_a_park(self) -> None:
        cover = ground.Landcover((
            area(1, {"landuse": "residential"}, (0.0, 0.0, 1.0, 1.0)),
            area(2, {"leisure": "park"}, (0.1, 0.1, 0.3, 0.3)),
        ))
        self.assertEqual(cover.at(0.2, 0.2), "grass")
        self.assertEqual(cover.at(0.7, 0.7), "urban")

    def test_a_way_that_says_nothing_about_the_ground_is_not_ground(self) -> None:
        for tags in ({"building": "yes"}, {"highway": "residential"}, {}):
            self.assertIsNone(ground.classify_way(tags))

    def test_outside_every_polygon_is_nothing_rather_than_a_guess(self) -> None:
        # The guess is the region's business, and it is made one layer up so
        # that the manifest can count it (§4 I5).
        cover = ground.Landcover((area(1, {"landuse": "forest"}),))
        self.assertIsNone(cover.at(0.05, 0.05))

    def test_the_region_answers_only_for_what_osm_did_not(self) -> None:
        self.assertIs(ground.material_for(ground.INFERRED, MAGHREB_SAHARA),
                      MAGHREB_SAHARA.ground)
        self.assertIs(ground.material_for("water", MAGHREB_SAHARA),
                      ground.SWATCHES["water"])


class Albedo(unittest.TestCase):
    """The table holds albedos. Chosen as colours, it renders as one white.

    This is the test that would have caught the first version of the module: it
    gave asphalt 0.38 and dry sand 0.78, both of which saturate a sunlit
    horizontal surface at the world's own light level, so the entire ground
    palette collapsed into the same value on screen.
    """

    # Published ranges, in the units glTF's baseColorFactor is in. Generous at
    # both ends: what is being pinned is that nobody typed a paint colour here.
    RANGES = {
        "water": (0.01, 0.09), "glacier": (0.70, 0.95), "rock": (0.12, 0.30),
        "sand": (0.28, 0.45), "wetland": (0.07, 0.20), "forest": (0.05, 0.18),
        "scrub": (0.08, 0.22), "orchard": (0.10, 0.25), "grass": (0.13, 0.28),
        "farmland": (0.12, 0.28), "bare": (0.15, 0.35), "urban": (0.08, 0.20),
    }

    @staticmethod
    def luminance(color) -> float:
        return 0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2]

    def test_every_class_is_inside_its_published_range(self) -> None:
        self.assertEqual(set(self.RANGES), set(ground.SWATCHES))
        for name, (low, high) in self.RANGES.items():
            value = self.luminance(ground.SWATCHES[name].color)
            self.assertGreaterEqual(value, low, f"{name} is darker than any real one")
            self.assertLessEqual(value, high, f"{name} would saturate in sunlight")

    def test_every_region_ground_is_a_plausible_albedo_too(self) -> None:
        from r1.atlas import _REGIONS
        for _box, profile in _REGIONS:
            value = self.luminance(profile.ground.color)
            self.assertGreater(value, 0.05, profile.name)
            self.assertLess(value, 0.45, f"{profile.name} would saturate in sunlight")

    def test_a_made_surface_is_darker_than_desert_sand(self) -> None:
        # The pair the whole table exists to keep apart. If these two ever come
        # within a few per cent of each other again, a city and a desert are the
        # same picture.
        made = self.luminance(profile_for(139.77, 35.68).ground.color)   # Tokyo
        sand = self.luminance(MAGHREB_SAHARA.ground.color)
        self.assertLess(made * 2.0, sand, f"made {made:.3f} vs sand {sand:.3f}")


class TerrainPartition(unittest.TestCase):
    def setUp(self) -> None:
        self.bounds = Bounds(0.0, 0.0, 1.0, 1.0)
        self.anchor = Anchor.at(0.5, 0.5, 0.0)

    def _build(self, classify=None):
        return build_terrain(self.bounds, FLAT, self.anchor, classify)

    def test_partitioning_the_terrain_creates_no_vertices(self) -> None:
        """The claim `ground.py` is built on, asserted rather than believed."""
        whole = self._build()
        cover = ground.Landcover((
            area(1, {"natural": "water"}, (0.0, 0.0, 0.5, 1.0)),
            area(2, {"landuse": "forest"}, (0.5, 0.0, 0.8, 1.0)),
        ))
        split = self._build(lambda x, y: cover.at(x, y) or ground.INFERRED)
        self.assertGreater(len(split), 2, "the classifier was not consulted")
        self.assertEqual(sum(len(m.positions) for m in split.values()),
                         sum(len(m.positions) for m in whole.values()))
        self.assertEqual(sum(len(m.indices) for m in split.values()),
                         sum(len(m.indices) for m in whole.values()))

    def test_with_no_classifier_it_is_one_unnamed_mesh(self) -> None:
        meshes = self._build()
        self.assertEqual(list(meshes), [""])
        self.assertEqual(len(meshes[""].indices) // 3,
                         2 * (TERRAIN_MESH_SIZE - 1) ** 2)

    def test_a_class_covering_everything_covers_everything(self) -> None:
        cover = ground.Landcover((area(1, {"natural": "water"}, (-1.0, -1.0, 2.0, 2.0)),))
        meshes = self._build(lambda x, y: cover.at(x, y) or ground.INFERRED)
        self.assertEqual(list(meshes), ["water"])


class WaterBitmap(unittest.TestCase):
    """The collision bitmap is the picture's own grid, so they cannot drift."""

    def test_the_bitmap_matches_the_terrain_the_player_sees(self) -> None:
        bounds = Bounds(0.0, 0.0, 1.0, 1.0)
        cover = ground.Landcover((area(1, {"natural": "water"}, (0.25, 0.25, 0.75, 0.75)),))
        rows = _water_grid(bounds, cover)
        self.assertEqual(len(rows), WATER_GRID)
        self.assertTrue(all(len(row) == WATER_GRID for row in rows))
        # Same question, same answer: the cell centre decides both.
        for row in range(WATER_GRID):
            lat = (row + 0.5) / WATER_GRID
            for col in range(WATER_GRID):
                lon = (col + 0.5) / WATER_GRID
                wet = cover.at(lon, lat) == "water"
                self.assertEqual(rows[row][col] == "1", wet, f"{row},{col}")

    def test_rows_run_south_to_north(self) -> None:
        """The native side indexes it that way; getting it upside down would
        block the far bank and let the player walk on the near one."""
        cover = ground.Landcover((area(1, {"natural": "water"}, (0.0, 0.0, 1.0, 0.25)),))
        rows = _water_grid(Bounds(0.0, 0.0, 1.0, 1.0), cover)
        self.assertIn("1", rows[0])
        self.assertNotIn("1", rows[-1])

    def test_dry_land_ships_an_empty_bitmap_not_a_missing_one(self) -> None:
        rows = _water_grid(Bounds(0.0, 0.0, 1.0, 1.0), ground.Landcover(()))
        self.assertEqual(len(rows), WATER_GRID)
        self.assertEqual(set("".join(rows)), {"0"})


if __name__ == "__main__":
    unittest.main()
