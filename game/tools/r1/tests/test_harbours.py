"""Harbours: the sea from the coastline, the works OSM mapped, the boats inferred.

These hold `r1/harbours.py` to its claims: land left of a coastline and sea
right of it; the sea floor sunk under the animated surface and dry land never
under it; every boat moored on water, never overlapping another, in the fleet
its harbour and country call for; and nothing built outside the tile it
belongs to.
"""

from __future__ import annotations

import math
import unittest
from pathlib import Path

from r1 import atlas, ground, harbours
from r1.geodesy import Anchor
from r1.sources import Bounds, OsmNode, OsmWay

GAME = Path(__file__).resolve().parents[3]
BOUNDS = Bounds(43.270, 6.630, 43.275, 6.640)   # south, west, north, east


def _way(osm_id, points, **tags):
    return OsmWay(osm_id, tuple(points), dict(tags))


# A coastline running west to east across the middle of the tile: land on its
# left (north), sea on its right (south).
MID = (BOUNDS.south + BOUNDS.north) / 2.0
COAST = _way(1, [(6.620, MID), (6.650, MID)], natural="coastline")


def _cells(sea, landcover=(), elevation=lambda lon, lat: 5.0):
    return harbours.Cells(BOUNDS, 40, sea, ground.Landcover(landcover), elevation)


class SeaTests(unittest.TestCase):
    def test_sea_is_right_of_the_coastline(self) -> None:
        sea, how = harbours.sea_geometry([COAST], BOUNDS, lambda lon, lat: 5.0)
        self.assertEqual(how, "coastline")
        from shapely.geometry import Point
        self.assertTrue(sea.contains(Point(6.635, BOUNDS.south + 0.0005)))
        self.assertFalse(sea.contains(Point(6.635, BOUNDS.north - 0.0005)))

    def test_reversed_coastline_puts_the_sea_north(self) -> None:
        reverse = _way(2, list(reversed(COAST.points)), natural="coastline")
        sea, _ = harbours.sea_geometry([reverse], BOUNDS, lambda lon, lat: 5.0)
        from shapely.geometry import Point
        self.assertTrue(sea.contains(Point(6.635, BOUNDS.north - 0.0005)))

    def test_an_island_keeps_its_land(self) -> None:
        # Counter-clockwise: land on the left is the inside of the ring.
        ring = [(6.633, 43.2715), (6.637, 43.2715), (6.637, 43.2735), (6.633, 43.2735), (6.633, 43.2715)]
        sea, _ = harbours.sea_geometry([_way(3, ring, natural="coastline")], BOUNDS,
                                       lambda lon, lat: 0.0)
        from shapely.geometry import Point
        self.assertFalse(sea.contains(Point(6.635, 43.2725)))
        self.assertTrue(sea.contains(Point(6.631, 43.2705)))

    def test_no_coastline_no_sea(self) -> None:
        self.assertEqual(harbours.sea_geometry([], BOUNDS, lambda lon, lat: 0.0), (None, "no coastline"))

    def test_cells_mark_the_sea_and_sink_its_floor(self) -> None:
        sea, _ = harbours.sea_geometry([COAST], BOUNDS, lambda lon, lat: 5.0)
        cells = _cells(sea, elevation=lambda lon, lat: -0.2)
        self.assertTrue(cells.has_sea)
        self.assertEqual(cells.codes[2][20], 2)     # south row
        self.assertEqual(cells.codes[37][20], 0)    # north row
        self.assertEqual(cells.adjust(2, 20, 0.0), harbours.SEA_FLOOR)
        # Dry land beside a sea is never drawn under it.
        self.assertEqual(cells.adjust(37, 20, -0.6), harbours.LAND_ABOVE_SEA)
        self.assertEqual(cells.rows()[2][20], "2")

    def test_mapped_water_at_sea_level_joins_the_sea(self) -> None:
        sea, _ = harbours.sea_geometry([COAST], BOUNDS, lambda lon, lat: 5.0)
        dock = _way(4, [(6.634, 43.2735), (6.636, 43.2735), (6.636, 43.2745),
                        (6.634, 43.2745), (6.634, 43.2735)], natural="water")
        low = _cells(sea, [dock], elevation=lambda lon, lat: 1.0)
        high = _cells(sea, [dock], elevation=lambda lon, lat: 40.0)
        self.assertEqual(low.at(6.635, 43.274), 2)
        self.assertEqual(high.at(6.635, 43.274), 1)   # a reservoir up the hill stays a lake

    def test_a_port_dock_is_sea_with_no_coastline(self) -> None:
        # Rotterdam: the coastline closes the river mouth, and the DEM reads the
        # docks at 5 m. What OSM says about the water decides.
        square = [(6.634, 43.2715), (6.636, 43.2715), (6.636, 43.2725), (6.634, 43.2725), (6.634, 43.2715)]
        dock = _way(5, square, natural="water", water="basin")
        port = _way(6, [(6.633, 43.271), (6.637, 43.271), (6.637, 43.273), (6.633, 43.273), (6.633, 43.271)],
                    landuse="port")
        lake = _cells(None, [dock], elevation=lambda lon, lat: 5.0)
        self.assertEqual(lake.at(6.635, 43.272), 1)
        docks = harbours.Cells(BOUNDS, 40, None, ground.Landcover([dock]), lambda lon, lat: 5.0,
                               harbours.tidal_water([dock, port], []))
        self.assertEqual(docks.at(6.635, 43.272), 2)
        tidal = _way(7, square, natural="water", water="river", tidal="yes")
        self.assertIsNotNone(harbours.tidal_water([tidal], []))
        self.assertIsNone(harbours.tidal_water([dock], []))


class BerthTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.anchor = Anchor.at((BOUNDS.west + BOUNDS.east) / 2, MID, 0.0)
        sea, _ = harbours.sea_geometry([COAST], BOUNDS, lambda lon, lat: 5.0)
        cls.cells = _cells(sea)
        # Six piers running south into the sea from the coast: a marina.
        cls.piers = [_way(100 + i, [(6.6325 + i * 0.0008, MID - 0.0002), (6.6325 + i * 0.0008, MID - 0.0018)],
                          man_made="pier") for i in range(6)]

    def _plan(self, maritime, landcover=(), profile=atlas.FRANCE, climate="mediterranean"):
        stats = harbours.HarbourStats()
        berths = harbours.plan_boats(maritime, [COAST], landcover, (), self.anchor, self.cells,
                                     profile, climate, GAME, stats)
        return berths, stats

    def test_boats_float_and_never_overlap(self) -> None:
        berths, _ = self._plan(self.piers)
        self.assertGreater(len(berths), 10)
        for b in berths:
            lon, lat, _ = self.anchor.engine_to_geodetic(b.x, 0.0, b.z)
            self.assertEqual(self.cells.at(lon, lat), 2, b.kind.name)
        for i, a in enumerate(berths):
            for b in berths[i + 1:]:
                gap = harbours._segment_distance(
                    (a.x - a.axis[0] * a.kind.length / 2, a.z - a.axis[1] * a.kind.length / 2),
                    (a.x + a.axis[0] * a.kind.length / 2, a.z + a.axis[1] * a.kind.length / 2),
                    (b.x - b.axis[0] * b.kind.length / 2, b.z - b.axis[1] * b.kind.length / 2),
                    (b.x + b.axis[0] * b.kind.length / 2, b.z + b.axis[1] * b.kind.length / 2))
                self.assertGreaterEqual(gap, (a.beam + b.beam) / 2)

    def test_a_pier_cluster_is_a_marina(self) -> None:
        berths, stats = self._plan(self.piers)
        kinds = {b.kind.kind for b in berths}
        self.assertTrue(kinds <= {"sail", "motor", "fishing"}, kinds)
        self.assertIn("sail", kinds)

    def test_hot_countries_row_and_the_low_countries_live_aboard(self) -> None:
        mix = harbours.mix_for("fishing", "tropical", atlas.SOUTHEAST_ASIA)
        self.assertGreater(mix["rowing"], harbours.mix_for("fishing", "temperate", atlas.FRANCE)["rowing"])
        self.assertIn("houseboat", harbours.mix_for("canal", "temperate", atlas.AMSTERDAM))
        self.assertNotIn("houseboat", harbours.mix_for("canal", "temperate", atlas.PARIS))

    def test_container_ships_need_a_long_quay(self) -> None:
        port = _way(300, [(6.631, MID + 0.0004), (6.639, MID + 0.0004), (6.639, MID + 0.002),
                          (6.631, MID + 0.002), (6.631, MID + 0.0004)], landuse="port")
        short = _way(301, [(6.6340, MID + 0.00005), (6.6348, MID + 0.00005)], man_made="quay")
        berths, _ = self._plan([short], [port])
        self.assertFalse(any(b.kind.kind == "cargo" for b in berths))

    def test_nothing_is_built_outside_the_tile(self) -> None:
        long_pier = _way(400, [(6.635, MID - 0.0002), (6.635, BOUNDS.south - 0.01)], man_made="pier")
        pieces = harbours._clipped(long_pier, BOUNDS)
        self.assertEqual(len(pieces), 1)
        self.assertGreaterEqual(min(lat for _, lat in pieces[0][0]), BOUNDS.south - 1e-12)


class NodeTests(unittest.TestCase):
    def test_every_hull_is_on_disk(self) -> None:
        for kind in harbours.BOATS.values():
            self.assertTrue((GAME / "assets/models/external/kenney_boats" / (kind.model + ".glb")).is_file(),
                            kind.model)

    def test_a_boat_node_faces_its_heading(self) -> None:
        anchor = Anchor.at(6.635, MID, 0.0)
        berth = harbours.Berth(harbours.BOATS["speed_a"], 10.0, -5.0, (1.0, 0.0), 3.0)
        nodes, manifest = harbours.boat_nodes([berth], anchor, GAME)
        self.assertAlmostEqual(manifest[0]["heading"], 90.0, places=6)   # +x is east
        _, qy, _, qw = nodes[0]["transform"]["rotation"]
        angle = 2.0 * math.atan2(qy, qw)          # rotation about Y
        # The node's forward (-Z) turned by `angle` must point east (+X).
        fx, fz = -math.sin(angle), -math.cos(angle)
        self.assertAlmostEqual(fx, 1.0, places=6)
        self.assertAlmostEqual(fz, 0.0, places=6)
        self.assertEqual(len(nodes[0]["children"]), 2)   # hull and helm: never flattened

    def test_a_traced_lighthouse_stands_on_its_footprint(self) -> None:
        # Cap Ferret: a building way, no node. Its centre and radius are kept.
        d = 0.00008
        ring = [(6.635 - d, MID - d), (6.635 + d, MID - d), (6.635 + d, MID + d), (6.635 - d, MID + d),
                (6.635 - d, MID - d)]
        way = _way(715849418, ring, building="yes", man_made="lighthouse", height="52")
        self.assertTrue(harbours.is_lighthouse(way.tags))
        node = harbours.traced_lighthouse(way)
        self.assertAlmostEqual(node.lon, 6.635, places=9)
        self.assertAlmostEqual(node.lat, MID, places=9)
        self.assertAlmostEqual(float(node.tags["r1:radius"]), 11.3, delta=1.0)
        self.assertEqual(node.tags["height"], "52")

    def test_a_container_ship_carries_its_boxes(self) -> None:
        anchor = Anchor.at(6.635, MID, 0.0)
        kind = harbours.BOATS["container_a"]
        berth = harbours.Berth(kind, 0.0, 0.0, (0.0, -1.0), kind.length / kind.length_ratio)
        nodes, _ = harbours.boat_nodes([berth], anchor, GAME)
        boxes = [c for c in nodes[0]["children"] if c["name"].startswith("Box")]
        self.assertGreater(len(boxes), 20)
        self.assertLessEqual(len(boxes), harbours.SHIP_CONTAINER_BUDGET)
        hull = nodes[0]["children"][0]["transform"]["position"][1]
        for box in boxes:
            x, y, z = box["transform"]["position"]
            self.assertLess(abs(x), berth.beam / 2)              # on the deck,
            self.assertLess(abs(z), kind.length / 2)             # not over the side,
            self.assertGreater(y, hull)                          # and above the keel

    def test_lighthouse_colours_come_from_the_tags(self) -> None:
        self.assertEqual(harbours._colours({"seamark:landmark:colour": "white;red"}), ["white", "red"])
        self.assertEqual(harbours._colours({}), ["white"])


if __name__ == "__main__":
    unittest.main()
