"""WGS84 geodesy for R1World.

Every coordinate in the pipeline lives in one of four frames. Converting
between them is the single operation the whole world rests on, so it lives
here and nowhere else.

  geodetic  (lon, lat, alt)   degrees / degrees / metres above the ellipsoid
  ECEF      (X, Y, Z)         earth-centred earth-fixed metres, float64
  ENU       (E, N, U)         metres on a plane tangent at one anchor
  engine    (x, y, z)         SaidaEngine's Y-up float32 world

The engine stores transforms as float32 (`src/scene/Transform.hpp`), which
holds ~0.1 mm of precision at 1 km from the origin but only ~1 m at 10000 km.
ECEF coordinates of any point on the surface are ~6.4e6 m, so they can never
be handed to the engine directly. Everything the engine sees is ENU relative
to a nearby anchor, and the anchor moves with the player (see `anchors.py`).

ENU is a rigid rotation and translation of ECEF, so it introduces no shape
error at all: a mountain 200 km away leans away from the anchor's up by the
real 1.8 degrees, and the horizon drops by the real 3.1 km. That curvature is
content, not error, and `sag()` reports it so terrain and horizon budgets can
be written against it.

The only true error is float32 quantisation, which is distance * 2^-23:
1.2 mm at 10 km, 2.4 cm at 200 km, 12 cm at 1000 km. Static geometry
tolerates that to the horizon; what does not is the camera and the physics
solver, where sub-millimetre jitter is visible. That sets the anchor policy,
not the geometry: re-anchor when the player leaves a few km, and distant
tiles can keep streaming against the same frame.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

# WGS84 defining parameters.
A = 6378137.0                  # semi-major axis (m)
F = 1.0 / 298.257223563        # flattening
B = A * (1.0 - F)              # semi-minor axis (m)
E2 = F * (2.0 - F)             # first eccentricity squared
EP2 = E2 / (1.0 - E2)          # second eccentricity squared

# Mean radius, used only for tangent-plane error estimates.
R_MEAN = 6371008.8


# ── geodetic <-> ECEF ───────────────────────────────────────────────────────

def geodetic_to_ecef(lon_deg: float, lat_deg: float, alt: float = 0.0):
    """(lon, lat, alt) -> ECEF metres."""
    lon = math.radians(lon_deg)
    lat = math.radians(lat_deg)
    sin_lat, cos_lat = math.sin(lat), math.cos(lat)
    n = A / math.sqrt(1.0 - E2 * sin_lat * sin_lat)   # prime vertical radius
    x = (n + alt) * cos_lat * math.cos(lon)
    y = (n + alt) * cos_lat * math.sin(lon)
    z = (n * (1.0 - E2) + alt) * sin_lat
    return x, y, z


def ecef_to_geodetic(x: float, y: float, z: float):
    """ECEF metres -> (lon, lat, alt).

    Bowring's method with one refinement: sub-millimetre for altitudes from
    the sea floor to low orbit, and unlike the iterative forms it always
    terminates.
    """
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    if p < 1e-9:                                       # on the spin axis
        return math.degrees(lon), 90.0 if z >= 0 else -90.0, abs(z) - B

    theta = math.atan2(z * A, p * B)
    sin_t, cos_t = math.sin(theta), math.cos(theta)
    lat = math.atan2(z + EP2 * B * sin_t ** 3, p - E2 * A * cos_t ** 3)

    sin_lat = math.sin(lat)
    n = A / math.sqrt(1.0 - E2 * sin_lat * sin_lat)
    alt = p / math.cos(lat) - n
    return math.degrees(lon), math.degrees(lat), alt


# ── ECEF <-> ENU ────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Anchor:
    """A local tangent frame: the origin the engine's float32 world sits on.

    Built once per anchor cell and cached, because the rotation matrix costs
    four trig calls and every vertex of every streamed tile goes through it.
    """
    lon: float
    lat: float
    alt: float
    x: float
    y: float
    z: float
    # Rows of the ECEF->ENU rotation, precomputed.
    e: tuple
    n: tuple
    u: tuple

    @staticmethod
    def at(lon_deg: float, lat_deg: float, alt: float = 0.0) -> "Anchor":
        x, y, z = geodetic_to_ecef(lon_deg, lat_deg, alt)
        lon, lat = math.radians(lon_deg), math.radians(lat_deg)
        sl, cl = math.sin(lon), math.cos(lon)
        sp, cp = math.sin(lat), math.cos(lat)
        return Anchor(
            lon_deg, lat_deg, alt, x, y, z,
            e=(-sl, cl, 0.0),
            n=(-sp * cl, -sp * sl, cp),
            u=(cp * cl, cp * sl, sp),
        )

    def ecef_to_enu(self, x: float, y: float, z: float):
        dx, dy, dz = x - self.x, y - self.y, z - self.z
        return (self.e[0] * dx + self.e[1] * dy + self.e[2] * dz,
                self.n[0] * dx + self.n[1] * dy + self.n[2] * dz,
                self.u[0] * dx + self.u[1] * dy + self.u[2] * dz)

    def enu_to_ecef(self, e: float, n: float, u: float):
        return (self.x + self.e[0] * e + self.n[0] * n + self.u[0] * u,
                self.y + self.e[1] * e + self.n[1] * n + self.u[1] * u,
                self.z + self.e[2] * e + self.n[2] * n + self.u[2] * u)

    def geodetic_to_enu(self, lon_deg: float, lat_deg: float, alt: float = 0.0):
        return self.ecef_to_enu(*geodetic_to_ecef(lon_deg, lat_deg, alt))

    def enu_to_geodetic(self, e: float, n: float, u: float):
        return ecef_to_geodetic(*self.enu_to_ecef(e, n, u))

    # ── engine frame ────────────────────────────────────────────────────────
    # SaidaEngine is Y-up, right-handed, -Z forward. Mapping ENU onto it as
    # (x=E, y=U, z=-N) keeps the handedness and puts north at -Z, so a yaw of 0
    # faces north and yaw grows clockwise from above, which is what a compass
    # bearing already means.

    def geodetic_to_engine(self, lon_deg: float, lat_deg: float, alt: float = 0.0):
        e, n, u = self.geodetic_to_enu(lon_deg, lat_deg, alt)
        return e, u, -n

    def engine_to_geodetic(self, x: float, y: float, z: float):
        return self.enu_to_geodetic(x, -z, y)


def sag(distance: float) -> float:
    """Metres the surface drops below the anchor's tangent plane, `distance` away.

    Earth's curvature as the player sees it, not an error term: 8 cm at 1 km,
    31 m at 20 km, 3.1 km at 200 km. Horizon distance, how much of a distant
    city is hidden, and how far a tile LOD must reach are all read off this.
    """
    return R_MEAN - math.sqrt(max(R_MEAN * R_MEAN - distance * distance, 0.0))


def haversine(lon1: float, lat1: float, lon2: float, lat2: float) -> float:
    """Great-circle distance in metres. Used for budgets and sanity checks."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2.0 * R_MEAN * math.asin(math.sqrt(h))


def float32_resolution(distance: float) -> float:
    """Smallest representable step in a float32 coordinate `distance` from 0.

    The engine's `Transform` is float32 (`src/scene/Transform.hpp`), so this
    is the real precision floor of anything handed to it. Anchor policy is
    written against it: keep the camera inside the radius where this stays
    under a millimetre.
    """
    return abs(distance) * 2.0 ** -23
