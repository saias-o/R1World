import math
import unittest

from r1.geodesy import (
    Anchor,
    R_MEAN,
    ecef_to_geodetic,
    float32_resolution,
    geodetic_to_ecef,
    haversine,
    sag,
)


REFERENCE_POINTS = (
    (2.3522, 48.8566, 35.0),
    (-74.0060, 40.7128, 10.0),
    (139.6917, 35.6895, 44.0),
    (0.0, 0.0, 0.0),
    (0.0, 90.0, 0.0),
    (0.0, -90.0, 0.0),
    (151.2093, -33.8688, 58.0),
    (-58.3816, -34.6037, 25.0),
)


class GeodesyTests(unittest.TestCase):
    def test_geodetic_ecef_round_trip_is_sub_micrometre(self):
        for expected in REFERENCE_POINTS:
            actual = ecef_to_geodetic(*geodetic_to_ecef(*expected))
            horizontal_error = haversine(
                expected[0], expected[1], actual[0], actual[1]
            )
            altitude_error = abs(expected[2] - actual[2])
            self.assertLess(horizontal_error, 1e-6, expected)
            self.assertLess(altitude_error, 1e-6, expected)

    def test_anchor_round_trip_preserves_near_and_horizon_points(self):
        anchor = Anchor.at(6.869433, 45.923697, 1035.0)
        for point in (
            (6.869433, 45.923697, 1035.0),
            (6.8840, 45.9280, 1100.0),
            (9.0, 46.4, 4807.0),
        ):
            engine = anchor.geodetic_to_engine(*point)
            actual = anchor.engine_to_geodetic(*engine)
            self.assertLess(haversine(point[0], point[1], actual[0], actual[1]), 1e-6)
            self.assertLess(abs(point[2] - actual[2]), 1e-6)

    def test_engine_axes_are_east_up_and_south(self):
        anchor = Anchor.at(6.869433, 45.923697, 1035.0)
        east = anchor.geodetic_to_engine(6.870433, 45.923697, 1035.0)
        north = anchor.geodetic_to_engine(6.869433, 45.924697, 1035.0)
        self.assertGreater(east[0], 0.0)
        self.assertLess(abs(east[2]), 1.0)
        self.assertLess(north[2], 0.0)
        self.assertLess(abs(north[0]), 1.0)

    def test_reference_precision_and_horizon_numbers(self):
        self.assertLess(float32_resolution(4_000.0), 0.0005)
        self.assertAlmostEqual(sag(200_000.0), 3140.0, delta=2.0)
        expected_horizon = math.sqrt(4_000.0 * (2.0 * R_MEAN + 4_000.0))
        self.assertAlmostEqual(expected_horizon / 1000.0, 225.8, delta=0.2)


if __name__ == "__main__":
    unittest.main()

