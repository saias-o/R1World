"""The Rio photo visibly contains a low Sun at the right of its skyline.

An EXIF clock without a timezone is not a UTC observation. This astronomical
constraint prevents a reference capture from silently returning to night.
"""
from datetime import datetime, timezone
import unittest

import gallery
from r1.solar import sun_angles


class PhotographLightingTests(unittest.TestCase):
    def test_rio_sun_matches_the_photographed_skyline(self):
        view = next(view for view in gallery.VIEWS if view["name"] == "rio")
        when = datetime.fromtimestamp(gallery.moment(view), timezone.utc)
        azimuth, elevation, *_ = sun_angles(*view["spawn"], when)
        self.assertGreater(elevation, 2.0)
        self.assertLess(elevation, 7.0)
        self.assertGreater(azimuth, 290.0)
        self.assertLess(azimuth, 300.0)
        self.assertIn("inférée", view["timeNote"])


if __name__ == "__main__":
    unittest.main()
