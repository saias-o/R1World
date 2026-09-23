"""Contract tests for the solar ephemerides and the atmosphere model.

The ephemeris half is checked against quantities that are true by astronomy and
independent of this implementation — noon elevation from latitude and
declination, the equinox day length, the midnight sun, the equation of time at
its known extremes — rather than against numbers this module produced. A bug
that shifted the Sun would have to also shift the sky for these to pass.

The atmosphere half is checked for the properties the renderer depends on:
monotone extinction, reddening at low Sun, and thinner air with altitude.
"""

import math
import unittest
from datetime import datetime, timedelta, timezone

from r1.solar import (
    NAUTICAL_TWILIGHT,
    SUNSET_ELEVATION,
    air_mass,
    day_events,
    hour_angle_at_elevation,
    julian_day,
    refraction,
    solar_noon,
    sun_angles,
    sun_direction,
    sun_state,
    transmittance,
)


CHAMONIX = (6.869433, 45.923697)
TROMSO = (18.9553, 69.6492)
QUITO = (-78.4678, -0.1807)
UTC = timezone.utc


class JulianDayTests(unittest.TestCase):
    def test_known_epochs(self):
        # J2000.0 is 2000-01-01 12:00 UTC by definition, and the Unix epoch is
        # a second published constant. Both fix the offset and the fraction.
        self.assertAlmostEqual(julian_day(datetime(2000, 1, 1, 12, tzinfo=UTC)), 2451545.0, places=9)
        self.assertAlmostEqual(julian_day(datetime(1970, 1, 1, 0, tzinfo=UTC)), 2440587.5, places=9)

    def test_naive_datetimes_are_read_as_utc(self):
        self.assertEqual(
            julian_day(datetime(2026, 6, 21, 10, 30)),
            julian_day(datetime(2026, 6, 21, 10, 30, tzinfo=UTC)),
        )

    def test_a_day_advances_the_julian_day_by_one(self):
        base = datetime(2026, 3, 20, 4, 17, 31, tzinfo=UTC)
        self.assertAlmostEqual(julian_day(base + timedelta(days=1)) - julian_day(base), 1.0, places=9)


class EphemerisTests(unittest.TestCase):
    def test_noon_elevation_matches_latitude_and_declination(self):
        # The one identity that cannot be fudged: at solar noon the Sun stands
        # at 90 - |lat - declination|. Checked on both solstices and an equinox
        # at three latitudes, north and south.
        for lon, lat in (CHAMONIX, TROMSO, QUITO):
            for date in (datetime(2026, 6, 21, tzinfo=UTC),
                         datetime(2026, 12, 21, tzinfo=UTC),
                         datetime(2026, 3, 20, tzinfo=UTC)):
                noon = solar_noon(lon, date)
                _, _, elevation, declination, _ = sun_angles(lon, lat, noon)
                expected = 90.0 - abs(lat - declination)
                self.assertAlmostEqual(elevation, expected, delta=0.02, msg=(lat, date))

    def test_noon_is_the_maximum_of_the_day(self):
        lon, lat = CHAMONIX
        noon = solar_noon(lon, datetime(2026, 9, 3, tzinfo=UTC))
        _, _, peak, _, _ = sun_angles(lon, lat, noon)
        for minutes in range(-720, 721, 10):
            _, _, elevation, _, _ = sun_angles(lon, lat, noon + timedelta(minutes=minutes))
            self.assertLessEqual(elevation, peak + 1e-6)

    def test_solstice_declination_matches_the_obliquity(self):
        for date, expected in ((datetime(2026, 6, 21, tzinfo=UTC), 23.44),
                               (datetime(2026, 12, 21, tzinfo=UTC), -23.44)):
            _, _, _, declination, _ = sun_angles(0.0, 0.0, solar_noon(0.0, date))
            self.assertAlmostEqual(declination, expected, delta=0.05)

    def test_equation_of_time_reaches_its_published_extremes(self):
        # It runs from about -14.2 min in mid-February to +16.4 min in early
        # November, crossing zero four times. Sampling the year pins both the
        # amplitude and the dates.
        samples = []
        for day in range(365):
            when = datetime(2026, 1, 1, 12, tzinfo=UTC) + timedelta(days=day)
            _, _, _, _, eqtime = sun_angles(0.0, 0.0, when)
            samples.append((when, eqtime))
        minimum = min(samples, key=lambda s: s[1])
        maximum = max(samples, key=lambda s: s[1])
        self.assertAlmostEqual(minimum[1], -14.2, delta=0.3)
        self.assertAlmostEqual(maximum[1], 16.4, delta=0.3)
        self.assertEqual(minimum[0].month, 2)
        self.assertEqual(maximum[0].month, 11)

    def test_azimuth_is_due_south_at_noon_in_the_north(self):
        lon, lat = CHAMONIX
        azimuth, _, _, _, _ = sun_angles(lon, lat, solar_noon(lon, datetime(2026, 9, 3, tzinfo=UTC)))
        self.assertAlmostEqual(azimuth, 180.0, delta=0.05)

    def test_azimuth_is_due_north_at_noon_in_the_deep_south(self):
        lon, lat = 151.2093, -33.8688                  # Sydney
        azimuth, _, _, _, _ = sun_angles(lon, lat, solar_noon(lon, datetime(2026, 6, 21, tzinfo=UTC)))
        self.assertLess(min(azimuth, 360.0 - azimuth), 0.05)

    def test_sun_rises_in_the_east_and_sets_in_the_west_at_the_equinox(self):
        lon, lat = CHAMONIX
        events = day_events(lon, lat, datetime(2026, 3, 20, tzinfo=UTC))
        sunrise_azimuth, _, _, _, _ = sun_angles(lon, lat, events["sunrise"])
        sunset_azimuth, _, _, _, _ = sun_angles(lon, lat, events["sunset"])
        self.assertAlmostEqual(sunrise_azimuth, 90.0, delta=1.5)
        self.assertAlmostEqual(sunset_azimuth, 270.0, delta=1.5)


class DayLengthTests(unittest.TestCase):
    def test_equinox_day_is_a_little_over_twelve_hours_everywhere(self):
        # Refraction and the solar radius make the equinox day longer than 12 h
        # at every latitude, and increasingly so towards the poles. That excess
        # is the signature that the -0.833 threshold is being applied.
        for lon, lat in (QUITO, CHAMONIX, (0.0, 60.0)):
            events = day_events(lon, lat, datetime(2026, 3, 20, tzinfo=UTC))
            self.assertGreater(events["dayLengthHours"], 12.0)
            self.assertLess(events["dayLengthHours"], 12.6)

    def test_chamonix_solstices_bracket_the_year(self):
        lon, lat = CHAMONIX
        longest = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        shortest = day_events(lon, lat, datetime(2026, 12, 21, tzinfo=UTC))
        self.assertAlmostEqual(longest["dayLengthHours"], 15.6, delta=0.2)
        self.assertAlmostEqual(shortest["dayLengthHours"], 8.7, delta=0.2)

    def test_tromso_has_a_midnight_sun_and_a_polar_night(self):
        lon, lat = TROMSO
        june = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        self.assertTrue(june["polarDay"])
        self.assertIsNone(june["sunrise"])
        self.assertEqual(june["dayLengthHours"], 24.0)

        december = day_events(lon, lat, datetime(2026, 12, 21, tzinfo=UTC))
        self.assertTrue(december["polarNight"])
        self.assertEqual(december["dayLengthHours"], 0.0)

        # The midnight sun is the visible claim: the Sun stays up all night.
        midnight = datetime(2026, 6, 21, 22, tzinfo=UTC)                 # ~00:00 local
        _, elevation, _, _, _ = sun_angles(lon, lat, midnight)
        self.assertGreater(elevation, SUNSET_ELEVATION)

    def test_tromso_june_noon_is_low_and_the_day_is_long(self):
        # §2.2's example: a Tromso June noon reaches only about 43.5 degrees,
        # which is why the light stays raking all day. Nothing else on Earth
        # combines that elevation with 24 h of daylight.
        lon, lat = TROMSO
        noon = solar_noon(lon, datetime(2026, 6, 21, tzinfo=UTC))
        _, _, elevation, _, _ = sun_angles(lon, lat, noon)
        self.assertAlmostEqual(elevation, 43.8, delta=0.3)

    def test_polar_hour_angle_is_undefined_rather_than_clamped(self):
        self.assertIsNone(hour_angle_at_elevation(80.0, 23.44, SUNSET_ELEVATION))


class RefractionTests(unittest.TestCase):
    def test_horizon_lift_is_about_half_a_degree(self):
        self.assertAlmostEqual(refraction(0.0), 0.4817, delta=0.01)

    def test_refraction_vanishes_overhead_and_never_goes_negative_above_it(self):
        self.assertEqual(refraction(89.0), 0.0)
        for elevation in range(0, 90):
            self.assertGreaterEqual(refraction(float(elevation)), 0.0)

    def test_apparent_elevation_is_above_the_true_one(self):
        lon, lat = CHAMONIX
        _, apparent, true_elevation, _, _ = sun_angles(lon, lat, datetime(2026, 9, 3, 5, 30, tzinfo=UTC))
        self.assertGreater(apparent, true_elevation)


class AtmosphereTests(unittest.TestCase):
    def test_air_mass_reference_values(self):
        self.assertAlmostEqual(air_mass(90.0), 1.0, delta=0.001)
        self.assertAlmostEqual(air_mass(30.0), 2.0, delta=0.01)
        self.assertGreater(air_mass(0.0), 30.0)

    def test_altitude_thins_the_air(self):
        # Chamonix keeps ~88% of the sea-level column, the Aiguille du Midi
        # ~63%. Alpine light is harder because of this, not because of a preset.
        self.assertAlmostEqual(air_mass(45.0, 1035.0) / air_mass(45.0, 0.0), 0.885, delta=0.01)
        self.assertAlmostEqual(air_mass(45.0, 3842.0) / air_mass(45.0, 0.0), 0.635, delta=0.01)

    def test_light_is_white_overhead_and_red_at_the_horizon(self):
        # "White" overhead is still slightly warm: even one air mass costs the
        # blue channel about 12 points more than the red, which is why noon
        # sunlight is 5500 K and not the 5800 K leaving the Sun.
        high = transmittance(80.0, 0.0, 2.5)
        self.assertLess(high[0] / high[2], 1.2)
        low = transmittance(1.0, 0.0, 2.5)
        self.assertGreater(low[0], low[1])
        self.assertGreater(low[1], low[2])
        self.assertGreater(low[0] / max(low[2], 1e-12), 20.0)

    def test_turbidity_greys_the_light_instead_of_reddening_it(self):
        clean = transmittance(20.0, 0.0, 2.0)
        hazy = transmittance(20.0, 0.0, 6.0)
        self.assertLess(sum(hazy), sum(clean))
        clean_ratio = clean[0] / clean[2]
        hazy_ratio = hazy[0] / hazy[2]
        self.assertGreater(hazy_ratio, clean_ratio)     # haze still reddens...
        self.assertLess(hazy_ratio / clean_ratio, 1.6)  # ...but far less than air mass does


class SunStateTests(unittest.TestCase):
    def test_direction_points_down_and_away_from_the_sun(self):
        # Sun due east at 30 degrees: light travels west and downward. Engine
        # axes are x=east, y=up, z=-north, so west is -x and down is -y.
        direction = sun_direction(90.0, 30.0)
        self.assertAlmostEqual(direction[0], -math.cos(math.radians(30.0)), places=9)
        self.assertAlmostEqual(direction[1], -0.5, places=9)
        self.assertAlmostEqual(direction[2], 0.0, places=9)
        self.assertAlmostEqual(math.sqrt(sum(c * c for c in direction)), 1.0, places=9)

    def test_shadows_fall_north_at_northern_noon(self):
        lon, lat = CHAMONIX
        # Noon Sun due south at 67.5 degrees: the beam travels north and steeply
        # down, so shadows are short and point north. Engine z is -north.
        state = sun_state(lon, lat, solar_noon(lon, datetime(2026, 6, 21, tzinfo=UTC)), 1035.0)
        self.assertLess(state.direction[2], -0.3)       # northward on the ground
        self.assertLess(state.direction[1], -0.9)       # steeply down
        self.assertLess(abs(state.direction[0]), 0.01)  # no east-west component

    def test_intensity_follows_the_day_and_is_zero_at_night(self):
        lon, lat = CHAMONIX
        noon = solar_noon(lon, datetime(2026, 6, 21, tzinfo=UTC))
        day = sun_state(lon, lat, noon, 1035.0)
        dusk = sun_state(lon, lat, noon + timedelta(hours=7.5), 1035.0)
        night = sun_state(lon, lat, noon + timedelta(hours=12), 1035.0)
        self.assertGreater(day.intensity, dusk.intensity)
        self.assertGreater(dusk.intensity, night.intensity)
        self.assertEqual(night.intensity, 0.0)
        self.assertFalse(night.is_up)
        self.assertTrue(day.is_up)

    def test_night_is_dark_but_not_black(self):
        # A real night: the ambient falls to a few thousandths, which is what
        # a moonless valley is, and never to zero, which is a hole. It stays
        # bluer than it is red.
        lon, lat = CHAMONIX
        night = sun_state(lon, lat, datetime(2026, 12, 21, 1, tzinfo=UTC), 1035.0)
        self.assertGreater(min(night.ambient), 0.0005)
        self.assertLess(max(night.ambient), 0.01)
        self.assertGreater(night.ambient[2], night.ambient[0])   # blue night

    def test_daylight_ambient_is_blue_and_brighter_than_night(self):
        lon, lat = CHAMONIX
        day = sun_state(lon, lat, solar_noon(lon, datetime(2026, 6, 21, tzinfo=UTC)), 1035.0)
        night = sun_state(lon, lat, datetime(2026, 12, 21, 1, tzinfo=UTC), 1035.0)
        self.assertGreater(sum(day.ambient), 3.0 * sum(night.ambient))
        self.assertGreater(day.ambient[2], day.ambient[0])

    def test_sun_colour_reddens_towards_sunset(self):
        lon, lat = CHAMONIX
        events = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        noon = sun_state(lon, lat, events["solarNoon"], 1035.0)
        setting = sun_state(lon, lat, events["sunset"] - timedelta(minutes=6), 1035.0)
        self.assertGreater(setting.color[0] / setting.color[2], 4.0 * noon.color[0] / noon.color[2])
        self.assertAlmostEqual(max(noon.color), 1.0, places=9)

    def test_horizon_is_pale_blue_by_day_and_orange_at_sunset(self):
        # The band above the horizon is the tell-tale of the whole model. Under
        # a high Sun it must stay cool and pale — an orange midday horizon was
        # the first thing this model got wrong — and it must turn warm only as
        # the Sun approaches the horizon.
        lon, lat = CHAMONIX
        events = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        noon = sun_state(lon, lat, events["solarNoon"], 1035.0).horizon_color
        self.assertGreater(noon[2], noon[0])
        self.assertLess(max(noon) - min(noon), 0.25)

        sunset = sun_state(lon, lat, events["sunset"] - timedelta(minutes=10), 1035.0).horizon_color
        self.assertGreater(sunset[0], sunset[2])
        self.assertGreater(sunset[0] / max(sunset[2], 1e-6), 3.0)

    def test_horizon_cools_again_after_the_sun_has_gone(self):
        # Once the disc is down there is no beam left to redden anything: the
        # band must fall back through pink to blue instead of holding red.
        lon, lat = CHAMONIX
        events = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        late = sun_state(lon, lat, events["sunset"] + timedelta(hours=1, minutes=45), 1035.0)
        self.assertLess(late.elevation, NAUTICAL_TWILIGHT)
        self.assertGreaterEqual(late.horizon_color[2], late.horizon_color[0])
        self.assertLess(max(late.horizon_color), 0.2)

    def test_ambient_stays_blue_at_dawn_instead_of_going_grey(self):
        # Skylight comes from the whole dome, so it is read off the zenith
        # column. Reading it off the Sun's slant path would wash it to grey at
        # exactly the moment a real dawn sky is at its bluest.
        lon, lat = CHAMONIX
        events = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        dawn = sun_state(lon, lat, events["sunrise"] + timedelta(minutes=20), 1035.0)
        self.assertGreater(dawn.ambient[2], 1.5 * dawn.ambient[0])

    def test_the_day_brightens_and_dims_monotonically(self):
        lon, lat = CHAMONIX
        events = day_events(lon, lat, datetime(2026, 6, 21, tzinfo=UTC))
        noon = events["solarNoon"]
        rising = [sun_state(lon, lat, noon - timedelta(hours=h), 1035.0).intensity
                  for h in range(7, -1, -1)]
        self.assertEqual(rising, sorted(rising))
        falling = [sun_state(lon, lat, noon + timedelta(hours=h), 1035.0).intensity
                   for h in range(0, 8)]
        self.assertEqual(falling, sorted(falling, reverse=True))

    def test_state_is_deterministic(self):
        lon, lat = CHAMONIX
        when = datetime(2026, 9, 3, 9, 40, tzinfo=UTC)
        first = sun_state(lon, lat, when, 1035.0)
        second = sun_state(lon, lat, when, 1035.0)
        self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
