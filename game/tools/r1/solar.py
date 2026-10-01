"""Solar ephemerides and atmosphere for R1World.

Rank 4 of the fidelity hierarchy (§2), and the cheapest rank on the list:
given a position on the ellipsoid and an instant in UTC, the Sun's place in the
sky is *computed*, not authored. Everything a lighting artist would otherwise
invent falls out of it — shadow direction and length, the colour of the light,
the length of the day, how long dusk lasts, how high noon reaches. A June noon
at Tromso looks like nowhere else on Earth, and it costs the few hundred lines
below rather than an asset budget.

Two computations live here and they are deliberately kept apart.

  Ephemerides   Where the Sun is. Pure astronomy, no free parameters, and
                accurate to about 0.01 degrees between 1950 and 2050 (NOAA's
                form of Meeus, *Astronomical Algorithms*, ch. 25 and 13).
                Nothing about this is tunable: it is either right or wrong,
                and `tests/test_solar.py` pins it against published values.

  Atmosphere    What the light looks like once it has crossed the air. Rayleigh
                scattering is fixed by physics; the aerosol load is not, and it
                is the one honest knob (`turbidity`). A Provence sky and a
                Brittany sky differ first by their turbidity (§2.2), so it is a
                per-region datum the Atlas will eventually supply. Until then
                the caller passes it explicitly.

Sun angles follow the convention the rest of the pipeline already uses for
bearings: azimuth in degrees clockwise from true north, elevation in degrees
above the true horizon. `sun_state()` also returns the light's travel direction
already in the engine frame (x=east, y=up, z=-north, cf. `geodesy.py`), because
that is what a `LightNode` consumes and converting it anywhere else invites the
sign error twice.

Determinism (I3): every function here is pure and float64. The same instant and
place produce the same light on every machine, so a scene generated from a date
is as reproducible as one generated from a hardcoded vector.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone

# Apparent elevation of the Sun's centre at sunrise and sunset: half the solar
# disc (0.267 deg) plus the standard 0.566 deg of horizon refraction.
SUNSET_ELEVATION = -0.833

# Twilight thresholds, by the usual definitions.
CIVIL_TWILIGHT = -6.0
NAUTICAL_TWILIGHT = -12.0
ASTRONOMICAL_TWILIGHT = -18.0

# Scale height of the atmosphere in metres, for the pressure correction that
# thins the air mass at altitude. Chamonix at 1035 m already keeps 88% of the
# sea-level column; the Aiguille du Midi at 3842 m keeps 63%.
PRESSURE_SCALE_HEIGHT = 8435.0

# Effective wavelengths of the three display primaries in micrometres. The
# whole colour model rests on these: scattering goes as lambda^-4, so blue is
# extinguished about 3.0x more than red per unit air mass, which is exactly why
# a low Sun is orange and the sky it lights is blue.
WAVELENGTHS = (0.610, 0.550, 0.465)

# Angstrom exponent for continental aerosol. Aerosol extinction is much flatter
# in wavelength than Rayleigh (1.3 against 4), so haze greys the light where
# clean air reddens it.
ANGSTROM_EXPONENT = 1.3

# The two calibration constants, and the only two numbers here that are not
# physics. Everything above computes *ratios* — between channels, and between
# one hour and the next — which is what carries the resemblance. Turning a ratio
# into an engine value needs a scale, and the renderer's ambient and fog terms
# are not radiometric quantities: they are shading inputs whose usable range was
# established by eye on the first alpine scene. These pin the model to that range so a high
# Sun lands where the hand-tuned scene already sat, and every other hour follows
# from the physics rather than from a second set of numbers. If the renderer
# ever gains a real exposure model, these are what it replaces.
AMBIENT_PEAK = 0.20   # brightest channel of the ambient term, Sun overhead
HORIZON_PEAK = 0.70   # brightest channel of the fog and low-sky term, likewise

# Engine light intensity for an overhead Sun through a clear column. Not a
# third calibration constant: it only fixes the unit the two above are read in,
# and every other hour is this scaled by what the air actually let through. It
# is named rather than defaulted inline because the runtime cycle
# (`scripts/sun_cycle.js`) has to be handed the same number the scene was lit
# with, and a literal repeated in two languages is a divergence waiting to
# happen.
PEAK_INTENSITY = 4.6


@dataclass(frozen=True)
class SunState:
    """The Sun at one instant, one place, under one atmosphere.

    `elevation` is apparent (refracted) and is what decides whether the Sun is
    visible; `true_elevation` is geometric and is what casts the shadows. They
    differ by up to 0.57 degrees at the horizon, which is more than the solar
    disc is wide.
    """

    when: datetime
    lon: float
    lat: float
    altitude: float

    azimuth: float          # degrees clockwise from true north
    elevation: float        # degrees above the horizon, refracted
    true_elevation: float   # degrees above the horizon, geometric
    declination: float      # degrees
    equation_of_time: float  # minutes
    air_mass: float         # relative optical air mass, 1.0 at the zenith

    direction: tuple[float, float, float]  # engine frame, light travel direction
    color: tuple[float, float, float]      # linear RGB, normalised to peak 1.0
    intensity: float                       # engine LightNode intensity
    ambient: tuple[float, float, float]    # linear RGB sky term
    horizon_color: tuple[float, float, float]  # linear RGB, fog and low sky
    ibl_intensity: float    # IBL diffuse and specular multiplier

    @property
    def is_up(self) -> bool:
        return self.elevation > SUNSET_ELEVATION


# ── ephemerides ─────────────────────────────────────────────────────────────

def julian_day(when: datetime) -> float:
    """UTC instant -> Julian Day number, the time axis all of this runs on.

    Naive datetimes are read as UTC; the pipeline never handles local time
    except at the very edge, because civil time zones are political data and
    the Sun is not.
    """
    if when.tzinfo is not None:
        when = when.astimezone(timezone.utc).replace(tzinfo=None)

    year, month = when.year, when.month
    if month <= 2:                                     # Jan and Feb belong to
        year -= 1                                      # the previous year in
        month += 12                                    # this formulation
    a = year // 100
    b = 2 - a + a // 4                                 # Gregorian correction
    day = (when.day
           + (when.hour + (when.minute + (when.second + when.microsecond / 1e6) / 60.0) / 60.0)
           / 24.0)
    return (math.floor(365.25 * (year + 4716))
            + math.floor(30.6001 * (month + 1))
            + day + b - 1524.5)


def _julian_century(when: datetime) -> float:
    return (julian_day(when) - 2451545.0) / 36525.0


def _solar_geometry(t: float) -> tuple[float, float]:
    """Julian century -> (apparent declination, equation of time) in deg, minutes.

    Meeus' low-precision solar theory in NOAA's arrangement. The nutation and
    aberration terms (the 125.04 - 1934.136 t argument) are what take this from
    a few arcminutes to a fraction of an arcminute; they cost two cosines.
    """
    mean_longitude = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360.0
    mean_anomaly = 357.52911 + t * (35999.05029 - 0.0001537 * t)
    eccentricity = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)

    m = math.radians(mean_anomaly)
    equation_of_centre = (math.sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t))
                          + math.sin(2 * m) * (0.019993 - 0.000101 * t)
                          + math.sin(3 * m) * 0.000289)
    true_longitude = mean_longitude + equation_of_centre

    omega = math.radians(125.04 - 1934.136 * t)
    apparent_longitude = true_longitude - 0.00569 - 0.00478 * math.sin(omega)

    mean_obliquity = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0
    obliquity = math.radians(mean_obliquity + 0.00256 * math.cos(omega))

    declination = math.degrees(math.asin(
        math.sin(obliquity) * math.sin(math.radians(apparent_longitude))
    ))

    # Equation of time: the gap between the real Sun and a fictitious uniform
    # one. It swings +-16 minutes over the year and is the reason solar noon
    # drifts against the clock even on a fixed meridian.
    y = math.tan(obliquity / 2.0) ** 2
    l0 = math.radians(mean_longitude)
    equation_of_time = 4.0 * math.degrees(
        y * math.sin(2 * l0)
        - 2.0 * eccentricity * math.sin(m)
        + 4.0 * eccentricity * y * math.sin(m) * math.cos(2 * l0)
        - 0.5 * y * y * math.sin(4 * l0)
        - 1.25 * eccentricity * eccentricity * math.sin(2 * m)
    )
    return declination, equation_of_time


def refraction(true_elevation: float) -> float:
    """Degrees the atmosphere lifts an object at `true_elevation`.

    Zero overhead, 0.57 deg at the horizon — enough that the Sun is wholly
    visible at the moment it is geometrically wholly set. Below the horizon the
    correction is capped, because the model has no meaning there and only needs
    to stay continuous for the twilight blend.
    """
    if true_elevation > 85.0:
        return 0.0
    tan_e = math.tan(math.radians(true_elevation))
    if true_elevation > 5.0:
        arcseconds = 58.1 / tan_e - 0.07 / tan_e ** 3 + 0.000086 / tan_e ** 5
    elif true_elevation > -0.575:
        e = true_elevation
        arcseconds = 1735.0 + e * (-518.2 + e * (103.4 + e * (-12.79 + e * 0.711)))
    else:
        arcseconds = -20.772 / tan_e
    return arcseconds / 3600.0


def sun_angles(lon: float, lat: float, when: datetime) -> tuple[float, float, float, float, float]:
    """(azimuth, apparent elevation, true elevation, declination, eqtime).

    The core ephemeris. Azimuth is degrees clockwise from true north and is
    defined even when the Sun is below the horizon, so a dawn sky can be
    coloured before the disc appears.
    """
    t = _julian_century(when)
    declination, equation_of_time = _solar_geometry(t)

    utc = when.astimezone(timezone.utc) if when.tzinfo is not None else when
    minutes = utc.hour * 60.0 + utc.minute + (utc.second + utc.microsecond / 1e6) / 60.0

    # True solar time, then the hour angle: 15 degrees per hour, zero at noon.
    true_solar_time = (minutes + equation_of_time + 4.0 * lon) % 1440.0
    hour_angle = math.radians(true_solar_time / 4.0 - 180.0)

    phi, delta = math.radians(lat), math.radians(declination)
    cos_zenith = (math.sin(phi) * math.sin(delta)
                  + math.cos(phi) * math.cos(delta) * math.cos(hour_angle))
    cos_zenith = max(-1.0, min(1.0, cos_zenith))
    zenith = math.acos(cos_zenith)
    true_elevation = 90.0 - math.degrees(zenith)

    # Azimuth from the spherical triangle, taken through atan2 so it stays
    # stable near the poles where the usual acos form loses all its precision.
    sin_zenith = math.sin(zenith)
    if sin_zenith < 1e-9:                              # Sun at the zenith
        azimuth = 180.0
    else:
        azimuth = math.degrees(math.atan2(
            math.sin(hour_angle),
            math.cos(hour_angle) * math.sin(phi) - math.tan(delta) * math.cos(phi),
        )) + 180.0
    return azimuth % 360.0, true_elevation + refraction(true_elevation), true_elevation, declination, equation_of_time


def sun_direction(azimuth: float, elevation: float) -> tuple[float, float, float]:
    """Engine-frame unit vector the sunlight *travels along*.

    A `LightNode` of type directional stores the direction light goes, not the
    direction of the source, so this is the vector pointing away from the Sun.
    Engine axes are x=east, y=up, z=-north (`geodesy.py`).
    """
    e = math.radians(elevation)
    a = math.radians(azimuth)
    east = math.cos(e) * math.sin(a)
    north = math.cos(e) * math.cos(a)
    up = math.sin(e)
    return -east, -up, north                           # z = -north, then negated


# ── day length and twilight ─────────────────────────────────────────────────

def hour_angle_at_elevation(lat: float, declination: float, elevation: float) -> float | None:
    """Hour angle in degrees at which the Sun crosses `elevation`, or None.

    None means the Sun never reaches that elevation on that day at that
    latitude: midnight sun, polar night, or a twilight that never ends. Those
    are not edge cases to be clamped away — they are the whole point of
    computing this rather than authoring a curve.
    """
    phi, delta = math.radians(lat), math.radians(declination)
    denominator = math.cos(phi) * math.cos(delta)
    if abs(denominator) < 1e-12:
        return None
    cos_h = (math.sin(math.radians(elevation)) - math.sin(phi) * math.sin(delta)) / denominator
    if cos_h > 1.0 or cos_h < -1.0:
        return None
    return math.degrees(math.acos(cos_h))


def solar_noon(lon: float, when: datetime) -> datetime:
    """UTC instant the Sun crosses the local meridian on `when`'s date."""
    midday = datetime(when.year, when.month, when.day, 12, tzinfo=timezone.utc)
    _, equation_of_time = _solar_geometry(_julian_century(midday))
    return midday + timedelta(minutes=-(4.0 * lon + equation_of_time))


def day_events(lon: float, lat: float, when: datetime) -> dict:
    """Sunrise, sunset, day length and civil dusk for `when`'s date, in UTC.

    `sunrise` and `sunset` are None on a polar day or night; `dayLengthHours`
    is then 24 or 0 and remains the value to trust.
    """
    noon = solar_noon(lon, when)
    declination, _ = _solar_geometry(_julian_century(noon))
    events = {"solarNoon": noon, "declination": declination}

    h = hour_angle_at_elevation(lat, declination, SUNSET_ELEVATION)
    if h is None:
        _, elevation, _, _, _ = sun_angles(lon, lat, noon)
        polar_day = elevation > SUNSET_ELEVATION
        events.update(sunrise=None, sunset=None,
                      dayLengthHours=24.0 if polar_day else 0.0,
                      polarDay=polar_day, polarNight=not polar_day)
    else:
        offset = timedelta(hours=h / 15.0)
        events.update(sunrise=noon - offset, sunset=noon + offset,
                      dayLengthHours=2.0 * h / 15.0,
                      polarDay=False, polarNight=False)

    civil = hour_angle_at_elevation(lat, declination, CIVIL_TWILIGHT)
    events["civilTwilightHours"] = (
        None if civil is None or h is None else (civil - h) / 15.0
    )
    return events


# ── atmosphere ──────────────────────────────────────────────────────────────

def air_mass(elevation: float, altitude: float = 0.0) -> float:
    """Relative optical air mass at an apparent `elevation`, from `altitude` m.

    Kasten-Young (1989), which unlike 1/sin(h) stays finite at the horizon: it
    reads 1.0 overhead, 2.0 at 30 degrees, and about 38 at the horizon. Scaled
    by the pressure ratio, so thin mountain air genuinely attenuates less —
    that is why alpine light is harder and bluer than sea-level light, and it
    comes out of the model instead of being dialled in.
    """
    h = max(elevation, -2.0)
    relative = 1.0 / (math.sin(math.radians(h)) + 0.50572 * (h + 6.07995) ** -1.6364)
    return relative * math.exp(-max(altitude, 0.0) / PRESSURE_SCALE_HEIGHT)


def _rayleigh_optical_depth(wavelength: float) -> float:
    """Vertical Rayleigh optical depth at sea level, standard atmosphere."""
    inverse = 1.0 / (wavelength ** 4)
    return 0.008569 * inverse * (1.0 + 0.0113 / wavelength ** 2 + 0.00013 / wavelength ** 4)


def _aerosol_optical_depth(wavelength: float, turbidity: float) -> float:
    """Angstrom aerosol depth. `turbidity` is Linke-like: 2 clean, 6 hazy."""
    beta = 0.025 * max(turbidity - 1.0, 0.0)
    return beta * wavelength ** -ANGSTROM_EXPONENT


def transmittance(elevation: float, altitude: float, turbidity: float) -> tuple[float, float, float]:
    """Per-channel fraction of direct sunlight that survives the air column.

    This single Beer-Lambert line is the whole of the sunset: at the zenith the
    three channels differ by a few percent and the light reads white; at one
    degree of elevation the blue channel has lost 99.9% and the red 90%, and
    the same Sun is orange without anyone having keyed a gradient.
    """
    m = air_mass(elevation, altitude)
    return tuple(
        math.exp(-(_rayleigh_optical_depth(w) + _aerosol_optical_depth(w, turbidity)) * m)
        for w in WAVELENGTHS
    )


def _normalise(rgb: tuple[float, float, float]) -> tuple[float, float, float]:
    peak = max(rgb)
    if peak <= 1e-9:
        return (0.0, 0.0, 0.0)
    return tuple(component / peak for component in rgb)


def _smoothstep(edge0: float, edge1: float, x: float) -> float:
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def hdri_daylight(elevation: float) -> float:
    """How much image-based lighting to keep, 0 at night and 1 by day.

    This is not the atmosphere: it is a correction applied to a *stand-in*.
    IBL samples one sky photograph, normalised like all of them (see
    `r1/skies.py`), so left alone it would light the ground at noon strength an
    hour after sunset. Scaling it down with the Sun's height does not make it
    right — only less wrong — and it is a linear ramp across the civil-twilight
    band rather than one of the smoothsteps above because there is no physical
    quantity here to model. The skybox itself is driven by `r1/skies.py`.

    It is named and shared because the generator bakes it and the runtime cycle
    re-derives it every frame; the same numbers must come out of both.
    """
    return max(0.0, min(1.0, (elevation + 6.0) / 12.0))


def sun_state(
    lon: float,
    lat: float,
    when: datetime,
    altitude: float = 0.0,
    turbidity: float = 2.5,
    peak_intensity: float = PEAK_INTENSITY,
) -> SunState:
    """Everything the renderer needs about the Sun at one instant.

    `peak_intensity` is the engine light intensity for an overhead Sun; the
    returned intensity is that scaled by how much light actually reaches the
    ground, so dawn is dim because the air ate it, not because a curve says so.
    """
    azimuth, elevation, true_elevation, declination, eqtime = sun_angles(lon, lat, when)
    direct = transmittance(elevation, altitude, turbidity)

    # The disc sets over about half a degree, and the terminator should not
    # snap. Fading over the last two degrees also stands in for the horizon
    # clutter (haze, ridgelines) that hides a real setting Sun early.
    visibility = _smoothstep(SUNSET_ELEVATION - 2.0, SUNSET_ELEVATION + 2.0, elevation)
    luminance = 0.2126 * direct[0] + 0.7152 * direct[1] + 0.0722 * direct[2]
    intensity = peak_intensity * luminance * visibility

    # Skylight is exactly what the direct beam lost: the complement of the
    # transmittance, which is blue for the same reason and by the same numbers
    # that make a low Sun red. Below the horizon it keeps a floor, so night is
    # navigable blue rather than black — the way a moonless night reads on
    # screen, and the one place this model is a rendering choice, not physics.
    # Read off the *zenith* column rather than the Sun's slant path: skylight
    # reaches the ground from the whole dome, and the overhead sky stays blue
    # all day even when the beam has gone orange. Taking 1 - T along the slant
    # path instead would wash the ambient to grey at dawn, which is the one
    # thing a dawn sky is not.
    sky = _normalise(tuple(1.0 - c for c in transmittance(90.0, altitude, turbidity)))
    daylight = _smoothstep(ASTRONOMICAL_TWILIGHT, 6.0, elevation)
    night_ambient = (0.001, 0.0015, 0.0025)
    ambient_peak = AMBIENT_PEAK * (0.83 * _smoothstep(-8.0, 25.0, elevation) + 0.17)
    ambient = tuple(
        night + (ambient_peak * s - night) * daylight
        for night, s in zip(night_ambient, sky)
    )

    # The horizon band — what fog and the low sky must match — is a long
    # scattering column, and what colours it is which light reaches it. Under a
    # high Sun that light is white and multiply scattered, so the horizon goes
    # pale and slightly blue while the zenith stays deep. As the Sun drops, the
    # column is lit by the reddened beam itself and the band turns orange, which
    # is why a distant ridge at dusk is pink under a blue zenith. `warmth` is
    # that handover, and it is driven by elevation alone.
    # `beam` closes the handover at the other end: once the disc is below the
    # horizon there is no direct light left to redden anything, and the band
    # settles back through pink to the blue of the residual sky. Without it the
    # model would hold a pure red horizon through a night that has none.
    beam = _smoothstep(NAUTICAL_TWILIGHT, 1.0, elevation)
    warmth = (1.0 - _smoothstep(0.0, 22.0, elevation)) * beam
    pale = tuple(0.58 + 0.42 * s for s in sky)
    warm = _normalise(direct)
    night_horizon = (0.0015, 0.002, 0.004)
    horizon_color = tuple(
        night + (HORIZON_PEAK * (p + (w - p) * warmth) - night) * daylight
        for night, p, w in zip(night_horizon, pale, warm)
    )

    # IBL is the one term that is a correction rather than a measurement;
    # `hdri_daylight` says why.
    hdri = hdri_daylight(elevation)

    return SunState(
        when=when, lon=lon, lat=lat, altitude=altitude,
        azimuth=azimuth, elevation=elevation, true_elevation=true_elevation,
        declination=declination, equation_of_time=eqtime,
        air_mass=air_mass(elevation, altitude),
        direction=sun_direction(azimuth, true_elevation),
        color=_normalise(direct),
        intensity=intensity,
        ambient=ambient,
        horizon_color=horizon_color,
        ibl_intensity=0.12 + 0.88 * hdri,
    )
