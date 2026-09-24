"""Global, metre-sized latitude rings. No Mercator polar cutoff.

Each tile owns its WGS84 tangent frame; the renderer positions it against the
current floating origin. Cache keys do not depend on a journey's spawn point.
"""
import math
from dataclasses import dataclass


@dataclass(frozen=True)
class Bounds:
    south: float
    west: float
    north: float
    east: float

ROWS = 36000
STEP = 180 / ROWS
VERSION = 15


def wrap(lon):
    return (lon + 180) % 360 - 180


def columns(row):
    return max(1, round(72000 * math.cos(math.radians(-90 + (row + .5) * STEP))))


@dataclass(frozen=True)
class Tile:
    row: int
    col: int

    def __post_init__(self):
        if not 0 <= self.row < ROWS or not 0 <= self.col < columns(self.row):
            raise ValueError("Invalid world tile")

    @property
    def key(self):
        return f"v{VERSION}_{self.row}_{self.col}"

    @property
    def bounds(self):
        w = 360 / columns(self.row)
        return Bounds(-90 + self.row * STEP, -180 + self.col * w,
                      -90 + (self.row + 1) * STEP, -180 + (self.col + 1) * w)

    @property
    def center(self):
        b = self.bounds
        return (b.west + b.east) / 2, (b.south + b.north) / 2


def tile_at(lon, lat):
    if not math.isfinite(lon) or not math.isfinite(lat) or not -90 <= lat <= 90:
        raise ValueError("Invalid WGS84 coordinate")
    row = min(ROWS - 1, max(0, math.floor((lat + 90) / STEP)))
    return Tile(row, min(columns(row) - 1, math.floor((wrap(lon) + 180) / 360 * columns(row))))


def neighborhood(lon, lat, radius=1):
    center = tile_at(lon, lat)
    result = {center}
    for row in range(max(0, center.row - radius), min(ROWS, center.row + radius + 1)):
        n = columns(row)
        c = math.floor((wrap(lon) + 180) / 360 * n)
        result.update(Tile(row, (c + d) % n) for d in range(-radius, radius + 1))
    # Center first; stable ordering for deterministic prefetch.
    return [center] + sorted(result - {center}, key=lambda t: (
        abs(t.row - center.row), abs(wrap(t.center[0] - lon)), t.row, t.col))
