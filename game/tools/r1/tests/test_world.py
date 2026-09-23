import json
import math
from pathlib import Path
import tempfile
import subprocess
import unittest
from unittest.mock import patch

from r1.atlas import GENERIC, TUNIS, profile_for
from r1.world_tiles import tile_at, neighborhood, columns, Tile, ROWS
from r1.world_service import cook, clip_roads, compact_buildings
from r1.sources import Bounds, OsmWay, ElevationGrid


def flat_ground(x, y):
    return (x, 5.0, y)


def square_way(osm_id, tags, side=10.0):
    """A closed, axis-aligned footprint in metres — no projection involved."""
    return OsmWay(osm_id, ((0, 0), (side, 0), (side, side), (0, side), (0, 0)), tags)


class GlobalTiles(unittest.TestCase):
    def test_native_coordinate_contract_matches_pipeline(self):
        exe=Path(__file__).resolve().parents[3]/"generated/world-windows/R1World.exe"
        if not exe.exists():self.skipTest("Build the native game first")
        result=subprocess.run([str(exe),"--geo-contract"],capture_output=True,text=True,check=True)
        for sample in json.loads(result.stdout):
            self.assertEqual(sample["tile"],tile_at(sample["lon"],sample["lat"]).key)
            self.assertLess(sample["roundTripError"],1e-7)
            self.assertTrue(all(math.isfinite(x) for x in sample["step"]))
            self.assertTrue(-90<=sample["step"][1]<=90)
    def test_full_globe_and_date_line(self):
        for lat in [-90,-89.999,-80,-33,0,48,80,89.999,90]:
            for lon in [-180,-179.999,0,179.999,180]:
                t=tile_at(lon,lat)
                self.assertTrue(0<=t.row<ROWS)
                self.assertTrue(0<=t.col<columns(t.row))
                near=neighborhood(lon,lat)
                self.assertEqual(near[0],t)
                self.assertLessEqual(len(near),9)
                self.assertEqual(len(near),len(set(near)))
        self.assertEqual(tile_at(-180,0),tile_at(180,0))
        near=neighborhood(179.999,0)
        self.assertTrue(any(t.col==0 for t in near))

    def test_tile_centers_round_trip(self):
        for row in range(0,ROWS,113):
            for col in [0,columns(row)//2,columns(row)-1]:
                t=Tile(row,col)
                self.assertEqual(tile_at(*t.center),t)

    def test_reject_non_finite_and_invalid_latitude(self):
        for lon,lat in [(math.nan,0),(math.inf,3),(1,91),(1,-91),(0,math.nan)]:
            with self.assertRaises(ValueError):tile_at(lon,lat)


class WorldContent(unittest.TestCase):
    def test_long_osm_way_is_clipped_before_sampling(self):
        bounds=Bounds(0,0,1,1)
        road=OsmWay(1,((-2,.5),(3,.5)),{"highway":"residential"})
        clipped=clip_roads((road,),bounds)
        self.assertEqual(clipped[0].points,((0.,.5),(1.,.5)))
        outside=OsmWay(2,((-2,-1),(3,-1)),{})
        self.assertEqual(clip_roads((outside,),bounds),())

    def test_building_height_and_footprint_are_preserved(self):
        """A measured height is the building's height, to the centimetre.

        The tile now infers everything OSM leaves out, so the thing worth
        pinning is what inference must never touch: `height` is a measurement
        and the roof yields to it, so the ridge lands at exactly 5 + 42.
        """
        w = square_way(1, {"height": "42"})
        parts, footprints, stats = compact_buildings((w,), flat_ground, GENERIC)
        top = max(p[1] for part in parts for p in part.mesh.positions)
        self.assertAlmostEqual(top, 47.0, places=6)
        self.assertEqual(stats.as_json()["heightMeasured"], 1)
        self.assertEqual(stats.as_json()["heightInferred"], 0)
        self.assertEqual(len(footprints[0]), 4)

    def test_the_region_decides_what_osm_does_not_say(self):
        """Two untagged buildings, two regions, two different buildings.

        This is the whole point of taking the world tiles through the Atlas:
        before it, an untagged footprint was three storeys and a flat grey roof
        from Chamonix to Tunis. The assertion is not that either answer is
        right — nobody knows how tall that particular building is — but that
        the region is being *consulted*, which is what makes it correctable.
        """
        w = square_way(4242, {"building": "yes"})
        heights, palettes = set(), set()
        for profile in (TUNIS, profile_for(2.35, 48.86), profile_for(139.77, 35.68)):
            parts, _, stats = compact_buildings((w,), flat_ground, profile)
            heights.add(round(max(p[1] for part in parts for p in part.mesh.positions), 3))
            palettes.add(tuple(sorted(part.material.name for part in parts)))
            self.assertEqual(stats.as_json()["heightInferred"], 1)
        self.assertEqual(len(heights), 3, f"regions agreed on a height: {heights}")
        self.assertEqual(len(palettes), 3, f"regions share a palette: {palettes}")

    def test_a_tagged_roof_beats_the_region_that_would_have_guessed(self):
        """Tunis draws flat roofs nine times in ten; a tag overrules all ten."""
        tagged = square_way(7, {"building": "yes", "roof:shape": "gabled"})
        _, _, stats = compact_buildings((tagged,), flat_ground, TUNIS)
        self.assertEqual(stats.as_json()["roofTagged"], 1)
        self.assertEqual(stats.as_json()["roofShapes"], {"gabled": 1})

    def test_the_same_building_is_the_same_building_every_run(self):
        """§4 I3. Nothing here may reach the global random state."""
        ways = tuple(square_way(osm_id, {"building": "yes"}) for osm_id in range(30, 60))
        first = compact_buildings(ways, flat_ground, TUNIS)
        second = compact_buildings(ways, flat_ground, TUNIS)
        self.assertEqual([(p.material.name, p.mesh.positions) for p in first[0]],
                         [(p.material.name, p.mesh.positions) for p in second[0]])

    def test_a_dense_tile_of_pitched_roofs_stays_inside_the_budget(self):
        """§4 I4. The headroom that pays for the roofs, pinned.

        Paris's densest measured tile is 524 buildings and 119 414 vertices —
        two thirds of the budget — so the margin is real but it is no longer
        wide. This builds a denser tile than any that has been cooked, out of
        footprints that are all rectangular (so every one of them gets its
        pitched roof, which the real city's irregular blocks do not) under the
        profile that draws the most expensive roofs of the thirty-four. If a
        palette edit ever makes a roof cost twice what it costs today, the
        failure belongs here and not in the middle of a teleport.
        """
        from r1.atlas import AMSTERDAM
        from r1.world_service import TILE_VERTEX_BUDGET
        ways = []
        for index in range(700):
            x, z = (index % 26) * 18.0, (index // 26) * 18.0
            ways.append(OsmWay(9000 + index,
                               ((x, z), (x + 12, z), (x + 12, z + 9), (x, z + 9), (x, z)),
                               {"building": "yes"}))
        parts, _, stats = compact_buildings(tuple(ways), flat_ground, AMSTERDAM)
        vertices = sum(len(part.mesh.positions) for part in parts)
        shapes = stats.as_json()["roofShapes"]
        self.assertEqual(stats.as_json()["count"], 700)
        # The test is only worth anything if the roofs it counts are pitched:
        # a profile edit that quietly flattened the region would otherwise make
        # this pass by building nothing.
        self.assertGreater(sum(v for k, v in shapes.items() if k != "flat"), 560, shapes)
        self.assertLess(vertices, TILE_VERTEX_BUDGET, f"{vertices} vertices")

    def test_the_preview_lod_never_models_an_opening(self):
        """§12.4 at generation time: the world gets bays, not holes.

        A world tile that started emitting reveals and glazing would pass every
        other test in this file and then fail the vertex budget in the middle
        of a city, which is the worst place to find out.
        """
        ways = tuple(square_way(osm_id, {"shop": "bakery"}) for osm_id in range(3))
        parts, _, stats = compact_buildings(ways, flat_ground, profile_for(2.35, 48.86))
        self.assertEqual(stats.as_json()["facadesDetailed"], 0)
        names = [part.name for part in parts]
        self.assertFalse([n for n in names if "Openings" in n], names)
        self.assertTrue(all(part.material.base_color_texture
                            for part in parts if part.name.startswith("Walls")))

    def test_source_failure_never_publishes_ready(self):
        with tempfile.TemporaryDirectory() as folder:
            with patch("r1.world_service.CACHE",Path(folder)),patch("r1.world_service.fetch_ground",side_effect=OSError("offline")):
                t=tile_at(2.35,48.85)
                with self.assertRaises(OSError):cook(t)
                self.assertFalse((Path(folder)/t.key/"ready.json").exists())

    def test_ready_cache_never_contacts_sources(self):
        with tempfile.TemporaryDirectory() as folder:
            t=tile_at(2.35,48.85)
            p=Path(folder)/t.key;p.mkdir()
            (p/"ready.json").write_text(json.dumps({"key":t.key}))
            with patch("r1.world_service.CACHE",Path(folder)),patch("r1.world_service.fetch_ground",side_effect=AssertionError("network")):
                self.assertEqual(cook(t)["key"],t.key)


if __name__=="__main__":unittest.main()
