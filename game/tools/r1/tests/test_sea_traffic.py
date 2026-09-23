"""Ships at sea: the shipped prior, the day, what the sync learns, and offline.

These hold `r1/sea_traffic.py` to its promises: the prior reads the real
lanes back (Dover busy, the Sahara empty); a prediction is deterministic for
its half-hour, so the game can match ships by id; a gale empties the marinas
but not the lanes; ships seen fewer than predicted make the next prediction
smaller; a ship seen live comes back where it was going; and nothing slips
past an offline proxy.
"""

from __future__ import annotations

import math
import os
import tempfile
import time
import unittest
from datetime import datetime, timezone
from pathlib import Path
from unittest import mock

from r1 import sea_traffic as sea
from r1 import shipping_prior

SUMMER_DAY = datetime(2026, 6, 20, 13, 0, tzinfo=timezone.utc)   # a Saturday
WINTER_NIGHT = datetime(2026, 1, 14, 2, 0, tzinfo=timezone.utc)


class PriorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.prior = sea.Prior()

    def test_the_lanes_are_where_the_ais_put_them(self) -> None:
        dover, _ = self.prior.at(*self.prior.cell_of(1.45, 51.0))
        sahara, axis = self.prior.at(*self.prior.cell_of(5.0, 25.0))
        self.assertAlmostEqual(dover[0], 8.0, delta=1.0)          # the calibration point
        self.assertEqual(sahara, (0.0,) * 4)
        self.assertIsNone(axis)

    def test_the_channel_runs_east_west(self) -> None:
        _, axis = self.prior.at(*self.prior.cell_of(-3.5, 49.8))
        self.assertIsNotNone(axis)
        self.assertLess(abs(axis - 70.0), 25.0)

    def test_only_a_few_blocks_are_ever_held(self) -> None:
        for lon in range(-170, 180, 20):
            self.prior.at(*self.prior.cell_of(lon, 0.0))
        self.assertLessEqual(len(self.prior._blocks), 4)

    def test_a_count_survives_its_byte(self) -> None:
        import numpy as np
        ships = np.array([1e-4, 0.013, 0.5, 8.0, 120.0])
        back = [shipping_prior.dequantize(q) for q in shipping_prior.quantize(ships)]
        for a, b in zip(ships, back):
            self.assertLess(abs(b / a - 1.0), 0.03)


class DayTests(unittest.TestCase):
    def test_leisure_follows_the_summer_and_the_day(self) -> None:
        self.assertGreater(sea.day_factor("leisure", 5.3, 43.3, SUMMER_DAY),
                           8 * sea.day_factor("leisure", 5.3, 43.3, WINTER_NIGHT))
        self.assertEqual(sea.day_factor("commercial", 5.3, 43.3, SUMMER_DAY),
                         sea.day_factor("commercial", 5.3, 43.3, WINTER_NIGHT))

    def test_the_south_has_its_summer_in_january(self) -> None:
        jan = datetime(2026, 1, 14, 3, 0, tzinfo=timezone.utc)      # 13 h in Sydney
        self.assertGreater(sea.day_factor("leisure", 151.2, -33.9, jan),
                           sea.day_factor("leisure", 151.2, -33.9, datetime(2026, 7, 14, 3, 0, tzinfo=timezone.utc)))

    def test_a_gale_empties_the_marinas_not_the_lanes(self) -> None:
        self.assertLess(sea.weather_factor("leisure", 4.0, 18.0), 0.05)
        self.assertGreater(sea.weather_factor("commercial", 4.0, 18.0), 0.95)
        self.assertEqual(sea.weather_factor("fishing", None, None), 1.0)


class PlanTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.prior = sea.Prior()
        cls.tmp = tempfile.TemporaryDirectory()
        cls.learned = sea.Learned(Path(cls.tmp.name) / "learned.sqlite")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.learned.db.close()
        cls.tmp.cleanup()

    def test_a_prediction_is_the_same_twice(self) -> None:
        now = 1_790_000_000.0
        a, _ = sea.plan_ships(self.prior, None, 1.45, 51.0, SUMMER_DAY.timestamp(), now)
        b, _ = sea.plan_ships(self.prior, None, 1.45, 51.0, SUMMER_DAY.timestamp(), now)
        self.assertEqual(a, b)
        self.assertGreater(len(a), 5)
        self.assertLessEqual(len(a), sea.MAX_SHIPS)
        for ship in a:
            self.assertLessEqual(ship["distance"], sea.RADIUS)
            self.assertIn(ship["model"], sea.harbours.BOATS)

    def test_a_ship_moves_on_within_its_slot(self) -> None:
        start = math.floor(1_790_000_000.0 / sea.SLOT) * sea.SLOT
        a, _ = sea.plan_ships(self.prior, None, 1.45, 51.0, SUMMER_DAY.timestamp(), start + 10.0)
        b, _ = sea.plan_ships(self.prior, None, 1.45, 51.0, SUMMER_DAY.timestamp(), start + 70.0)
        before = {s["id"]: s for s in a}
        moved = [s for s in b if s["id"] in before and s["speed"] > 0]
        self.assertTrue(moved)
        for s in moved:
            e, n = sea._metres(before[s["id"]]["lon"], before[s["id"]]["lat"], s["lon"], s["lat"])
            self.assertAlmostEqual(math.hypot(e, n), s["speed"] * 60.0, delta=1.0)

    def test_no_ships_in_the_desert(self) -> None:
        ships, _ = sea.plan_ships(self.prior, None, 5.0, 25.0, SUMMER_DAY.timestamp(), time.time())
        self.assertEqual(ships, [])

    def test_seeing_fewer_ships_predicts_fewer(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        learned = sea.Learned(Path(tmp.name) / "l.sqlite")
        try:
            now, game = 1_790_000_000.0, SUMMER_DAY.timestamp()
            before = len(sea.plan_ships(self.prior, learned, 1.45, 51.0, game, now)[0])
            for _ in range(20):             # twenty windows that heard nothing
                sea.learn(learned, self.prior, {}, 1.45, 51.0, 0.3, SUMMER_DAY)
            after = len(sea.plan_ships(self.prior, learned, 1.45, 51.0, game, now)[0])
            self.assertLess(after, before / 2)
        finally:
            learned.db.close()
            tmp.cleanup()

    def test_a_ship_seen_live_is_where_it_was_going(self) -> None:
        # Seen ten minutes ago, in real time; the game's clock is now too.
        game = time.time()
        seen = {227000001: {"lon": 1.40, "lat": 51.00, "knots": 12.0, "cog": 90.0, "t": game - 600.0,
                            "classB": False, "kind": 0, "length": 180.0}}
        sea.learn(self.learned, self.prior, seen, 1.45, 51.0, 0.3, datetime.fromtimestamp(game, timezone.utc))
        ships, source = sea.plan_ships(self.prior, self.learned, 1.45, 51.0, game, game)
        live = [s for s in ships if s["id"] == "ais-227000001"]
        self.assertEqual(len(live), 1)
        self.assertEqual(source["live"], 1)
        east, north = sea._metres(1.40, 51.00, live[0]["lon"], live[0]["lat"])
        self.assertAlmostEqual(east, 12.0 * sea.KNOT * 600.0, delta=20.0)
        self.assertAlmostEqual(north, 0.0, delta=20.0)

    def test_weather_falls_back_to_the_month(self) -> None:
        sq = sea.square(1.45, 51.0)
        self.learned.db.execute("INSERT OR REPLACE INTO weather VALUES(?, ?, ?, ?)",
                                (sq, sea.day_number(SUMMER_DAY) - 400, 2.0, 8.0))  # a May, a year before
        self.learned.db.execute("INSERT OR REPLACE INTO weather VALUES(?, ?, ?, ?)",
                                (sq, sea.day_number(SUMMER_DAY) - 3, 1.2, 6.0))
        self.assertEqual(self.learned.weather(1.45, 51.0, sea.day_number(SUMMER_DAY) - 3), (1.2, 6.0, "jour"))
        hs, _, how = self.learned.weather(1.45, 51.0, sea.day_number(SUMMER_DAY) + 5)
        self.assertEqual((hs, how), (1.2, "climatologie"))
        self.assertEqual(self.learned.weather(-150.0, 0.0, 1)[2], "neutre")


class KindTests(unittest.TestCase):
    def test_ais_types_become_kinds(self) -> None:
        self.assertEqual(sea.KINDS[sea.ais_kind(30, False, None)], "fishing")
        self.assertEqual(sea.KINDS[sea.ais_kind(37, True, None)], "leisure")
        self.assertEqual(sea.KINDS[sea.ais_kind(61, False, None)], "passenger")
        self.assertEqual(sea.KINDS[sea.ais_kind(71, False, None)], "commercial")
        self.assertEqual(sea.KINDS[sea.ais_kind(None, True, None)], "leisure")
        self.assertEqual(sea.KINDS[sea.ais_kind(None, False, 200.0)], "commercial")

    def test_every_hull_has_a_document(self) -> None:
        names = {n for hulls in sea.HULLS.values() for n, _ in hulls}
        docs, specs = sea.hull_docs(sorted(names))
        self.assertEqual(set(docs), names)
        boxes = [c for c in docs["container_a"]["children"] if c["name"].startswith("Box")]
        self.assertTrue(boxes)
        self.assertGreater(specs["container_a"]["length"], 100.0)


class OfflineTests(unittest.TestCase):
    def test_the_live_feed_cannot_slip_past_an_offline_proxy(self) -> None:
        with mock.patch.dict(os.environ, {"HTTPS_PROXY": "http://127.0.0.1:9"}):
            with self.assertRaises(OSError):
                sea._proxied_socket("stream.aisstream.io", 443, 2.0)

    def test_offline_weather_fails_fast_and_stores_nothing(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        learned = sea.Learned(Path(tmp.name) / "l.sqlite")
        try:
            with mock.patch.dict(os.environ, {"HTTPS_PROXY": "http://127.0.0.1:9", "HTTP_PROXY": "http://127.0.0.1:9"}):
                started = time.monotonic()
                with self.assertRaises(OSError):
                    sea.fetch_weather(learned, 1.45, 51.0, sea.day_number(SUMMER_DAY))
                self.assertLess(time.monotonic() - started, 10.0)
            self.assertEqual(learned.db.execute("SELECT COUNT(*) FROM weather").fetchone()[0], 0)
        finally:
            learned.db.close()
            tmp.cleanup()


if __name__ == "__main__":
    unittest.main()
