"""Contract tests for the lane graph a tile ships, and for the fleet's paint.

The simulation is the engine add-on and has its own tests
(`engine/plugins/traffic/tests`), which run without this project. What is held
here is the half only R1World can get wrong: that the graph describes roads
somebody actually surveyed, that it never sends a car somewhere the tile did
not draw a road, that a busy place gets more cars than an empty one, and that
the colours entering the game are albedos rather than paint chips.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from r1 import traffic
from r1.external_assets import GAME_ROOT

GAME = GAME_ROOT


class Way:
    """The shape of an OSM way, as `world_service` hands it over."""

    def __init__(self, points, **tags):
        self.points = points
        self.tags = tags


def flat(lon, lat, alt=0.0):
    # The tile's own geodetic-to-engine mapping, stubbed: metres of longitude
    # are not metres, and nothing under test cares. Engine z points south.
    return (lon, alt, -lat)


def graph(ways, buildings=0, lon=2.35, lat=48.85):
    return traffic.build_graph(ways, flat, buildings, lon, lat)


class WhatBecomesALane(unittest.TestCase):
    def test_a_street_becomes_two_lanes(self) -> None:
        g = graph([Way([(0, 0), (0, 100)], highway="residential")])
        self.assertEqual(len(g["nodes"]), 2)
        self.assertEqual(len(g["lanes"]), 2, "a two-way street is two lanes")
        self.assertEqual({tuple(lane[:2]) for lane in g["lanes"]}, {(0, 1), (1, 0)})

    def test_a_one_way_becomes_one(self) -> None:
        g = graph([Way([(0, 0), (0, 100)], highway="primary", oneway="yes")])
        self.assertEqual([tuple(lane[:2]) for lane in g["lanes"]], [(0, 1)])

    def test_a_reversed_one_way_runs_the_other_way(self) -> None:
        g = graph([Way([(0, 0), (0, 100)], highway="primary", oneway="-1")])
        self.assertEqual([tuple(lane[:2]) for lane in g["lanes"]], [(1, 0)])

    def test_a_roundabout_is_one_way_without_being_tagged_one(self) -> None:
        g = graph([Way([(0, 0), (0, 30), (20, 30)], highway="tertiary",
                       junction="roundabout")])
        self.assertTrue(all(len(g["lanes"]) == 2 for _ in [0]))
        self.assertEqual([tuple(lane[:2]) for lane in g["lanes"]], [(0, 1), (1, 2)])

    def test_what_is_not_a_road_carries_no_car(self) -> None:
        for tags in ({"highway": "footway"}, {"highway": "cycleway"},
                     {"highway": "steps"}, {"highway": "pedestrian"},
                     # Driveways and parking aisles: a city of them would put
                     # more cars in car parks than on streets.
                     {"highway": "service"},
                     {"highway": "residential", "area": "yes"}):
            self.assertEqual(graph([Way([(0, 0), (0, 100)], **tags)])["lanes"], [], tags)

    def test_no_car_drives_where_no_road_was_drawn(self) -> None:
        # The street mesh skips bridges and tunnels (street_surfaces.py), so a
        # lane on one would be a car crossing thin air.
        for tags in ({"bridge": "yes"}, {"tunnel": "yes"}):
            self.assertEqual(
                graph([Way([(0, 0), (0, 100)], highway="primary", **tags)])["lanes"], [], tags)

    def test_ways_that_meet_share_their_junction(self) -> None:
        # Two ways that end at the same point must be one graph, or every
        # crossroads is a pair of dead ends.
        g = graph([Way([(0, 0), (0, 50)], highway="residential", oneway="yes"),
                   Way([(0, 50), (60, 50)], highway="residential", oneway="yes")])
        self.assertEqual(len(g["nodes"]), 3)
        ends = {lane[1] for lane in g["lanes"] if lane[0] == 0}
        self.assertEqual(len(ends), 1)
        junction = ends.pop()
        self.assertTrue(any(lane[0] == junction for lane in g["lanes"]),
                        "the second way must leave the junction the first arrives at")

    def test_a_span_shorter_than_a_car_is_not_a_lane(self) -> None:
        g = graph([Way([(0, 0), (0, 0.5), (0, 80)], highway="residential")])
        self.assertEqual(len(g["nodes"]), 2)


class Speeds(unittest.TestCase):
    """§4 I5: a tagged limit beats the estimate, and the tile says which."""

    def test_a_tagged_limit_is_used_and_counted_as_measured(self) -> None:
        g = graph([Way([(0, 0), (0, 100)], highway="primary", maxspeed="30")])
        self.assertAlmostEqual(g["lanes"][0][2], round(30 / 3.6, 1), places=1)
        self.assertEqual((g["speedsTagged"], g["speedsInferred"]), (1, 0))

    def test_miles_per_hour_are_miles_per_hour(self) -> None:
        g = graph([Way([(0, 0), (0, 100)], highway="primary", maxspeed="30 mph")])
        self.assertAlmostEqual(g["lanes"][0][2], round(30 * 0.44704, 1), places=1)

    def test_an_untagged_road_falls_back_to_its_class_and_says_so(self) -> None:
        g = graph([Way([(0, 0), (0, 100)], highway="residential")])
        self.assertAlmostEqual(g["lanes"][0][2], traffic.CLASS_SPEED["residential"], places=1)
        self.assertEqual((g["speedsTagged"], g["speedsInferred"]), (0, 1))

    def test_nonsense_does_not_become_a_speed(self) -> None:
        for value in ("none", "walk", "signals", "", "1e9"):
            g = graph([Way([(0, 0), (0, 100)], highway="residential", maxspeed=value)])
            self.assertAlmostEqual(g["lanes"][0][2], traffic.CLASS_SPEED["residential"],
                                   places=1, msg=value)


class Weights(unittest.TestCase):
    """Where the traffic goes, which is what "pas assez de trafic" was about."""

    def test_an_avenue_outweighs_a_back_street(self) -> None:
        avenue = graph([Way([(0, 0), (0, 100)], highway="primary")])["lanes"][0][3]
        back = graph([Way([(0, 0), (0, 100)], highway="residential")])["lanes"][0][3]
        self.assertGreater(avenue, back * 3)

    def test_every_driveable_class_has_a_weight_and_a_speed(self) -> None:
        for name in traffic.DRIVEABLE:
            self.assertIn(name, traffic.CLASS_SPEED, name)
            self.assertIn(name, traffic.CLASS_WEIGHT, name)


class HowBusy(unittest.TestCase):
    """More buildings means more cars — the density signal the player sees."""

    def test_more_buildings_means_more_cars(self) -> None:
        street = [Way([(0, 0), (0, 400)], highway="residential")]
        quiet = graph(street, buildings=10)["cars"]
        busy = graph(street, buildings=600)["cars"]
        self.assertGreater(busy, quiet)

    def test_more_road_means_more_cars(self) -> None:
        short = graph([Way([(0, 0), (0, 200)], highway="primary")], buildings=50)["cars"]
        long = graph([Way([(0, 0), (0, 3000)], highway="primary")], buildings=50)["cars"]
        self.assertGreater(long, short)

    def test_a_place_with_no_road_has_no_cars(self) -> None:
        self.assertEqual(graph([], buildings=500)["cars"], 0)
        self.assertEqual(graph([Way([(0, 0), (0, 100)], highway="footway")])["cars"], 0)

    def test_the_cap_holds_against_any_density(self) -> None:
        huge = graph([Way([(0, 0), (0, 20000)], highway="primary")], buildings=100000)
        self.assertLessEqual(huge["cars"], traffic.TILE_CAR_CAP)


class WhichSide(unittest.TestCase):
    def test_the_left_hand_world_drives_on_the_left(self) -> None:
        for lon, lat in ((-0.12, 51.50), (139.76, 35.68), (151.20, -33.86),
                         (77.20, 28.61), (18.42, -33.92)):
            self.assertTrue(traffic.drives_on_the_left(lon, lat), (lon, lat))

    def test_the_rest_of_it_drives_on_the_right(self) -> None:
        for lon, lat in ((2.35, 48.85), (-73.98, 40.75), (13.40, 52.52),
                         (-58.38, -34.60), (116.40, 39.90)):
            self.assertFalse(traffic.drives_on_the_left(lon, lat), (lon, lat))

    def test_the_tile_carries_the_answer(self) -> None:
        self.assertTrue(graph([], lon=-0.12, lat=51.50)["leftHand"])
        self.assertFalse(graph([], lon=2.35, lat=48.85)["leftHand"])


class Paint(unittest.TestCase):
    """Rule 2 of CLAUDE.md, applied to colours that never came from a kit."""

    def test_the_shipped_table_is_the_module_s(self) -> None:
        shipped = json.loads((GAME / "assets" / "world" / "traffic_paints.json")
                             .read_text(encoding="utf-8"))
        self.assertEqual([p["name"] for p in shipped["paints"]],
                         [name for name, _ in traffic.PAINTS])
        for entry, (_name, colour) in zip(shipped["paints"], traffic.PAINTS):
            self.assertEqual(tuple(entry["albedo"]), colour)

    def test_no_car_is_bright_enough_to_saturate(self) -> None:
        # The lesson the ground table and the prop palette both learned: above
        # roughly 0.35, a sunlit surface goes white at this world's light level.
        for name, colour in traffic.PAINTS:
            self.assertLessEqual(max(colour), 0.35, f"{name} would saturate in sunlight")
            self.assertGreaterEqual(min(colour), 0.04, f"{name} is darker than any paint")

    def test_the_fleet_is_not_one_colour(self) -> None:
        self.assertGreaterEqual(len(traffic.PAINTS), 6)
        self.assertEqual(len({colour for _name, colour in traffic.PAINTS}),
                         len(traffic.PAINTS))


if __name__ == "__main__":
    unittest.main()
