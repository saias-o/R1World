"""The building chain — plan §10, "Bâtiments — le cœur du travail".

The plan puts building footprints at rank 3 of the fidelity hierarchy, heights
and gabarits at rank 5, materials at 6 and roofs at 7. Everything from rank 5
down is inferred, and the plan is explicit about what makes the difference once
you are inferring:

> Ce qui fait la différence ici : l'alignement. Des fenêtres alignées sur des
> planchers réguliers, des travées régulières, une gouttière continue et un
> faîtage droit valent plus que dix fois plus de triangles répartis au hasard.

So this module is organised around alignment rather than around detail. A wall
is divided into a whole number of bays and the openings sit on the axis of each;
floor levels are shared by every wall of a building, so a window on the north
face is at the height of the window on the west face; the ridge is straight and
the eave is continuous because both are derived from one oriented box rather
than from each edge separately.

The chain, in order, and where each step gets its answer:

  footprint      OSM, exact (rank 3) — cleaned of the duplicate and collinear
                 vertices real OSM ways carry, then rectified so right angles
                 are right, which is what an extrusion makes visible.
  height         `height` tag, else `building:levels` x the region's storey
                 height, else drawn from the region's distribution (rank 5).
  roof           `roof:shape`/`roof:height`/`roof:angle` when tagged, else drawn
                 from the region (rank 7). Pitched roofs are built over the
                 footprint's oriented box and are therefore gated on the
                 footprint actually being close to rectangular; an L-shaped
                 building gets a flat roof rather than a wrong ridge, and says
                 so in the manifest.
  facade         Synthesised (rank 11), and only within a detail radius —
                 §12.4, le détail suit la vitesse. Beyond it a building is its
                 massing and its roof, which is what carries the silhouette.
  material       Drawn from the region's palette (rank 6, "le point critique").

Every inference is recorded per building and aggregated into the manifest,
because §4 I5 requires the game to know what it does not know: a building whose
height was measured and a building whose height was guessed must not be
indistinguishable in the data, even when they look alike on screen.

**Winding.** Footprints are kept with a positive signed area in the engine's
(x, z) plane. For a ring in that orientation, the outward horizontal normal of
the edge p0->p1 is `(dz, -dx)`: take the unit square (0,0) (1,0) (1,1) (0,1),
whose interior is at z > 0 on the first edge, and the formula gives -z. Every
wall, reveal and roof band below is built from that frame rather than from a
vertex order, because a vertex order is a convention a reader has to trust and a
normal is one they can check.
"""

from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

from .atlas import RegionProfile, Swatch, seeded
from .mesh import Material, Mesh, MeshPart
from .polygons import (
    convex_hull,
    inset_polygon,
    polygon_area,
    triangulate,
)

Point = tuple[float, float]
Vec3 = tuple[float, float, float]

# Salts keeping one building's draws independent of each other (see
# `atlas.seeded`): a building whose wall colour decided its roof pitch would
# produce visible runs of identical houses.
SALT_WALL = 0x57414C4C
SALT_ROOF = 0x524F4F46
SALT_SHAPE = 0x53484150
SALT_HEIGHT = 0x48474854

# Below this a "building" is a mapping artefact — a wall fragment, a duplicated
# node, a way closed on itself — and extruding it produces a spike rather than a
# building. The threshold is deliberately under the smallest thing a person
# would call a building: a 1.5 m2 footprint is a metre and a bit square, which
# is a transformer hut or a garden shed, and those are real and mapped. Set it
# where it looks safe — 4 m2, say — and 8% of an alpine village's footprints disappear,
# which is a loss of rank-3 data (§2.1: "c'est la forme des villes") traded for
# nothing.
MIN_FOOTPRINT_AREA = 1.5

# Two vertices closer than this are the same vertex. OSM ways routinely carry
# them, and they turn into zero-area wall quads with undefined normals.
WELD_DISTANCE = 0.05

# A vertex deviating from the line through its neighbours by less than this is
# not a corner. Dropping them is what makes bay division regular: a wall split
# in two by a 3 cm jog would otherwise get two independent bay grids.
COLLINEAR_SAGITTA = 0.08

# Rectification snaps an edge to the footprint's dominant axis when it is
# already within this angle of it. Wider than this and the edge is a real
# diagonal — a corner cut, a gable end on a plan — and moving it would be
# inventing a building the survey did not find.
RECTIFY_TOLERANCE_DEG = 7.0

# A pitched roof is built over the oriented bounding box, so it is only honest
# when the footprint fills that box. Below this ratio the building is L-shaped,
# stepped or splayed, and gets a flat roof instead of a ridge running through
# thin air.
RECTANGULARITY_FOR_PITCH = 0.82

# The widest span a single ridge may cover, across the building. This is not a
# stylistic limit, it is what a ridge *is*: at the region's pitch, a roof rises
# by half the span times the tangent, so a 40 m-wide building would carry an
# 11 m roof over 8 m of wall — a barn, drawn on top of a hotel. Real buildings
# that wide have a flat roof, a low-slope one, or several ridges; the third is a
# chantier of its own (§10 speaks of a full roof generator with arêtiers and
# noues), so until it exists a building too wide for one ridge gets the flat
# roof it most likely has, and says so in the manifest.
MAX_RIDGE_SPAN = 19.0

# And whatever the span, a roof taller than the walls under it is a silhouette
# nobody in a village has. Where the pitch would break this the roof is
# flattened to it, which lowers the pitch rather than the building.
MAX_ROOF_OVER_WALL = 1.0

# Roof slab thickness. It is what turns a roof from a zero-thickness sheet into
# a roof: it gives the eave a fascia to catch light and a soffit underneath, and
# the deep shadow line along the top of the facade is most of what reads as
# "alpine" from the street.
ROOF_THICKNESS = 0.22

# Two wall segments this close and this parallel are one party wall. Neither
# side gets windows: an opening onto a neighbour's masonry is the kind of error
# that is invisible in a screenshot and obvious in motion.
PARTY_WALL_DISTANCE = 0.7
PARTY_WALL_OVERLAP = 1.2
PARTY_WALL_CELL = 8.0


# ── footprint hygiene ───────────────────────────────────────────────────────

def _weld(ring: list[Point]) -> list[Point]:
    """Drop consecutive duplicates, including across the closing edge."""
    out: list[Point] = []
    for point in ring:
        if out and math.dist(out[-1], point) < WELD_DISTANCE:
            continue
        out.append(point)
    while len(out) > 2 and math.dist(out[0], out[-1]) < WELD_DISTANCE:
        out.pop()
    return out


def _drop_collinear(ring: list[Point]) -> list[Point]:
    """Remove vertices that do not turn.

    The test is a distance, not an angle: a vertex is kept when it lies further
    than `COLLINEAR_SAGITTA` from the chord joining its neighbours. An angular
    test would keep a 2 mm jog on a 40 m wall and drop a real 1° splay on a 2 m
    one, which is backwards.
    """
    if len(ring) < 4:
        return ring
    out: list[Point] = []
    count = len(ring)
    for index in range(count):
        previous = ring[index - 1]
        current = ring[index]
        following = ring[(index + 1) % count]
        chord = math.dist(previous, following)
        if chord < 1e-9:
            continue
        area2 = abs(
            (following[0] - previous[0]) * (previous[1] - current[1])
            - (previous[0] - current[0]) * (following[1] - previous[1])
        )
        if area2 / chord > COLLINEAR_SAGITTA:
            out.append(current)
    return out if len(out) >= 3 else ring


def _dominant_axis(ring: list[Point]) -> float:
    """The angle, in [0, pi/2), of the footprint's dominant orthogonal grid.

    Every edge votes with its own length, folded modulo 90 degrees so that an
    edge and the wall perpendicular to it support the same grid. The vote is
    taken as a vector sum on the doubled-and-folded angle rather than as a mean,
    because angles near 0 and near 90 are the same grid and averaging them
    numerically would land at 45.
    """
    sin_sum = 0.0
    cos_sum = 0.0
    count = len(ring)
    for index in range(count):
        p0 = ring[index]
        p1 = ring[(index + 1) % count]
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        length = math.hypot(dx, dy)
        if length < 1e-9:
            continue
        angle = math.atan2(dy, dx) * 4.0  # x4: fold 90 deg onto a full turn
        sin_sum += math.sin(angle) * length
        cos_sum += math.cos(angle) * length
    if abs(sin_sum) < 1e-12 and abs(cos_sum) < 1e-12:
        return 0.0
    return (math.atan2(sin_sum, cos_sum) / 4.0) % (math.pi / 2.0)


def _rectify(ring: list[Point]) -> list[Point]:
    """Snap near-orthogonal edges onto the footprint's own grid.

    OSM footprints are traced by hand from imagery, so a building's right angles
    arrive as 88 and 92 degrees. That is invisible on a map and conspicuous once
    the outline is extruded three storeys and given a straight ridge: the walls
    lean, the roof does not sit square, and the eave wanders.

    Each edge within `RECTIFY_TOLERANCE_DEG` of the dominant grid is replaced by
    the line through its midpoint at the exact grid angle; the corrected corners
    are the intersections of consecutive lines. Edges further off the grid keep
    their own direction, so a genuine diagonal survives. A pair of consecutive
    lines that are nearly parallel has no usable intersection and keeps the
    original vertex, which is the only fallback needed: it degrades to "not
    rectified here" rather than to a spike.
    """
    count = len(ring)
    if count < 4:
        return ring
    grid = _dominant_axis(ring)

    lines: list[tuple[Point, tuple[float, float]]] = []  # (point on line, direction)
    for index in range(count):
        p0 = ring[index]
        p1 = ring[(index + 1) % count]
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        length = math.hypot(dx, dy)
        if length < 1e-9:
            lines.append((p0, (1.0, 0.0)))
            continue
        angle = math.atan2(dy, dx)
        # Distance to the nearest arm of the grid, in [0, 45] degrees.
        folded = (angle - grid) % (math.pi / 2.0)
        offset = folded if folded <= math.pi / 4.0 else folded - math.pi / 2.0
        midpoint = ((p0[0] + p1[0]) * 0.5, (p0[1] + p1[1]) * 0.5)
        if abs(offset) <= math.radians(RECTIFY_TOLERANCE_DEG):
            snapped = angle - offset
            lines.append((midpoint, (math.cos(snapped), math.sin(snapped))))
        else:
            lines.append((midpoint, (dx / length, dy / length)))

    out: list[Point] = []
    for index in range(count):
        # Vertex `index` is where the edge before it meets the edge after it.
        (a_point, a_dir) = lines[index - 1]
        (b_point, b_dir) = lines[index]
        denominator = a_dir[0] * b_dir[1] - a_dir[1] * b_dir[0]
        if abs(denominator) < 1e-6:
            out.append(ring[index])
            continue
        wx = b_point[0] - a_point[0]
        wy = b_point[1] - a_point[1]
        t = (wx * b_dir[1] - wy * b_dir[0]) / denominator
        candidate = (a_point[0] + a_dir[0] * t, a_point[1] + a_dir[1] * t)
        # A corner that moved further than the tolerance could plausibly explain
        # is not a corner we understood; keep the surveyed one.
        out.append(candidate if math.dist(candidate, ring[index]) < 1.5 else ring[index])
    return out


def clean_footprint(ring: list[Point]) -> list[Point] | None:
    """Weld, simplify, rectify and orient a raw OSM ring, or reject it."""
    welded = _weld(ring)
    if len(welded) < 3:
        return None
    simplified = _drop_collinear(welded)
    if len(simplified) < 3 or abs(polygon_area(simplified)) < MIN_FOOTPRINT_AREA:
        return None
    rectified = _rectify(simplified)
    if abs(polygon_area(rectified)) < MIN_FOOTPRINT_AREA:
        rectified = simplified
    # Positive area throughout the module; see the module docstring.
    if polygon_area(rectified) < 0.0:
        rectified.reverse()
    return rectified


# ── oriented bounding box ───────────────────────────────────────────────────

@dataclass(frozen=True)
class OrientedBox:
    """The footprint's own rectangle: centre, unit axes, half extents.

    `u` is the long axis. A gabled ridge runs along it, which is what makes a
    row of houses on the same street share a ridge direction without anything
    ever telling them to: they share it because their footprints do.
    """

    cx: float
    cz: float
    ux: float
    uz: float
    half_u: float
    half_v: float

    @property
    def vx(self) -> float:
        return -self.uz

    @property
    def vz(self) -> float:
        return self.ux

    def point(self, a: float, b: float) -> Point:
        """Box coordinates to world: `a` along the long axis, `b` across it."""
        return (
            self.cx + self.ux * a + self.vx * b,
            self.cz + self.uz * a + self.vz * b,
        )

    @property
    def area(self) -> float:
        return 4.0 * self.half_u * self.half_v


def oriented_box(ring: list[Point]) -> OrientedBox:
    """Minimum-area enclosing rectangle, by rotating calipers over the hull.

    The minimum-area rectangle always has a side flush with a hull edge, so
    testing every hull edge direction is exact rather than a search. For a
    footprint that is already rectangular this returns that rectangle to the
    last digit, which is the case that matters: it is what puts the ridge on the
    building's own axis instead of near it.
    """
    hull = convex_hull(ring)
    if len(hull) < 3:
        xs = [p[0] for p in ring]
        zs = [p[1] for p in ring]
        cx, cz = (min(xs) + max(xs)) * 0.5, (min(zs) + max(zs)) * 0.5
        return OrientedBox(cx, cz, 1.0, 0.0,
                           max(0.5, (max(xs) - min(xs)) * 0.5),
                           max(0.5, (max(zs) - min(zs)) * 0.5))

    best: OrientedBox | None = None
    best_area = float("inf")
    for index in range(len(hull)):
        p0 = hull[index]
        p1 = hull[(index + 1) % len(hull)]
        dx, dz = p1[0] - p0[0], p1[1] - p0[1]
        length = math.hypot(dx, dz)
        if length < 1e-9:
            continue
        ax, az = dx / length, dz / length
        bx, bz = -az, ax
        along = [p[0] * ax + p[1] * az for p in hull]
        across = [p[0] * bx + p[1] * bz for p in hull]
        extent_a = max(along) - min(along)
        extent_b = max(across) - min(across)
        area = extent_a * extent_b
        if area >= best_area:
            continue
        mid_a = (max(along) + min(along)) * 0.5
        mid_b = (max(across) + min(across)) * 0.5
        cx = ax * mid_a + bx * mid_b
        cz = az * mid_a + bz * mid_b
        if extent_a >= extent_b:
            box = OrientedBox(cx, cz, ax, az, extent_a * 0.5, extent_b * 0.5)
        else:
            box = OrientedBox(cx, cz, bx, bz, extent_b * 0.5, extent_a * 0.5)
        best, best_area = box, area
    assert best is not None
    return best


# ── party walls ─────────────────────────────────────────────────────────────

@dataclass
class PartyWallIndex:
    """Which wall segments are shared with a neighbouring building.

    A village street is mostly party walls, and a party wall has no windows.
    Getting this wrong is not subtle: openings appear on a face that is buried
    in the next house, and rows of buildings read as free-standing boxes that
    happen to touch.

    The index is a uniform grid over segment midpoints. It is O(n) to build and
    O(1) per query at village density, and it never compares two segments of the
    same building — a building touching itself is a concave footprint, not a
    party wall.
    """

    cells: dict[tuple[int, int], list[tuple[int, Point, Point]]] = field(default_factory=dict)

    def _key(self, point: Point) -> tuple[int, int]:
        return (int(math.floor(point[0] / PARTY_WALL_CELL)),
                int(math.floor(point[1] / PARTY_WALL_CELL)))

    def add(self, owner: int, p0: Point, p1: Point) -> None:
        midpoint = ((p0[0] + p1[0]) * 0.5, (p0[1] + p1[1]) * 0.5)
        self.cells.setdefault(self._key(midpoint), []).append((owner, p0, p1))

    def is_party(self, owner: int, p0: Point, p1: Point) -> bool:
        midpoint = ((p0[0] + p1[0]) * 0.5, (p0[1] + p1[1]) * 0.5)
        cx, cz = self._key(midpoint)
        for dx in (-1, 0, 1):
            for dz in (-1, 0, 1):
                for other, q0, q1 in self.cells.get((cx + dx, cz + dz), ()):
                    if other == owner:
                        continue
                    if _segments_face_each_other(p0, p1, q0, q1):
                        return True
        return False


def _segments_face_each_other(p0: Point, p1: Point, q0: Point, q1: Point) -> bool:
    """True when two wall segments are close, parallel and actually overlap.

    All three conditions are needed. Close and parallel without overlap is two
    houses across a narrow gap in the same street alignment; close and
    overlapping without being parallel is a corner touching a facade.
    """
    dx, dz = p1[0] - p0[0], p1[1] - p0[1]
    length = math.hypot(dx, dz)
    if length < 1e-6:
        return False
    ax, az = dx / length, dz / length
    nx, nz = az, -ax

    # Perpendicular distance of both endpoints of the other segment.
    d0 = (q0[0] - p0[0]) * nx + (q0[1] - p0[1]) * nz
    d1 = (q1[0] - p0[0]) * nx + (q1[1] - p0[1]) * nz
    if abs(d0) > PARTY_WALL_DISTANCE or abs(d1) > PARTY_WALL_DISTANCE:
        return False

    # Overlap of their projections onto this segment's axis.
    t0 = (q0[0] - p0[0]) * ax + (q0[1] - p0[1]) * az
    t1 = (q1[0] - p0[0]) * ax + (q1[1] - p0[1]) * az
    low, high = (t0, t1) if t0 <= t1 else (t1, t0)
    overlap = min(high, length) - max(low, 0.0)
    return overlap >= PARTY_WALL_OVERLAP


# ── the height and storey model ─────────────────────────────────────────────

_NUMBER = re.compile(r"[-+]?\d+(?:[.,]\d+)?")


def _tagged_length(raw: str) -> float | None:
    """Metres from an OSM length tag, honouring the foot/inch notation."""
    match = _NUMBER.search(raw or "")
    if not match:
        return None
    value = float(match.group(0).replace(",", "."))
    if "ft" in raw or "'" in raw:
        value *= 0.3048
    return value


_COMPASS = {
    "n": 0.0, "nne": 22.5, "ne": 45.0, "ene": 67.5,
    "e": 90.0, "ese": 112.5, "se": 135.0, "sse": 157.5,
    "s": 180.0, "ssw": 202.5, "sw": 225.0, "wsw": 247.5,
    "w": 270.0, "wnw": 292.5, "nw": 315.0, "nnw": 337.5,
}


def _slope_sign(raw: str) -> float:
    """Which way a single-slope roof falls, from `roof:direction`.

    The tag is a bearing and may be written either as degrees or as a compass
    point; both forms are common and neither is rare enough to ignore. This
    generator has one ridge line per building, so a bearing can only choose a
    sign along it — the northern half turns the slope one way, the southern half
    the other. An absent or unreadable tag falls on the first, which is a guess
    and is why a skillion is drawn from the Atlas only when nothing was tagged.
    """
    text = (raw or "").strip().lower()
    if not text:
        return 1.0
    if text in _COMPASS:
        return 1.0 if _COMPASS[text] < 180.0 else -1.0
    bearing = _tagged_length(text)
    if bearing is None:
        return 1.0
    return 1.0 if (bearing % 360.0) < 180.0 else -1.0


def _stack(storeys: int, profile: RegionProfile, commercial: bool) -> float:
    """Wall height for a storey count, given what is on the ground floor.

    A shop does not shorten the flats above it: a building with a 3.6 m
    commercial ground floor and two storeys over it is *taller* than the same
    building with three dwellings. Sharing a fixed wall height between them
    instead is how a generator ends up with blank upper floors — the squeezed
    storeys no longer have the room a window needs, and the facade silently
    loses its openings above the shopfront.

    A measured total height does not come through here. When the survey says how
    tall the building is, the floors share that number and a squeeze is a fact
    about the building rather than an artefact of this function.
    """
    if commercial and storeys >= 2:
        return profile.ground_storey_height + (storeys - 1) * profile.storey_height
    return storeys * profile.storey_height


@dataclass(frozen=True)
class Gabarit:
    """A building's vertical dimensions, and where each of them came from."""

    wall_height: float      # ground to eaves
    roof_height: float      # eaves to ridge; 0 for a flat roof
    storeys: int
    height_source: str      # "tag:height" | "tag:levels" | "atlas"
    roof_shape: str
    roof_source: str        # "tag:shape" | "atlas" | "atlas:not-rectangular"


def plan_gabarit(
    tags: dict[str, str],
    osm_id: int,
    profile: RegionProfile,
    box: OrientedBox,
    rectangularity: float,
    commercial: bool = False,
) -> Gabarit:
    """Decide how tall the building is, and what is on top of it.

    Order of authority, and it is the whole point of the function: a measured
    value always wins, an inferred one is only reached when nothing was
    measured, and which of the two happened is returned rather than forgotten
    (§4 I5).
    """
    shape_rng = seeded(osm_id, SALT_SHAPE)
    height_rng = seeded(osm_id, SALT_HEIGHT)

    # Roof shape first: it decides how much of a tagged total height is roof.
    tagged_shape = (tags.get("roof:shape", "") or "").strip().lower()
    known = {"flat", "gabled", "hipped", "pyramidal", "skillion",
             "half-hipped", "gambrel", "mansard", "round", "dome"}
    if tagged_shape in known:
        # Forms this generator does not build are mapped to their nearest
        # buildable relative rather than silently becoming flat: a mansard read
        # as hipped is wrong in the detail and right in the silhouette, which is
        # the rank that matters (§2.1).
        shape = {
            "half-hipped": "hipped", "gambrel": "gabled",
            "mansard": "hipped", "round": "gabled", "dome": "pyramidal",
        }.get(tagged_shape, tagged_shape)
        roof_source = "tag:shape"
    else:
        shape = profile.roof_shape(shape_rng)
        roof_source = "atlas"

    # A pitched roof is built over the oriented box (see the module docstring),
    # so a footprint that does not fill its box cannot have one. This is a
    # refusal, not a fallback: an L-shaped building with a straight ridge across
    # the notch looks worse than the same building with a flat roof.
    if shape != "flat" and rectangularity < RECTANGULARITY_FOR_PITCH:
        shape = "flat"
        roof_source = "atlas:not-rectangular"
    # Too wide for one ridge. Checked after the shape is known and before the
    # height is derived from it, because the span is what makes the height
    # absurd rather than the other way round.
    if shape not in ("flat", "skillion") and box.half_v * 2.0 > MAX_RIDGE_SPAN:
        shape = "flat"
        roof_source = "atlas:span"

    pitch = _tagged_length(tags.get("roof:angle", ""))
    pitch_degrees = pitch if pitch and 5.0 <= pitch <= 60.0 else profile.roof_pitch(shape_rng)
    if shape == "flat":
        roof_height = 0.0
    else:
        tagged_roof = _tagged_length(tags.get("roof:height", ""))
        if tagged_roof is not None and 0.5 <= tagged_roof <= 25.0:
            roof_height = tagged_roof
        else:
            # Rise over the half-width for a ridge, over the full width for a
            # single slope: a skillion at the same pitch is twice as tall.
            run = box.half_v if shape != "skillion" else box.half_v * 2.0
            roof_height = min(12.0, max(0.6, math.tan(math.radians(pitch_degrees)) * run))

    levels = _tagged_length(tags.get("building:levels", ""))
    total = _tagged_length(tags.get("height", ""))
    if total is not None and 2.0 <= total <= 300.0:
        # `height` is the total *including* the roof, and it is a measurement:
        # the building must come out exactly that tall. So the roof is what
        # gives way when the inferred pitch would leave no wall under it — the
        # inferred quantity yields to the measured one, never the reverse, which
        # is the whole of §4 I5 in one expression.
        reserve = min(2.4, total * 0.6)
        roof_height = min(roof_height, max(0.0, total - reserve))
        wall_height = total - roof_height
        storeys = (max(1, int(round(levels))) if levels
                   else max(1, round(wall_height / profile.storey_height)))
        height_source = "tag:height"
    elif levels is not None and 1.0 <= levels <= 80.0:
        storeys = max(1, int(round(levels)))
        wall_height = _stack(storeys, profile, commercial)
        height_source = "tag:levels"
    else:
        storeys = profile.storeys(height_rng)
        # Half a storey of jitter, so a street of untagged buildings has an
        # eave line that steps the way a real one does instead of being ruled.
        wall_height = (_stack(storeys, profile, commercial)
                       + (height_rng.random() - 0.5) * 0.9)
        height_source = "atlas"

    # The floor applies to a guess, not to a measurement: raising a tagged
    # 2.2 m building to 2.4 would be correcting the survey from a default.
    if height_source == "atlas":
        wall_height = max(2.4, wall_height)
    wall_height = min(90.0, wall_height)

    # Last, once both are known: a roof does not out-rise its own walls. A
    # tagged `roof:height` is a measurement and is left alone — a church spire
    # is allowed to be a church spire — but an inferred one yields, because it
    # came from a pitch and a span rather than from anyone looking.
    if roof_source != "tag:shape" or not _tagged_length(tags.get("roof:height", "")):
        roof_height = min(roof_height, wall_height * MAX_ROOF_OVER_WALL)
    return Gabarit(wall_height, roof_height, storeys, height_source, shape, roof_source)


# ── mesh buckets ────────────────────────────────────────────────────────────

class MeshBook:
    """One mesh per material, not one per building.

    A thousand buildings each with its own mesh would be a thousand draw calls;
    bucketing them by the swatch they drew gives one per palette entry, which is
    what the renderer wants and what `MeshPart` expresses.
    """

    def __init__(self, uv_mode: str = "none") -> None:
        self._meshes: dict[str, Mesh] = {}
        self._swatches: dict[str, Swatch] = {}
        self._uv_mode = uv_mode

    def mesh(self, swatch: Swatch) -> Mesh:
        if swatch.name not in self._meshes:
            self._meshes[swatch.name] = Mesh(uv_mode=self._uv_mode)
            self._swatches[swatch.name] = swatch
        return self._meshes[swatch.name]

    def parts(self, prefix: str, texture: str | None = None,
              double_sided: bool = False, materials=None) -> list[MeshPart]:
        """One part per swatch, optionally all sharing one base-colour texture.

        The texture multiplies the swatch colour rather than replacing it, so a
        single greyscale facade sheet serves every palette entry in every
        region: the openings come from the image and the render colour comes
        from the Atlas. One texture, thirty-four regions, and no atlas to bake.
        """
        out: list[MeshPart] = []
        for name in sorted(self._meshes):  # sorted: the GLB must be reproducible
            mesh = self._meshes[name]
            if not mesh.indices:
                continue
            swatch = self._swatches[name]
            if materials is not None:
                # A material per swatch, from whoever knows what it is made of
                # (`surfaces.py` in the world); the swatch keeps the albedo.
                material = materials(swatch, double_sided)
            else:
                colour = (swatch.color[0], swatch.color[1], swatch.color[2], 1.0)
                material = Material(name, colour, swatch.roughness,
                                    double_sided=double_sided, base_color_texture=texture)
            out.append(MeshPart(f"{prefix} — {name}", mesh, material))
        return out

    def triangle_count(self) -> int:
        return sum(len(mesh.indices) for mesh in self._meshes.values()) // 3


# ── wall frames and faces ───────────────────────────────────────────────────

@dataclass(frozen=True)
class WallFrame:
    """A wall's own coordinate system: `s` along it, `t` up, depth inward."""

    ox: float
    oz: float
    ground: float
    ax: float       # unit along
    az: float
    nx: float       # unit outward normal, (dz, -dx) for a positive-area ring
    nz: float
    length: float

    def point(self, s: float, t: float, depth: float = 0.0) -> Vec3:
        return (
            self.ox + self.ax * s - self.nx * depth,
            self.ground + t,
            self.oz + self.az * s - self.nz * depth,
        )


def wall_frame(p0: Point, p1: Point, ground: float) -> WallFrame | None:
    dx, dz = p1[0] - p0[0], p1[1] - p0[1]
    length = math.hypot(dx, dz)
    if length < 1e-6:
        return None
    ax, az = dx / length, dz / length
    return WallFrame(p0[0], p0[1], ground, ax, az, az, -ax, length)


def _face(mesh: Mesh, frame: WallFrame, s0: float, t0: float,
          s1: float, t1: float, depth: float = 0.0,
          uv_scale: tuple[float, float] | None = None) -> None:
    """An outward-facing rectangle on a wall plane.

    The vertex order is `(s0,t0) (s0,t1) (s1,t1) (s1,t0)`, which is what makes
    the normal come out along the frame's outward axis; the reverse order gives
    the inward one. Derived once in the module docstring so no call site has to
    re-derive it.

    `uv_scale` is `(u per metre, v per metre)` and exists for the walls that
    carry a facade texture instead of modelled openings. The caller passes
    *whole bays per wall length* and *whole storeys per wall height* rather
    than a constant, which is the same alignment rule the modelled facades
    obey (§10: alignment before triangles) — a texture stretched to a fraction
    of a floor is the tell that gives a tiled facade away.
    """
    if s1 - s0 < 1e-4 or t1 - t0 < 1e-4:
        return
    uvs = None
    if uv_scale is not None:
        u, v = uv_scale
        uvs = ((s0 * u, -t0 * v), (s0 * u, -t1 * v),
               (s1 * u, -t1 * v), (s1 * u, -t0 * v))
    mesh.add_quad(
        frame.point(s0, t0, depth), frame.point(s0, t1, depth),
        frame.point(s1, t1, depth), frame.point(s1, t0, depth), uvs,
    )


def _reveal(mesh: Mesh, frame: WallFrame, s0: float, t0: float,
            s1: float, t1: float, depth: float) -> None:
    """The four faces of a recessed opening, each turned into the opening.

    Without these an opening is a hole with no thickness, and the facade reads
    as printed on. `depth` is small — twelve centimetres in the alpine profile —
    and it is the difference between a window and a decal.
    """
    if depth <= 1e-4:
        return
    # Jambs: at s0 facing toward larger s, at s1 facing toward smaller s.
    mesh.add_quad(frame.point(s0, t0), frame.point(s0, t1),
                  frame.point(s0, t1, depth), frame.point(s0, t0, depth))
    mesh.add_quad(frame.point(s1, t0), frame.point(s1, t0, depth),
                  frame.point(s1, t1, depth), frame.point(s1, t1))
    # Sill, facing up; head, facing down.
    mesh.add_up_quad(frame.point(s0, t0), frame.point(s1, t0),
                     frame.point(s1, t0, depth), frame.point(s0, t0, depth))
    mesh.add_quad(frame.point(s0, t1), frame.point(s1, t1),
                  frame.point(s1, t1, depth), frame.point(s0, t1, depth))


# ── roofs ───────────────────────────────────────────────────────────────────

def _add_slab(mesh: Mesh, loop: list[Vec3], thickness: float) -> None:
    """A planar convex face given thickness: top, underside and an edge band.

    Every roof surface goes through here, which is what gives every roof an
    eave with a fascia and a soffit rather than a sheet of paper. The loop must
    be convex — every face this module builds is — so a fan triangulation is
    valid and no ear clipping is needed on a 3D polygon.

    The loop is normalised to a positive signed area in (x, z) first. With that
    orientation, a fan taken in order faces *down*, which is why the top face
    goes through `add_up_triangle` (which corrects it) and the underside does
    not (it wants exactly that). The edge band's order follows from the same
    convention and is derived in the module docstring.
    """
    if len(loop) < 3:
        return
    flat = [(p[0], p[2]) for p in loop]
    if polygon_area(flat) < 0.0:
        loop = list(reversed(loop))
    if thickness <= 0.0:
        # A roof surface with no edge. Six vertices where a slab costs
        # twenty-four, and the whole of what it gives up is the fascia and the
        # soffit under the eave — rank 12, and the only rank a preview LOD can
        # afford to lose (see `build_buildings`). The ridge, the pitch, the
        # overhang and the colour are all still here, and they are the ranks
        # that carry the silhouette.
        for index in range(1, len(loop) - 1):
            mesh.add_up_triangle(loop[0], loop[index], loop[index + 1])
        return
    under = [(p[0], p[1] - thickness, p[2]) for p in loop]

    for index in range(1, len(loop) - 1):
        mesh.add_up_triangle(loop[0], loop[index], loop[index + 1])
        mesh.add_triangle(under[0], under[index], under[index + 1])
    for index in range(len(loop)):
        nxt = (index + 1) % len(loop)
        mesh.add_quad(loop[index], loop[nxt], under[nxt], under[index])


def _roof_loops(box: OrientedBox, shape: str, eave: float,
                y_eave: float, y_ridge: float,
                direction: float) -> list[list[Vec3]]:
    """The roof surfaces of one building, as convex 3D loops.

    Everything is expressed in box coordinates and only turned into world points
    at the end, which is what keeps the ridge straight and the eave line
    continuous: they are single lines in that frame, not a per-edge decision.
    """
    hu, hv = box.half_u + eave, box.half_v + eave

    def at(a: float, b: float, y: float) -> Vec3:
        x, z = box.point(a, b)
        return (x, y, z)

    if shape == "gabled":
        return [
            [at(-hu, -hv, y_eave), at(hu, -hv, y_eave), at(hu, 0.0, y_ridge), at(-hu, 0.0, y_ridge)],
            [at(hu, hv, y_eave), at(-hu, hv, y_eave), at(-hu, 0.0, y_ridge), at(hu, 0.0, y_ridge)],
        ]
    if shape == "hipped":
        # The ridge is shorter than the box by the hip run at each end. When the
        # box is squarer than it is long the ridge vanishes and the roof is a
        # pyramid, which is the correct degenerate case rather than a special one.
        ridge = max(0.0, hu - hv)
        if ridge < 0.05:
            return _roof_loops(box, "pyramidal", eave, y_eave, y_ridge, direction)
        return [
            [at(-hu, -hv, y_eave), at(hu, -hv, y_eave), at(ridge, 0.0, y_ridge), at(-ridge, 0.0, y_ridge)],
            [at(hu, hv, y_eave), at(-hu, hv, y_eave), at(-ridge, 0.0, y_ridge), at(ridge, 0.0, y_ridge)],
            [at(-hu, hv, y_eave), at(-hu, -hv, y_eave), at(-ridge, 0.0, y_ridge)],
            [at(hu, -hv, y_eave), at(hu, hv, y_eave), at(ridge, 0.0, y_ridge)],
        ]
    if shape == "pyramidal":
        apex = at(0.0, 0.0, y_ridge)
        corners = [at(-hu, -hv, y_eave), at(hu, -hv, y_eave),
                   at(hu, hv, y_eave), at(-hu, hv, y_eave)]
        return [[corners[i], corners[(i + 1) % 4], apex] for i in range(4)]
    if shape == "skillion":
        # `direction` is the OSM bearing the slope faces, folded onto the box's
        # own across-axis: a slope can only fall one way or the other across the
        # ridge line this generator has, so the tag chooses the sign.
        low, high = (-hv, hv) if direction >= 0.0 else (hv, -hv)
        return [[at(-hu, low, y_eave), at(hu, low, y_eave),
                 at(hu, high, y_ridge), at(-hu, high, y_ridge)]]
    return []


def _gable_walls(box: OrientedBox, shape: str, y_eave: float, y_ridge: float,
                 direction: float) -> list[list[Vec3]]:
    """The triangles of masonry a pitched roof leaves under itself.

    They belong to the wall material, not the roof: a gable end is the wall
    carried up to the ridge, and rendering it as roofing is the single most
    common tell of a generated building.

    They are built at the *wall* plane rather than at the eave, so the roof
    still overhangs them. The strip of sky visible between the two under a deep
    overhang is what the overhang is: a real gable has exactly that, and the
    slab's underside closes it from below.
    """
    hu, hv = box.half_u, box.half_v

    def at(a: float, b: float, y: float) -> Vec3:
        x, z = box.point(a, b)
        return (x, y, z)

    if shape == "gabled":
        return [
            [at(-hu, -hv, y_eave), at(-hu, hv, y_eave), at(-hu, 0.0, y_ridge)],
            [at(hu, hv, y_eave), at(hu, -hv, y_eave), at(hu, 0.0, y_ridge)],
        ]
    if shape == "skillion":
        low, high = (-hv, hv) if direction >= 0.0 else (hv, -hv)
        return [
            [at(-hu, low, y_eave), at(-hu, high, y_eave), at(-hu, high, y_ridge)],
            [at(hu, high, y_eave), at(hu, low, y_eave), at(hu, high, y_ridge)],
        ]
    return []


def _emit_parapet(mesh: Mesh, ring: list[Point], y_deck: float, height: float) -> None:
    """A low wall around a flat roof, with its coping and its inner face.

    A flat roof capped flush reads as a box that was cut off. Every flat roof
    that is not an industrial shed has a parapet, and it costs three quads an
    edge.
    """
    inner = inset_polygon(ring, 0.28)
    if inner is ring:
        return
    top = y_deck + height
    count = len(ring)
    for index in range(count):
        nxt = (index + 1) % count
        frame = wall_frame(ring[index], ring[nxt], y_deck)
        if frame is None:
            continue
        _face(mesh, frame, 0.0, 0.0, frame.length, height)
        # Coping and inner face, both from the inset ring so the corners meet.
        a, b = ring[index], ring[nxt]
        ia, ib = inner[index], inner[nxt]
        mesh.add_up_quad((a[0], top, a[1]), (b[0], top, b[1]),
                         (ib[0], top, ib[1]), (ia[0], top, ia[1]))
        inner_frame = wall_frame(ib, ia, y_deck)
        if inner_frame is not None:
            _face(mesh, inner_frame, 0.0, 0.0, inner_frame.length, height)


# ── towers ──────────────────────────────────────────────────────────────────

# What a place of worship is called in OSM, on the building or on the amenity.
WORSHIP_BUILDINGS = {"church", "chapel", "cathedral", "basilica", "mosque",
                     "synagogue", "temple", "monastery", "shrine"}


def is_worship(tags: dict[str, str]) -> bool:
    return (tags.get("amenity") == "place_of_worship"
            or tags.get("building", "") in WORSHIP_BUILDINGS)


def steeple_style(tags: dict[str, str]) -> str:
    """Spire, minaret or plain tower, from what the survey says it is.

    Rank 10 is "forte signature culturelle" and this is the cheapest place in
    the whole generator to earn it: the same footprint, the same walls, and a
    silhouette that says which building this is from a kilometre away. The
    religion tag is a *measurement* -- it is only ever read, never guessed.
    """
    religion = tags.get("religion", "")
    if religion == "muslim" or tags.get("building") == "mosque":
        return "minaret"
    if religion in ("jewish", "buddhist", "hindu", "sikh") or not religion:
        # No spire for a faith that does not build them, and none for a
        # building nobody labelled: an unlabelled place of worship gets the
        # tower without the spire, which is wrong nowhere.
        return "tower" if religion else "spire"
    return "spire"


def _emit_steeple(walls: Mesh, roofs: Mesh, box: OrientedBox, ground: float,
                  wall_height: float, style: str) -> None:
    """A bell tower at one end of the nave, and the spire over it.

    **Why this is built and not downloaded.** OSM gives a church its real
    outline (rank 3, measured, exact), and dropping a fixed church model on top
    of it would throw that away and stand a generic shape where a surveyed one
    exists. No CC0 kit has a church in any case -- the nearest thing on offer is
    a fantasy crypt. What actually makes a village read as a village at any
    distance is the *silhouette* of the spire above the roofline, and that is
    forty vertices derived from the footprint the survey drew.

    It also inherits the region: a Paris church is limestone under zinc and a
    Nordic one is painted timber under red tile, because the tower is drawn into
    the same meshes as the walls and the roof it belongs to, with no palette of
    its own.
    """
    # The tower sits at one end of the long axis and is as wide as the nave
    # allows, capped so that a cathedral does not get a tower the width of a
    # street. The end is fixed rather than drawn, because a ridge has two and a
    # seeded choice would make the same church face differently on two machines
    # if the seed table ever changed.
    side = max(2.2, min(box.half_v * 1.5, 7.0))
    centre_a = -(box.half_u - side * 0.5) if box.half_u > side else 0.0
    cx, cz = box.point(centre_a, 0.0)
    yaw = math.atan2(box.ux, box.uz)

    if style == "minaret":
        # Slender and much taller than the roof, with a small cap. A minaret is
        # not a bell tower with different proportions -- it is the proportion.
        side = min(side * 0.45, 3.2)
        shaft = max(12.0, wall_height * 2.8)
        cap = side * 1.6
    elif style == "spire":
        shaft = max(8.0, wall_height * 1.7)
        cap = side * 2.4
    else:
        shaft = max(6.0, wall_height * 1.35)
        cap = side * 0.45

    walls.add_box((cx, ground + shaft * 0.5, cz), (side, shaft, side), yaw)
    # The cap is a pyramid on the tower's own square, which is what makes the
    # spire land square on the shaft instead of near it.
    apex = (cx, ground + shaft + cap, cz)
    half = side * 0.5
    corners = []
    for dx, dz in ((-half, -half), (half, -half), (half, half), (-half, half)):
        x = cx + dx * math.cos(yaw) + dz * math.sin(yaw)
        z = cz - dx * math.sin(yaw) + dz * math.cos(yaw)
        corners.append((x, ground + shaft, z))
    for index in range(4):
        roofs.add_triangle(corners[index], corners[(index + 1) % 4], apex)


# ── facades ─────────────────────────────────────────────────────────────────

def floor_levels(wall_height: float, storeys: int, profile: RegionProfile,
                 commercial: bool) -> list[tuple[float, float]]:
    """The `(base, height)` of each floor, shared by every wall of a building.

    Sharing them is the whole reason this is a function and not a loop inside
    the wall builder: windows on two different faces of the same building must
    sit at the same height, and they do so here because they are read off one
    list rather than recomputed per wall.
    """
    if storeys <= 0:
        return []
    if commercial and storeys >= 2:
        ground = min(profile.ground_storey_height, wall_height * 0.55)
        rest = (wall_height - ground) / (storeys - 1)
        return [(0.0, ground)] + [
            (ground + rest * index, rest) for index in range(storeys - 1)
        ]
    even = wall_height / storeys
    return [(even * index, even) for index in range(storeys)]


def _emit_facade(walls: Mesh, trim: Mesh, glass: Mesh, frame: WallFrame,
                 levels: list[tuple[float, float]], wall_height: float,
                 profile: RegionProfile, commercial: bool) -> None:
    """One wall, divided into bays, with an opening on the axis of each.

    The bay count is the whole number nearest the region's target spacing, so
    bays are regular *within* a wall and the leftover is spread across all of
    them rather than dumped at one end. That is what a mason does and what the
    eye checks first.

    A bay too narrow for an opening with its piers, or a floor too short for one
    with its lintel, gets solid masonry instead of a cramped window. Refusing is
    part of the alignment: one squeezed window destroys the rhythm of a facade
    more surely than a blank bay does.
    """
    bays = max(1, int(round(frame.length / profile.bay_width)))
    bay = frame.length / bays

    for floor_index, (base, height) in enumerate(levels):
        shopfront = commercial and floor_index == 0
        if shopfront:
            # A shopfront is as wide as the bay will allow, and the masonry
            # between two of them is a slim pier rather than a dwelling's. Using
            # the same pier rule as a window is how a generator ends up with a
            # village whose ground floors are all blank: at the region's bay
            # spacing, 78% of a bay plus two 55 cm piers does not fit in the bay.
            pier = profile.shopfront_pier_min
            width = min(bay * profile.shopfront_width_ratio, bay - 2.0 * pier)
            opening_height = min(profile.shopfront_height, height - profile.lintel_min)
            sill = profile.shopfront_sill
        else:
            pier = profile.pier_min
            width = profile.window_width
            opening_height = profile.window_height
            sill = profile.window_sill

        fits = (
            width + 2.0 * pier <= bay
            and sill + opening_height + profile.lintel_min <= height
            and opening_height > 0.3
            and width > 0.5
        )
        if not fits:
            _face(walls, frame, 0.0, base, frame.length, base + height)
            continue

        top = base + height
        for index in range(bays):
            centre = (index + 0.5) * bay
            s0, s1 = centre - width * 0.5, centre + width * 0.5
            t0, t1 = base + sill, base + sill + opening_height
            # The masonry around the opening: below, above, and the two piers.
            _face(walls, frame, index * bay, base, (index + 1) * bay, t0)
            _face(walls, frame, index * bay, t1, (index + 1) * bay, top)
            _face(walls, frame, index * bay, t0, s0, t1)
            _face(walls, frame, s1, t0, (index + 1) * bay, t1)
            _reveal(trim, frame, s0, t0, s1, t1, profile.window_inset)
            _face(glass, frame, s0, t0, s1, t1, profile.window_inset)

    # Anything above the top floor — the remainder of a tagged height that the
    # storey division could not absorb — is plain wall up to the eaves.
    if levels:
        last = levels[-1][0] + levels[-1][1]
        if wall_height - last > 0.05:
            _face(walls, frame, 0.0, last, frame.length, wall_height)


# ── the chain ───────────────────────────────────────────────────────────────

@dataclass
class BuildingStats:
    """What the run inferred, for the manifest. §4 I5 in one dataclass."""

    total: int = 0
    rejected: int = 0
    height_measured: int = 0
    height_inferred: int = 0
    roof_tagged: int = 0
    roof_inferred: int = 0
    roof_flattened: int = 0
    detailed: int = 0
    party_walls: int = 0
    steeples: int = 0
    shapes: dict[str, int] = field(default_factory=dict)

    def as_json(self) -> dict:
        return {
            "count": self.total,
            "rejected": self.rejected,
            "heightMeasured": self.height_measured,
            "heightInferred": self.height_inferred,
            "roofTagged": self.roof_tagged,
            "roofInferred": self.roof_inferred,
            "roofFlattenedForShape": self.roof_flattened,
            "facadesDetailed": self.detailed,
            "partyWalls": self.party_walls,
            "steeples": self.steeples,
            "roofShapes": dict(sorted(self.shapes.items())),
        }


@dataclass
class PlannedBuilding:
    osm_id: int
    tags: dict[str, str]
    ring: list[Point]
    ground: float
    box: OrientedBox
    gabarit: Gabarit
    commercial: bool
    detailed: bool
    foundation: float


def plan_buildings(
    ways,
    ground_of,
    profile: RegionProfile,
    detail_center: Point,
    detail_radius: float,
) -> tuple[list[PlannedBuilding], PartyWallIndex, BuildingStats]:
    """Everything decided before a single triangle is written.

    Planning the whole set first is what makes party walls knowable: a wall is
    only shared once its neighbour exists, so no building can be emitted until
    every footprint has been cleaned and placed.
    """
    stats = BuildingStats()
    planned: list[PlannedBuilding] = []
    index = PartyWallIndex()

    for way in ways:
        raw = [(lon, lat) for lon, lat in way.points[:-1]]
        placed = [ground_of(lon, lat) for lon, lat in raw]
        ring = clean_footprint([(point[0], point[2]) for point in placed])
        if ring is None:
            stats.rejected += 1
            continue
        ground = sum(point[1] for point in placed) / len(placed)
        # Keep floors and roofs level, but close the downhill gap with solid
        # masonry. Sample along long edges too: a hollow between two corners
        # must not open a hole under an otherwise correctly grounded facade.
        samples = [p[1] for p in placed]
        for i, a in enumerate(raw):
            b = raw[(i + 1) % len(raw)]
            n = max(1, math.ceil(math.dist(placed[i], placed[(i + 1) % len(raw)]) / 4.))
            for j in range(1, n):
                samples.append(ground_of(a[0]+(b[0]-a[0])*j/n, a[1]+(b[1]-a[1])*j/n)[1])
        foundation = min(samples) - .30
        box = oriented_box(ring)
        rectangularity = abs(polygon_area(ring)) / box.area if box.area > 1e-6 else 0.0
        commercial = profile.is_commercial(way.tags)
        gabarit = plan_gabarit(way.tags, way.osm_id, profile, box, rectangularity,
                               commercial)

        centre_x = sum(p[0] for p in ring) / len(ring)
        centre_z = sum(p[1] for p in ring) / len(ring)
        distance = math.dist((centre_x, centre_z), detail_center)

        planned.append(PlannedBuilding(
            osm_id=way.osm_id,
            tags=way.tags,
            ring=ring,
            ground=ground,
            box=box,
            gabarit=gabarit,
            commercial=commercial,
            detailed=distance <= detail_radius,
            foundation=foundation,
        ))

    for owner, building in enumerate(planned):
        count = len(building.ring)
        for edge in range(count):
            index.add(owner, building.ring[edge], building.ring[(edge + 1) % count])

    return planned, index, stats


def build_buildings(
    ways,
    ground_of,
    profile: RegionProfile,
    detail_center: Point = (0.0, 0.0),
    detail_radius: float = 190.0,
    wall_texture: str | None = None,
    roof_thickness: float = ROOF_THICKNESS,
    wall_materials=None,
    roof_materials=None,
) -> tuple[list[MeshPart], list[list[Point]], BuildingStats]:
    """Turn OSM building ways into meshes, footprints and a provenance record.

    `ground_of(lon, lat)` returns the engine-space point on the terrain, so this
    module never learns what a projection or an elevation grid is.

    `roof_thickness` of zero drops the fascia and the soffit and leaves the
    roof surface alone. It is the second half of the same bargain as
    `wall_texture`, and it is the larger half: on a dense tile of gabled
    buildings the slab edges are 46% of every vertex in the file, which is more
    than the walls they sit on. A roof read from the street keeps its ridge,
    its pitch, its overhang and its colour without them.

    `wall_texture` puts a tiled facade sheet on the plain walls in place of
    modelled openings. It is what lets the world tiles keep rank 11 at a
    hundredth of the cost: the whole city gets bays and floors without paying
    for holes in the masonry.

    `detail_radius` is §12.4 applied at generation time rather than at runtime:
    a facade three hundred metres away contributes nothing a silhouette does not
    already carry, and the triangles it would cost are the triangles the near
    buildings need. Buildings outside it keep their exact footprint, their
    measured height and their roof — everything that carries recognition — and
    lose only their openings.
    """
    planned, party, stats = plan_buildings(
        ways, ground_of, profile, detail_center, detail_radius
    )

    walls = MeshBook()
    foundations = MeshBook()
    # Roof faces get UVs across and up their own slope, so rows of tiles run
    # along the eave however the roof is turned (see `Mesh.uv_mode`).
    roofs = MeshBook("slope" if roof_materials is not None else "none")
    trim = Mesh()
    glass = Mesh()
    footprints: list[list[Point]] = []

    for owner, building in enumerate(planned):
        stats.total += 1
        footprints.append(building.ring)
        gabarit = building.gabarit
        stats.shapes[gabarit.roof_shape] = stats.shapes.get(gabarit.roof_shape, 0) + 1
        if gabarit.height_source == "atlas":
            stats.height_inferred += 1
        else:
            stats.height_measured += 1
        if gabarit.roof_source == "tag:shape":
            stats.roof_tagged += 1
        else:
            stats.roof_inferred += 1
        if gabarit.roof_source == "atlas:not-rectangular":
            stats.roof_flattened += 1
        if building.detailed:
            stats.detailed += 1

        wall_swatch = profile.wall_swatch(seeded(building.osm_id, SALT_WALL))
        roof_swatch = profile.roof_swatch(seeded(building.osm_id, SALT_ROOF))
        wall_mesh = walls.mesh(wall_swatch)
        roof_mesh = roofs.mesh(roof_swatch)

        y_eave = building.ground + gabarit.wall_height
        levels = floor_levels(gabarit.wall_height, gabarit.storeys, profile,
                              building.commercial)

        count = len(building.ring)
        for edge in range(count):
            p0 = building.ring[edge]
            p1 = building.ring[(edge + 1) % count]
            frame = wall_frame(p0, p1, building.ground)
            if frame is None:
                continue
            if not (_tagged_length(building.tags.get("min_height", "")) or
                    _tagged_length(building.tags.get("building:min_level", ""))):
                # Untextured plinth: stretching window rows down to the ground
                # would turn the foundation itself into another inhabited floor.
                _face(foundations.mesh(wall_swatch), frame, 0.,
                      building.foundation-building.ground, frame.length, 0.)
            shared = party.is_party(owner, p0, p1)
            if shared:
                stats.party_walls += 1
            if building.detailed and not shared and frame.length >= 2.0:
                _emit_facade(wall_mesh, trim, glass, frame, levels,
                             gabarit.wall_height, profile, building.commercial)
            else:
                # Only when there is a texture to align. A wall with no sheet on
                # it gains nothing from UVs and loses about a percent of its
                # vertices to the dedup, since two quads that shared a corner
                # stop sharing it once their texture coordinates differ.
                scale = None
                if wall_texture is not None or wall_materials is not None:
                    bays = max(1.0, round(frame.length / profile.bay_width))
                    scale = (bays / frame.length,
                             gabarit.storeys / max(1e-3, gabarit.wall_height))
                _face(wall_mesh, frame, 0.0, 0.0, frame.length,
                      gabarit.wall_height, uv_scale=scale)

        # The deck. Under a pitched roof it is never seen from outside, and it
        # is emitted anyway: the oriented box is larger than the footprint, so
        # without it a grazing view under the eave looks into the building.
        for a, b, c in triangulate(building.ring):
            pa, pb, pc = building.ring[a], building.ring[b], building.ring[c]
            roof_mesh.add_up_triangle(
                (pa[0], y_eave, pa[1]), (pb[0], y_eave, pb[1]), (pc[0], y_eave, pc[1])
            )

        if gabarit.roof_shape == "flat":
            _emit_parapet(wall_mesh, building.ring, y_eave, profile.parapet_height)
            if is_worship(building.tags):
                _emit_steeple(wall_mesh, roof_mesh, building.box, building.ground,
                              gabarit.wall_height, steeple_style(building.tags))
                stats.steeples += 1
            continue

        if is_worship(building.tags):
            _emit_steeple(wall_mesh, roof_mesh, building.box, building.ground,
                          gabarit.wall_height, steeple_style(building.tags))
            stats.steeples += 1

        slope_sign = _slope_sign(building.tags.get("roof:direction", ""))
        y_ridge = y_eave + gabarit.roof_height
        for loop in _roof_loops(building.box, gabarit.roof_shape,
                                profile.eave_overhang, y_eave, y_ridge, slope_sign):
            _add_slab(roof_mesh, loop, roof_thickness)
        # A gable is vertical, so the up-facing helpers say nothing about it: it
        # is emitted in the order `_gable_walls` chose, which faces outward by
        # the same convention as the wall quads.
        for gable in _gable_walls(building.box, gabarit.roof_shape,
                                  y_eave, y_ridge, slope_sign):
            wall_mesh.add_triangle(*gable)

    # A roof with no thickness is one surface, so it has to be visible from
    # under the eave as well as from above; a slab closes itself and does not.
    parts = (walls.parts("Walls", wall_texture, materials=wall_materials)
             + foundations.parts("Foundations")
             + roofs.parts("Roofs", double_sided=roof_thickness <= 0.0,
                           materials=roof_materials))
    if trim.indices:
        parts.append(MeshPart("Openings — reveals", trim,
                              Material(profile.trim.name,
                                       (*profile.trim.color, 1.0), profile.trim.roughness)))
    if glass.indices:
        parts.append(MeshPart("Openings — glazing", glass,
                              Material(profile.glass.name,
                                       (*profile.glass.color, 1.0),
                                       profile.glass.roughness, metallic=0.0)))
    return parts, footprints, stats
