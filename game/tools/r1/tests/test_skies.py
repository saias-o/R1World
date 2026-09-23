"""The sky series: where each photograph belongs, and what the runtime reads.

`test_sun_cycle_parity.py` holds `sun_cycle.js` to `r1/skies.py` term by term.
This file holds `r1/skies.py` to what it claims: that each sky is placed where
the Sun stood in it, that the runtime carries the same table the rebuild wrote,
that a sky is turned to face the real Sun, and that the Sun was taken out.
"""

from __future__ import annotations

import json
import math
import re
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

from r1 import skies, solar


class SkyTableTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.table = skies.load_table()
        cls.frames = cls.table["frames"]

    def test_every_source_has_a_frame_and_a_texture(self) -> None:
        self.assertEqual([f["name"] for f in self.frames], [s.name for s in skies.SOURCES])
        for frame in self.frames:
            self.assertTrue((skies.GAME_ROOT / frame["texture"]).is_file(), frame["texture"])

    def test_each_sky_sits_where_its_sun_stood(self) -> None:
        # Measured, never chosen (CLAUDE.md §4): the elevation a sky is shown
        # at is the Sun's, at the instant and place it was photographed.
        for source, frame in zip(skies.SOURCES, self.frames):
            elevation, _ = skies.capture_sun(source)
            self.assertAlmostEqual(frame["elevation"], elevation, places=3, msg=source.name)

    def test_the_timestamps_are_local_time(self) -> None:
        # Poly Haven publishes the camera's clock. Read as SAST, "dawn" is just
        # before sunrise and "night" is after astronomical dusk; read as UTC,
        # both would be taken with the Sun well up.
        by_name = {f["name"]: f for f in self.frames}
        self.assertTrue(-6.0 < by_name["dawn"]["elevation"] < solar.SUNSET_ELEVATION)
        self.assertLess(by_name["night"]["elevation"], solar.NAUTICAL_TWILIGHT)
        self.assertGreater(by_name["noon"]["elevation"], 45.0)

    def test_the_runtime_carries_the_same_table(self) -> None:
        script = skies.SCRIPT_PATH.read_text(encoding="utf-8")
        match = re.search(re.escape(skies.SCRIPT_BEGIN) + r"\s*const SKIES = (.*?);\s*"
                          + re.escape(skies.SCRIPT_END), script, re.S)
        self.assertIsNotNone(match, "sun_cycle.js has lost its SKIES block")
        self.assertEqual(json.loads(match.group(1)),
                         json.loads(skies.script_table(self.table)[len("const SKIES = "):-1]))


class SelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.frames = skies.load_table()["frames"]

    def _index(self, name: str) -> int:
        return next(i for i, f in enumerate(self.frames) if f["name"] == name)

    def test_at_a_photograph_that_photograph_is_shown_alone(self) -> None:
        for i, frame in enumerate(self.frames):
            azimuth = 90.0 if frame["branch"] == "rising" else 270.0
            a, b, blend = skies.select_skies(self.frames, frame["elevation"], azimuth)
            shown = a if blend == 0.0 else b if blend == 1.0 else None
            self.assertEqual(shown, i, frame["name"])

    def test_morning_and_evening_use_their_own_photographs(self) -> None:
        dawn, dusk = self._index("dawn"), self._index("dusk_1")
        rising = skies.select_skies(self.frames, -1.2, 80.0)
        setting = skies.select_skies(self.frames, -1.2, 280.0)
        self.assertIn(dawn, rising[:2])
        self.assertIn(dusk, setting[:2])
        self.assertNotIn(dusk, rising[:2])
        self.assertNotIn(dawn, setting[:2])

    def test_beyond_the_series_the_ends_hold(self) -> None:
        night, noon = self._index("night"), self._index("noon")
        self.assertEqual(skies.select_skies(self.frames, -40.0, 10.0), (night, night, 0.0))
        self.assertEqual(skies.select_skies(self.frames, 85.0, 170.0), (noon, noon, 0.0))

    def test_the_fade_is_continuous_in_elevation(self) -> None:
        # A smoothstep's steepest slope is 1.5 per span, so between two samples
        # the weights may move by 2 × 1.5 × step / span and no more: anything
        # larger is a jump, not a fade.
        step = 0.06
        for azimuth in (90.0, 270.0):
            order = skies.branch_frames(self.frames, azimuth < 180.0)
            span = min(self.frames[b]["elevation"] - self.frames[a]["elevation"]
                       for a, b in zip(order, order[1:]))
            bound = 3.0 * step / span * 1.01
            previous = None
            for sample in range(0, 1200):
                elevation = -20.0 + sample * step
                a, b, blend = skies.select_skies(self.frames, elevation, azimuth)
                # The drawn sky as a weight per photograph.
                weights = np.zeros(len(self.frames))
                weights[a] += 1.0 - blend
                weights[b] += blend
                if previous is not None:
                    self.assertLess(np.abs(weights - previous).sum(), bound,
                                    f"sky jumps at {elevation:.2f}° (azimuth {azimuth})")
                previous = weights


class RotationTests(unittest.TestCase):
    def test_the_photographed_sun_faces_the_real_one(self) -> None:
        # The shader looks a direction up at atan2(z, x) after turning it by
        # -rotation; the photographed Sun's column must be where the real Sun is.
        frame = {"sunU": 0.6}
        for azimuth in (0.0, 45.0, 90.0, 181.0, 275.9, 359.0):
            rotation = skies.sky_rotation(frame, azimuth)
            a = math.radians(azimuth)
            x, z = math.sin(a), -math.cos(a)
            phi = math.atan2(z, x) - rotation
            u = (phi / (2.0 * math.pi) + 0.5) % 1.0
            self.assertAlmostEqual(u, frame["sunU"], places=9, msg=str(azimuth))


class StateTests(unittest.TestCase):
    def test_the_sky_meets_the_fog_at_every_hour(self) -> None:
        table = skies.load_table()
        band = {f["texture"]: f["horizon"] for f in table["frames"]}
        for hour in range(24):
            sun = solar.sun_state(2.35, 48.86, datetime(2026, 6, 21, hour, 10, tzinfo=timezone.utc))
            sky = skies.sky_state(sun, table)
            shown = sky.exposure * (band[sky.texture] * (1.0 - sky.blend)
                                    + band[sky.blend_texture] * sky.blend)
            fog = sun.horizon_color
            self.assertAlmostEqual(shown, 0.2126 * fog[0] + 0.7152 * fog[1] + 0.0722 * fog[2],
                                   places=9, msg=f"{hour}:10 UTC")

    def test_night_is_dimmer_than_day_and_the_disc_sets(self) -> None:
        noon = solar.sun_state(2.35, 48.86, datetime(2026, 6, 21, 12, 0, tzinfo=timezone.utc))
        night = solar.sun_state(2.35, 48.86, datetime(2026, 6, 21, 23, 30, tzinfo=timezone.utc))
        day_sky = skies.sky_state(noon)
        night_sky = skies.sky_state(night)
        table = skies.load_table()
        luminance = {f["texture"]: f["luminance"] for f in table["frames"]}
        shown_day = day_sky.exposure * luminance[day_sky.texture]
        shown_night = night_sky.exposure * luminance[night_sky.texture]
        self.assertLess(shown_night, 0.25 * shown_day)
        self.assertGreater(max(day_sky.sun_color), 0.0)
        self.assertEqual(max(night_sky.sun_color), 0.0)
        self.assertEqual(night_sky.texture, "assets/skies/qwantani_night.hdr")

    def test_the_disc_points_at_the_sun(self) -> None:
        state = solar.sun_state(2.35, 48.86, datetime(2026, 6, 21, 9, 0, tzinfo=timezone.utc))
        sky = skies.sky_state(state)
        self.assertAlmostEqual(sum(c * c for c in sky.sun_direction), 1.0, places=9)
        self.assertGreater(sky.sun_direction[1], 0.0)          # up
        self.assertGreater(sky.sun_direction[0], 0.0)          # east in the morning


class ImageTests(unittest.TestCase):
    def test_hdr_round_trip(self) -> None:
        rng = np.random.default_rng(3)
        image = rng.gamma(1.0, 1.0, (16, 32, 3)) * np.logspace(-3, 4, 32)[None, :, None]
        image[0, :5] = 0.0
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sky.hdr"
            skies.write_hdr(path, image)
            back = skies.read_hdr(path)
        # RGBE keeps eight bits of mantissa against the brightest channel.
        np.testing.assert_allclose(back, image, rtol=0.0, atol=0.0 + 1e-2 * image.max(axis=2, keepdims=True).max())
        self.assertTrue(np.all(back[0, :5] == 0.0))

    def test_the_sun_was_taken_out(self) -> None:
        noon = next(s for s in skies.SOURCES if s.name == "noon")
        cleaned = skies.read_hdr(skies.GAME_ROOT / noon.texture)
        lum = skies.luminance(cleaned)
        upper = lum[: lum.shape[0] // 2]
        # The photograph's Sun was 600 000 times its sky; nothing close is left.
        self.assertLess(upper.max(), 20.0 * np.median(upper))


if __name__ == "__main__":
    unittest.main()
