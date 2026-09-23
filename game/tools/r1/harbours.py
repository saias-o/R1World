"""Harbours: the sea, what is built into it, and the boats moored there.

Until this module a coastal tile had no sea. OSM does not draw the sea as a
polygon: it draws the coastline, a line with the land on its left, and that
line was downloaded with every tile (`way[natural]`) and never read. So the
sea stopped wherever a tile was not entirely ocean, and a harbour was a grassy
field at sea level.

**The sea is rebuilt from the coastline** (`sea_geometry`). The tile's box is
split by every coastline way that crosses it, and each piece is decided by
which side of the line it lies on — land left, sea right, the convention OSM
has always held. A piece no coastline touches is decided by the measured
ground (sea is at 0 m in Copernicus). Cells under the rebuilt sea, and the
mapped water at sea level beside them — a harbour basin, a dock, an estuary —
are sea-level water: the terrain under them is sunk out of sight and the
engine's animated water covers them, as it covers the open ocean.

**What is built into it is what OSM mapped** (rule 4): piers are decks on
piles along their surveyed line, at their tagged width; breakwaters and
groynes are rock mounds; quays are walls; lighthouses stand on their node,
banded in the colours their seamark tags give, plain white with a red lantern
where none are tagged.

**The boats are inferred, and say so.** Nobody maps a moored boat, so the
fleet is drawn from what the harbour is: a marina moors sailing and motor
yachts, a fishing harbour fishing boats — with rowing boats and pirogues in the
hot countries — a port container ships and tugs along its long quays, and the
canals of the Low Countries their houseboats. Every boat is a scene node, never
tile geometry (CLAUDE.md, rule 5), and every one is listed in the tile's
manifest with its handling, because every one can be boarded and sailed.
"""

from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .runtime_packages import ensure_dependencies
ensure_dependencies()
from shapely.geometry import LineString, Point, Polygon, box  # noqa: E402
from shapely.ops import split, unary_union  # noqa: E402
from shapely.prepared import prep  # noqa: E402

from . import atlas, surfaces
from .atlas import seeded
from .mesh import Material, Mesh, MeshPart
from .polygons import triangulate
from .streets import length_tag

# Mapped water whose ground is at or under this is sea-level water when the
# tile touches the sea: a dock, a harbour basin, an estuary.
SEA_LEVEL_WATER_MAX = 2.5
# What the terrain under the sea is sunk to, so the animated surface at 0 m
# covers it with room for its waves.
SEA_FLOOR = -4.0
# Land beside a sea surface is kept at least this high: behind a dike, a
# polder under sea level would otherwise be drawn flooded (the 90 m DEM does
# not see dikes). A rendering guard, and the one place this module lifts a
# measured height.
LAND_ABOVE_SEA = 0.25

# Engine materials for the painted works. Albedos (rule 2).
PAINT = {
    "white": (0.72, 0.72, 0.70), "red": (0.40, 0.05, 0.04), "black": (0.04, 0.04, 0.045),
    "green": (0.05, 0.24, 0.08), "yellow": (0.60, 0.48, 0.08), "grey": (0.30, 0.30, 0.30),
    "blue": (0.05, 0.10, 0.35),
}

SALT_BOAT = 0x0B0A7
SALT_SIDE = 0x51DE
SALT_YARD = 0x7A2D


# ── the fleet ───────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class BoatKind:
    """A hull the harbours can moor, and how it handles once boarded.

    Lengths are real ones — a speedboat 7 m, a container feeder 150-190 m — and
    the kit model is scaled to them, never the other way round. The kit's
    cargo ships are drawn stubby (2.7 lengths to the beam) where a real one is
    six or seven, so those carry the beam they should have (`length_ratio`).
    Handling: top speed in m/s, acceleration in m/s², turn rate in °/s at
    steerage way.
    """

    name: str
    model: str
    kind: str
    length: float
    top: float
    accel: float
    turn: float
    length_ratio: float | None = None
    draft: float = 0.25   # fraction of the scaled height below the waterline


BOATS = {b.name: b for b in (
    BoatKind("speed_a", "boat_speed_a", "motor", 7.5, 17.0, 3.0, 38.0),
    BoatKind("speed_c", "boat_speed_c", "motor", 7.0, 17.0, 3.0, 40.0),
    BoatKind("speed_e", "boat_speed_e", "motor", 6.0, 15.0, 3.2, 42.0),
    BoatKind("speed_g", "boat_speed_g", "motor", 8.5, 18.0, 2.8, 36.0),
    BoatKind("speed_j", "boat_speed_j", "motor", 9.5, 19.0, 2.6, 32.0),
    BoatKind("sail_a", "boat_sail_a", "sail", 10.0, 4.5, 0.7, 22.0, draft=0.12),
    BoatKind("sail_b", "boat_sail_b", "sail", 11.0, 4.5, 0.7, 20.0, draft=0.12),
    BoatKind("fishing", "boat_fishing_small", "fishing", 11.0, 5.0, 0.8, 22.0),
    # The kit's rowing boats are measured with their oars out; the hull is
    # about a third as wide as it is long.
    BoatKind("row_large", "boat_row_large", "rowing", 5.0, 1.8, 0.6, 30.0, 3.0, draft=0.35),
    BoatKind("row_small", "boat_row_small", "rowing", 3.8, 1.6, 0.6, 34.0, 3.0, draft=0.35),
    BoatKind("tug", "boat_tug_a", "tug", 28.0, 6.0, 0.6, 22.0),
    BoatKind("cargo_a", "ship_cargo_a", "cargo", 190.0, 9.0, 0.10, 3.0, 6.5),
    BoatKind("cargo_b", "ship_cargo_b", "cargo", 170.0, 9.0, 0.11, 3.2, 6.5),
    # The kit's third freighter has a bare deck: loaded with the yard's boxes
    # (DECK_LOADS) it is the container ship, the commonest hull in a port.
    BoatKind("container_a", "ship_cargo_c", "cargo", 180.0, 9.0, 0.11, 3.2, 6.5),
    BoatKind("container_b", "ship_cargo_c", "cargo", 150.0, 9.0, 0.12, 3.5, 6.5),
    BoatKind("ferry", "ship_ocean_liner", "liner", 170.0, 11.0, 0.15, 3.5, 7.0),
    BoatKind("house_a", "boat_house_a", "houseboat", 16.0, 2.5, 0.4, 12.0, 4.2),
    BoatKind("house_b", "boat_house_b", "houseboat", 19.0, 2.5, 0.4, 12.0, 4.2),
    BoatKind("house_c", "boat_house_c", "houseboat", 18.0, 2.5, 0.4, 12.0, 4.2),
    BoatKind("house_d", "boat_house_d", "houseboat", 16.0, 2.5, 0.4, 12.0, 4.2),
)}
BY_KIND: dict[str, tuple[BoatKind, ...]] = {}
for _b in BOATS.values():
    BY_KIND[_b.kind] = BY_KIND.get(_b.kind, ()) + (_b,)

CONTAINER_MODELS = ("cargo_container_a", "cargo_container_b", "cargo_container_c")
CONTAINER_SIZE = (2.44, 2.59, 12.19)   # a forty-foot box, width, height, length

# Where a hull carries boxes, in its model's own units (bow +Z): the deck's
# span along the hull, its half-width and its height. Measured on the mesh:
# ship-cargo-c's hatches run from its stern to the foot of its bridge.
DECK_LOADS = {"ship_cargo_c": (-4.9, -0.5, 1.5, 1.58)}
SHIP_CONTAINER_BUDGET = 80

# What a harbour moors, by what it is; the weights are shares of the berths.
_MIX = {
    "marina":  {"sail": 0.50, "motor": 0.45, "fishing": 0.05},
    "fishing": {"fishing": 0.55, "motor": 0.20, "rowing": 0.15, "sail": 0.10},
    "port":    {"tug": 0.25, "motor": 0.15, "fishing": 0.10, "cargo": 0.50},
    "ferry":   {"motor": 0.45, "sail": 0.25, "fishing": 0.20, "liner": 0.10},
    "canal":   {"motor": 0.60, "rowing": 0.40},
    "shore":   {"motor": 0.40, "rowing": 0.35, "sail": 0.25},
}
# How many of a line's berths are taken.
_FILL = {"marina": 0.85, "fishing": 0.60, "port": 0.70, "ferry": 0.55, "canal": 0.45,
         "shore": 0.30}
TILE_BOAT_BUDGET = 70
TILE_SHIP_BUDGET = 4
TILE_CONTAINER_BUDGET = 60


def mix_for(context: str, climate: str, profile) -> dict[str, float]:
    """The berth shares of a harbour, adjusted for the country it is in."""
    mix = dict(_MIX[context])
    # The hot countries' small harbours are rowing boats and pirogues far more
    # than yachts; the canals of the Low Countries are lined with houseboats.
    if climate in ("tropical", "arid") and context in ("fishing", "shore"):
        mix["rowing"] = mix.get("rowing", 0.0) + 0.25
        mix["sail"] = mix.get("sail", 0.0) * 0.4
    if context == "canal" and profile in (atlas.AMSTERDAM, atlas.LOW_COUNTRIES):
        mix["houseboat"] = 1.2
    total = sum(mix.values())
    return {k: v / total for k, v in mix.items() if v > 0}


# ── the sea ─────────────────────────────────────────────────────────────────

def sea_geometry(coastlines, bounds, elevation_at):
    """(the sea inside the tile in lon/lat, or None; how it was decided)."""
    tile = box(bounds.west, bounds.south, bounds.east, bounds.north)
    lines = [LineString(way.points) for way in coastlines if len(way.points) >= 2]
    lines = [line for line in lines if line.intersects(tile)]
    if not lines:
        return None, "no coastline"
    try:
        pieces = [g for g in split(tile, unary_union(lines)).geoms if g.area > 0]
    except (ValueError, TypeError) as error:   # degenerate splitter geometry
        return None, f"unresolved: {error}"
    votes = [[0, 0] for _ in pieces]
    lat = (bounds.south + bounds.north) / 2.0
    k = math.cos(math.radians(lat))
    eps = 2e-6
    for way in coastlines:
        for (x0, y0), (x1, y1) in zip(way.points, way.points[1:]):
            mid = ((x0 + x1) / 2.0, (y0 + y1) / 2.0)
            if not (bounds.west <= mid[0] <= bounds.east and bounds.south <= mid[1] <= bounds.north):
                continue
            east, north = (x1 - x0) * k, y1 - y0
            length = math.hypot(east, north)
            if length < 1e-12:
                continue
            # Right of the direction of travel is the sea.
            rx, ry = north / length, -east / length
            for side, sign in ((0, 1.0), (1, -1.0)):
                probe = Point(mid[0] + sign * rx * eps / k, mid[1] + sign * ry * eps)
                for i, piece in enumerate(pieces):
                    if piece.contains(probe):
                        votes[i][side] += 1
                        break
    sea = []
    for piece, (wet, dry) in zip(pieces, votes):
        if wet or dry:
            if wet > dry:
                sea.append(piece)
        else:
            p = piece.representative_point()
            if elevation_at(p.x, p.y) <= 0.5:
                sea.append(piece)
    if not sea:
        return None, "coastline, no sea in tile"
    return unary_union(sea), "coastline"


_TIDAL_WATER = ("harbour", "dock", "lagoon", "bay", "strait", "fjord", "sound")


def tidal_water(landcover, maritime):
    """Mapped water that is the sea's, though no coastline crosses the tile.

    An estuary, a port's docks and a lagoon are cut off from the coastline by
    OSM itself, which closes the coast across a river mouth. What OSM says
    about them decides, not the elevation model, which reads a Rotterdam basin
    at 5 m: `tidal=yes`, a harbour/dock/lagoon water, or water lying inside a
    mapped harbour or port (lon, lat; None if there is none).
    """
    harbour_areas, water = [], []
    for way in list(landcover) + list(maritime):
        t = way.tags
        if not (len(way.points) >= 4 and way.points[0] == way.points[-1]):
            continue
        try:
            poly = Polygon(way.points).buffer(0)
        except ValueError:
            continue
        if poly.is_empty:
            continue
        if (t.get("leisure") == "marina" or t.get("landuse") in ("port", "harbour")
                or t.get("industrial") == "port" or "harbour" in t):
            harbour_areas.append(poly)
        if t.get("natural") == "water" or t.get("water") or t.get("waterway") == "riverbank":
            water.append((t, poly))
    ports = unary_union(harbour_areas) if harbour_areas else None
    keep = [poly for t, poly in water
            if t.get("tidal") == "yes" or t.get("water") in _TIDAL_WATER
            or (ports is not None and ports.intersects(poly.representative_point()))]
    return unary_union(keep) if keep else None


class Cells:
    """The tile's 40x40 cells: 0 land, 1 inland water, 2 sea-level water.

    The same grid the terrain is partitioned on and the game walks and sails
    on (the manifest's `water` rows), so the picture, the collision and the
    boats cannot disagree about where the shore is.
    """

    def __init__(self, bounds, size, sea, landcover, elevation_at, tidal=None):
        self.bounds, self.size = bounds, size
        prepared = prep(sea) if sea is not None else None
        tidal = prep(tidal) if tidal is not None else None
        self.codes = []
        for row in range(size):
            lat = bounds.south + (bounds.north - bounds.south) * (row + 0.5) / size
            line = []
            for col in range(size):
                lon = bounds.west + (bounds.east - bounds.west) * (col + 0.5) / size
                if prepared is not None and prepared.contains(Point(lon, lat)):
                    line.append(2)
                elif landcover.at(lon, lat) == "water":
                    level = elevation_at(lon, lat)
                    tide = tidal is not None and tidal.contains(Point(lon, lat))
                    line.append(2 if tide or (prepared is not None and level <= SEA_LEVEL_WATER_MAX) else 1)
                else:
                    line.append(0)
            self.codes.append(line)
        self.has_sea = any(2 in line for line in self.codes)
        self._sea = prepared

    def sea_at(self, lon: float, lat: float) -> bool:
        """Is this point under the rebuilt sea (a triangle-centre question)?"""
        return self._sea is not None and self._sea.contains(Point(lon, lat))

    def at(self, lon: float, lat: float) -> int:
        b = self.bounds
        col = int((lon - b.west) / (b.east - b.west) * self.size)
        row = int((lat - b.south) / (b.north - b.south) * self.size)
        if not (0 <= col < self.size and 0 <= row < self.size):
            return -1
        return self.codes[row][col]

    def rows(self) -> list[str]:
        return ["".join(str(c) for c in line) for line in self.codes]

    def adjust(self, row: int, col: int, height: float) -> float:
        """The height of terrain vertex (row, col) with the sea taken into account."""
        if not self.has_sea:
            return height
        around = [self.codes[r][c]
                  for r in (row - 1, row) for c in (col - 1, col)
                  if 0 <= r < self.size and 0 <= c < self.size]
        if around and all(code == 2 for code in around):
            return min(height, SEA_FLOOR)
        if 2 not in around and 0 in around:
            return max(height, LAND_ABOVE_SEA)
        return height


# ── geometry helpers ────────────────────────────────────────────────────────

def _sea_y(anchor, lon, lat) -> float:
    return anchor.geodetic_to_engine(lon, lat, 0.0)[1]


def _subdivide(points, step):
    out = [points[0]]
    for a, b in zip(points, points[1:]):
        n = max(1, math.ceil(math.dist(a, b) / step))
        out += [(a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n) for i in range(1, n + 1)]
    return out


def _box(mesh: Mesh, cx, cz, y0, y1, half, yaw=0.0):
    c, s = math.cos(yaw), math.sin(yaw)
    corners = [(cx + c * dx - s * dz, cz + s * dx + c * dz)
               for dx, dz in ((-half, -half), (half, -half), (half, half), (-half, half))]
    for (x0, z0), (x1, z1) in zip(corners, corners[1:] + corners[:1]):
        mesh.add_quad((x0, y0, z0), (x1, y0, z1), (x1, y1, z1), (x0, y1, z0))


def _pile(mesh: Mesh, cx, cz, y0, y1, half):
    for dx, dz in ((half, 0.0), (0.0, half)):
        mesh.add_quad((cx - dx, y0, cz - dz), (cx + dx, y0, cz + dz),
                      (cx + dx, y1, cz + dz), (cx - dx, y1, cz - dz))


def _strip(mesh: Mesh, left, right, y_of, uvs=True):
    """A ribbon between two polylines of (x, z) with heights from y_of(i)."""
    run = 0.0
    for i in range(len(left) - 1):
        a0, a1, b0, b1 = left[i], left[i + 1], right[i], right[i + 1]
        seg = math.dist(((a0[0] + b0[0]) / 2, (a0[1] + b0[1]) / 2), ((a1[0] + b1[0]) / 2, (a1[1] + b1[1]) / 2))
        w = math.dist(a0, b0)
        quad_uv = ((run, 0.0), (run + seg, 0.0), (run + seg, w), (run, w)) if uvs else None
        mesh.add_up_quad((a0[0], y_of(i), a0[1]), (a1[0], y_of(i + 1), a1[1]),
                         (b1[0], y_of(i + 1), b1[1]), (b0[0], y_of(i), b0[1]), quad_uv)
        run += seg


def _offsets(line, half):
    """Left and right edges of a polyline widened by `half` on each side."""
    left, right = [], []
    for i, (x, z) in enumerate(line):
        a = line[max(0, i - 1)]
        b = line[min(len(line) - 1, i + 1)]
        dx, dz = b[0] - a[0], b[1] - a[1]
        length = math.hypot(dx, dz) or 1.0
        nx, nz = -dz / length, dx / length
        left.append((x + nx * half, z + nz * half))
        right.append((x - nx * half, z - nz * half))
    return left, right


# ── the works ───────────────────────────────────────────────────────────────

@dataclass
class HarbourStats:
    coastline: str = "no coastline"
    sea_cells: int = 0
    piers: int = 0
    piles_dropped: bool = False
    breakwaters: int = 0
    quays: int = 0
    lighthouses: int = 0
    boats: dict = field(default_factory=dict)
    containers: int = 0
    berths_refused: int = 0

    def as_json(self) -> dict:
        return {"coastline": self.coastline, "seaCells": self.sea_cells, "piers": self.piers,
                "breakwaters": self.breakwaters, "quays": self.quays,
                "lighthouses": self.lighthouses, "boats": self.boats,
                "containers": self.containers, "berthsRefused": self.berths_refused,
                "pilesDropped": self.piles_dropped,
                "boatsAreInferred": True}


def _engine_line(anchor, way):
    return [(p[0], p[2]) for p in (anchor.geodetic_to_engine(lon, lat, 0.0) for lon, lat in way.points)]


def _engine_points(anchor, points):
    return [(p[0], p[2]) for p in (anchor.geodetic_to_engine(lon, lat, 0.0) for lon, lat in points)]


def _clipped(way, bounds):
    """The parts of a way inside the tile, as (points, closed) pairs.

    Overpass returns every way whole, and a pier or a coastline that runs on
    into the next tile would otherwise be built in both, and built wrong in
    this one: outside its box the ground is sampled at the box's edge, which
    once put a pier fifty metres up a hillside.
    """
    tile = box(bounds.west, bounds.south, bounds.east, bounds.north)
    closed = len(way.points) >= 4 and way.points[0] == way.points[-1]
    try:
        shape = Polygon(way.points).buffer(0) if closed else LineString(way.points)
        cut = shape.intersection(tile)
    except ValueError:
        return []
    out = []
    for g in getattr(cut, "geoms", [cut]):
        if g.is_empty:
            continue
        if g.geom_type == "Polygon":
            out.append((list(g.exterior.coords), True))
        elif g.geom_type == "LineString" and len(g.coords) >= 2:
            out.append((list(g.coords), False))
    return out


def build_works(maritime, features, ground, anchor, cells: Cells, profile, stats: HarbourStats,
                piles: bool = True):
    """Piers, breakwaters, groynes, quays and lighthouses; (parts, decks).

    Heights come from two surfaces: the ground (`ground`) and the water, which
    is the sea's 0 m where the tile has a sea and the ground itself where it
    has only a lake. A pier deck is the higher of the ground and the water plus
    its freeboard, so it starts on the quay and runs out level over the water.
    """
    deck, riprap, quay, paint = Mesh(), Mesh(uv_mode="slope"), Mesh(uv_mode="slope"), {}
    decks = []

    def surfaces_at(x, z):
        lon, lat, _ = anchor.engine_to_geodetic(x, 0.0, z)
        g = ground(lon, lat)[1]
        return g, (_sea_y(anchor, lon, lat) if cells.has_sea else g)

    pieces = [(way, points, closed) for way in maritime
              for points, closed in _clipped(way, cells.bounds)]
    for way, points, closed in pieces:
        kind = way.tags.get("man_made")
        line = _engine_points(anchor, points)
        if kind == "pier":
            stats.piers += 1
            floating = way.tags.get("floating") == "yes"
            freeboard = 0.45 if floating else 1.3
            if closed:
                ring = line[:-1]
                y = max(max(g, w + freeboard) for g, w in (surfaces_at(x, z) for x, z in ring))
                for a, b, c in triangulate(ring):
                    pa, pb, pc = ring[a], ring[b], ring[c]
                    deck.add_up_triangle((pa[0], y, pa[1]), (pb[0], y, pb[1]), (pc[0], y, pc[1]),
                                         ((pa[0], -pa[1]), (pb[0], -pb[1]), (pc[0], -pc[1])))
                for p0, p1 in zip(ring, ring[1:] + ring[:1]):
                    deck.add_quad((p0[0], y - 2.5, p0[1]), (p1[0], y - 2.5, p1[1]),
                                  (p1[0], y, p1[1]), (p0[0], y, p0[1]))
                decks.append({"y": round(y, 3), "points": [[round(x, 3), round(z, 3)] for x, z in ring]})
                continue
            half = length_tag(way.tags.get("width"), 2.5 if floating else 3.0) / 2.0
            path = _subdivide(line, 6.0)
            levels = [surfaces_at(x, z) for x, z in path]
            heights = [max(g, w + freeboard) for g, w in levels]
            left, right = _offsets(path, half)
            _strip(deck, left, right, lambda i: heights[i])
            for side in (left, right):
                for i in range(len(side) - 1):
                    a, b = side[i], side[i + 1]
                    deck.add_quad((a[0], heights[i] - 0.35, a[1]), (b[0], heights[i + 1] - 0.35, b[1]),
                                  (b[0], heights[i + 1], b[1]), (a[0], heights[i], a[1]))
            if not floating and piles:
                # Piles every twelve metres, down past the sea floor: two crossed
                # faces each, which is all of one the eye gets between a deck and
                # the water, at half the vertices of a box.
                for i in range(0, len(levels), 2):
                    g, w = levels[i]
                    for side in (left, right):
                        _pile(deck, side[i][0], side[i][1], min(g, w) + SEA_FLOOR, heights[i] - 0.35, 0.16)
            for i in range(len(path) - 1):
                decks.append({"y": round((heights[i] + heights[i + 1]) / 2, 3),
                              "points": [[round(p[0], 3), round(p[1], 3)] for p in
                                         (left[i], left[i + 1], right[i + 1], right[i])]})
        elif kind in ("breakwater", "groyne"):
            stats.breakwaters += 1
            crest = 2.5 if kind == "breakwater" else 1.2
            top_half = length_tag(way.tags.get("width"), 4.0 if kind == "breakwater" else 2.0) / 2.0
            path = _subdivide(line[:-1] if closed else line, 8.0)
            levels = [surfaces_at(x, z) for x, z in path]
            tops = [max(g, w) + crest for g, w in levels]
            feet = [w + SEA_FLOOR for g, w in levels]
            # Rock armour at one in one and a half, from the sea floor.
            base_half = top_half + (crest - SEA_FLOOR) * 1.5
            tl, tr = _offsets(path, top_half)
            bl, br = _offsets(path, base_half)
            _strip(riprap, tl, tr, lambda i: tops[i], uvs=False)
            for outer, inner in ((bl, tl), (tr, br)):
                for i in range(len(path) - 1):
                    o0, o1, n0, n1 = outer[i], outer[i + 1], inner[i], inner[i + 1]
                    y_out0 = feet[i] if outer is bl else tops[i]
                    y_out1 = feet[i + 1] if outer is bl else tops[i + 1]
                    y_in0 = tops[i] if outer is bl else feet[i]
                    y_in1 = tops[i + 1] if outer is bl else feet[i + 1]
                    riprap.add_up_quad((o0[0], y_out0, o0[1]), (o1[0], y_out1, o1[1]),
                                       (n1[0], y_in1, n1[1]), (n0[0], y_in0, n0[1]))
        elif kind == "quay":
            stats.quays += 1
            path = _subdivide(line, 8.0)
            levels = [surfaces_at(x, z) for x, z in path]
            for i in range(len(path) - 1):
                a, b = path[i], path[i + 1]
                (ga, wa), (gb, wb) = levels[i], levels[i + 1]
                quay.add_quad((a[0], wa + SEA_FLOOR, a[1]), (b[0], wb + SEA_FLOOR, b[1]),
                              (b[0], max(gb, wb + 0.8), b[1]), (a[0], max(ga, wa + 0.8), a[1]))

    for node in features:
        if node.tags.get("man_made") != "lighthouse":
            continue
        stats.lighthouses += 1
        _lighthouse(paint, node, ground)

    parts = []
    if deck.indices:
        parts.append(MeshPart("Harbour — piers", deck,
                              surfaces.material("Pier deck", (0.16, 0.13, 0.10), 0.85, "deck")))
    if riprap.indices:
        parts.append(MeshPart("Harbour — breakwaters", riprap,
                              surfaces.material("Rock armour", (0.24, 0.23, 0.21), 0.95, "riprap",
                                                double_sided=True)))
    if quay.indices:
        parts.append(MeshPart("Harbour — quays", quay,
                              surfaces.material("Quay wall", (0.20, 0.20, 0.19), 0.9, "quay",
                                                double_sided=True)))
    for name in sorted(paint):
        colour = PAINT.get(name, PAINT["white"]) if name != "glass" else (0.06, 0.08, 0.09)
        parts.append(MeshPart(f"Lighthouse — {name}", paint[name],
                              Material(f"Lighthouse {name}", (*colour, 1.0),
                                       0.15 if name == "glass" else 0.6)))
    return parts, decks


def _colours(tags) -> list[str]:
    raw = (tags.get("seamark:landmark:colour") or tags.get("seamark:light:colour")
           or tags.get("colour") or tags.get("building:colour") or "")
    names = [c.strip().lower() for c in raw.replace(",", ";").split(";") if c.strip()]
    return [c for c in names if c in PAINT] or ["white"]


def _lighthouse(paint: dict, node, ground):
    """A tower on its surveyed node, in the colours its tags give."""
    x, y, z = ground(node.lon, node.lat)
    height = length_tag(node.tags.get("height"), 20.0)
    height = max(8.0, min(height, 60.0))
    colours = _colours(node.tags)
    banded = len(colours) > 1
    sides, r0, r1 = 16, max(2.2, height * 0.13), max(1.5, height * 0.08)
    surveyed = length_tag(node.tags.get("r1:radius"), 0.0)
    if 1.0 <= surveyed <= 15.0:              # a footprint wins over the guess (rule 4)
        r0, r1 = surveyed, max(1.2, surveyed * 0.62)
    bands = 5 if banded else 1

    def ring(radius, h):
        return [(x + radius * math.cos(2 * math.pi * i / sides), y + h,
                 z + radius * math.sin(2 * math.pi * i / sides)) for i in range(sides)]

    for band in range(bands):
        h0, h1 = height * band / bands, height * (band + 1) / bands
        ra, rb = r0 + (r1 - r0) * band / bands, r0 + (r1 - r0) * (band + 1) / bands
        mesh = paint.setdefault(colours[band % len(colours)], Mesh())
        lo, hi = ring(ra, h0), ring(rb, h1)
        for i in range(sides):
            j = (i + 1) % sides
            mesh.add_quad(lo[j], lo[i], hi[i], hi[j])
    # Gallery, lantern and cap.
    top = paint.setdefault("black" if "black" in colours else "red", Mesh())
    gallery = ring(r1 + 0.8, height)
    for i in range(sides):
        j = (i + 1) % sides
        top.add_up_triangle((x, y + height + 0.3, z), gallery[i], gallery[j])
    glass = paint.setdefault("glass", Mesh())
    lo, hi = ring(r1 * 0.75, height + 0.3), ring(r1 * 0.75, height + 2.8)
    for i in range(sides):
        j = (i + 1) % sides
        glass.add_quad(lo[j], lo[i], hi[i], hi[j])
    cap = ring(r1 * 0.9, height + 2.8)
    for i in range(sides):
        j = (i + 1) % sides
        top.add_up_triangle((x, y + height + 4.2, z), cap[i], cap[j])


def lighthouse_points(features) -> list[tuple[float, float]]:
    return [(n.lon, n.lat) for n in features if n.tags.get("man_made") == "lighthouse"]


def is_lighthouse(tags) -> bool:
    return tags.get("man_made") == "lighthouse" or tags.get("building") == "lighthouse"


def traced_lighthouse(way):
    """A lighthouse OSM traced as a footprint, as the node the tower stands on.

    Cap Ferret's is mapped this way: without it the tower is extruded as a
    plain 52 m grey block. The footprint's centre and radius are surveyed, so
    they are kept (`r1:radius`, metres) rather than guessed from the height.
    """
    from .sources import OsmNode
    ring = list(way.points[:-1] if way.points[0] == way.points[-1] else way.points)
    lon = sum(p[0] for p in ring) / len(ring)
    lat = sum(p[1] for p in ring) / len(ring)
    k = math.cos(math.radians(lat))
    radius = max(math.hypot((p[0] - lon) * k, p[1] - lat) for p in ring) * 111_320.0
    tags = dict(way.tags, man_made="lighthouse")
    tags["r1:radius"] = f"{radius:.2f}"
    return OsmNode(way.osm_id, lon, lat, tags)


# ── contexts ────────────────────────────────────────────────────────────────

_PORT_WORDS = ("cargo", "container", "industrial", "bulk", "roro", "tanker")
_FERRY_WORDS = ("ferry", "passenger")


def _areas(maritime, landcover, anchor):
    """Harbour areas in engine (x, z), each with the kind of harbour it is."""
    out = []
    for way in list(maritime) + list(landcover):
        t = way.tags
        if not (len(way.points) >= 4 and way.points[0] == way.points[-1]):
            continue
        context = None
        if t.get("leisure") == "marina":
            context = "marina"
        elif t.get("landuse") == "port" or t.get("industrial") == "port" or (
                "harbour" in t and any(w in (t.get("harbour:category", "") + t.get("harbour", ""))
                                       for w in _PORT_WORDS)):
            context = "port"
        elif "harbour" in t or t.get("landuse") == "harbour":
            context = "fishing"
        if context is None:
            continue
        ring = _engine_line(anchor, way)
        try:
            poly = Polygon(ring).buffer(0)
        except ValueError:
            continue
        if not poly.is_empty:
            out.append((context, poly))
    return out


def _points(features, anchor):
    out = []
    for node in features:
        t = node.tags
        category = (t.get("seamark:harbour:category", "") + " " + t.get("harbour:category", "")).lower()
        if t.get("leisure") == "marina" or "marina" in category:
            context = "marina"
        elif any(w in category for w in _PORT_WORDS):
            context = "port"
        elif any(w in category for w in _FERRY_WORDS):
            context = "ferry"
        elif "harbour" in t or t.get("seamark:type") == "harbour":
            context = "fishing"
        else:
            continue
        x, _, z = anchor.geodetic_to_engine(node.lon, node.lat, 0.0)
        out.append((context, Point(x, z)))
    return out


def _context_of(x, z, areas, points, default):
    """The kind of harbour a berth line belongs to: the mapped area it touches,
    else its own default (a pier cluster is a marina), else the nearest
    harbour point within 300 m."""
    here = Point(x, z)
    for context, poly in areas:
        if poly.distance(here) < 25.0:
            return context
    if default in ("marina", "canal"):
        return default
    for context, point in points:
        if point.distance(here) < 300.0:
            return context
    return default


# ── mooring ─────────────────────────────────────────────────────────────────

def _model_extent(game_root: Path, model: str, cache={}):
    if model not in cache:
        payload = (game_root / model).read_bytes()
        length, = struct.unpack_from("<I", payload, 12)
        doc = json.loads(payload[20:20 + length].decode("utf-8"))
        low, high = [math.inf] * 3, [-math.inf] * 3
        for mesh in doc.get("meshes", []):
            for primitive in mesh.get("primitives", []):
                accessor = doc["accessors"][primitive["attributes"]["POSITION"]]
                low = [min(a, b) for a, b in zip(low, accessor["min"])]
                high = [max(a, b) for a, b in zip(high, accessor["max"])]
        cache[model] = (low, high)
    return cache[model]


def _segment_distance(a0, a1, b0, b1):
    def point_segment(p, s0, s1):
        dx, dz = s1[0] - s0[0], s1[1] - s0[1]
        length = dx * dx + dz * dz
        t = 0.0 if length < 1e-12 else max(0.0, min(1.0, ((p[0] - s0[0]) * dx + (p[1] - s0[1]) * dz) / length))
        return math.dist(p, (s0[0] + dx * t, s0[1] + dz * t))
    return min(point_segment(a0, b0, b1), point_segment(a1, b0, b1),
               point_segment(b0, a0, a1), point_segment(b1, a0, a1))


@dataclass
class Berth:
    kind: BoatKind
    x: float
    z: float
    axis: tuple[float, float]    # unit direction of the hull in (x, z)
    beam: float
    heading: float = 0.0


def _pick(mix, rng):
    cursor = rng.random()
    for name, share in sorted(mix.items()):
        cursor -= share
        if cursor <= 0:
            return name
    return sorted(mix)[-1]


def plan_boats(maritime, coastlines, landcover, features, anchor, cells: Cells, profile, climate,
               game_root: Path, stats: HarbourStats):
    """Berths along piers, quays, harbour shores and canal banks; the boats in them."""
    areas = _areas(maritime, landcover, anchor)
    points = _points(features, anchor)
    lines = []   # (osm id, polyline in (x, z), clearance from the line, default context)
    # A cluster of piers is a marina even where the marina itself is mapped
    # as a relation this query does not fetch. An inference, and a safe one:
    # nothing else builds six piers side by side.
    piers = [w for w in maritime if w.tags.get("man_made") == "pier"]
    pier_default = "marina" if len(piers) >= 6 else "shore"
    bounds = cells.bounds

    def add(way, clearance, default):
        for points, _closed in _clipped(way, bounds):
            line = _engine_points(anchor, points)
            # A quay OSM drew with a node every few metres is still one
            # straight wall: merge the collinear runs so a 190 m ship can find
            # the 190 m of it (a 1.5 m tolerance moves no berth off the water).
            if len(line) > 2:
                line = list(LineString(line).simplify(1.5).coords)
            lines.append((way.osm_id, line, clearance, default))

    for way in maritime:
        kind = way.tags.get("man_made")
        if kind == "pier":
            add(way, length_tag(way.tags.get("width"), 3.0) / 2.0, pier_default)
        elif kind == "quay":
            add(way, 0.0, None)
    if areas or points:
        for way in coastlines:
            add(way, 0.0, None)
    for way in landcover:
        t = way.tags
        if t.get("water") == "canal" or t.get("waterway") == "canal":
            add(way, 0.0, "canal")
        elif areas and (t.get("natural") == "water" or t.get("water")):
            # A dock's own shore: most port basins have no quay way, only the
            # water polygon. It takes its context from the area it lies in.
            add(way, 0.0, None)

    def water(x, z, need_sea=False):
        lon, lat, _ = anchor.engine_to_geodetic(x, 0.0, z)
        code = cells.at(lon, lat)
        return code == 2 if need_sea else code > 0

    placed: list[Berth] = []
    ships = 0
    half_cell = (bounds.north - bounds.south) * 111_320.0 / cells.size / 2.0
    for osm_id, line, clearance, default in lines:
        rng = seeded(osm_id, SALT_BOAT)
        for i in range(len(line) - 1):
            a, b = line[i], line[i + 1]
            seg = math.dist(a, b)
            if seg < 6.0:
                continue
            ux, uz = (b[0] - a[0]) / seg, (b[1] - a[1]) / seg
            mid = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            context = _context_of(mid[0], mid[1], areas, points, default)
            if context is None:
                continue
            mix = mix_for(context, climate, profile)
            fill = _FILL[context]
            for sign in (1.0, -1.0):
                nx, nz = -uz * sign, ux * sign
                t = 2.0
                while t < seg - 2.0 and len(placed) < TILE_BOAT_BUDGET:
                    name = _pick(mix, rng)
                    if name in ("cargo", "liner") and (seg < 130.0 or ships >= TILE_SHIP_BUDGET):
                        name = "tug" if "tug" in mix else "motor"
                    choices = BY_KIND.get(name) or BY_KIND["motor"]
                    kind = choices[rng.randrange(len(choices))]
                    low, high = _model_extent(game_root, "assets/models/external/kenney_boats/" + kind.model + ".glb")
                    model_length = max(1e-3, high[2] - low[2])
                    beam = (kind.length / kind.length_ratio if kind.length_ratio
                            else (high[0] - low[0]) * kind.length / model_length)
                    if t + kind.length > seg - 1.0:
                        t += 3.0
                        continue
                    if rng.random() > fill:
                        t += kind.length * 0.7
                        continue
                    along = t + kind.length / 2.0
                    offset = clearance + beam / 2.0 + 0.7
                    cx = a[0] + ux * along + nx * offset
                    cz = a[1] + uz * along + nz * offset
                    # The flank against the quay lies inside the shore's own
                    # cell, whose centre may be on land: it is sampled at least
                    # half a cell out from the line, the open flank where it is.
                    inner = min(beam / 2, max(clearance + 0.7, half_cell) - offset)
                    flanks = (inner, beam / 2)
                    ends = [(cx + ux * kind.length / 2 * f + nx * g,
                             cz + uz * kind.length / 2 * f + nz * g)
                            for f in (-1, 0, 1) for g in flanks]
                    big = kind.kind in ("cargo", "liner")
                    if not all(water(x, z, need_sea=big) for x, z in ends):
                        stats.berths_refused += 1
                        t += 3.0
                        continue
                    a0 = (cx - ux * kind.length / 2, cz - uz * kind.length / 2)
                    a1 = (cx + ux * kind.length / 2, cz + uz * kind.length / 2)
                    clash = any(_segment_distance(a0, a1,
                                                  (o.x - o.axis[0] * o.kind.length / 2, o.z - o.axis[1] * o.kind.length / 2),
                                                  (o.x + o.axis[0] * o.kind.length / 2, o.z + o.axis[1] * o.kind.length / 2))
                                < (beam + o.beam) / 2 + 0.5 for o in placed)
                    if clash:
                        t += 3.0
                        continue
                    flip = rng.random() < 0.5
                    axis = (-ux, -uz) if flip else (ux, uz)
                    placed.append(Berth(kind, cx, cz, axis, beam))
                    ships += big
                    stats.boats[kind.kind] = stats.boats.get(kind.kind, 0) + 1
                    t += kind.length + (1.5 if context == "marina" else 3.0)
    return placed


def hull_beam(kind: BoatKind, game_root: Path) -> float:
    """The beam a hull is moored and sailed with: measured on the model, or
    its length over `length_ratio` where the model's width lies (oars)."""
    if kind.length_ratio:
        return kind.length / kind.length_ratio
    low, high = _model_extent(game_root, "assets/models/external/kenney_boats/" + kind.model + ".glb")
    return (high[0] - low[0]) * kind.length / max(1e-3, high[2] - low[2])


def boat_nodes(berths, anchor, game_root: Path):
    """Scene nodes for the berths, and the manifest entries the game boards them by."""
    nodes, manifest = [], []
    for index, berth in enumerate(berths):
        kind = berth.kind
        model = "assets/models/external/kenney_boats/" + kind.model + ".glb"
        low, high = _model_extent(game_root, model)
        s = kind.length / max(1e-3, high[2] - low[2])
        sx = (berth.beam / max(1e-3, high[0] - low[0])) if kind.length_ratio else s
        height = (high[1] - low[1]) * s
        lon, lat, _ = anchor.engine_to_geodetic(berth.x, 0.0, berth.z)
        y = _sea_y(anchor, lon, lat)
        # Compass heading of the bow: the hull's axis is (east, -north).
        heading = math.degrees(math.atan2(berth.axis[0], -berth.axis[1])) % 360.0
        half = math.radians(-heading) / 2.0
        name = f"Boat {index}"
        nodes.append({
            "type": "Node", "name": name, "enabled": True, "groups": ["boat"],
            "transform": {"position": [round(berth.x, 3), round(y, 3), round(berth.z, 3)],
                          "rotation": [0.0, math.sin(half), 0.0, math.cos(half)],
                          "scale": [1.0, 1.0, 1.0]},
            "children": [{
                "type": "Node", "name": "Hull", "enabled": True,
                # The kit draws its bows towards +Z, the node's forward is -Z.
                "transform": {"position": [0.0, round(-low[1] * s - height * kind.draft, 3), 0.0],
                              "rotation": [0.0, 1.0, 0.0, 0.0],
                              "scale": [round(sx, 5), round(s, 5), round(s, 5)]},
                "importedFrom": model,
            }, {
                # Two children keep the loader from flattening the boat into
                # its mesh (world.cpp plantNode): the helm is what the game
                # moves, and its transform must stay the boat's own.
                "type": "Node", "name": "Helm", "enabled": True,
            }],
        })
        if kind.model in DECK_LOADS:
            hull_y = -low[1] * s - height * kind.draft
            nodes[-1]["children"].extend(
                _deck_load(DECK_LOADS[kind.model], hull_y, sx, s, seeded(index + 1, SALT_YARD ^ SALT_BOAT),
                           game_root))
        manifest.append({"name": name, "kind": kind.kind, "model": kind.name,
                         "x": round(berth.x, 3), "y": round(y, 3), "z": round(berth.z, 3),
                         "heading": round(heading, 3), "length": kind.length,
                         "beam": round(berth.beam, 3), "top": kind.top, "accel": kind.accel,
                         "turn": kind.turn})
    return nodes, manifest


def _deck_load(deck, hull_y, sx, s, rng, game_root):
    """Stacks of forty-foot boxes on a hull's deck, as children of the boat.

    Children, so they sail and sway with it once someone takes the helm. The
    hull node is turned half a revolution, so model (x, z) is (-x, -z) here.
    """
    z0, z1, half_width, deck_y = deck
    w, h, l = CONTAINER_SIZE
    bays = int((z1 - z0) * s // (l + 0.3))
    rows = int(2 * half_width * sx // (w + 0.05))
    out = []
    for bay in range(bays):
        mz = z0 + (z1 - z0) * (bay + 0.5) / bays
        for row in range(rows):
            mx = -half_width + 2 * half_width * (row + 0.5) / rows
            for tier in range(1 + rng.randrange(3)):
                if len(out) >= SHIP_CONTAINER_BUDGET:
                    return out
                model = "assets/models/external/kenney_boats/" + CONTAINER_MODELS[rng.randrange(3)] + ".glb"
                low, high = _model_extent(game_root, model)
                cy = hull_y + deck_y * s + tier * h
                out.append({
                    "type": "Node", "name": f"Box {len(out)}", "enabled": True,
                    "transform": {"position": [round(-mx * sx, 3), round(cy - low[1] * h / (high[1] - low[1]), 3),
                                               round(-mz * s, 3)],
                                  "rotation": [0.0, 0.0, 0.0, 1.0],
                                  "scale": [round(w / (high[0] - low[0]), 5), round(h / (high[1] - low[1]), 5),
                                            round(l / (high[2] - low[2]), 5)]},
                    "importedFrom": model,
                })
    return out


# ── container yards ─────────────────────────────────────────────────────────

def plan_containers(maritime, landcover, anchor, cells: Cells, footprints, ground, game_root, stats):
    """Stacks of containers on the port land beside the quays."""
    yards = [poly for context, poly in _areas(maritime, landcover, anchor) if context == "port"]
    if not yards:
        return []
    blocked = unary_union([Polygon(r).buffer(2.0) for r in footprints if len(r) >= 3]) if footprints else None
    nodes = []
    w, h, l = CONTAINER_SIZE
    for yi, yard in enumerate(yards):
        rng = seeded(yi + 1, SALT_YARD)
        # Rows along the yard's longest side.
        ring = list(yard.exterior.coords)
        edge = max(zip(ring, ring[1:]), key=lambda e: math.dist(e[0], e[1]))
        ux, uz = edge[1][0] - edge[0][0], edge[1][1] - edge[0][1]
        n = math.hypot(ux, uz) or 1.0
        ux, uz = ux / n, uz / n
        yaw = math.atan2(ux, uz)
        minx, minz, maxx, maxz = yard.bounds
        step_a, step_b = l + 1.5, w + 0.4
        for i in range(int((maxx - minx) / step_b) + 1):
            for j in range(int((maxz - minz) / step_a) + 1):
                if len(nodes) >= TILE_CONTAINER_BUDGET:
                    break
                # A lattice in the rows' own frame, centred on the yard.
                ca, cb = (j - (maxz - minz) / step_a / 2) * step_a, (i - (maxx - minx) / step_b / 2) * step_b
                cx = (minx + maxx) / 2 + ux * ca - uz * cb
                cz = (minz + maxz) / 2 + uz * ca + ux * cb
                p = Point(cx, cz)
                if not yard.contains(p) or (blocked is not None and blocked.contains(p)):
                    continue
                lon, lat, _ = anchor.engine_to_geodetic(cx, 0.0, cz)
                if cells.at(lon, lat) != 0 or rng.random() > 0.55:
                    continue
                gy = ground(lon, lat)[1]
                for level in range(1 + rng.randrange(4)):
                    model = "assets/models/external/kenney_boats/" + CONTAINER_MODELS[rng.randrange(3)] + ".glb"
                    low, high = _model_extent(game_root, model)
                    s = l / max(1e-3, high[2] - low[2])
                    sx = w / max(1e-3, high[0] - low[0])
                    sy = h / max(1e-3, high[1] - low[1])
                    half = yaw / 2.0
                    nodes.append({
                        "type": "Node", "name": f"Container {len(nodes)}", "enabled": True,
                        "transform": {"position": [round(cx, 3), round(gy - low[1] * sy + level * h, 3), round(cz, 3)],
                                      "rotation": [0.0, math.sin(half), 0.0, math.cos(half)],
                                      "scale": [round(sx, 5), round(sy, 5), round(s, 5)]},
                        "importedFrom": model,
                    })
                    stats.containers += 1
    return nodes


# ── the sea surface ─────────────────────────────────────────────────────────

def sea_node(bounds, anchor, name="Sea"):
    """The animated water over one tile, and no further: a larger plane would
    flood the low ground of the next tile, and two overlapping planes at the
    same height flicker."""
    corners = [anchor.geodetic_to_engine(lon, lat, 0.0)
               for lon, lat in ((bounds.west, bounds.south), (bounds.east, bounds.north),
                                (bounds.west, bounds.north), (bounds.east, bounds.south))]
    half = max(max(abs(c[0]), abs(c[2])) for c in corners) + 1.0
    return {"type": "Water", "name": name, "size": round(half, 2), "amplitude": 0.12,
            "wavelength": 9.0, "shoreMode": 0,
            "transform": {"position": [0.0, 0.0, 0.0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}}
