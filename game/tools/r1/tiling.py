"""The tile scheme: how the Earth is cut up, and what each cut carries.

R1World uses standard Web Mercator (slippy) tiles, z/x/y, because that is the
native grid of every source the pipeline reads — OSM vector tiles, Copernicus
DEM, ESA WorldCover, Sentinel imagery. Inventing a grid would mean resampling
all of them at import for no gain.

Mercator tiles shrink with latitude by cos(lat): a z14 tile is 2445 m across
at the equator, 1610 m in Paris, 1222 m in Oslo. The scheme does not correct
for that. A layer keeps one fixed zoom worldwide and the streamer simply loads
more tiles at high latitude — they are proportionally lighter, because they
cover proportionally less ground, so the budget per ring stays roughly flat.
Correcting it instead (zoom varying by latitude band) would buy uniform tile
counts at the price of seams between bands, which is the worse trade.

Layers are not levels of detail of one thing. Each carries different content
and is loaded at a different radius, so a mountain 200 km away exists as
terrain and nothing else, while a wall 20 m away exists down to its window
frames. They are loaded independently and simultaneously.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Iterator

try:
    from .geodesy import R_MEAN
except ImportError:  # Allow running the module directly from this directory.
    from geodesy import R_MEAN

A_EQ = 6378137.0
EARTH_CIRCUMFERENCE = 2.0 * math.pi * A_EQ
MAX_MERCATOR_LAT = 85.0511287798066   # where the projection is cut square
REFERENCE_PRESET = "reference"
COMFORT_PRESET = "comfort"
PRESETS = (REFERENCE_PRESET, COMFORT_PRESET)


def _validate_zoom(z: int) -> None:
    if not isinstance(z, int) or isinstance(z, bool) or z < 0 or z > 30:
        raise ValueError("zoom must be an integer in [0, 30]")


def _validate_tile(x: int, y: int, z: int) -> None:
    _validate_zoom(z)
    limit = 1 << z
    if not isinstance(x, int) or isinstance(x, bool) or not 0 <= x < limit:
        raise ValueError(f"tile x must be an integer in [0, {limit})")
    if not isinstance(y, int) or isinstance(y, bool) or not 0 <= y < limit:
        raise ValueError(f"tile y must be an integer in [0, {limit})")


# ── slippy tile maths ───────────────────────────────────────────────────────

def tile_of(lon: float, lat: float, z: int) -> tuple:
    """(lon, lat) -> (x, y) of the tile containing it at zoom z."""
    _validate_zoom(z)
    if not math.isfinite(lon) or not math.isfinite(lat):
        raise ValueError("longitude and latitude must be finite")
    lon = ((lon + 180.0) % 360.0) - 180.0
    lat = max(-MAX_MERCATOR_LAT, min(MAX_MERCATOR_LAT, lat))
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    r = math.radians(lat)
    y = int((1.0 - math.asinh(math.tan(r)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def tile_bounds(x: int, y: int, z: int) -> tuple:
    """(x, y, z) -> (west, south, east, north) in degrees."""
    _validate_tile(x, y, z)
    n = 1 << z

    def lat_at(ty):
        return math.degrees(math.atan(math.sinh(math.pi * (1.0 - 2.0 * ty / n))))

    return (x / n * 360.0 - 180.0, lat_at(y + 1),
            (x + 1) / n * 360.0 - 180.0, lat_at(y))


def tile_center(x: int, y: int, z: int) -> tuple:
    w, s, e, n = tile_bounds(x, y, z)
    return (w + e) / 2.0, (s + n) / 2.0


def tile_span_m(x: int, y: int, z: int) -> tuple:
    """Ground size of a tile in metres, as (width, height).

    Width follows cos(lat); height differs from it slightly because Mercator
    stretches north-south by the same factor but the tile's own latitude range
    is not symmetric about its centre.
    """
    w, s, e, n = tile_bounds(x, y, z)
    lat = math.radians((s + n) / 2.0)
    width = math.radians(e - w) * R_MEAN * math.cos(lat)
    height = math.radians(n - s) * R_MEAN
    return width, height


def quadkey(x: int, y: int, z: int) -> str:
    """Bing-style quadkey. One string that is its own path and its own parent
    chain, which is what the on-disk cache and the tile server both key on."""
    _validate_tile(x, y, z)
    out = []
    for i in range(z, 0, -1):
        bit = 1 << (i - 1)
        d = 0
        if x & bit:
            d += 1
        if y & bit:
            d += 2
        out.append(str(d))
    return "".join(out)


def from_quadkey(key: str) -> tuple:
    if not isinstance(key, str) or any(ch not in "0123" for ch in key):
        raise ValueError("quadkey must contain only digits 0 through 3")
    _validate_zoom(len(key))
    x = y = 0
    z = len(key)
    for i, ch in enumerate(key):
        bit = 1 << (z - i - 1)
        d = int(ch)
        if d & 1:
            x |= bit
        if d & 2:
            y |= bit
    return x, y, z


# ── the layer table ─────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Layer:
    name: str
    zoom: int
    reference_radius_m: float
    comfort_radius_m: float
    terrain_post_m: float  # DEM sample spacing baked into this layer's mesh
    content: str

    def radius_m(self, preset: str = REFERENCE_PRESET) -> float:
        """Residence radius for one of the two hardware presets."""
        if preset == REFERENCE_PRESET:
            return self.reference_radius_m
        if preset == COMFORT_PRESET:
            return self.comfort_radius_m
        raise ValueError(f"unknown preset {preset!r}; expected one of {PRESETS}")

    def tile_span_equator(self) -> float:
        return EARTH_CIRCUMFERENCE / (1 << self.zoom)

    def grid_per_tile(self) -> int:
        """Terrain vertices per tile edge implied by the post spacing."""
        return max(2, round(self.tile_span_equator() / self.terrain_post_m))


# Radii are read off the horizon, not guessed. Eye height 1.7 m sees 4.7 km;
# a 300 m tower sees 62 km; a 4000 m summit sees 226 km. L1 at 300 km is what
# makes the view from a mountain honest; L5 at 400 m is what makes a street
# honest. Everything between interpolates.
LAYERS = (
    Layer("globe",     5, 20_000_000, 20_000_000, 4000.0,
          "ellipsoid shell, ocean, coarse relief, satellite albedo"),
    Layer("region",    9,    300_000,    300_000,  240.0,
          "relief silhouettes, coastline, major hydrography, snow/ice"),
    Layer("landscape", 12,     40_000,     60_000,   60.0,
          "terrain, landcover ground materials, forest canopy as volume, "
          "motorways, large water bodies, city blocks as massing"),
    Layer("local",     14,      6_000,     12_000,   15.0,
          "terrain, road network with geometry, building footprints extruded "
          "as flat-shaded blocks, tree clusters as billboard clusters"),
    Layer("street",    16,      1_000,      2_000,    5.0,
          "individual buildings with facade atlas and roofs, road markings, "
          "individual trees, water with shore, walls and fences"),
    Layer("detail",    17,        200,        400,    5.0,
          "street furniture, signage, parked vehicles, ground clutter, "
          "vegetation instances, facade detail geometry"),
)

BY_NAME = {l.name: l for l in LAYERS}


def tiles_in_radius(
    lon: float,
    lat: float,
    layer: Layer,
    preset: str = REFERENCE_PRESET,
) -> Iterator[tuple]:
    """Every (x, y, z) of `layer` whose centre is within its radius.

    Walks the bounding box in tile space and rejects on true ground distance,
    so the loaded set is a disc rather than a square — at 12 km that is 21%
    fewer tiles for the same view distance.
    """
    radius_m = layer.radius_m(preset)
    z = layer.zoom
    n = 1 << z
    cx, cy = tile_of(lon, lat, z)
    span_w, span_h = tile_span_m(cx, cy, z)
    rx = int(math.ceil(radius_m / max(span_w, 1.0)))
    ry = int(math.ceil(radius_m / max(span_h, 1.0)))
    r2 = radius_m ** 2

    for dy in range(-ry, ry + 1):
        y = cy + dy
        if y < 0 or y >= n:
            continue
        for dx in range(-rx, rx + 1):
            x = (cx + dx) % n                      # wrap the antimeridian
            tw, th = tile_span_m(x, y, z)
            if (dx * tw) ** 2 + (dy * th) ** 2 <= r2:
                yield x, y, z


def loaded_set_size(lat: float, preset: str = REFERENCE_PRESET) -> dict:
    """Tile counts per layer at a latitude — the streamer's working set.

    High latitude costs more tiles and not more content; this is what says by
    how much, and it is the number the residency budget is sized against.
    """
    return {
        layer.name: sum(1 for _ in tiles_in_radius(0.0, lat, layer, preset))
        for layer in LAYERS
    }


def horizon_distance(eye_height_m: float) -> float:
    """Geometric horizon for an eye that high, ignoring refraction."""
    return math.sqrt(max(eye_height_m, 0.0) * (2.0 * R_MEAN + eye_height_m))
