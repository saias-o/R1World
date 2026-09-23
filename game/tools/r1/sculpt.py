"""A small vocabulary for describing a monument, and nothing else.

`landmarks.py` holds twenty buildings that a generic extrusion cannot draw: a
tower that is a lattice, a statue, a dome. Each one is written as a recipe in
this vocabulary, so that the recipe reads like the building's own description
-- a square plinth, then an octagonal drum, then an onion of this profile --
rather than as a list of triangles. The vocabulary is deliberately short:

  prism      a footprint extruded between two heights (flat shaded)
  loft       a stack of footprints with the same vertex count (flat shaded)
  lathe      a profile turned about a vertical axis (smooth), with an optional
             elliptical section and a twist -- domes, drums, columns, onions
  grid       any rectangular grid of points (smooth) -- a robe, a shell
  beam       a square member between two points -- a lattice, a cable
  panel      a wall with arched openings cut through or recessed into it

Everything is built in the *recipe frame*: metres, y up, the origin on the
ground at the monument's anchor. `Sculpt.parts` then turns the recipe frame to
the monument's measured bearing, which is the only placement a model needs: a
monument exists once on Earth, so its orientation is baked into its geometry
rather than carried by a node.

**Levels of detail.** The same recipe draws every level. A sculpture made
inside `detail(1)` or `detail(2)` asks each primitive for less: fewer sides
to a dome, no opening narrower than the eye can resolve at that distance, no
lattice member thinner than a pixel. A recipe only intervenes where a shape
must change character with distance -- a lattice tower seen from five
kilometres is a solid silhouette, not a sparser lattice.

**Winding.** A triangle faces the side its right-hand normal points to, as in
`mesh.py` and glTF. Grids decide their orientation from a reference point that
is inside them rather than from the order a recipe happened to list its points
in: a recipe author cannot get it backwards, and a test checks that every
closed piece encloses a positive volume.
"""

from __future__ import annotations

import math
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Callable, Iterable, Sequence

from .mesh import Material, Mesh, MeshPart

Vec2 = tuple[float, float]
Vec3 = tuple[float, float, float]


def _sub(a: Vec3, b: Vec3) -> Vec3:
    return a[0] - b[0], a[1] - b[1], a[2] - b[2]


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _dot(a: Vec3, b: Vec3) -> float:
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _unit(v: Vec3) -> Vec3:
    length = math.sqrt(_dot(v, v))
    if length < 1e-12:
        return 0.0, 1.0, 0.0
    return v[0] / length, v[1] / length, v[2] / length


def _round(v: Vec3) -> Vec3:
    # Six decimals is a micrometre: enough for the weld to recognise a shared
    # corner computed twice by two different expressions.
    return round(v[0], 6), round(v[1], 6), round(v[2], 6)


def signed_area(ring: Sequence[Vec2]) -> float:
    """Shoelace area in the (x, z) plane; positive is `buildings.py`'s winding."""
    return 0.5 * sum(a[0] * b[1] - b[0] * a[1]
                     for a, b in zip(ring, list(ring[1:]) + [ring[0]]))


def positive(ring: Sequence[Vec2]) -> list[Vec2]:
    ring = list(ring)
    return ring if signed_area(ring) > 0 else ring[::-1]


# ── footprints ──────────────────────────────────────────────────────────────

def rect(width: float, depth: float, cx: float = 0.0, cz: float = 0.0) -> list[Vec2]:
    hx, hz = width * 0.5, depth * 0.5
    return positive([(cx - hx, cz - hz), (cx + hx, cz - hz),
                     (cx + hx, cz + hz), (cx - hx, cz + hz)])


def ngon(radius: float, sides: int, cx: float = 0.0, cz: float = 0.0,
         phase: float = 0.0) -> list[Vec2]:
    return positive([(cx + radius * math.cos(phase + math.tau * i / sides),
                      cz + radius * math.sin(phase + math.tau * i / sides))
                     for i in range(sides)])


def ellipse(rx: float, rz: float, sides: int, cx: float = 0.0,
            cz: float = 0.0) -> list[Vec2]:
    return positive([(cx + rx * math.cos(math.tau * i / sides),
                      cz + rz * math.sin(math.tau * i / sides)) for i in range(sides)])


def chamfered(width: float, depth: float, cut: float) -> list[Vec2]:
    """A rectangle with its corners cut at 45°: the Taj Mahal's plan."""
    hx, hz = width * 0.5, depth * 0.5
    return positive([(-hx + cut, -hz), (hx - cut, -hz), (hx, -hz + cut), (hx, hz - cut),
                     (hx - cut, hz), (-hx + cut, hz), (-hx, hz - cut), (-hx, -hz + cut)])


def star(outer: float, inner: float, points: int, phase: float = 0.0) -> list[Vec2]:
    ring = []
    for i in range(points * 2):
        r = outer if i % 2 == 0 else inner
        a = phase + math.pi * i / points
        ring.append((r * math.cos(a), r * math.sin(a)))
    return positive(ring)


def scaled(ring: Sequence[Vec2], sx: float, sz: float | None = None) -> list[Vec2]:
    sz = sx if sz is None else sz
    return [(x * sx, z * sz) for x, z in ring]


def moved(ring: Sequence[Vec2], dx: float, dz: float) -> list[Vec2]:
    return [(x + dx, z + dz) for x, z in ring]


def rotated(ring: Sequence[Vec2], angle: float) -> list[Vec2]:
    c, s = math.cos(angle), math.sin(angle)
    return [(x * c - z * s, x * s + z * c) for x, z in ring]


# ── the sculpture ───────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Finish:
    """What a surface is made of. `colour` is a measured albedo (CLAUDE.md
    rule 2); `family` names one of the project's photographed textures
    (`surfaces.py`) when the material has one, and the texture then averages
    to that albedo rather than to its own."""

    name: str
    colour: tuple[float, float, float]
    roughness: float = 0.85
    metallic: float = 0.0
    family: str | None = None
    # For surfaces with no inside: a shell, a sail, a leaf of drapery.
    double_sided: bool = False


# The level of detail new sculptures are made at: 0 up close, 1 across a
# district (a few hundred metres to two kilometres), 2 across a city.
_LEVEL = 0


@contextmanager
def detail(level: int):
    """Build everything inside at `level` (see the module doc)."""
    global _LEVEL
    previous, _LEVEL = _LEVEL, level
    try:
        yield
    finally:
        _LEVEL = previous


# The thinnest member, the narrowest opening, drawn at each level: about a
# pixel at the distance the level starts being used (1080 p, 62° field).
THINNEST = (0.0, 0.6, 1.8)
NARROWEST = (0.0, 3.0, 8.0)
# The slenderest turned piece (a column, a finial) kept, by its diameter.
SLENDEREST = (0.0, 0.9, 2.0)


def sides_at(level: int, sides: int, least: int = 5) -> int:
    return max(least, sides >> level) if level else sides


class Sculpt:
    """Meshes by finish, in the recipe frame, at one level of detail."""

    def __init__(self, lod: int | None = None) -> None:
        self.meshes: dict[str, Mesh] = {}
        self.finishes: dict[str, Finish] = {}
        self.lod = _LEVEL if lod is None else lod

    def mesh(self, finish: Finish) -> Mesh:
        known = self.finishes.get(finish.name)
        if known is not None and known != finish:
            raise ValueError(f"two finishes are called {finish.name!r}")
        self.finishes[finish.name] = finish
        mesh = self.meshes.get(finish.name)
        if mesh is None:
            # Faces added without explicit UVs are laid out in their own plane,
            # in metres, so a stone texture keeps its real course height on
            # every wall however the wall is turned. From a distance the
            # texture is its average, which the finish's albedo already is,
            # and without UVs neighbouring faces share their corners.
            mesh = self.meshes[finish.name] = Mesh(uv_mode="none" if self.lod else "slope")
        return mesh

    @property
    def vertices(self) -> int:
        return sum(len(m.positions) for m in self.meshes.values())

    def merge(self, other: "Sculpt",
              position: Callable[[Vec3], Vec3] = lambda p: p,
              normal: Callable[[Vec3], Vec3] = lambda n: n) -> None:
        """Add another sculpture's triangles, moved by a rigid transform.

        How a leaning tower leans: built upright, then turned as one piece
        about its foot, so every course keeps its level relative to the next.
        """
        for name, source in other.meshes.items():
            target = self.mesh(other.finishes[name])
            for k in range(0, len(source.indices), 3):
                tri = [source.indices[k + q] for q in range(3)]
                target.add_smooth_triangle(
                    tuple(_round(position(source.positions[i])) for i in tri),
                    tuple(_unit(normal(source.normals[i])) for i in tri),
                    tuple(source.texcoords[i] for i in tri))

    def parts(self, bearing: float, material_for: Callable[[Finish], Material]) -> list[MeshPart]:
        """The model turned so the recipe's +x points to `bearing`.

        `bearing` is a compass bearing in degrees, clockwise from north. The
        engine is x east and z south, so the recipe's +x goes to
        (sin b, -cos b) and its +z to (cos b, sin b): a proper rotation, which
        keeps every triangle's winding.
        """
        b = math.radians(bearing)
        sb, cb = math.sin(b), math.cos(b)

        def turn(v: Vec3) -> Vec3:
            return (v[0] * sb + v[2] * cb, v[1], -v[0] * cb + v[2] * sb)

        parts = []
        for name in sorted(self.meshes):
            source = self.meshes[name]
            if not source.indices:
                continue
            mesh = Mesh()
            mesh.positions = [_round(turn(p)) for p in source.positions]
            mesh.normals = [_round(turn(n)) for n in source.normals]
            mesh.texcoords = list(source.texcoords)
            mesh.indices = list(source.indices)
            parts.append(MeshPart(name, mesh, material_for(self.finishes[name])))
        return parts


# ── flat-shaded solids ──────────────────────────────────────────────────────

def _wall(mesh: Mesh, p0: Vec2, p1: Vec2, y0: float, y1: float,
          y0b: float | None = None, y1b: float | None = None) -> None:
    """One outward face of a positive ring's edge p0 -> p1 (see module doc)."""
    y0b = y0 if y0b is None else y0b
    y1b = y1 if y1b is None else y1b
    a, b = (p1[0], y0b, p1[1]), (p0[0], y0, p0[1])
    c, d = (p0[0], y1, p0[1]), (p1[0], y1b, p1[1])
    if math.dist(a, b) < 1e-9 and math.dist(c, d) < 1e-9:
        return
    if abs(y1 - y0) < 1e-9 and abs(y1b - y0b) < 1e-9:
        return
    mesh.add_triangle(a, b, c)
    mesh.add_triangle(a, c, d)


def cap(mesh: Mesh, ring: Sequence[Vec2], y: float, up: bool = True) -> None:
    from .polygons import triangulate
    ring = positive(ring)
    for i, j, k in triangulate(ring):
        a, b, c = ((ring[n][0], y, ring[n][1]) for n in (i, j, k))
        if up:
            mesh.add_up_triangle(a, b, c)
        else:
            # Down: the mirror of what `add_up_triangle` would have chosen.
            normal = _cross(_sub(b, a), _sub(c, a))
            if normal[1] > 0:
                b, c = c, b
            mesh.add_triangle(a, b, c)


def prism(s: Sculpt, finish: Finish, ring: Sequence[Vec2], y0: float, y1: float,
          top: bool = True, bottom: bool = False, roof: Finish | None = None,
          walls: bool = True) -> None:
    """`walls=False` keeps only the caps, for a block whose faces are panels:
    two coplanar walls would fight, and the plain one hides the openings."""
    ring = positive(ring)
    mesh = s.mesh(finish)
    for i in range(len(ring) if walls else 0):
        _wall(mesh, ring[i], ring[(i + 1) % len(ring)], y0, y1)
    if top:
        cap(s.mesh(roof or finish), ring, y1, up=True)
    if bottom:
        cap(mesh, ring, y0, up=False)


def loft(s: Sculpt, finish: Finish, levels: Sequence[tuple[Sequence[Vec2], float]],
         top: bool = True, roof: Finish | None = None) -> None:
    """Consecutive footprints joined face to face: a tapering shaft, a pyramid.

    Every footprint has the same vertex count and the same winding, and a ring
    may shrink to a point (a pyramid's apex) by repeating one point.
    """
    rings = [positive(r) if len(set(r)) > 2 else list(r) for r, _ in levels]
    mesh = s.mesh(finish)
    for (lower, y0), (upper, y1) in zip(zip(rings, [h for _, h in levels]),
                                        zip(rings[1:], [h for _, h in levels][1:])):
        n = len(lower)
        for i in range(n):
            j = (i + 1) % n
            a, b = (lower[j][0], y0, lower[j][1]), (lower[i][0], y0, lower[i][1])
            c, d = (upper[i][0], y1, upper[i][1]), (upper[j][0], y1, upper[j][1])
            if math.dist(a, b) > 1e-9:
                mesh.add_triangle(a, b, c)
            if math.dist(c, d) > 1e-9:
                mesh.add_triangle(a, c, d)
    last, height = rings[-1], levels[-1][1]
    if top and len(set(last)) > 2:
        cap(s.mesh(roof or finish), last, height, up=True)


def steps(s: Sculpt, finish: Finish, ring: Sequence[Vec2], y0: float,
          rises: Sequence[tuple[float, float]]) -> float:
    """A stepped base: each (rise, inset) is one course. Returns the top."""
    y = y0
    for rise, inset in rises:
        prism(s, finish, ring, y, y + rise)
        y += rise
        ring = _inset(ring, inset)
    return y


def _inset(ring: Sequence[Vec2], d: float) -> list[Vec2]:
    from .polygons import inset_polygon
    ring = positive(ring)
    return positive(inset_polygon(ring, d)) if d else ring


def box(s: Sculpt, finish: Finish, cx: float, y0: float, cz: float,
        width: float, height: float, depth: float, top: bool = True) -> None:
    if min(width, depth) < THINNEST[s.lod]:
        return
    prism(s, finish, rect(width, depth, cx, cz), y0, y0 + height, top=top)


def beam(s: Sculpt, finish: Finish, a: Vec3, b: Vec3, width: float,
         depth: float | None = None) -> None:
    """A square member from a to b, open at both ends (they meet other members).

    Its sides are turned to the member's own vertical plane, so a leg of the
    Eiffel Tower shows the same face to the ground at every angle it leans.
    """
    depth = width if depth is None else depth
    if max(width, depth) < THINNEST[s.lod]:
        return
    axis = _sub(b, a)
    length = math.sqrt(_dot(axis, axis))
    if length < 1e-6:
        return
    t = _unit(axis)
    helper = (0.0, 1.0, 0.0) if abs(t[1]) < 0.95 else (1.0, 0.0, 0.0)
    u = _unit(_cross(t, helper))
    v = _unit(_cross(u, t))
    hu, hv = width * 0.5, depth * 0.5
    corners = [(hu, hv), (-hu, hv), (-hu, -hv), (hu, -hv)]
    ring_a = [tuple(a[k] + u[k] * cu + v[k] * cv for k in range(3)) for cu, cv in corners]
    ring_b = [tuple(b[k] + u[k] * cu + v[k] * cv for k in range(3)) for cu, cv in corners]
    mesh = s.mesh(finish)
    for i in range(4):
        j = (i + 1) % 4
        p, q, r, w = ring_a[i], ring_a[j], ring_b[j], ring_b[i]
        mid = tuple((p[k] + q[k]) * 0.5 - a[k] for k in range(3))
        normal = _cross(_sub(q, p), _sub(r, p))
        if _dot(normal, mid) < 0:
            p, q, r, w = q, p, w, r
        mesh.add_triangle(p, q, r)
        mesh.add_triangle(p, r, w)


def spike(s: Sculpt, finish: Finish, base: Vec3, tip: Vec3, width: float) -> None:
    """A four-sided point: a crown's ray, a pinnacle, a finial."""
    axis = _sub(tip, base)
    t = _unit(axis)
    helper = (0.0, 1.0, 0.0) if abs(t[1]) < 0.95 else (1.0, 0.0, 0.0)
    u = _unit(_cross(t, helper))
    v = _unit(_cross(u, t))
    h = width * 0.5
    ring = [tuple(base[k] + (u[k] * a + v[k] * b) * h for k in range(3))
            for a, b in ((1, 1), (-1, 1), (-1, -1), (1, -1))]
    mesh = s.mesh(finish)
    for i in range(4):
        p, q = ring[i], ring[(i + 1) % 4]
        normal = _cross(_sub(q, p), _sub(tip, p))
        mid = tuple((p[k] + q[k]) * 0.5 - base[k] for k in range(3))
        if _dot(normal, mid) < 0:
            p, q = q, p
        mesh.add_triangle(p, q, tip)


def slab(s: Sculpt, finish: Finish, outline: Sequence[Vec2], origin: Vec3,
         across: Vec3, up: Vec3, thickness: float, edge: Finish | None = None) -> None:
    """A flat outline in the plane (across, up) through `origin`, given
    `thickness` about that plane: a tablet, a pediment, a sleeve, a gable."""
    from .polygons import triangulate
    a, b = _unit(across), _unit(up)
    n = _unit(_cross(a, b))
    outline = positive(outline)
    half = thickness * 0.5

    def at(p: Vec2, side: float) -> Vec3:
        return tuple(origin[k] + a[k] * p[0] + b[k] * p[1] + n[k] * side for k in range(3))

    mesh = s.mesh(finish)
    for i, j, k in triangulate(list(outline)):
        # Positive in (across, up) means counter-clockwise seen from +n.
        mesh.add_triangle(at(outline[i], half), at(outline[j], half), at(outline[k], half))
        mesh.add_triangle(at(outline[i], -half), at(outline[k], -half), at(outline[j], -half))
    rim = s.mesh(edge or finish)
    for p, q in zip(outline, list(outline[1:]) + [outline[0]]):
        # The outline is counter-clockwise, so outward is to the right of p->q.
        rim.add_triangle(at(p, -half), at(q, -half), at(q, half))
        rim.add_triangle(at(p, -half), at(q, half), at(p, half))


def tube(s: Sculpt, finish: Finish, points: Sequence[Vec3], radii: Sequence[float],
         sides: int = 10, closed_ends: bool = True, squash: float = 1.0) -> None:
    """A smooth limb along a polyline: an arm, a torch, a mast, a horse's neck."""
    sides = sides_at(s.lod, sides, 4)
    rows = []
    frames = []
    for k, p in enumerate(points):
        prev_p = points[max(0, k - 1)]
        next_p = points[min(len(points) - 1, k + 1)]
        t = _unit(_sub(next_p, prev_p))
        helper = (0.0, 1.0, 0.0) if abs(t[1]) < 0.9 else (1.0, 0.0, 0.0)
        u = _unit(_cross(t, helper))
        v = _unit(_cross(u, t))
        frames.append((u, v))
        r = radii[k]
        rows.append([tuple(p[q] + (u[q] * math.cos(math.tau * i / sides)
                                   + v[q] * squash * math.sin(math.tau * i / sides)) * r
                           for q in range(3)) for i in range(sides)])
    if closed_ends:
        rows = [[tuple(points[0])] * sides] + rows + [[tuple(points[-1])] * sides]
        axis_points = [points[0]] + list(points) + [points[-1]]
    else:
        axis_points = list(points)

    def inside(c: Vec3) -> Vec3:
        # The nearest point of the axis polyline.
        best, where = float("inf"), axis_points[0]
        for a, b in zip(axis_points, axis_points[1:]):
            ab = _sub(b, a)
            length = _dot(ab, ab)
            t = 0.0 if length < 1e-12 else max(0.0, min(1.0, _dot(_sub(c, a), ab) / length))
            q = tuple(a[i] + ab[i] * t for i in range(3))
            d = _dot(_sub(c, q), _sub(c, q))
            if d < best:
                best, where = d, q
        return where

    grid(s, finish, rows, closed=True, inside=inside)


def ellipsoid(s: Sculpt, finish: Finish, centre: Vec3, radii: Vec3, rings: int = 8,
              sides: int = 14) -> None:
    # A unit sphere, stretched: normals scale by the inverse of the stretch.
    # It is judged by its real size, not the unit one: a head is not a finial.
    if s.lod and 2 * max(radii) < SLENDEREST[s.lod]:
        return
    unit = Sculpt(0)
    lathe(unit, finish, sphere_profile(1.0, 0.0, max(4, rings >> s.lod)),
          sides=sides_at(s.lod, sides, 6))
    s.merge(unit,
            lambda p: (centre[0] + p[0] * radii[0], centre[1] + p[1] * radii[1],
                       centre[2] + p[2] * radii[2]),
            lambda n: (n[0] / radii[0], n[1] / radii[1], n[2] / radii[2]))


# ── smooth surfaces ─────────────────────────────────────────────────────────

def grid(s: Sculpt, finish: Finish, rows: Sequence[Sequence[Vec3]], closed: bool = True,
         inside: Callable[[Vec3], Vec3] | None = None,
         uvs: Sequence[Sequence[Vec2]] | None = None, crease: float = 50.0,
         pattern: Callable[[int, int], Finish] | None = None,
         facing: Callable[[int, Vec3], Vec3] | None = None) -> None:
    """Rows of points, each row a ring (closed) or a strip, joined smoothly.

    `inside(p)` gives a point inside the surface near p; the grid faces away
    from it. Without it the grid faces away from each row's centroid, which is
    right for anything turned about its own axis. Normals are averaged across
    shared corners unless the faces meet at more than `crease` degrees, so a
    robe is smooth and the rim of a platform stays sharp.

    `facing(row, centre)`, when given, is the outward direction itself and
    wins over `inside`: a lathe knows it from its profile, which is the only
    reliable answer for a horizontal ring such as a balcony, where the
    direction to the axis is perpendicular to the face and says nothing.

    `pattern(row, column)` may give each face its own finish: the stripes of an
    onion dome in Moscow are a pattern, not a second dome.
    """
    rows = [list(r) for r in rows]
    count = len(rows[0])
    if s.lod:
        uvs = [[(0.0, 0.0)] * (count + 1) for _ in rows]
    if any(len(r) != count for r in rows):
        raise ValueError("every row of a grid needs the same number of points")
    columns = count if closed else count - 1
    if uvs is None:
        uvs = []
        for r in rows:
            u, line = 0.0, [(0.0, r[0][1])]
            for i in range(1, count + (1 if closed else 0)):
                u += math.dist(r[i - 1], r[i % count])
                line.append((u, r[i % count][1]))
            uvs.append(line)
    elif closed:
        uvs = [list(line) + [line[0]] for line in uvs] if len(uvs[0]) == count else uvs

    if inside is None:
        centres = []
        for r in rows:
            n = len(r)
            centres.append((sum(p[0] for p in r) / n, sum(p[1] for p in r) / n,
                            sum(p[2] for p in r) / n))

    faces = []   # (row, column, (pa, pb, pc), normal)
    for j in range(len(rows) - 1):
        for i in range(columns):
            k = (i + 1) % count
            a, b = rows[j][i], rows[j][k]
            c, d = rows[j + 1][k], rows[j + 1][i]
            for tri in ((a, b, c), (a, c, d)):
                normal = _cross(_sub(tri[1], tri[0]), _sub(tri[2], tri[0]))
                if _dot(normal, normal) < 1e-14:
                    continue
                centre = tuple(sum(p[q] for p in tri) / 3 for q in range(3))
                if facing is not None:
                    out = facing(j, centre)
                else:
                    ref = inside(centre) if inside else (
                        (centres[j][0] + centres[j + 1][0]) * 0.5, centre[1],
                        (centres[j][2] + centres[j + 1][2]) * 0.5)
                    out = _sub(centre, ref)
                faces.append((j, i, tri, normal, _dot(normal, out) < 0))

    # Each face's outward unit normal, then per-corner averages.
    accumulated: dict[Vec3, list[Vec3]] = {}
    oriented = []
    for j, i, tri, normal, flip in faces:
        if flip:
            tri = (tri[0], tri[2], tri[1])
            normal = (-normal[0], -normal[1], -normal[2])
        unit = _unit(normal)
        oriented.append((j, i, tri, unit))
        for p in tri:
            accumulated.setdefault(_round(p), []).append(unit)

    def corner(p: Vec3, face: Vec3) -> Vec3:
        limit = math.cos(math.radians(crease))
        near = [n for n in accumulated[_round(p)] if _dot(n, face) >= limit]
        total = (sum(n[0] for n in near), sum(n[1] for n in near), sum(n[2] for n in near))
        return _unit(total)

    def uv_of(p: Vec3, j: int, i: int) -> Vec2:
        # The corner's own row and column: find which of the quad's four it is.
        for jj, ii in ((j, i), (j, i + 1), (j + 1, i + 1), (j + 1, i)):
            if _round(rows[jj][ii % count]) == _round(p):
                return uvs[jj][ii]
        return 0.0, 0.0

    for j, i, tri, unit in oriented:
        finish_here = pattern(j, i) if pattern else finish
        s.mesh(finish_here).add_smooth_triangle(
            tuple(_round(p) for p in tri),
            tuple(corner(p, unit) for p in tri),
            tuple(uv_of(p, j, i) for p in tri))


def lathe(s: Sculpt, finish: Finish, profile: Sequence[tuple[float, float]],
          sides: int = 24, cx: float = 0.0, cz: float = 0.0, sx: float = 1.0,
          sz: float = 1.0, phase: float = 0.0, twist: float = 0.0, crease: float = 50.0,
          pattern: Callable[[int, int], Finish] | None = None, flutes: int = 0,
          flute_depth: float = 0.06) -> None:
    """A profile of (radius, height) pairs from the bottom up, turned about y.

    A radius of zero closes the surface at that end. `sx`/`sz` stretch the
    section into an ellipse; `twist` turns each row by that many radians per
    metre of height -- the spiral of a Moscow onion. `flutes` carves that
    many shallow channels down the shaft of a column, `flute_depth` deep as a
    fraction of the radius; it takes two sides per flute and is dropped from
    a distance, where a channel is narrower than a pixel.
    """
    if s.lod and 2 * max(r for r, _ in profile) * max(sx, sz) < SLENDEREST[s.lod]:
        return
    sides = sides_at(s.lod, sides, 6)
    if flutes and not s.lod:
        sides = max(sides, flutes * 2)
    else:
        flutes = 0
    if s.lod >= 2 and len(profile) > 4:
        # Across a city a profile is its outline: every other row, and the ends.
        profile = list(profile[:-1:2]) + [profile[-1]]
    rows = []
    for r, y in profile:
        turn = phase + twist * y
        row = []
        for i in range(sides):
            a = math.tau * i / sides
            k = 1.0 - flute_depth * (0.5 + 0.5 * math.cos(flutes * a)) if flutes else 1.0
            row.append((cx + sx * r * k * math.cos(turn + a), y,
                        cz + sz * r * k * math.sin(turn + a)))
        rows.append(row)
    def facing(j: int, centre: Vec3) -> Vec3:
        # The profile runs up the outside, so outward is to its right:
        # (dy, -dr) in the (radius, height) plane.
        (r0, y0), (r1, y1) = profile[j], profile[j + 1]
        dr, dy = r1 - r0, y1 - y0
        radial = _unit(((centre[0] - cx) / sx, 0.0, (centre[2] - cz) / sz))
        return (radial[0] * dy, -dr, radial[2] * dy)

    grid(s, finish, rows, closed=True, crease=crease, pattern=pattern, facing=facing)


def sphere_profile(radius: float, y0: float, rings: int = 8, start: float = -90.0,
                   end: float = 90.0) -> list[tuple[float, float]]:
    """(r, y) of a sphere between two latitudes, centred at height y0."""
    out = []
    for i in range(rings + 1):
        lat = math.radians(start + (end - start) * i / rings)
        out.append((max(0.0, radius * math.cos(lat)), y0 + radius * math.sin(lat)))
    return out


def onion_profile(radius: float, y0: float, height: float, neck: float = 0.55,
                  rings: int = 10) -> list[tuple[float, float]]:
    """A Russian onion: swelling past its drum, then drawn to a point."""
    out = [(radius * neck, y0)]
    for i in range(1, rings + 1):
        t = i / rings
        # A bulge that peaks at a third of the height and closes as a cusp.
        r = radius * (math.sin(math.pi * min(1.0, t * 1.5)) ** 0.8 if t < 1 / 1.5 * 0.5
                      else math.cos((t - 1 / 3) / (2 / 3) * math.pi / 2) ** 1.6)
        out.append((max(0.0, max(r, radius * neck * (1 - t) if t < 0.2 else 0.0)), y0 + height * t))
    out[-1] = (0.0, y0 + height)
    return out


# ── walls with openings ─────────────────────────────────────────────────────

@dataclass(frozen=True)
class Opening:
    """A doorway or window: [left, right] along the wall, from `sill` up to
    `spring`, then an arch of `rise` over it -- round when rise is half the
    width, pointed when more, a segment when less, flat when zero."""

    left: float
    right: float
    sill: float
    spring: float
    rise: float = 0.0
    segments: int = 6

    def outline(self) -> list[Vec2]:
        """The top edge from left to right, x-monotone by construction."""
        w = self.right - self.left
        if self.rise <= 1e-6:
            return [(self.left, self.spring), (self.right, self.spring)]
        half = w * 0.5
        mid = self.left + half
        points = []
        n = self.segments
        if self.rise > half + 1e-6:
            # Pointed: two arcs of radius R centred on the springing line so
            # they meet at `rise` above the middle.
            radius = (half * half + self.rise * self.rise) / (2 * half)
            for i in range(n + 1):
                t = i / n
                if t <= 0.5:
                    cx = self.left + radius
                    angle = math.pi - math.acos(max(-1, min(1, (radius - half * t * 2) / radius)))
                    x = self.left + half * t * 2
                    y = self.spring + math.sqrt(max(0.0, radius ** 2 - (cx - x) ** 2))
                else:
                    cx = self.right - radius
                    x = mid + half * (t - 0.5) * 2
                    y = self.spring + math.sqrt(max(0.0, radius ** 2 - (x - cx) ** 2))
                points.append((x, min(y, self.spring + self.rise)))
            points[n // 2] = (mid, self.spring + self.rise)
            return points
        for i in range(n + 1):
            angle = math.pi * (1 - i / n)
            points.append((mid + half * math.cos(angle),
                           self.spring + self.rise * math.sin(angle)))
        return points


class Face:
    """A vertical plane: origin on the ground, `along` horizontal, facing
    `(−along.z, along.x)` -- its left when walked along."""

    def __init__(self, origin: Vec2, along: Vec2, base: float = 0.0) -> None:
        length = math.hypot(*along)
        self.o = origin
        self.u = (along[0] / length, along[1] / length)
        self.n = (-self.u[1], self.u[0])
        self.base = base

    @classmethod
    def of_edge(cls, p0: Vec2, p1: Vec2, base: float = 0.0) -> "Face":
        """The outward face of a positive ring's edge p0 -> p1, origin at p0,
        measured along the edge: the face runs p1 -> p0 in `Face`'s own
        convention, so `along` is negated and s is counted from p0 inward."""
        return cls(p1, (p0[0] - p1[0], p0[1] - p1[1]), base)

    def at(self, s: float, t: float, depth: float = 0.0) -> Vec3:
        return (self.o[0] + self.u[0] * s - self.n[0] * depth,
                self.base + t,
                self.o[1] + self.u[1] * s - self.n[1] * depth)


def _face_quad(mesh: Mesh, f: Face, s0: float, t0: float, s1: float, t1: float,
               depth: float = 0.0, s0t1: float | None = None, s1t1: float | None = None,
               back: bool = False) -> None:
    top0 = t1 if s0t1 is None else s0t1
    top1 = t1 if s1t1 is None else s1t1
    a, b = f.at(s0, t0, depth), f.at(s1, t0, depth)
    c, d = f.at(s1, top1, depth), f.at(s0, top0, depth)
    if back:
        a, b, c, d = b, a, d, c
    if abs(top1 - t0) > 1e-9 or abs(top0 - t0) > 1e-9:
        if math.dist(a, b) > 1e-9:
            mesh.add_triangle(a, b, c)
        if math.dist(c, d) > 1e-9:
            mesh.add_triangle(a, c, d)


def panel(s: Sculpt, finish: Finish, face: Face, s0: float, s1: float, t0: float,
          t1: float, openings: Iterable[Opening] = (), depth: float = 0.6,
          through: bool = False, recess: Finish | None = None, back: bool = True) -> None:
    """The wall [s0, s1] x [t0, t1] of `face`, with openings.

    Each opening's reveal is `depth` deep. `through` opens it right through a
    wall of that thickness and builds the wall's back face too (an arcade);
    otherwise `recess` closes it at the back (a niche, a blind arch, a window
    read as dark glass), unless `back` is false: a passage that runs on into
    a space another panel has built. Openings must not overlap along the wall;
    stack them in separate panels.
    """
    mesh = s.mesh(finish)
    cursor = s0
    if s.lod:
        openings = [Opening(o.left, o.right, o.sill, o.spring, o.rise,
                            max(2, o.segments >> s.lod))
                    for o in openings if o.right - o.left >= NARROWEST[s.lod]]
    for o in sorted(openings, key=lambda o: o.left):
        faces = [(face, False)] + ([(face, True)] if through else [])
        top = o.outline()
        for f, rear in faces:
            d = depth if rear else 0.0
            _face_quad(mesh, f, cursor, t0, o.left, t1, d, back=rear)
            if o.sill > t0 + 1e-6:
                _face_quad(mesh, f, o.left, t0, o.right, o.sill, d, back=rear)
            # The springing zone, then a strip above each segment of the arch.
            for (xa, ya), (xb, yb) in zip(top, top[1:]):
                _face_quad(mesh, f, xa, ya, xb, t1, d, s0t1=t1, s1t1=t1, back=rear) \
                    if abs(ya - yb) < 1e-9 else _spandrel(mesh, f, xa, ya, xb, yb, t1, d, rear)
        # The reveal, facing into the opening.
        outline = [(o.left, o.sill)] + top + [(o.right, o.sill)]
        for (xa, ya), (xb, yb) in zip(outline, outline[1:]):
            a, b = face.at(xa, ya, 0.0), face.at(xb, yb, 0.0)
            c, d_ = face.at(xb, yb, depth), face.at(xa, ya, depth)
            mesh.add_triangle(a, c, b)
            mesh.add_triangle(a, d_, c)
        if not through and back:
            # Sill and back plate.
            a, b = face.at(o.left, o.sill, 0.0), face.at(o.right, o.sill, 0.0)
            c, d_ = face.at(o.right, o.sill, depth), face.at(o.left, o.sill, depth)
            mesh.add_up_quad(a, b, c, d_)
            back_mesh = s.mesh(recess or finish)
            points = [face.at(x, y, depth) for x, y in outline]
            centre = face.at((o.left + o.right) * 0.5, o.sill, depth)
            facing = (face.n[0], 0.0, face.n[1])
            for p, q in zip(points, points[1:]):
                if _dot(_cross(_sub(p, centre), _sub(q, centre)), facing) < 0:
                    p, q = q, p
                back_mesh.add_triangle(centre, p, q)
        cursor = o.right
    for f, rear in [(face, False)] + ([(face, True)] if through else []):
        _face_quad(mesh, f, cursor, t0, s1, t1, depth if rear else 0.0, back=rear)


def _spandrel(mesh: Mesh, f: Face, xa: float, ya: float, xb: float, yb: float,
              top: float, depth: float, back: bool) -> None:
    a, b = f.at(xa, ya, depth), f.at(xb, yb, depth)
    c, d = f.at(xb, top, depth), f.at(xa, top, depth)
    tris = [(a, b, c), (a, c, d)]
    facing = (f.n[0], 0.0, f.n[1]) if not back else (-f.n[0], 0.0, -f.n[1])
    for p, q, r in tris:
        normal = _cross(_sub(q, p), _sub(r, p))
        if _dot(normal, normal) < 1e-14:
            continue
        if _dot(normal, facing) < 0:
            q, r = r, q
        mesh.add_triangle(p, q, r)


def arcade(width: float, count: int, sill: float, spring: float, pier: float,
           rise: float | None = None, start: float = 0.0, segments: int = 6) -> list[Opening]:
    """`count` equal arches across `width`, piers of `pier` between and at the ends."""
    bay = width / count
    span = bay - pier
    rise = span * 0.5 if rise is None else rise
    return [Opening(start + i * bay + pier * 0.5, start + (i + 1) * bay - pier * 0.5,
                    sill, spring, rise, segments) for i in range(count)]
