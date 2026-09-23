"""The recipes: each landmark described in the vocabulary of `sculpt.py`.

A recipe says what the building *is*, in the numbers its builders or its
surveyors published, and draws nothing a player could not check against a
photograph. Where a dimension is known it is written with its source in a
comment; where the shape between known dimensions is interpolated, the recipe
says so.

**Colours are albedos** (CLAUDE.md rule 2), measured or estimated from the
material's published reflectance, never painted to look right on a screen:
Eiffel's bronze paint is darker than it looks in photographs, and the Taj
Mahal's marble is held under the ~0.35 at which this world's light saturates.
A finish with a `family` wears one of the project's own photographed textures
at its real size (`surfaces.py`), averaged to that albedo.

Every recipe builds in its own frame: y up, the anchor at the origin, +x along
the axis named in its docstring. `landmarks.py` turns it to the measured
bearing.
"""

from __future__ import annotations

import math

from .sculpt import (
    Face, Finish, Opening, Sculpt, arcade, beam, box, cap, chamfered, ellipse, ellipsoid,
    grid, lathe, loft, ngon, panel, positive, prism, rect, slab, sphere_profile, spike,
    star, tube,
)

# ── finishes ────────────────────────────────────────────────────────────────

# "Brun Tour Eiffel", the 2010s-2020s paint: a dark bronze, ~0.08 luminance.
EIFFEL_IRON = Finish("Eiffel wrought iron, bronze paint", (0.105, 0.078, 0.056), 0.6)
DARK_GLASS = Finish("Glazing", (0.035, 0.045, 0.055), 0.12)


def _interp(table, y: float) -> float:
    """Piecewise-linear in log space: towers taper geometrically, not linearly."""
    if y <= table[0][0]:
        return table[0][1]
    for (y0, v0), (y1, v1) in zip(table, table[1:]):
        if y <= y1:
            t = (y - y0) / (y1 - y0)
            return math.exp(math.log(v0) * (1 - t) + math.log(v1) * t)
    return table[-1][1]


# ── Tour Eiffel ─────────────────────────────────────────────────────────────

# Recipe +x runs along one face; the tower is square, so either face will do.
# Measured on OSM way 5013364: faces at 44.1° and 134.1°.
EIFFEL_BEARING = 44.1

# Outer half-width of the tower, metres, against height. Base 124.9 m between
# the outer corners of the pillars; first floor 57.64 m, second 115.73 m, third
# 276.13 m (SETE). Between them the curve is interpolated geometrically.
_EIFFEL_HALF = ((0.0, 62.45), (57.64, 35.2), (115.73, 20.6), (195.0, 13.0),
                (276.13, 8.4), (290.0, 6.2), (300.0, 4.2))
# Width of one pillar: 25 m square at its foot, and at the second floor the
# four pillars meet at the axis, so the width there is the half-width.
_EIFFEL_PILLAR = ((0.0, 25.0), (57.64, 22.2), (115.73, 20.6))


def _lattice_legs(s: Sculpt, half, pillar, levels, finish_at, chord0: float = 1.7) -> None:
    """Four lattice pillars at the corners of a square tower, each a box of
    four chords braced on all four faces, leaning in along `half` and
    narrowing along `pillar` until they meet. Beyond a few hundred metres the
    lattice is finer than a pixel and each pillar is drawn as the solid it
    reads as."""
    if s.lod:
        _solid_legs(s, half, pillar, _every(levels, s.lod + 1), finish_at)
        return
    for qx in (-1, 1):
        for qz in (-1, 1):
            rings = []
            for y in levels:
                w = _interp(half, y)
                p = min(_interp(pillar, y), w)
                inner = w - p
                rings.append([(qx * w, y, qz * w), (qx * inner, y, qz * w),
                              (qx * inner, y, qz * inner), (qx * w, y, qz * inner)])
            for k, (lower, upper) in enumerate(zip(rings, rings[1:])):
                finish = finish_at((lower[0][1] + upper[0][1]) * 0.5)
                chord = chord0 - chord0 * 0.53 * k / len(levels)
                for c in range(4):
                    beam(s, finish, lower[c], upper[c], chord)
                    # The two diagonals of each of the pillar's four faces:
                    # the lattice that is the whole reason this is not a box.
                    d = (c + 1) % 4
                    beam(s, finish, lower[c], upper[d], chord * 0.32)
                    beam(s, finish, lower[d], upper[c], chord * 0.32)
                    beam(s, finish, upper[c], upper[d], chord * 0.35)


def _lattice_shaft(s: Sculpt, half, levels, finish_at, chord0: float = 2.2,
                   inner: float = 0.7) -> None:
    """One square shaft of four corner columns, braced across each face;
    each corner column is itself a lattice box (its inner chord at `inner`)."""
    if s.lod:
        kept = _every(levels, s.lod + 1)
        for y0, y1 in zip(kept, kept[1:]):
            w0, w1 = _interp(half, y0), _interp(half, y1)
            loft(s, finish_at((y0 + y1) * 0.5), [(rect(2 * w0, 2 * w0), y0), (rect(2 * w1, 2 * w1), y1)],
                 top=False)
        return
    corners = []
    for y in levels:
        w = _interp(half, y)
        corners.append([(w, y, w), (-w, y, w), (-w, y, -w), (w, y, -w)])
    for k, (lower, upper) in enumerate(zip(corners, corners[1:])):
        finish = finish_at((lower[0][1] + upper[0][1]) * 0.5)
        chord = chord0 - chord0 * 0.55 * k / len(levels)
        for c in range(4):
            d = (c + 1) % 4
            beam(s, finish, lower[c], upper[c], chord)
            beam(s, finish, lower[c], upper[d], chord * 0.2 + 0.2)
            beam(s, finish, lower[d], upper[c], chord * 0.2 + 0.2)
            beam(s, finish, upper[c], upper[d], chord * 0.22 + 0.2)
            if inner:
                ci = (lower[c][0] * inner, lower[c][1], lower[c][2] * inner)
                ui = (upper[c][0] * inner, upper[c][1], upper[c][2] * inner)
                beam(s, finish, ci, ui, chord * 0.6)
                beam(s, finish, lower[c], ui, chord * 0.16 + 0.1)


def _every(levels, step: int) -> list:
    """Every `step`-th level, always keeping the last."""
    kept = list(levels[::step])
    if kept[-1] != levels[-1]:
        kept.append(levels[-1])
    return kept


def _solid_legs(s: Sculpt, half, pillar, levels, finish_at) -> None:
    for qx in (-1, 1):
        for qz in (-1, 1):
            rings = []
            for y in levels:
                w = _interp(half, y)
                inner = w - min(_interp(pillar, y), w)
                rings.append((positive([(qx * w, qz * w), (qx * inner, qz * w),
                                        (qx * inner, qz * inner), (qx * w, qz * inner)]), y))
            for lower, upper in zip(rings, rings[1:]):
                loft(s, finish_at((lower[1] + upper[1]) * 0.5), [lower, upper], top=False)


def _eiffel_legs(s: Sculpt) -> None:
    _lattice_legs(s, _EIFFEL_HALF, _EIFFEL_PILLAR,
                  [0.0, 6.0, 12.5, 19.5, 27.0, 35.0, 43.5, 51.0, 57.64,
                   66.0, 75.0, 84.5, 94.5, 105.0, 115.73], lambda y: EIFFEL_IRON)


def _eiffel_arches(s: Sculpt) -> None:
    """The four great arches under the first floor, one on each face.

    Decorative and structural only in appearance, and the most recognisable
    line of the tower after its profile: apex just under the first-floor
    girders, springing from the inner edges of the pillars.
    """
    segments = 14
    for side in range(4):
        angle = side * math.pi / 2
        c, sn = math.cos(angle), math.sin(angle)

        def put(x: float, y: float, z: float):
            return (x * c - z * sn, y, x * sn + z * c)

        def face_z(y: float) -> float:
            return _interp(_EIFFEL_HALF, y) - 0.8

        spring_y, apex_y = 14.0, 39.0
        for ring_offset in (0.0, 4.2):
            points = []
            for i in range(segments + 1):
                t = math.pi * i / segments
                y = spring_y + ring_offset + (apex_y - spring_y) * math.sin(t)
                half = _interp(_EIFFEL_HALF, spring_y) - _interp(_EIFFEL_PILLAR, spring_y)
                x = half * math.cos(t) * (1.0 - 0.05 * ring_offset)
                points.append(put(x, y, face_z(y)))
            for a, b in zip(points, points[1:]):
                beam(s, EIFFEL_IRON, a, b, 1.2, 1.6)
            if ring_offset:
                # Spandrel posts from the outer arch up to the first floor.
                for i in range(1, segments, 2):
                    a = points[i]
                    along = a[0] * c + a[2] * sn   # the post's place along the face
                    beam(s, EIFFEL_IRON, a, put(along, 50.5, face_z(50.5)), 0.5)


def _eiffel_floor(s: Sculpt, y: float, skirt: float, overhang: float, opening: float,
                  pavilion: float, arches: int) -> None:
    """A platform: a square frame of girders with an arcaded skirt round it,
    and the glazed pavilions set back on its deck."""
    half = _interp(_EIFFEL_HALF, y) + overhang
    inner = half - opening
    bottom, top = y - skirt, y + 0.4
    for face in _square_faces(half, bottom):
        panel(s, EIFFEL_IRON, face, 0.0, 2 * half, 0.0, top - bottom,
              arcade(2 * half, arches, 0.9, skirt - 2.2, 2 * half / arches * 0.3, rise=1.0),
              depth=0.8, recess=DARK_GLASS)
    _square_frame(s, EIFFEL_IRON, half, max(inner, 0.0), bottom, top)
    if pavilion > 0:
        _square_frame(s, DARK_GLASS, half - 2.5, half - 9.5, top, top + pavilion,
                      roof=EIFFEL_IRON, outer_walls=True)


def _square_frame(s: Sculpt, finish: Finish, half: float, inner: float, y0: float, y1: float,
                  roof: Finish | None = None, outer_walls: bool = False) -> None:
    """A square ring between `inner` and `half`: its deck, its soffit, its
    inner walls, and its outer walls only when no panel supplies them."""
    from .sculpt import _wall
    outer_ring = rect(2 * half, 2 * half)
    inner_ring = rect(2 * inner, 2 * inner) if inner > 0 else None
    mesh = s.mesh(finish)
    for i in range(4):
        j = (i + 1) % 4
        if outer_walls:
            _wall(mesh, outer_ring[i], outer_ring[j], y0, y1)
        if inner_ring is None:
            continue
        # Inner walls face the opening: walk the inner ring backwards.
        _wall(mesh, inner_ring[j], inner_ring[i], y0, y1)
    for y, up, target in ((y1, True, s.mesh(roof or finish)), (y0, False, mesh)):
        if inner_ring is None:
            cap(target, outer_ring, y, up=up)
            continue
        for i in range(4):
            j = (i + 1) % 4
            quad = [outer_ring[i], outer_ring[j], inner_ring[j], inner_ring[i]]
            cap(target, quad, y, up=up)


def _rot(x: float, z: float, c: float, sn: float):
    return (x * c - z * sn, x * sn + z * c)


def _eiffel_shaft(s: Sculpt) -> None:
    """Above the second floor the four pillars carry on as the four corner
    columns of one square shaft, braced across each face."""
    _lattice_shaft(s, _EIFFEL_HALF, [115.73, 125.0, 135.0, 146.0, 158.0, 171.0, 185.0,
                                     200.0, 215.0, 230.0, 245.0, 260.0, 272.0],
                   lambda y: EIFFEL_IRON)


def eiffel_tower() -> Sculpt:
    """330 m to the antenna. Three floors, four pillars, one lattice."""
    s = Sculpt()
    # Masonry footings under each pillar, into the ground so a slope never
    # shows daylight beneath them.
    for qx in (-1, 1):
        for qz in (-1, 1):
            c = 62.45 - 12.5
            box(s, Finish("Eiffel pillar footing, limestone", (0.27, 0.25, 0.21), 0.9, family="stone"),
                qx * c, -3.0, qz * c, 26.0, 4.0, 26.0)
    _eiffel_legs(s)
    _eiffel_arches(s)
    _eiffel_floor(s, 57.64, skirt=6.5, overhang=1.8, opening=14.0, pavilion=4.0, arches=15)
    _eiffel_floor(s, 115.73, skirt=4.0, overhang=1.8, opening=20.6 + 1.8 - 0.01, pavilion=0.0,
                  arches=9)
    # The second floor deck is closed: the pillars meet under it.
    half = _interp(_EIFFEL_HALF, 115.73) + 1.8
    cap(s.mesh(EIFFEL_IRON), [(-half, -half), (half, -half), (half, half), (-half, half)],
        116.15)
    prism(s, DARK_GLASS, rect(20.0, 20.0), 116.15, 120.0, roof=EIFFEL_IRON)
    _eiffel_shaft(s)
    # The third floor: the closed cabin, its open deck above, the top.
    prism(s, EIFFEL_IRON, rect(19.0, 19.0), 272.0, 273.2)
    prism(s, DARK_GLASS, rect(16.6, 16.6), 273.2, 278.5, top=False)
    prism(s, EIFFEL_IRON, rect(18.2, 18.2), 278.5, 279.6)
    loft(s, EIFFEL_IRON, [(rect(12.0, 12.0), 279.6), (rect(9.0, 9.0), 287.0),
                          (rect(6.0, 6.0), 296.0)])
    lathe(s, EIFFEL_IRON, [(2.4, 296.0), (2.4, 300.0), (1.8, 300.0), (1.3, 312.0),
                           (0.55, 312.0), (0.35, 325.0), (0.12, 330.0), (0.0, 330.0)],
          sides=8, crease=30.0)
    return s


# ── shared finishes and helpers ─────────────────────────────────────────────

SHADOW = Finish("Shadowed interior", (0.018, 0.018, 0.02), 1.0)
GILT = Finish("Gold leaf", (0.36, 0.26, 0.07), 0.35, metallic=0.8)
LEAD = Finish("Lead sheet roofing", (0.12, 0.13, 0.145), 0.55, family="metal_seam")


def _put(x: float, y: float, z: float, angle: float):
    """(x, y, z) turned `angle` radians about the vertical axis."""
    c, s = math.cos(angle), math.sin(angle)
    return (x * c - z * s, y, x * s + z * c)


def _square_faces(half: float, base: float = 0.0):
    """The four outward faces of a square footprint, each run left to right
    as seen from outside, starting with the one on +z."""
    faces = []
    for a in (0.0, -math.pi / 2, math.pi, math.pi / 2):
        c, sn = math.cos(a), math.sin(a)
        faces.append(Face(_rot(-half, half, c, sn), _rot(1.0, 0.0, c, sn), base))
    return faces


def _ring_faces(ring, base: float = 0.0):
    """The outward faces of a ring, with their lengths."""
    ring = positive(ring)
    out = []
    for i in range(len(ring)):
        p0, p1 = ring[i], ring[(i + 1) % len(ring)]
        out.append((Face.of_edge(p0, p1, base), math.dist(p0, p1)))
    return out


def _outward(p0, p1, base: float = 0.0, centre=(0.0, 0.0)) -> Face:
    """The face along p0 -> p1 turned away from `centre`."""
    face = Face(p0, (p1[0] - p0[0], p1[1] - p0[1]), base)
    mid = ((p0[0] + p1[0]) * 0.5 - centre[0], (p0[1] + p1[1]) * 0.5 - centre[1])
    if face.n[0] * mid[0] + face.n[1] * mid[1] < 0:
        face = Face(p1, (p0[0] - p1[0], p0[1] - p1[1]), base)
    return face


def _disc(s: Sculpt, finish: Finish, face: Face, sc: float, tc: float, radius: float,
          out: float, sides: int = 20) -> None:
    """A flat round on a wall, standing `out` proud of it: a dial, a rose."""
    centre = face.at(sc, tc, -out)
    ring = [face.at(sc + radius * math.cos(math.tau * i / sides),
                    tc + radius * math.sin(math.tau * i / sides), -out) for i in range(sides)]
    mesh = s.mesh(finish)
    n = (face.n[0], 0.0, face.n[1])
    for p, q in zip(ring, ring[1:] + ring[:1]):
        normal = _cross3(_sub3(p, centre), _sub3(q, centre))
        if normal[0] * n[0] + normal[2] * n[2] > 0:
            mesh.add_triangle(centre, p, q)
        else:
            mesh.add_triangle(centre, q, p)


def _sub3(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _cross3(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def _gable(s: Sculpt, finish: Finish, centre, along, width: float, y0: float,
           rise: float, thickness: float) -> None:
    """A triangular gable or pediment standing on `y0`, facing across `along`."""
    slab(s, finish, [(-width * 0.5, 0.0), (width * 0.5, 0.0), (0.0, rise)],
         (centre[0], y0, centre[1]), (along[0], 0.0, along[1]), (0.0, 1.0, 0.0), thickness)


def _pitched(s: Sculpt, finish: Finish, x0: float, x1: float, half: float, eave: float,
             ridge: float, cz: float = 0.0, hips: bool = False) -> None:
    """A roof along x over [x0, x1] x [cz - half, cz + half]."""
    inset = (ridge - eave) if hips else 0.0
    inset = min(inset, (x1 - x0) * 0.5 - 0.01) if hips else 0.0
    loft(s, finish, [(rect(x1 - x0, 2 * half, (x0 + x1) * 0.5, cz), eave),
                     (rect(x1 - x0 - 2 * inset, 0.02, (x0 + x1) * 0.5, cz), ridge)], top=False)


def _plaque(s: Sculpt, finish: Finish, face: Face, s0: float, s1: float, t0: float, t1: float,
            out: float) -> None:
    """A flat rectangle standing `out` proud of a wall: an inlay, a relief
    panel, a band of lettering read from the street."""
    a, b = face.at(s0, t0, -out), face.at(s1, t0, -out)
    c, d = face.at(s1, t1, -out), face.at(s0, t1, -out)
    mesh = s.mesh(finish)
    n = (face.n[0], 0.0, face.n[1])
    normal = _cross3(_sub3(b, a), _sub3(c, a))
    if normal[0] * n[0] + normal[2] * n[2] < 0:
        b, d = d, b
    mesh.add_quad(a, b, c, d)


def _rect_frame(s: Sculpt, finish: Finish, width: float, depth: float, band: float,
                y0: float, y1: float) -> None:
    """A rectangular ring of solid `band` wide: an entablature with no roof
    inside it. Its outer walls are left to panels; its inner walls, deck and
    soffit are drawn here."""
    from .sculpt import _wall
    outer = rect(width, depth)
    inner = rect(width - 2 * band, depth - 2 * band)
    mesh = s.mesh(finish)
    for i in range(4):
        j = (i + 1) % 4
        _wall(mesh, inner[j], inner[i], y0, y1)
        quad = [outer[i], outer[j], inner[j], inner[i]]
        cap(mesh, quad, y1, up=True)
        cap(mesh, quad, y0, up=False)


def _doric_frieze(s: Sculpt, finish: Finish, width: float, depth: float, y0: float,
                  architrave: float, frieze: float, cornice: float, bays_long: int,
                  bays_short: int) -> None:
    """Architrave, triglyphs and metopes, cornice, round a rectangle: two
    metopes to a bay, the metopes set back between the triglyphs."""
    for face, length in _ring_faces(rect(width, depth), y0):
        bays = bays_long if length > (width + depth) / 2 else bays_short
        panel(s, finish, face, 0.0, length, 0.0, architrave)
        metopes = 2 * bays
        glyph = length / metopes * 0.4
        panel(s, finish, face, 0.0, length, architrave, architrave + frieze,
              arcade(length, metopes, architrave + 0.12, architrave + frieze - 0.12, glyph,
                     rise=0.0), depth=0.14, recess=finish)
        panel(s, finish, face, 0.0, length, architrave + frieze,
              architrave + frieze + cornice)
    # The cornice's projecting lip -- a ring, not a lid: the Parthenon has had
    # no roof since 1687.
    top = y0 + architrave + frieze + cornice
    prism(s, finish, rect(width + 0.7, depth + 0.7), top - 0.35, top, top=False)
    _rect_frame(s, finish, width + 0.7, depth + 0.7, 0.35 + min(width, depth) * 0.08,
                top - 0.35, top)


# ── Statue of Liberty ───────────────────────────────────────────────────────

VERDIGRIS = Finish("Copper, verdigris patina", (0.16, 0.29, 0.24), 0.7)
STONY_CREEK = Finish("Stony Creek granite", (0.26, 0.22, 0.20), 0.85, family="stone")
FORT_GRANITE = Finish("Fort Wood granite", (0.22, 0.21, 0.19), 0.9, family="stone")


def statue_of_liberty() -> Sculpt:
    """93 m from the ground to the torch. Recipe +x is the way she faces.

    The eleven-pointed star of Fort Wood, the concrete foundation rising out
    of it, Hunt's granite pedestal (27.1 m), then Bartholdi's copper: 46.05 m
    from heel to torch, 33.99 m heel to crown, a 12.8 m raised right arm, a
    7.19 m tablet in the left. Her right is +z.
    """
    s = Sculpt()
    prism(s, FORT_GRANITE, star(47.5, 36.0, 11, phase=math.pi / 2), -3.0, 6.2)
    loft(s, STONY_CREEK, [(rect(27.4, 27.4), 6.2), (rect(24.0, 24.0), 17.0)])
    prism(s, STONY_CREEK, rect(21.2, 21.2), 17.0, 19.8)
    # The pedestal: plinth, die with its loggias, cornice, attic, parapet.
    loft(s, STONY_CREEK, [(rect(19.0, 19.0), 19.8), (rect(18.2, 18.2), 24.0)])
    for face in _square_faces(15.9 * 0.5, 24.0):
        panel(s, STONY_CREEK, face, 0.0, 15.9, 0.0, 14.5,
              arcade(15.9, 3, 8.8, 12.6, 1.8, rise=0.0), depth=1.1, recess=SHADOW)
    prism(s, STONY_CREEK, rect(17.6, 17.6), 38.5, 40.5)
    loft(s, STONY_CREEK, [(rect(14.6, 14.6), 40.5), (rect(13.8, 13.8), 45.6)], top=False)
    prism(s, STONY_CREEK, rect(14.8, 14.8), 45.6, 46.9)

    # The robe, as sections from the hem up: (height, front-back, side-side,
    # forward shift). The folds are a ripple on the section, deepest at the hem.
    sections = [(46.9, 5.6, 6.6, 0.0), (48.5, 5.0, 5.9, 0.2), (53.0, 4.4, 5.3, 0.3),
                (58.0, 3.9, 4.9, 0.3), (62.5, 3.55, 4.55, 0.2), (66.0, 3.4, 4.45, 0.1),
                (69.5, 3.35, 4.6, 0.0), (72.5, 3.1, 4.9, -0.1), (74.4, 2.6, 4.3, -0.1),
                (75.3, 1.6, 2.0, 0.0), (76.4, 1.3, 1.3, 0.1)]
    rows = []
    for y, rx, rz, shift in sections:
        depth = 0.07 * max(0.0, (74.0 - y) / 27.0)
        row = []
        for i in range(28):
            a = math.tau * i / 28
            ripple = 1.0 + depth * math.sin(9 * a + 0.35 * y) + depth * 0.5 * math.sin(17 * a)
            row.append((shift + rx * ripple * math.cos(a), y, rz * ripple * math.sin(a)))
        rows.append(row)
    grid(s, VERDIGRIS, rows, inside=lambda p: (0.0, p[1], 0.0))
    ellipsoid(s, VERDIGRIS, (0.35, 78.5, 0.0), (2.45, 2.75, 2.25), rings=8, sides=14)
    # The nose, the knot of hair at the nape, the stola over the left shoulder.
    ellipsoid(s, VERDIGRIS, (2.75, 78.3, 0.0), (0.45, 0.7, 0.35), rings=4, sides=6)
    ellipsoid(s, VERDIGRIS, (-1.9, 77.4, 0.0), (1.0, 1.1, 1.2), rings=5, sides=8)
    tube(s, VERDIGRIS, [(2.9, 73.5, -3.7), (3.5, 68.0, -0.5), (3.8, 61.0, 3.2)],
         [0.7, 0.8, 0.9], sides=8, squash=0.45)
    # The diadem and its seven rays, fanned across the front and tilted up.
    lathe(s, VERDIGRIS, [(2.55, 79.7), (2.7, 80.3), (2.5, 80.9)], sides=14, cx=0.35)
    # The windows of the crown's observation deck.
    for k in range(9):
        a = math.radians(-60 + 15 * k)
        centre = (0.35 + 2.72 * math.cos(a), 80.3, 2.72 * math.sin(a))
        ellipsoid(s, SHADOW, centre, (0.12, 0.22, 0.12), rings=3, sides=5)
    for k in range(7):
        a = math.radians(-78 + 26 * k)
        up = math.radians(22)
        d = (math.cos(a) * math.cos(up), math.sin(up), math.sin(a) * math.cos(up))
        base = (0.35 + 2.5 * d[0], 80.3, 2.5 * d[2])
        spike(s, VERDIGRIS, base, (base[0] + 3.0 * d[0], base[1] + 3.0 * d[1],
                                   base[2] + 3.0 * d[2]), 0.75)
    # The raised right arm, the hand, the torch and its gilded flame.
    tube(s, VERDIGRIS, [(0.0, 72.2, 3.9), (0.25, 76.0, 4.7), (0.45, 80.0, 5.3),
                        (0.6, 84.4, 5.7)], [1.75, 1.35, 1.0, 0.75], sides=10)
    tube(s, VERDIGRIS, [(0.6, 84.2, 5.7), (0.65, 88.6, 5.8)], [0.5, 0.45], sides=8)
    lathe(s, VERDIGRIS, [(0.4, 88.4), (1.7, 89.1), (1.75, 89.8), (1.2, 89.9)],
          sides=12, cx=0.65, cz=5.8)
    lathe(s, GILT, [(1.1, 89.9), (1.05, 91.0), (0.6, 92.3), (0.0, 93.0)],
          sides=10, cx=0.65, cz=5.8)
    # The left arm, bent, holding the tablet against her side.
    tube(s, VERDIGRIS, [(0.0, 72.4, -3.9), (0.6, 68.2, -5.0), (2.0, 66.2, -4.6)],
         [1.3, 0.95, 0.7], sides=10)
    tilt = math.radians(14.0)
    slab(s, VERDIGRIS, [(-2.07, -3.6), (2.07, -3.6), (2.07, 3.6), (-2.07, 3.6)],
         (1.6, 65.4, -5.35), (1.0, 0.0, 0.25), (0.0, math.cos(tilt), -math.sin(tilt)), 0.61)
    return s


# ── Big Ben (Elizabeth Tower) ───────────────────────────────────────────────

ANSTON = Finish("Anston limestone", (0.30, 0.27, 0.20), 0.9, family="stone")
CLOCK_DIAL = Finish("Pot opal glass dial", (0.34, 0.33, 0.29), 0.3)
CAST_IRON = Finish("Painted cast iron", (0.05, 0.06, 0.075), 0.5)


def big_ben() -> Sculpt:
    """96 m. A stone shaft to the clock stage, the four 7 m dials centred at
    55 m... 62 m, the belfry, then Pugin's cast-iron spire. Recipe +x is
    along one face; OSM way 123557148 puts the faces at 6.3° and 96.3°."""
    s = Sculpt()
    for qx in (-1, 1):
        for qz in (-1, 1):
            box(s, ANSTON, qx * 5.9, -3.0, qz * 5.9, 1.5, 58.0, 1.5)
    # Tall pointed lancets, three to a face, in six tiers: the vertical grain
    # that makes the shaft read as Gothic from across the river.
    for face in _square_faces(6.1):
        panel(s, ANSTON, face, 0.0, 12.2, -3.0, 7.0,
              [Opening(4.6, 7.6, 0.0, 4.2, 1.5, 5)], depth=0.9, recess=SHADOW)
        for tier in range(6):
            t0 = 7.0 + tier * 7.6
            panel(s, ANSTON, face, 0.0, 12.2, t0, t0 + 7.6,
                  arcade(12.2, 3, t0 + 1.4, t0 + 5.2, 1.9, rise=1.6, segments=4),
                  depth=0.45, recess=SHADOW)
        panel(s, ANSTON, face, 0.0, 12.2, 52.6, 55.0)
    # The clock stage, projecting, with its dial and gilded surround.
    for face in _square_faces(6.8, 55.0):
        # Blind tracery under the dial, a gilded square frame round it, the
        # dial, its hour marks, and the hands (minute 4.2 m, hour 2.7 m).
        panel(s, ANSTON, face, 0.0, 13.6, 0.0, 2.6,
              arcade(13.6, 7, 0.4, 1.8, 0.5, rise=0.5, segments=3), depth=0.3, recess=GILT)
        panel(s, ANSTON, face, 0.0, 13.6, 2.6, 14.0)
        _plaque(s, GILT, face, 2.35, 11.25, 2.55, 11.45, 0.06)
        _disc(s, ANSTON, face, 6.8, 7.0, 4.35, 0.1, 24)
        _disc(s, CLOCK_DIAL, face, 6.8, 7.0, 3.5, 0.2, 24)
        for k in range(12):
            a = math.tau * k / 12
            x, y = 6.8 + 3.05 * math.cos(a), 7.0 + 3.05 * math.sin(a)
            _plaque(s, GILT, face, x - 0.12, x + 0.12, y - 0.3, y + 0.3, 0.24)
        hub = face.at(6.8, 7.0, -0.32)
        for length, angle, width in ((4.2, math.radians(60), 0.22), (2.7, math.radians(-30), 0.3)):
            tip = face.at(6.8 + length * 0.78 * math.cos(angle),
                          7.0 + length * 0.78 * math.sin(angle), -0.32)
            beam(s, CAST_IRON, hub, tip, width, 0.08)
    # Parapet and gables over each dial, gilded pinnacles at the corners.
    prism(s, ANSTON, rect(14.2, 14.2), 69.0, 70.2)
    for face in _square_faces(7.1, 69.0):
        for along in (1.2, 12.9):
            base = face.at(along, 1.2, 0.3)
            spike(s, GILT, base, (base[0], base[1] + 2.6, base[2]), 0.45)
    for k in range(4):
        a = k * math.pi / 2
        c = _put(0.0, 0.0, 6.6, a)
        _gable(s, ANSTON, (c[0], c[2]), _put(1.0, 0.0, 0.0, a)[::2], 10.0, 70.2, 4.2, 0.8)
    for qx in (-1, 1):
        for qz in (-1, 1):
            lathe(s, GILT, [(0.75, 69.0), (0.75, 73.5), (0.0, 78.5)], sides=8,
                  cx=qx * 6.6, cz=qz * 6.6)
    # Belfry: a narrower stage with open arches.
    prism(s, CAST_IRON, rect(11.2, 11.2), 70.2, 79.5, top=False)
    for face in _square_faces(5.6, 70.2):
        panel(s, CAST_IRON, face, 0.0, 11.2, 0.0, 9.3,
              arcade(11.2, 3, 1.2, 5.8, 1.1, rise=2.0, segments=4), depth=0.5,
              recess=SHADOW)
    # The spire: a steep iron pyramid, the Ayrton lantern, the finial.
    loft(s, CAST_IRON, [(rect(11.6, 11.6), 79.5), (rect(5.2, 5.2), 86.5)], top=False)
    prism(s, GILT, rect(4.4, 4.4), 86.5, 88.6, top=False)
    loft(s, CAST_IRON, [(rect(4.8, 4.8), 88.6), (rect(0.5, 0.5), 94.0)])
    spike(s, GILT, (0.0, 94.0, 0.0), (0.0, 96.0, 0.0), 0.5)
    return s


# ── Colosseum ───────────────────────────────────────────────────────────────

TRAVERTINE = Finish("Tivoli travertine", (0.30, 0.26, 0.19), 0.9, family="stone")
ROMAN_BRICK = Finish("Roman brick and tufa", (0.21, 0.12, 0.08), 0.9, family="brick_red")
ARENA_DECK = Finish("Arena floor, oak boards", (0.19, 0.13, 0.08), 0.8, family="deck")


def _ellipse_point(a: float, b: float, phi: float):
    return a * math.cos(phi), b * math.sin(phi)


def colosseum() -> Sculpt:
    """The Flavian amphitheatre: 189 x 156 m, 48 m high. Recipe +x is the
    major axis. The outer ring stands on the north (-z) side, and on the rest
    of the circuit the second ring is what faces the street."""
    s = Sculpt()
    a, b = 94.5, 78.0
    # Eighty bays up close; from a distance each panel carries two, then
    # four, of the same arches, and the ring has fewer facets.
    bays = 80 >> s.lod
    arches = 80 // bays
    # The standing stretch of the outer ring, in the ellipse's own angle.
    intact = (math.radians(196), math.radians(346))

    def standing(phi: float) -> bool:
        return intact[0] <= phi % math.tau <= intact[1]

    levels = [(0.0, 10.5, 7.0), (10.5, 22.4, 6.4), (22.4, 33.5, 6.4)]
    for i in range(bays):
        p0, p1 = math.tau * i / bays, math.tau * (i + 1) / bays
        mid = (p0 + p1) * 0.5
        outer = [_ellipse_point(a, b, p0), _ellipse_point(a, b, p1)]
        second = [_ellipse_point(a - 13.0, b - 13.0, p0), _ellipse_point(a - 13.0, b - 13.0, p1)]
        if standing(mid):
            face = _outward(outer[0], outer[1])
            width = math.dist(*outer)
            for t0, t1, opening in levels:
                panel(s, TRAVERTINE, face, 0.0, width, t0, t1,
                      arcade(width, arches, t0, t0 + opening - 2.1, 1.8, segments=5),
                      depth=2.4, recess=SHADOW)
            # The attic: solid, a small window in every other bay.
            openings = [o for j, o in enumerate(arcade(width, arches, 5.0, 7.2, width / arches * 0.7,
                                                       rise=0.0)) if (i * arches + j) % 2]
            panel(s, TRAVERTINE, face, 0.0, width, 33.5, 48.5,
                  [Opening(o.left, o.right, 33.5 + o.sill, 33.5 + o.spring) for o in openings],
                  depth=0.8, recess=SHADOW)
            # Engaged half-columns between the arches -- Tuscan, Ionic,
            # Corinthian up the storeys -- and flat pilasters on the attic.
            for j in range(arches if s.lod == 0 else 0):
                at = width * j / arches
                for t0, t1, _ in levels:
                    foot = face.at(at, t0, -0.05)
                    lathe(s, TRAVERTINE, [(0.42, t0 + 0.4), (0.38, t1 - 1.4), (0.55, t1 - 0.9),
                                          (0.55, t1 - 0.6)], sides=8, cx=foot[0], cz=foot[2],
                          crease=40.0)
                foot = face.at(at, 33.5, -0.1)
                box(s, TRAVERTINE, foot[0], 33.5, foot[2], 0.9, 14.0, 0.9)
            for y in (10.5, 22.4, 33.5, 48.5) if s.lod == 0 else (48.5,):
                prism(s, TRAVERTINE, _band(a, b, p0, p1, 0.7), y - 0.5, y + 0.3)
            prism(s, ROMAN_BRICK, _quad(outer, second), 0.0, 30.0, walls=False)
        else:
            # The second ring, now the outer face: two arcades, then broken
            # walls whose height steps down bay by bay.
            face = _outward(second[0], second[1])
            width = math.dist(*second)
            k = i * arches
            ruin = 25.0 + 6.0 * math.sin(k * 1.7) * math.sin(k * 0.43)
            for t0, t1, opening in levels[:2]:
                panel(s, TRAVERTINE, face, 0.0, width, t0, t1,
                      arcade(width, arches, t0, t0 + opening - 1.9, 1.6, segments=5),
                      depth=2.0, recess=SHADOW)
            panel(s, ROMAN_BRICK, face, 0.0, width, 22.4, ruin,
                  arcade(width, arches, 23.4, 27.0, 2.0, segments=4)
                  if ruin > 29.0 else [], depth=1.2, recess=SHADOW)
            inner = [_ellipse_point(a - 15.5, b - 15.5, p0), _ellipse_point(a - 15.5, b - 15.5, p1)]
            prism(s, ROMAN_BRICK, _quad(second, inner), 0.0, ruin, walls=False)
    # The ends of the standing ring, shored by brick buttresses (Stern's in
    # 1807, Valadier's in 1820).
    for phi, sign in ((intact[0], -1.0), (intact[1], 1.0)):
        x, z = _ellipse_point(a, b, phi)
        tx, tz = -a * math.sin(phi), b * math.cos(phi)
        length = math.hypot(tx, tz)
        tx, tz = tx / length * sign, tz / length * sign
        nx, nz = x / a, z / b
        ln = math.hypot(nx, nz)
        nx, nz = nx / ln, nz / ln
        slab(s, ROMAN_BRICK, [(0.0, 0.0), (14.0, 0.0), (0.0, 46.0)],
             (x - nx * 6.0, 0.0, z - nz * 6.0), (tx, 0.0, tz), (0.0, 1.0, 0.0), 12.0)
    # The cavea: the raked seating, fallen to its brick vaults.
    rows = []
    around = 64 >> s.lod
    for (ra, rb, y) in ((47.0, 29.0, 4.2), (52.0, 34.0, 8.5), (64.0, 47.0, 17.0),
                        (78.0, 62.0, 27.5)):
        rows.append([(ra * math.cos(math.tau * i / around), y,
                      rb * math.sin(math.tau * i / around)) for i in range(around)])
    grid(s, ROMAN_BRICK, rows, inside=lambda p: (p[0] * 1.4, p[1] + 30.0, p[2] * 1.4))
    for i in range(0, 64, 2 << s.lod):
        phi = math.tau * i / 64
        inner = (47.0 * math.cos(phi), 29.0 * math.sin(phi))
        outer = (76.0 * math.cos(phi), 60.0 * math.sin(phi))
        span = math.dist(inner, outer)
        slab(s, ROMAN_BRICK, [(0.0, 0.0), (span, 0.0), (span, 26.0), (0.0, 5.2)],
             (inner[0], 0.0, inner[1]), (outer[0] - inner[0], 0.0, outer[1] - inner[1]),
             (0.0, 1.0, 0.0), 1.0)
    # The arena: its podium wall, the hypogeum's walls, the rebuilt deck.
    prism(s, TRAVERTINE, ellipse(47.0, 29.0, 48), -6.0, 4.2, top=False)
    prism(s, ROMAN_BRICK, ellipse(43.5, 27.5, 48), -6.0, -5.9)
    for k in range(-5, 6):
        z = k * 4.6
        half = 43.5 * math.sqrt(max(0.0, 1 - (z / 27.5) ** 2)) - 1.0
        if half > 2:
            box(s, ROMAN_BRICK, 0.0, -6.0, z, 2 * half, 5.6, 0.9)
    deck = [(12.0, 27.5 * math.sqrt(1 - (12 / 43.5) ** 2))]
    for i in range(33):
        phi = math.pi * 0.5 - math.pi * i / 32
        x, z = 43.5 * math.cos(phi), 27.5 * math.sin(phi)
        if x > 12.0:
            deck.append((x, z))
    deck.append((12.0, -27.5 * math.sqrt(1 - (12 / 43.5) ** 2)))
    prism(s, ARENA_DECK, deck, -0.4, 0.0)
    return s


def _band(a: float, b: float, p0: float, p1: float, out: float):
    q0, q1 = _ellipse_point(a + out, b + out, p0), _ellipse_point(a + out, b + out, p1)
    i0, i1 = _ellipse_point(a - 1.0, b - 1.0, p0), _ellipse_point(a - 1.0, b - 1.0, p1)
    return [i0, i1, q1, q0]


def _quad(outer, inner):
    return [outer[0], outer[1], inner[1], inner[0]]


# ── Taj Mahal ───────────────────────────────────────────────────────────────

MAKRANA = Finish("Makrana marble", (0.36, 0.35, 0.32), 0.45, family="stone")
MAKRANA_SHADE = Finish("Makrana marble, in shade", (0.25, 0.24, 0.22), 0.5, family="stone")
RED_SANDSTONE = Finish("Red sandstone", (0.24, 0.10, 0.07), 0.85, family="stone")
INLAY = Finish("Black marble calligraphy inlay", (0.03, 0.03, 0.03), 0.3)


def _onion(radius: float, y0: float, height: float) -> list[tuple[float, float]]:
    """A Mughal onion: rising from its drum, swelling past it, drawn up to
    the finial. The outline is the Taj's own, normalised."""
    shape = [(1.00, 0.00), (1.06, 0.08), (1.14, 0.18), (1.17, 0.28), (1.14, 0.40),
             (1.03, 0.52), (0.85, 0.64), (0.62, 0.76), (0.38, 0.87), (0.18, 0.95),
             (0.06, 0.99), (0.0, 1.0)]
    return [(radius * r, y0 + height * t) for r, t in shape]


def _chhatri(s: Sculpt, cx: float, cz: float, y0: float, radius: float, height: float,
             finish: Finish = MAKRANA) -> None:
    """A domed kiosk: eight posts, a cornice, an onion and a finial."""
    posts = height * 0.45
    for k in range(8):
        a = math.tau * k / 8
        box(s, finish, cx + radius * 0.85 * math.cos(a), y0, cz + radius * 0.85 * math.sin(a),
            radius * 0.22, posts, radius * 0.22)
    prism(s, finish, ngon(radius * 1.12, 8, cx, cz, math.pi / 8), y0 + posts,
          y0 + posts + radius * 0.25)
    lathe(s, finish, _onion(radius * 0.95, y0 + posts + radius * 0.25, height * 0.45),
          sides=12, cx=cx, cz=cz)
    lathe(s, GILT, [(radius * 0.08, y0 + posts + radius * 0.25 + height * 0.44),
                    (radius * 0.08, y0 + height), (0.0, y0 + height)], sides=6, cx=cx, cz=cz)


def taj_mahal() -> Sculpt:
    """73 m to the finial. The marble plinth (95.4 m square, 6.7 m high), the
    chamfered mausoleum (56.6 m) with its four great iwans, the onion on its
    drum, four chhatris, four 41.6 m minarets. Symmetric; +x is east."""
    s = Sculpt()
    plinth = 6.7
    prism(s, RED_SANDSTONE, rect(100.0, 100.0), -3.0, 0.0)
    prism(s, MAKRANA, rect(95.4, 95.4), 0.0, plinth, walls=False)
    for face in _square_faces(47.7):
        panel(s, MAKRANA, face, 0.0, 95.4, 0.0, plinth,
              arcade(95.4, 21, 0.8, 4.4, 1.4, rise=1.3, segments=4), depth=0.5,
              recess=MAKRANA_SHADE)
    plan = chamfered(56.6, 56.6, 13.0)
    walls_top = plinth + 25.5
    prism(s, MAKRANA, plan, plinth, walls_top, walls=False)
    height = walls_top - plinth
    for face, length in _ring_faces(plan, plinth):
        if length > 25:
            # The pishtaq: a frame rising past the parapet round a deep iwan,
            # flanked by two storeys of smaller arched niches.
            mid = length * 0.5
            panel(s, MAKRANA, face, mid - 11.0, mid + 11.0, 0.0, height,
                  [Opening(mid - 6.3, mid + 6.3, 0.0, 14.5, 8.0, 8)], depth=5.0,
                  recess=MAKRANA_SHADE)
            # The Quranic band framing the iwan: black marble set in white.
            for s0, s1, t0, t1 in ((mid - 8.4, mid - 7.6, 0.4, 23.6),
                                   (mid + 7.6, mid + 8.4, 0.4, 23.6),
                                   (mid - 8.4, mid + 8.4, 22.8, 23.6)):
                _plaque(s, INLAY, face, s0, s1, t0, t1, 0.03)
            spans = ((0.0, mid - 11.0), (mid + 11.0, length))
        else:
            spans = ((0.0, length),)
        for lo, hi in spans:
            for t0, t1 in ((0.0, height * 0.5), (height * 0.5, height)):
                panel(s, MAKRANA, face, lo, hi, t0, t1,
                      [Opening(lo + 1.2, hi - 1.2, t0 + 1.4, t1 - 4.6,
                               min(3.0, (hi - lo - 2.4) * 0.55), 5)],
                      depth=1.6, recess=MAKRANA_SHADE)
    for k in range(4):
        # The pishtaq's frame, standing proud of the parapet.
        a = k * math.pi / 2
        ring = [_put(x, 0.0, z, a) for x, z in
                ((-11.0, 27.3), (11.0, 27.3), (11.0, 28.3), (-11.0, 28.3))]
        prism(s, MAKRANA, [(p[0], p[2]) for p in ring], walls_top, plinth + 31.0)
    # Guldastas: the slender pinnacles on the mausoleum's eight corners.
    for x, z in plan:
        lathe(s, MAKRANA, [(0.9, plinth), (0.8, walls_top + 2.5), (1.1, walls_top + 3.0),
                           (0.0, walls_top + 5.5)], sides=8, cx=x, cz=z, crease=40.0)
    # Drum, dome, finial.
    lathe(s, MAKRANA, [(11.6, walls_top), (11.6, walls_top + 8.0), (12.2, walls_top + 8.0),
                       (12.2, walls_top + 8.6)], sides=28, crease=40.0)
    dome_base = walls_top + 8.6
    lathe(s, MAKRANA, _onion(12.2, dome_base, 25.0), sides=28)
    lathe(s, GILT, [(0.9, dome_base + 24.6), (0.9, dome_base + 26.0), (0.5, dome_base + 26.4),
                    (0.5, dome_base + 29.4), (0.0, plinth + 66.3)], sides=8)
    for qx in (-1, 1):
        for qz in (-1, 1):
            _chhatri(s, qx * 17.5, qz * 17.5, walls_top, 4.4, 14.0)
    # The minarets, at the plinth's corners, with three balconies each.
    for qx in (-1, 1):
        for qz in (-1, 1):
            cx, cz = qx * 44.0, qz * 44.0
            lathe(s, MAKRANA, [(3.1, plinth), (2.9, plinth + 13.6)], sides=16, cx=cx, cz=cz)
            for y0, y1, r0, r1 in ((13.6, 25.4, 2.8, 2.6), (25.4, 35.6, 2.5, 2.35)):
                lathe(s, MAKRANA, [(r0, plinth + y0), (r1, plinth + y1)], sides=16,
                      cx=cx, cz=cz)
            for y in (13.6, 25.4, 35.6):
                lathe(s, MAKRANA, [(2.6, plinth + y - 0.5), (3.9, plinth + y + 0.2),
                                   (3.9, plinth + y + 0.8), (0.0, plinth + y + 0.8)],
                      sides=16, cx=cx, cz=cz, crease=30.0)
            _chhatri(s, cx, cz, plinth + 36.4, 2.6, 6.8)
    return s


# ── the pyramids of Giza ────────────────────────────────────────────────────

GIZA_CORE = Finish("Giza limestone core blocks", (0.30, 0.24, 0.16), 0.95, family="stone")
TURA_CASING = Finish("Tura limestone casing", (0.33, 0.29, 0.21), 0.85, family="stone")


def _pyramid(base: float, apex: float, top: float, courses: int,
             casing_from: float | None, depth: float | None = None) -> Sculpt:
    """Courses of stone, each a riser and a tread: the pyramids today are
    stepped, not smooth, and the steps are what the eye reads close up.
    `apex` is the height as built, `top` where the stone stops now; above
    `casing_from` the smooth Tura casing survives (Khafre's cap).
    Aligned to the cardinal points to within a twentieth of a degree."""
    s = Sculpt()
    depth = base if depth is None else depth
    prism(s, GIZA_CORE, rect(base + 2.0, depth + 2.0), -4.0, 0.0)
    rise = top / courses
    smooth_from = casing_from if casing_from is not None else top
    mesh = s.mesh(GIZA_CORE)
    y = 0.0
    if s.lod:
        # A metre-high course is under a pixel from a kilometre away.
        k = 1.0 - smooth_from / apex
        loft(s, GIZA_CORE, [(rect(base, depth), 0.0),
                            (rect(base * k, depth * k), smooth_from)], top=False)
        y = smooth_from
    while y < smooth_from - 1e-6:
        y1 = min(y + rise, smooth_from)
        k0, k1 = 1.0 - y / apex, 1.0 - y1 / apex
        ring = rect(base * k0, depth * k0)
        inner = rect(base * k1, depth * k1)
        for i in range(4):
            j = (i + 1) % 4
            p0, p1 = ring[i], ring[j]
            mesh.add_triangle((p1[0], y, p1[1]), (p0[0], y, p0[1]), (p0[0], y1, p0[1]))
            mesh.add_triangle((p1[0], y, p1[1]), (p0[0], y1, p0[1]), (p1[0], y1, p1[1]))
            mesh.add_up_quad((ring[i][0], y1, ring[i][1]), (ring[j][0], y1, ring[j][1]),
                             (inner[j][0], y1, inner[j][1]), (inner[i][0], y1, inner[i][1]))
        y = y1
    if casing_from is not None:
        k0, k1 = 1.0 - casing_from / apex, 1.0 - top / apex
        loft(s, TURA_CASING, [(rect(base * k0, depth * k0), casing_from),
                              (rect(max(base * k1, 0.3), max(depth * k1, 0.3)), top)])
    else:
        k = 1.0 - top / apex
        cap(mesh, rect(base * k, depth * k), top)
    return s


def khufu() -> Sculpt:
    """Base 230.3 m; 146.6 m as built, 138.5 m today: the capstone and the
    top courses are gone, leaving a platform about ten metres square."""
    return _pyramid(230.3, 146.6, 138.5, 150, None)


def khafre() -> Sculpt:
    """Base 215.3 m; 143.5 m as built, 136.4 m today, the last of its Tura
    casing still on the top quarter."""
    return _pyramid(215.3, 143.5, 136.4, 110, 108.0)


def menkaure() -> Sculpt:
    """102.2 x 104.6 m; 65.5 m as built, 61 m today."""
    return _pyramid(102.2, 65.5, 61.0, 60, None, depth=104.6)


def _robe(s: Sculpt, finish: Finish, sections, folds: int, depth: float, top_fold: float,
          count: int = 28) -> None:
    """A draped figure's body as sections (height, front-back, side-side,
    forward shift), with vertical folds deepest at the hem."""
    from .sculpt import sides_at
    count = sides_at(s.lod, count, 8)
    if s.lod:
        depth = 0.0
    rows = []
    for y, rx, rz, shift in sections:
        amount = depth * max(0.0, (top_fold - y) / (top_fold - sections[0][0]))
        row = []
        for i in range(count):
            a = math.tau * i / count
            ripple = 1.0 + amount * math.sin(folds * a + 0.3 * y) + amount * 0.5 * math.sin(
                (2 * folds - 1) * a)
            row.append((shift + rx * ripple * math.cos(a), y, rz * ripple * math.sin(a)))
        rows.append(row)
    grid(s, finish, rows, inside=lambda p: (0.0, p[1], 0.0))


# ── Christ the Redeemer ─────────────────────────────────────────────────────

SOAPSTONE = Finish("Soapstone mosaic tesserae", (0.30, 0.30, 0.28), 0.7)
PALE_CONCRETE = Finish("Rendered concrete", (0.27, 0.26, 0.24), 0.9, family="concrete")


def christ_the_redeemer() -> Sculpt:
    """30 m of figure on an 8 m pedestal, arms spread 28 m, on the summit of
    Corcovado. Recipe +x is the way he faces; his arms lie along z."""
    s = Sculpt()
    prism(s, PALE_CONCRETE, rect(11.5, 11.5), -4.0, 0.6)
    loft(s, PALE_CONCRETE, [(rect(9.6, 9.6), 0.6), (rect(8.7, 8.7), 7.2)], top=False)
    prism(s, PALE_CONCRETE, rect(9.5, 9.5), 7.2, 8.0)
    # The chapel of Nossa Senhora Aparecida in the pedestal: its door.
    _plaque(s, SHADOW, Face((4.55, 1.6), (0.0, -1.0), 0.6), 0.0, 3.2, 0.0, 3.6, 0.02)
    _robe(s, SOAPSTONE, [(8.0, 2.3, 3.1, 0.0), (9.5, 2.15, 2.85, 0.0), (14.0, 1.95, 2.6, 0.0),
                         (19.0, 1.85, 2.65, 0.0), (24.0, 1.8, 2.9, 0.0), (28.5, 1.75, 3.4, 0.05),
                         (31.4, 1.6, 3.6, 0.05), (32.4, 1.1, 2.2, 0.1), (33.3, 0.8, 0.85, 0.15)],
          folds=7, depth=0.05, top_fold=30.0)
    ellipsoid(s, SOAPSTONE, (0.3, 35.9, 0.0), (1.15, 1.85, 1.05), rings=8, sides=12)
    # Hair to the shoulders behind, the nose, the Sacred Heart on the chest.
    ellipsoid(s, SOAPSTONE, (-0.35, 35.2, 0.0), (1.05, 2.2, 1.25), rings=6, sides=10)
    ellipsoid(s, SOAPSTONE, (1.42, 35.8, 0.0), (0.22, 0.35, 0.16), rings=4, sides=6)
    ellipsoid(s, SOAPSTONE, (1.62, 29.4, 0.45), (0.2, 0.5, 0.45), rings=4, sides=6)
    # The belt of the tunic.
    lathe(s, SOAPSTONE, [(1.95, 21.8), (2.02, 22.1), (1.95, 22.4)], sides=16, sx=0.95, sz=1.5)
    for side in (-1, 1):
        tube(s, SOAPSTONE, [(0.0, 31.3, side * 3.0), (0.0, 31.45, side * 8.5),
                            (0.1, 31.25, side * 12.9)], [1.35, 0.95, 0.65], sides=10)
        ellipsoid(s, SOAPSTONE, (0.1, 31.15, side * 13.6), (0.35, 0.95, 0.8), rings=5, sides=8)
        # The sleeve hanging from the forearm: the cross's lower edge.
        slab(s, SOAPSTONE, [(3.0, 31.0), (12.4, 30.8), (11.8, 29.3), (8.0, 26.8), (3.2, 23.2)],
             (0.0, 0.0, 0.0), (0.0, 0.0, float(side)), (0.0, 1.0, 0.0), 0.9)
        # The rolled hem of the sleeve, which is what catches the light.
        tube(s, SOAPSTONE, [(0.0, 23.2, side * 3.2), (0.0, 26.8, side * 8.0),
                            (0.0, 29.3, side * 11.8)], [0.5, 0.45, 0.4], sides=8)
    return s


# ── Sydney Opera House ──────────────────────────────────────────────────────

SHELL_TILE = Finish("Glazed chevron tiles", (0.33, 0.32, 0.28), 0.3, double_sided=True)
TARANA = Finish("Tarana granite", (0.22, 0.14, 0.10), 0.85, family="stone")
SHELL_RIB = Finish("Matte buff lid tiles", (0.27, 0.25, 0.21), 0.7)
BRONZE_GLASS = Finish("Topaz glass, bronze mullions", (0.05, 0.045, 0.04), 0.15,
                      double_sided=True)


def _shell(s: Sculpt, xt: float, yt: float, zc: float, xr: float, y0: float, width: float,
           facing: int = 1, bulge: float = 0.13, n: int = 10) -> None:
    """One of Utzon's shells: two curved triangles meeting at a ridge. The
    ridge leaves the podium steeply behind and leans forward to the tip; the
    mouth's edges bow outward as they fall from the tip to the podium, and a
    glass wall closes the mouth, set back under it."""
    def ridge(v: float):
        q = v * math.pi / 2
        return (xr + (xt - xr) * (1 - math.cos(q)), y0 + (yt - y0) * math.sin(q), zc)

    foot_x = xt - facing * 3.0
    n = max(4, n >> s.lod)
    for side in (-1, 1):
        def mouth(v: float):
            return (foot_x + (xt - foot_x) * v ** 0.8, y0 + (yt - y0) * math.sin(v * math.pi / 2)
                    ** 1.3, zc + side * width * 0.5 * (1 - v) ** 0.85)
        rows = []
        for j in range(n + 1):
            v = j / n
            r, m = ridge(v), mouth(v)
            chord = math.dist(r, m)
            lift = min(1.0, v * 4.0)
            row = []
            for i in range(n + 1):
                u = i / n
                push = chord * bulge * math.sin(u * math.pi) * lift
                row.append((r[0] + (m[0] - r[0]) * u,
                            r[1] + (m[1] - r[1]) * u + push * 0.55,
                            r[2] + (m[2] - r[2]) * u + push * side * 0.85))
            rows.append(row)
        grid(s, SHELL_TILE, rows, closed=False, facing=lambda j, c, side=side: (0.0, 0.5, side))
        # The shell's segments: ribs of matte tiles between the glossy ones.
        for i in range(2, n, 3):
            line = [rows[j][i] for j in range(0, n + 1, 2)]
            for p, q in zip(line, line[1:]):
                beam(s, SHELL_RIB, (p[0], p[1] + 0.12, p[2] + side * 0.12),
                     (q[0], q[1] + 0.12, q[2] + side * 0.12), 0.7, 0.2)
        glass = s.mesh(BRONZE_GLASS)
        edge = [rows[j][n] for j in range(n + 1)]
        for p, q in zip(edge, edge[1:]):
            fp = (p[0] - facing * 4.0, y0, p[2])
            fq = (q[0] - facing * 4.0, y0, q[2])
            glass.add_triangle(p, q, fq)
            glass.add_triangle(p, fq, fp)


def sydney_opera_house() -> Sculpt:
    """Utzon's shells on Hall's granite podium: the Concert Hall's four shells
    to the west, the Joan Sutherland Theatre's to the east, the restaurant's
    pair at the south-west. Highest tip 67 m above the harbour. Recipe +x
    is the halls' axis, pointing out along Bennelong Point (NNE)."""
    s = Sculpt()
    top = 7.0
    prism(s, TARANA, rect(152.0, 104.0, -4.0, 0.0), -3.0, top)
    # The monumental steps down to the forecourt.
    for k in range(7):
        prism(s, TARANA, rect(22.0 - k * 3.0, 96.0, -91.0 + k * 1.5, 0.0), -3.0, (k + 1) * top / 7)
    halls = ((-24.0, 1.0, 0.0), (22.0, 0.9, -4.0))
    for zc, scale, shift in halls:
        for xt, yt, xr, width, facing in ((56.0, 44.0, 20.0, 34.0, 1), (37.0, 56.0, -4.0, 44.0, 1),
                                          (13.0, 65.0, -32.0, 50.0, 1),
                                          (-54.0, 38.0, -24.0, 34.0, -1)):
            _shell(s, xt * scale + shift, top + (yt - top) * scale, zc, xr * scale + shift, top,
                   width * scale, facing)
    for xt, yt, xr, width in ((-66.0, 24.0, -50.0, 20.0), (-56.0, 19.0, -44.0, 15.0)):
        _shell(s, xt, yt, -36.0, xr, top, width, -1)
    return s


# ── Burj Khalifa ────────────────────────────────────────────────────────────

BURJ_GLASS = Finish("Reflective glazing, stainless fins", (0.11, 0.13, 0.15), 0.2, metallic=0.5)
STAINLESS = Finish("Stainless steel", (0.28, 0.28, 0.29), 0.35, metallic=0.8)
BURJ_FIN = Finish("Stainless steel fins", (0.24, 0.25, 0.27), 0.3, metallic=0.8)


def _tripod(lengths, half: float = 14.5, tip: int = 5, lod: int = 0):
    """The Y plan: three rounded wings at 120° round a hexagonal core."""
    points = []
    tip = max(2, tip >> lod)
    inner = half / math.sin(math.pi / 3)
    for i, length in enumerate(lengths):
        th = i * math.tau / 3
        u = (math.cos(th), math.sin(th))
        v = (-u[1], u[0])
        points.append((inner * math.cos(th - math.pi / 3), inner * math.sin(th - math.pi / 3)))
        centre = (u[0] * (length - half), u[1] * (length - half))
        for k in range(tip + 1):
            a = -math.pi / 2 + math.pi * k / tip
            points.append((centre[0] + (u[0] * math.cos(a) + v[0] * math.sin(a)) * half,
                           centre[1] + (u[1] * math.cos(a) + v[1] * math.sin(a)) * half))
    return positive(points)


def burj_khalifa() -> Sculpt:
    """828 m. The Y plan's three wings step back one at a time in a spiral,
    twenty-four setbacks, until the core rises alone and becomes the spire.
    Recipe +x is the first wing; OSM way 446646206 puts the wing tips at
    66°, 186° and 306°."""
    s = Sculpt()
    prism(s, BURJ_GLASS, _tripod((86.0, 86.0, 86.0), 13.0, lod=s.lod), -3.0, 9.0)
    lengths = [60.0, 60.0, 60.0]
    y = -3.0
    steps = [40.0 + k * 21.5 for k in range(24)]
    for k, y1 in enumerate(steps + [560.0]):
        plan = _tripod(lengths, lod=s.lod)
        prism(s, BURJ_GLASS, plan, y, y1, walls=bool(s.lod))
        for face, length in _ring_faces(plan, y) if s.lod == 0 else ():
            # Stainless fins between the glass: bright vertical stripes on
            # the long faces of each wing.
            bays = int(length / 6.0)
            panel(s, BURJ_GLASS, face, 0.0, length, 0.0, y1 - y,
                  arcade(length, bays, 0.0, y1 - y, 3.6, rise=0.0) if bays >= 2 else [],
                  depth=0.4, recess=BURJ_FIN)
        if k < len(steps):
            lengths[k % 3] = max(25.5, lengths[k % 3] - 4.3)
        y = y1
    lathe(s, BURJ_GLASS, [(13.0, 560.0), (12.0, 585.0), (9.0, 585.0)], sides=16, crease=30.0)
    lathe(s, STAINLESS, [(9.0, 585.0), (6.2, 640.0), (4.2, 700.0), (2.4, 765.0),
                         (1.1, 810.0), (0.0, 828.0)], sides=12)
    return s


# ── Empire State Building ───────────────────────────────────────────────────

INDIANA = Finish("Indiana limestone, aluminium spandrels", (0.30, 0.28, 0.24), 0.8, family="stone")


def _notched(width: float, depth: float, notch: float):
    if notch <= 0:
        return rect(width, depth)
    hx, hz, n = width * 0.5, depth * 0.5, notch
    return positive([(-hx + n, -hz), (hx - n, -hz), (hx - n, -hz + n), (hx, -hz + n),
                     (hx, hz - n), (hx - n, hz - n), (hx - n, hz), (-hx + n, hz),
                     (-hx + n, hz - n), (-hx, hz - n), (-hx, -hz + n), (-hx + n, -hz + n)])


def empire_state_building() -> Sculpt:
    """381 m to the roof, 443.2 m to the antenna. The five-storey base fills
    the lot (129.5 x 60 m), then the setbacks at the 6th, 21st, 25th, 30th,
    72nd, 81st and 86th floors, the mooring mast, the antenna. Recipe +x runs
    along 34th Street."""
    s = Sculpt()
    stages = [(-3.0, 25.0, 129.5, 60.0, 0.0), (25.0, 80.0, 100.0, 54.0, 0.0),
              (80.0, 95.0, 80.0, 50.0, 2.0), (95.0, 114.0, 66.0, 46.0, 3.0),
              (114.0, 274.0, 58.0, 41.0, 4.0), (274.0, 308.0, 46.0, 33.0, 3.0),
              (308.0, 320.0, 34.0, 26.0, 2.0), (320.0, 324.0, 28.0, 22.0, 0.0)]
    for y0, y1, width, depth, notch in stages:
        plan = _notched(width, depth, notch)
        prism(s, INDIANA, plan, y0, y1, walls=False)
        for face, length in _ring_faces(plan, y0):
            bays = int(length / 5.2)
            if bays < 1 or y1 - y0 < 8:
                panel(s, INDIANA, face, 0.0, length, 0.0, y1 - y0)
                continue
            # The window bays run unbroken from base to setback: the
            # vertical grain of the building, in recessed dark strips.
            panel(s, INDIANA, face, 0.0, length, 0.0, y1 - y0,
                  arcade(length, bays, 3.0 if y0 < 0 else 1.2, y1 - y0 - 1.5, 2.1, rise=0.0),
                  depth=0.5, recess=DARK_GLASS)
    for k in range(8):
        # The mooring mast's aluminium fins, one on each face of its octagon.
        a = math.tau * (k + 0.5) / 8
        beam(s, STAINLESS, (10.4 * math.cos(a), 324.0, 10.4 * math.sin(a)),
             (7.4 * math.cos(a), 366.0, 7.4 * math.sin(a)), 0.9, 1.2)
    lathe(s, INDIANA, [(10.0, 324.0), (10.0, 340.0), (8.5, 340.0), (8.5, 356.0), (7.0, 356.0),
                       (7.0, 368.0), (5.0, 373.0), (3.5, 376.0), (3.5, 381.0), (0.0, 381.0)],
          sides=8, crease=30.0)
    lathe(s, STAINLESS, [(1.0, 381.0), (0.8, 410.0), (0.4, 430.0), (0.15, 443.2),
                         (0.0, 443.2)], sides=6)
    return s


# ── Leaning Tower of Pisa ───────────────────────────────────────────────────

PISA_MARBLE = Finish("San Giuliano marble", (0.35, 0.33, 0.29), 0.7, family="stone")
PISA_SHADE = Finish("San Giuliano marble, in shade", (0.22, 0.21, 0.19), 0.8, family="stone")


def leaning_tower() -> Sculpt:
    """55.86 m on the low side, leaning 3.97° to the south. A blind arcade,
    six open galleries of thirty columns, the bell chamber. Built upright
    and turned about its foot; recipe +x is the way it leans."""
    s = Sculpt()
    tower = Sculpt()
    prism(s, PISA_MARBLE, ngon(9.2, 24), -2.0, 0.3)
    ring = ngon(7.74, 30)
    for face, length in _ring_faces(ring, -1.0):
        panel(tower, PISA_MARBLE, face, 0.0, length, 0.0, 12.0,
              [Opening(0.18 * length, 0.82 * length, 2.0, 8.5, 0.32 * length, 4)],
              depth=0.4, recess=PISA_SHADE)
    y = 11.0
    for gallery in range(6):
        y1 = y + 6.3
        lathe(tower, PISA_SHADE, [(6.3, y), (6.3, y1)], sides=24)
        for k in range(30):
            a = math.tau * k / 30
            lathe(tower, PISA_MARBLE, [(0.33, y), (0.29, y1 - 1.35), (0.42, y1 - 1.1),
                                       (0.42, y1 - 1.0)], sides=6,
                  cx=7.25 * math.cos(a), cz=7.25 * math.sin(a))
        # The arches' band and the cornice that is the next gallery's floor.
        lathe(tower, PISA_MARBLE, [(6.9, y1 - 1.3), (7.6, y1 - 1.3), (7.6, y1 - 0.55),
                                   (7.95, y1 - 0.55), (7.95, y1), (6.3, y1)], sides=30,
              crease=30.0)
        y = y1
    lathe(tower, PISA_MARBLE, [(5.3, y), (5.3, 54.6), (5.9, 54.6), (5.9, 55.3), (4.0, 55.3),
                               (3.2, 55.86), (0.0, 55.86)], sides=24, crease=30.0)
    for k in range(12):
        a = math.tau * k / 12
        face = Face((5.35 * math.cos(a + 0.2), 5.35 * math.sin(a + 0.2)),
                    (math.cos(a - 0.2) - math.cos(a + 0.2), math.sin(a - 0.2) - math.sin(a + 0.2)))
        _disc(tower, SHADOW, face, 0.9, y + 3.0, 0.8, 0.05, 8)
    lean = math.radians(3.97)
    c, sn = math.cos(lean), math.sin(lean)
    turn = lambda p: (p[0] * c + p[1] * sn, -p[0] * sn + p[1] * c, p[2])
    s.merge(tower, turn, turn)
    return s


# ── Arc de Triomphe ─────────────────────────────────────────────────────────

PARIS_STONE = Finish("Château-Landon limestone", (0.33, 0.30, 0.24), 0.9, family="stone")


def arc_de_triomphe() -> Sculpt:
    """50 m high, 44.82 m wide, 22.21 m deep. The great arch (29.19 x
    14.62 m) runs through along the Champs-Élysées; the small arches
    (18.68 x 8.44 m) cross it through the piers. Recipe +x is along the
    long facades (OSM way 226413508: 25.2°)."""
    s = Sculpt()
    width, depth, entablature = 44.82, 22.21, 37.2
    prism(s, PARIS_STONE, rect(width + 4.0, depth + 4.0), -2.0, 0.4)
    prism(s, PARIS_STONE, rect(width, depth), 0.4, entablature, walls=False)
    long_face = Face((-width / 2, depth / 2), (1.0, 0.0), 0.4)
    panel(s, PARIS_STONE, long_face, 0.0, width, 0.0, entablature - 0.4,
          [Opening(width / 2 - 7.31, width / 2 + 7.31, 0.0, 21.88 - 0.4, 7.31, 12)],
          depth=depth, through=True)
    for sign in (-1, 1):
        face = Face((sign * width / 2, sign * depth / 2), (0.0, -float(sign)), 0.4)
        panel(s, PARIS_STONE, face, 0.0, depth, 0.0, entablature - 0.4,
              [Opening(depth / 2 - 4.22, depth / 2 + 4.22, 0.0, 14.46 - 0.4, 4.22, 8)],
              depth=(width - 14.62) / 2, back=False)
        # Rude's and Cortot's groups, on the piers of both long facades.
        for zs in (-1, 1):
            box(s, PARIS_STONE, sign * (width / 2 - 7.6), 3.8, zs * (depth / 2 + 0.6),
                8.0, 12.5, 1.2)
    # The great frieze (the departure and return of the armies) and, on the
    # attic, the thirty shields bearing the names of Napoleon's victories.
    prism(s, PARIS_STONE, rect(width + 0.5, depth + 0.5), 29.8, 32.6, walls=False)
    for face, length in _ring_faces(rect(width + 0.5, depth + 0.5), 29.8):
        panel(s, PARIS_STONE, face, 0.0, length, 0.0, 2.8,
              arcade(length, int(length / 1.6), 0.35, 2.45, 0.5, rise=0.0),
              depth=0.18, recess=PARIS_STONE)
    for face, length in _ring_faces(rect(width - 0.6, depth - 0.6), 39.2):
        count = 10 if length > 30 else 5
        for k in range(count):
            _disc(s, PARIS_STONE, face, length * (k + 0.5) / count, 5.4, 0.85, 0.18, 10)
    prism(s, PARIS_STONE, rect(width + 1.6, depth + 1.6), entablature, entablature + 2.0)
    prism(s, PARIS_STONE, rect(width - 0.6, depth - 0.6), entablature + 2.0, 49.4)
    prism(s, PARIS_STONE, rect(width, depth), 49.4, 50.0)
    return s


# ── Notre-Dame de Paris ─────────────────────────────────────────────────────

ROSE = Finish("Stained glass rose in its tracery", (0.06, 0.05, 0.075), 0.3)


def _plain_faces(s: Sculpt, finish: Finish, ring, y0: float, y1: float, skip=None) -> None:
    for face, length in _ring_faces(ring, y0):
        if skip is not None and skip(face):
            continue
        panel(s, finish, face, 0.0, length, 0.0, y1 - y0)


def _rose_tracery(s: Sculpt, face: Face, sc: float, tc: float, radius: float) -> None:
    """Sixteen stone spokes and an inner ring across a rose window."""
    centre = face.at(sc, tc, -0.3)
    for k in range(16):
        a = math.tau * k / 16
        rim = face.at(sc + radius * math.cos(a), tc + radius * math.sin(a), -0.3)
        beam(s, PARIS_STONE, centre, rim, 0.22, 0.2)
        a2 = math.tau * (k + 1) / 16
        p = face.at(sc + radius * 0.45 * math.cos(a), tc + radius * 0.45 * math.sin(a), -0.3)
        q = face.at(sc + radius * 0.45 * math.cos(a2), tc + radius * 0.45 * math.sin(a2), -0.3)
        beam(s, PARIS_STONE, p, q, 0.2, 0.2)


def notre_dame() -> Sculpt:
    """128 m long, the nave vaulted at 33 m under a lead roof ridged at 43 m,
    the west towers 69 m, the spire rebuilt to 96 m. Flying buttresses along
    both flanks and round the chevet. Recipe +x runs from the west front to
    the apse (OSM way 201611261: 116.4°)."""
    s = Sculpt()
    # Aisles and chapels, and their lean-to roofs against the nave.
    prism(s, PARIS_STONE, rect(96.0, 40.0, -4.0), -2.0, 11.0, top=False)
    loft(s, LEAD, [(rect(96.0, 40.0, -4.0), 11.0), (rect(96.0, 13.4, -4.0), 20.0)], top=False)
    # The nave vessel with its clerestory.
    prism(s, PARIS_STONE, rect(96.0, 13.0, -4.0), 11.0, 33.0, walls=False, top=False)
    for face in (Face((-52.0, 6.5), (1.0, 0.0)), Face((44.0, -6.5), (-1.0, 0.0))):
        panel(s, PARIS_STONE, face, 0.0, 96.0, 11.0, 33.0,
              arcade(96.0, 14, 22.0, 29.0, 2.4, rise=2.8, segments=4), depth=0.6, recess=SHADOW)
    loft(s, LEAD, [(rect(96.0, 14.4, -4.0), 33.0), (rect(96.0, 0.02, -4.0), 43.0)], top=False)
    # The transept, gabled at both ends, a rose in each.
    prism(s, PARIS_STONE, rect(14.0, 48.0, 2.0), -2.0, 33.0, top=False)
    loft(s, LEAD, [(rect(14.4, 48.0, 2.0), 33.0), (rect(0.02, 48.0, 2.0), 43.0)], top=False)
    for face in (Face((-5.0, 24.0), (1.0, 0.0)), Face((9.0, -24.0), (-1.0, 0.0))):
        _disc(s, PARIS_STONE, face, 7.0, 22.0, 6.6, 0.08, 20)
        _disc(s, ROSE, face, 7.0, 22.0, 6.0, 0.14, 20)
        _rose_tracery(s, face, 7.0, 22.0, 6.0)
    # The chevet: the apse, the ambulatory round it, their roofs.
    apse = [(44.0 + 6.5 * math.cos(a), 6.5 * math.sin(a))
            for a in [math.radians(-90 + 180 * k / 8) for k in range(9)]]
    prism(s, PARIS_STONE, apse, 11.0, 33.0, top=False)
    loft(s, LEAD, [(apse, 33.0), ([(44.0, 0.0)] * len(apse), 43.0)])
    ambulatory = [(44.0 + 20.0 * math.cos(a), 20.0 * math.sin(a))
                  for a in [math.radians(-90 + 180 * k / 10) for k in range(11)]]
    prism(s, PARIS_STONE, ambulatory, -2.0, 11.0, top=False)
    loft(s, LEAD, [(ambulatory, 11.0), ([(44.0 + 7.0 * math.cos(math.atan2(z, x - 44.0)),
                                          7.0 * math.sin(math.atan2(z, x - 44.0)))
                                         for x, z in ambulatory], 20.0)], top=False)
    # Flying buttresses: a pier on the aisle wall, a pinnacle, the flyer.
    for x in range(-46, 42, 7):
        for side in (-1, 1):
            box(s, PARIS_STONE, x, -2.0, side * 21.2, 1.6, 26.0, 2.4)
            spike(s, PARIS_STONE, (x, 24.0, side * 21.2), (x, 29.0, side * 21.2), 1.4)
            beam(s, PARIS_STONE, (x, 22.5, side * 20.2), (x, 29.5, side * 6.9), 0.9, 1.2)
    for k in range(5):
        a = math.radians(-60 + 30 * k)
        pier = (44.0 + 21.5 * math.cos(a), 21.5 * math.sin(a))
        box(s, PARIS_STONE, pier[0], -2.0, pier[1], 2.0, 26.0, 2.0)
        beam(s, PARIS_STONE, (pier[0], 22.5, pier[1]),
             (44.0 + 7.0 * math.cos(a), 29.5, 7.0 * math.sin(a)), 0.9, 1.2)
    # The west front: portals, the gallery of kings, the rose, the great
    # gallery; then the two towers.
    front = rect(12.0, 41.0, -58.0)
    prism(s, PARIS_STONE, front, -2.0, 43.0, walls=False)
    _plain_faces(s, PARIS_STONE, front, -2.0, 43.0, skip=lambda f: f.n[0] < -0.9)
    west = Face((-64.0, -20.5), (0.0, 1.0))
    panel(s, PARIS_STONE, west, 0.0, 41.0, -2.0, 17.0,
          [Opening(3.5, 10.0, 0.0, 8.0, 4.8, 6), Opening(15.0, 26.0, 0.0, 9.5, 6.5, 6),
           Opening(31.0, 37.5, 0.0, 8.0, 4.8, 6)], depth=3.0, recess=SHADOW)
    panel(s, PARIS_STONE, west, 0.0, 41.0, 17.0, 21.0,
          arcade(41.0, 28, 17.5, 20.2, 0.5, rise=0.3, segments=2), depth=0.6, recess=SHADOW)
    panel(s, PARIS_STONE, west, 0.0, 41.0, 21.0, 36.0,
          [Opening(5.0, 9.0, 23.5, 31.0, 2.4, 4), Opening(32.0, 36.0, 23.5, 31.0, 2.4, 4)],
          depth=1.0, recess=SHADOW)
    _disc(s, ROSE, west, 20.5, 28.5, 4.8, 0.1, 20)
    _rose_tracery(s, west, 20.5, 28.5, 4.8)
    panel(s, PARIS_STONE, west, 0.0, 41.0, 36.0, 43.0,
          arcade(41.0, 14, 36.8, 41.4, 0.7, rise=0.9, segments=3), depth=1.2, recess=SHADOW)
    for side in (-1, 1):
        tower = rect(12.0, 14.0, -58.0, side * 13.5)
        prism(s, PARIS_STONE, tower, 43.0, 68.2, walls=False)
        for face, length in _ring_faces(tower, 43.0):
            panel(s, PARIS_STONE, face, 0.0, length, 0.0, 25.2,
                  arcade(length, 2, 4.0, 19.0, length * 0.18, rise=1.8, segments=4),
                  depth=1.2, recess=SHADOW)
        prism(s, PARIS_STONE, rect(12.8, 14.8, -58.0, side * 13.5), 68.2, 69.2, walls=False)
        for face, length in _ring_faces(rect(12.8, 14.8, -58.0, side * 13.5), 68.2):
            # The open balustrade round the top, and a pinnacle at each corner.
            panel(s, PARIS_STONE, face, 0.0, length, 0.0, 1.0)
            panel(s, PARIS_STONE, face, 0.0, length, 1.0, 2.2,
                  arcade(length, int(length / 0.9), 1.15, 2.05, 0.3, rise=0.0), depth=0.2,
                  through=True)
            corner = face.at(0.0, 0.0, 0.4)
            spike(s, PARIS_STONE, corner, (corner[0], 73.0, corner[2]), 0.9)
    # The chimeras' gallery between the towers, at the foot of their belfries.
    for z in [-6.0 + 12.0 * k / 7 for k in range(8)]:
        spike(s, PARIS_STONE, (-63.6, 43.0, z), (-63.8, 45.2, z), 0.7)
    # The spire over the crossing, and its cock.
    lathe(s, LEAD, [(3.9, 40.0), (3.9, 52.0), (3.3, 52.0), (0.15, 95.3), (0.0, 95.3)],
          sides=8, crease=30.0)
    spike(s, GILT, (2.0, 95.2, 0.0), (2.0, 96.0, 0.0), 0.4)
    return s


# ── Sagrada Família ─────────────────────────────────────────────────────────

MONTJUIC = Finish("Montjuïc sandstone", (0.28, 0.24, 0.18), 0.9, family="stone")
MOSAIC = Finish("Venetian glass mosaic, gold and red", (0.34, 0.20, 0.08), 0.3)
TRENCADIS = Finish("White Venetian glass and trencadís", (0.34, 0.34, 0.33), 0.25)
CYPRESS = Finish("Green ceramic cypress", (0.05, 0.17, 0.07), 0.4)


def _gaudi_tower(s: Sculpt, cx: float, cz: float, radius: float, y0: float, height: float,
                 finial: float, crown: Finish = MOSAIC) -> float:
    """A Gaudí spire: a paraboloid drawn to a point, then its ceramic
    pinnacle. Returns the height where the stone stops."""
    stone_top = y0 + height - finial
    profile = [(radius * (1.0 - 0.82 * (k / 8) ** 2.2), y0 + (stone_top - y0) * k / 8)
               for k in range(9)]
    # The towers are hollow, and a spiral of openings winds up each one: a
    # dark column of faces that turns with the height.
    lathe(s, MONTJUIC, profile, sides=12, cx=cx, cz=cz, twist=0.035,
          pattern=lambda j, i: SHADOW if 1 <= j <= 6 and i % 3 == 1 else MONTJUIC)
    if finial > 0:
        r = profile[-1][0]
        lathe(s, crown, [(r, stone_top), (r * 1.7, stone_top + finial * 0.35),
                         (r * 1.3, stone_top + finial * 0.7), (0.0, stone_top + finial)],
              sides=8, cx=cx, cz=cz)
    else:
        cap(s.mesh(MONTJUIC), ngon(profile[-1][0], 12, cx, cz), stone_top)
    return stone_top


def sagrada_familia() -> Sculpt:
    """The basilica as it stands in 2026: the Nativity (NE) and Passion (SW)
    façades with their four bell towers each, the four Evangelists (135 m)
    round the tower of Jesus Christ (172.5 m), the tower of the Virgin Mary
    (138 m) over the apse. The Glory façade is not built and is not drawn.
    Recipe +x runs up the nave to the Glory side (OSM: 134.5°); +z is the
    Passion side."""
    s = Sculpt()
    nave = rect(70.0, 45.0, 27.0)
    prism(s, MONTJUIC, nave, -2.0, 45.0, top=False, walls=False)
    for face, length in _ring_faces(nave, -2.0):
        # Tall windows between the buttresses, in two tiers.
        bays = max(1, int(length / 7.0))
        panel(s, MONTJUIC, face, 0.0, length, 0.0, 25.0,
              arcade(length, bays, 6.0, 19.0, 3.4, rise=2.2, segments=4), depth=1.2,
              recess=SHADOW)
        panel(s, MONTJUIC, face, 0.0, length, 25.0, 47.0,
              arcade(length, bays, 29.0, 40.0, 3.4, rise=2.6, segments=4), depth=1.2,
              recess=SHADOW)
    loft(s, MONTJUIC, [(rect(70.0, 45.0, 27.0), 45.0), (rect(70.0, 0.02, 27.0), 60.0)], top=False)
    prism(s, MONTJUIC, rect(30.0, 60.0), -2.0, 45.0, top=False)
    loft(s, MONTJUIC, [(rect(30.0, 60.0), 45.0), (rect(0.02, 60.0), 60.0)], top=False)
    apse = [(-15.0 + 22.5 * math.cos(a), 22.5 * math.sin(a))
            for a in [math.radians(90 + 180 * k / 8) for k in range(9)]]
    prism(s, MONTJUIC, apse, -2.0, 35.0, top=False)
    loft(s, MONTJUIC, [(apse, 35.0), ([(-15.0, 0.0)] * len(apse), 55.0)])
    for x in range(-12, 62, 7):
        for side in (-1, 1):
            spike(s, MONTJUIC, (x, 45.0, side * 22.5), (x, 53.0, side * 22.5), 2.2)
    for k in range(7):
        a = math.radians(90 + 180 * k / 6)
        spike(s, MONTJUIC, (-15.0 + 22.5 * math.cos(a), 35.0, 22.5 * math.sin(a)),
              (-15.0 + 23.0 * math.cos(a), 48.0, 23.0 * math.sin(a)), 2.4)
    # The two built façades, each a block of portals under a gable.
    for side, heights in ((-1, (98.5, 107.0, 107.0, 98.5)), (1, (105.0, 112.0, 112.0, 105.0))):
        zc = side * 32.0
        block = rect(32.0, 5.0, 0.0, zc)
        prism(s, MONTJUIC, block, -2.0, 42.0, walls=False)
        _plain_faces(s, MONTJUIC, block, -2.0, 42.0,
                     skip=lambda f, side=side: f.n[1] * side > 0.9)
        face = Face((16.0, zc + side * 2.5), (-1.0, 0.0)) if side < 0 else \
            Face((-16.0, zc + side * 2.5), (1.0, 0.0))
        panel(s, MONTJUIC, face, 0.0, 32.0, -2.0, 42.0,
              [Opening(3.0, 9.0, 0.0, 14.0, 5.0, 6), Opening(12.0, 20.0, 0.0, 17.0, 7.0, 6),
               Opening(23.0, 29.0, 0.0, 14.0, 5.0, 6)], depth=3.5, recess=SHADOW)
        _gable(s, MONTJUIC, (0.0, zc), (1.0, 0.0), 26.0, 42.0, 18.0, 3.0)
        if side < 0:
            lathe(s, CYPRESS, [(2.6, 55.0), (2.0, 62.0), (0.0, 70.0)], sides=10, cz=zc - 1.0)
            # The Nativity façade's carved mass: grottoes and foliage.
            for k in range(14):
                x = -14.0 + 28.0 * ((k * 0.618) % 1.0)
                y = 20.0 + 22.0 * ((k * 0.382) % 1.0)
                ellipsoid(s, MONTJUIC, (x, y, zc - 3.0), (2.2, 2.8, 1.4), rings=4, sides=7)
        else:
            # The Passion façade's six leaning columns, like tendons, under
            # a plain frieze.
            for k in range(6):
                x = -12.5 + 5.0 * k
                beam(s, MONTJUIC, (x, 0.0, zc + 7.0), (x * 0.85, 21.0, zc + 3.5), 2.2)
            box(s, MONTJUIC, 0.0, 21.0, zc + 4.5, 32.0, 4.0, 5.0)
        for x, h in zip((-13.0, -5.0, 5.0, 13.0), heights):
            _gaudi_tower(s, x, zc + side * 1.0, 4.6, 0.0, h, 12.0)
    for qx in (-1, 1):
        for qz in (-1, 1):
            _gaudi_tower(s, qx * 10.5, qz * 10.5, 7.5, 45.0, 90.0, 8.0)
    top = _gaudi_tower(s, 0.0, 0.0, 12.5, 45.0, 114.0, 0.0)
    beam(s, TRENCADIS, (0.0, top - 1.0, 0.0), (0.0, 172.5, 0.0), 2.2)
    for axis in ((1.0, 0.0), (0.0, 1.0)):
        beam(s, TRENCADIS, (-4.0 * axis[0], 166.0, -4.0 * axis[1]),
             (4.0 * axis[0], 166.0, 4.0 * axis[1]), 2.0)
    top = _gaudi_tower(s, -19.0, 0.0, 9.0, 35.0, 99.0, 0.0)
    for across in ((1.0, 0.0, 0.0), (0.0, 0.0, 1.0)):
        slab(s, TRENCADIS, star(3.75, 1.9, 12), (-19.0, top + 3.9, 0.0), across,
             (0.0, 1.0, 0.0), 0.8)
    return s


# ── Brandenburger Tor ───────────────────────────────────────────────────────

ELBE = Finish("Elbe sandstone", (0.30, 0.27, 0.20), 0.9, family="stone")


def brandenburg_gate() -> Sculpt:
    """26 m to the quadriga. Twelve Doric columns, six a side, framing five
    passages (the middle one 5.65 m, the others 3.8 m); entablature, attic,
    and Schadow's quadriga driving east. Recipe +x runs north-south along
    the gate (OSM way 518071791: 173.4°); east, where the horses face, is -z."""
    s = Sculpt()
    prism(s, ELBE, rect(34.5, 13.0), -2.0, 0.6)
    passages = (3.8, 3.8, 5.65, 3.8, 3.8)
    column = 1.75
    total = sum(passages) + 6 * column
    x = -total / 2
    centres = []
    for i in range(6):
        centres.append(x + column / 2)
        x += column + (passages[i] if i < 5 else 0.0)
    for xc in centres:
        for zc in (-4.8, 4.8):
            lathe(s, ELBE, [(0.92, 0.6), (0.82, 12.7)], sides=16, cx=xc, cz=zc,
                  flutes=20, flute_depth=0.05)
            lathe(s, ELBE, [(0.82, 12.7), (1.1, 13.2), (1.1, 13.4)], sides=16, cx=xc, cz=zc)
            box(s, ELBE, xc, 13.4, zc, 2.3, 0.5, 2.3)
        box(s, ELBE, xc, 0.6, 0.0, column * 0.9, 13.3, 8.0)
    for sign in (-1, 1):
        box(s, ELBE, sign * (total / 2 + 0.8), 0.6, 0.0, 1.6, 13.3, 11.2)
    _rect_frame(s, ELBE, total + 3.2, 12.4, 6.2, 13.9, 17.0)
    _doric_frieze(s, ELBE, total + 3.2, 12.4, 13.9, 1.3, 1.5, 0.3, 6, 2)
    prism(s, ELBE, rect(total + 4.0, 13.2), 17.0, 17.7)
    prism(s, ELBE, rect(total - 1.0, 10.6), 17.7, 20.3)
    prism(s, ELBE, rect(total - 3.0, 9.6), 20.3, 20.8)
    prism(s, ELBE, rect(8.5, 5.6), 20.8, 21.4)
    y0 = 21.4
    for xh in (-2.1, -0.7, 0.7, 2.1):
        tube(s, VERDIGRIS, [(xh, y0 + 2.0, 0.6), (xh, y0 + 2.15, -1.6)], [0.45, 0.42], sides=8)
        for zl in (0.4, -1.4):
            for dx in (-0.18, 0.18):
                beam(s, VERDIGRIS, (xh + dx, y0, zl), (xh + dx, y0 + 1.9, zl), 0.16)
        tube(s, VERDIGRIS, [(xh, y0 + 2.3, -1.5), (xh, y0 + 3.3, -2.2), (xh, y0 + 3.05, -2.9)],
             [0.34, 0.26, 0.2], sides=6)
    box(s, VERDIGRIS, 0.0, y0, 1.6, 2.4, 1.6, 1.4)
    tube(s, VERDIGRIS, [(0.0, y0 + 1.6, 1.8), (0.0, y0 + 4.2, 1.7)], [0.55, 0.35], sides=8)
    ellipsoid(s, VERDIGRIS, (0.0, y0 + 4.55, 1.65), (0.28, 0.35, 0.28), rings=4, sides=6)
    beam(s, VERDIGRIS, (0.7, y0 + 2.8, 1.5), (0.7, 26.0, 1.3), 0.12)
    return s


# ── St Peter's Basilica ─────────────────────────────────────────────────────

LEAD_DOME = Finish("Lead-covered dome", (0.13, 0.14, 0.15), 0.5, family="metal_seam")


def st_peters_basilica() -> Sculpt:
    """136.57 m to the cross. Michelangelo's Greek cross and its drum and
    dome over the crossing, Maderno's nave and façade (114.69 m wide) to the
    east. Recipe +x runs east along the axis (OSM way 244159210: 89.4°); the
    crossing is 26 m west of the footprint's centre."""
    s = Sculpt()
    cx = -26.0
    prism(s, TRAVERTINE, rect(92.0, 92.0, cx), -2.0, 44.5, roof=LEAD)
    for angle in (math.pi, math.pi / 2, -math.pi / 2):
        centre = (cx + 46.0 * math.cos(angle), 46.0 * math.sin(angle))
        half = [(centre[0] + 23.0 * math.cos(angle + math.radians(-90 + 180 * k / 8)),
                 centre[1] + 23.0 * math.sin(angle + math.radians(-90 + 180 * k / 8)))
                for k in range(9)]
        prism(s, TRAVERTINE, half, -2.0, 44.5, roof=LEAD)
    prism(s, TRAVERTINE, rect(76.0, 60.0, 58.0), -2.0, 44.5, top=False)
    loft(s, LEAD, [(rect(76.0, 60.0, 58.0), 44.5), (rect(76.0, 0.02, 58.0), 52.0)], top=False)
    prism(s, TRAVERTINE, rect(64.0, 96.0, 52.0), -2.0, 30.0, roof=LEAD)
    # The façade.
    front = rect(12.0, 114.7, 102.0)
    prism(s, TRAVERTINE, front, -2.0, 45.5, walls=False)
    _plain_faces(s, TRAVERTINE, front, -2.0, 45.5, skip=lambda f: f.n[0] > 0.9)
    face = Face((108.0, 57.35), (0.0, -1.0))
    panel(s, TRAVERTINE, face, 0.0, 114.7, -2.0, 27.5,
          [Opening(57.35 + k * 12.0 - 2.9, 57.35 + k * 12.0 + 2.9, 0.0, 10.0, 2.9, 6)
           for k in range(-2, 3)], depth=2.5, recess=SHADOW)
    panel(s, TRAVERTINE, face, 0.0, 114.7, 27.5, 33.0)
    panel(s, TRAVERTINE, face, 0.0, 114.7, 33.0, 45.5,
          arcade(114.7, 9, 35.0, 41.5, 8.0, rise=0.0), depth=0.8, recess=SHADOW)
    for z in (-27.0, -21.0, -9.5, -3.5, 3.5, 9.5, 21.0, 27.0):
        lathe(s, TRAVERTINE, [(1.4, -2.0), (1.25, 25.8)], sides=12, cx=108.0, cz=z,
              flutes=24, flute_depth=0.04)
        lathe(s, TRAVERTINE, [(1.25, 25.8), (1.7, 26.6), (1.7, 27.5)], sides=12,
              cx=108.0, cz=z)
    for z in (-51.0, -41.0, -34.0, 34.0, 41.0, 51.0):
        box(s, TRAVERTINE, 108.3, -2.0, z, 0.6, 29.5, 2.6)
    _gable(s, TRAVERTINE, (108.8, 0.0), (0.0, 1.0), 30.0, 38.5, 7.0, 1.6)
    for k in range(13):
        z = -30.0 + k * 5.0
        tube(s, TRAVERTINE, [(106.5, 45.5, z), (106.5, 50.4, z)], [0.7, 0.55], sides=6)
        ellipsoid(s, TRAVERTINE, (106.5, 50.9, z), (0.4, 0.5, 0.4), rings=4, sides=6)
    for z in (-54.0, 54.0):
        lathe(s, LEAD_DOME, sphere_profile(4.0, 47.0, 5, 0.0, 90.0), sides=12, cx=102.0, cz=z)
        lathe(s, TRAVERTINE, [(4.0, 45.5), (4.0, 47.0)], sides=12, cx=102.0, cz=z)
    # Drum with its paired buttresses, the ribbed dome, the lantern, the cross.
    lathe(s, TRAVERTINE, [(21.5, 44.5), (21.5, 67.0), (22.6, 67.0), (22.6, 71.0),
                          (21.0, 71.0), (21.0, 75.0)], sides=32, cx=cx, crease=40.0)
    for k in range(16):
        # Sixteen buttresses, each fronted by a pair of Corinthian columns.
        a = math.tau * k / 16
        ring = [(cx + x * math.cos(a) - z * math.sin(a), x * math.sin(a) + z * math.cos(a))
                for x, z in rect(2.6, 3.0, 22.4)]
        prism(s, TRAVERTINE, ring, 44.5, 67.0)
        for d in (-1.0, 1.0):
            px = cx + 24.0 * math.cos(a) - d * 1.2 * math.sin(a)
            pz = 24.0 * math.sin(a) + d * 1.2 * math.cos(a)
            lathe(s, TRAVERTINE, [(0.6, 44.5), (0.52, 65.8), (0.75, 66.4), (0.75, 67.0)],
                  sides=10, cx=px, cz=pz, crease=40.0)
    dome = [(21.0, 75.0), (20.4, 82.0), (18.8, 90.0), (16.0, 98.0), (12.2, 105.0),
            (7.6, 110.5), (4.6, 113.5)]
    lathe(s, LEAD_DOME, dome, sides=32, cx=cx)
    for k in range(16):
        a = math.tau * k / 16
        points = [(cx + (r + 0.35) * math.cos(a), y, (r + 0.35) * math.sin(a)) for r, y in dome]
        for p, q in zip(points, points[1:]):
            beam(s, TRAVERTINE, p, q, 1.0, 0.8)
    lathe(s, TRAVERTINE, [(4.6, 113.5), (4.6, 122.0), (5.2, 122.0), (5.2, 123.5), (3.2, 123.5),
                          (2.6, 128.5), (1.0, 131.0), (0.9, 132.5)], sides=16, cx=cx,
          crease=40.0)
    ellipsoid(s, GILT, (cx, 133.4, 0.0), (1.1, 1.1, 1.1), rings=6, sides=10)
    beam(s, GILT, (cx, 134.0, 0.0), (cx, 136.57, 0.0), 0.35)
    beam(s, GILT, (cx, 135.6, -0.8), (cx, 135.6, 0.8), 0.3)
    for z in (-40.0, 40.0):
        lathe(s, TRAVERTINE, [(7.0, 44.5), (7.0, 51.0)], sides=16, cx=cx + 40.0, cz=z)
        lathe(s, LEAD_DOME, sphere_profile(7.0, 51.0, 5, 0.0, 90.0), sides=16, cx=cx + 40.0, cz=z)
        lathe(s, TRAVERTINE, [(1.4, 57.8), (1.4, 61.0), (0.0, 62.5)], sides=8, cx=cx + 40.0, cz=z)
    return s


# ── St Basil's Cathedral ────────────────────────────────────────────────────

RED_BRICK = Finish("Red brick", (0.24, 0.075, 0.05), 0.85, family="brick_red")
WHITE_TRIM = Finish("White limestone trim", (0.33, 0.32, 0.29), 0.8)
TENT = Finish("Glazed tent tiles", (0.25, 0.27, 0.21), 0.5)
ONION = {
    "green": Finish("Onion, green glaze", (0.05, 0.19, 0.09), 0.45),
    "yellow": Finish("Onion, yellow glaze", (0.36, 0.28, 0.06), 0.45),
    "blue": Finish("Onion, blue glaze", (0.05, 0.10, 0.24), 0.45),
    "red": Finish("Onion, red glaze", (0.26, 0.05, 0.04), 0.45),
    "white": WHITE_TRIM,
    "gold": GILT,
}


def _kokoshniks(s: Sculpt, cx: float, cz: float, radius: float, y: float, count: int,
                height: float) -> None:
    """A ring of keel-arched gables round a drum: the tiers of kokoshniks."""
    if s.lod >= 2:
        return
    width = 2 * radius * math.sin(math.pi / count) * 1.05
    outline = [(-width / 2, 0.0), (width / 2, 0.0), (width / 2, height * 0.45),
               (width * 0.2, height * 0.8), (0.0, height), (-width * 0.2, height * 0.8),
               (-width / 2, height * 0.45)]
    if s.lod:
        outline = [(-width / 2, 0.0), (width / 2, 0.0), (0.0, height)]
    for k in range(count):
        a = math.tau * k / count
        slab(s, WHITE_TRIM, outline, (cx + radius * math.cos(a), y, cz + radius * math.sin(a)),
             (-math.sin(a), 0.0, math.cos(a)), (0.0, 1.0, 0.0), 0.6)


def _russian_onion(s: Sculpt, cx: float, cz: float, y0: float, radius: float, height: float,
                   colours, mode: str) -> None:
    profile = [(radius * r, y0 + height * t) for r, t in
               ((0.62, 0.0), (0.9, 0.12), (1.0, 0.28), (0.93, 0.45), (0.66, 0.63),
                (0.36, 0.8), (0.12, 0.93), (0.0, 1.0))]
    finishes = [ONION[c] for c in colours]
    if mode == "spiral":
        lathe(s, finishes[0], profile, sides=16, cx=cx, cz=cz, twist=0.35 / radius,
              pattern=lambda j, i: finishes[(i // 2) % len(finishes)])
    elif mode == "check":
        lathe(s, finishes[0], profile, sides=16, cx=cx, cz=cz,
              pattern=lambda j, i: finishes[(i + j) % len(finishes)])
    else:
        lathe(s, finishes[0], profile, sides=16, cx=cx, cz=cz)
    top = y0 + height
    beam(s, GILT, (cx, top - 0.2, cz), (cx, top + 2.4, cz), 0.18)
    beam(s, GILT, (cx - 0.6, top + 1.7, cz), (cx + 0.6, top + 1.7, cz), 0.14)


def st_basils_cathedral() -> Sculpt:
    """65 m. The tented Church of the Intercession, four great octagonal
    chapels on the cardinal axes and four small ones between, each under its
    own patterned onion, on a gallery of red brick; the bell tower to the
    south-east. The chapels stand on the cardinal axes, so recipe +x is
    north."""
    s = Sculpt()
    platform = ngon(23.0, 16)
    prism(s, RED_BRICK, platform, -2.0, 6.5, walls=False)
    for face, length in _ring_faces(platform, -2.0):
        # The open gallery that runs round all nine churches.
        panel(s, RED_BRICK, face, 0.0, length, 0.0, 8.5,
              arcade(length, 2, 3.2, 6.4, 1.6, rise=1.2, segments=4), depth=1.2,
              recess=SHADOW)
    prism(s, WHITE_TRIM, ngon(23.4, 16), 6.0, 6.7)
    prism(s, RED_BRICK, ngon(7.5, 8, phase=math.pi / 8), 6.5, 30.0, top=False)
    _kokoshniks(s, 0.0, 0.0, 7.3, 30.0, 8, 4.5)
    prism(s, RED_BRICK, ngon(6.0, 8, phase=math.pi / 8), 30.0, 35.0, top=False)
    loft(s, TENT, [(ngon(5.8, 8, phase=math.pi / 8), 35.0), (ngon(0.9, 8, phase=math.pi / 8), 58.0)],
         top=False)
    lathe(s, WHITE_TRIM, [(0.9, 58.0), (0.9, 59.6)], sides=8)
    _russian_onion(s, 0.0, 0.0, 59.6, 1.6, 3.2, ("gold",), "plain")
    great = (((17.0, 0.0), ("green", "yellow"), "spiral"), ((-17.0, 0.0), ("red", "white"), "spiral"),
             ((0.0, 17.0), ("blue", "white"), "spiral"), ((0.0, -17.0), ("green", "yellow"), "check"))
    for (x, z), colours, mode in great:
        prism(s, RED_BRICK, ngon(4.6, 8, x, z, math.pi / 8), 6.5, 26.0, top=False)
        _kokoshniks(s, x, z, 4.4, 26.0, 8, 2.8)
        prism(s, WHITE_TRIM, ngon(3.3, 8, x, z, math.pi / 8), 26.0, 30.0, top=False)
        _russian_onion(s, x, z, 30.0, 4.2, 8.2, colours, mode)
    small = (((9.5, 9.5), ("green", "white"), "check"), ((-9.5, 9.5), ("yellow", "red"), "spiral"),
             ((9.5, -9.5), ("blue", "yellow"), "spiral"), ((-9.5, -9.5), ("red", "green"), "check"))
    for (x, z), colours, mode in small:
        prism(s, RED_BRICK, ngon(3.0, 8, x, z, math.pi / 8), 6.5, 20.0, top=False)
        _kokoshniks(s, x, z, 2.9, 20.0, 8, 2.0)
        prism(s, WHITE_TRIM, ngon(2.2, 8, x, z, math.pi / 8), 20.0, 23.0, top=False)
        _russian_onion(s, x, z, 23.0, 2.9, 5.8, colours, mode)
    # The bell tower, south-east (recipe +z is east, -x south).
    bx, bz = -18.0, 21.0
    prism(s, RED_BRICK, rect(6.5, 6.5, bx, bz), -2.0, 14.0)
    prism(s, WHITE_TRIM, ngon(3.6, 8, bx, bz, math.pi / 8), 14.0, 20.0)
    loft(s, TENT, [(ngon(3.8, 8, bx, bz, math.pi / 8), 20.0),
                   (ngon(0.5, 8, bx, bz, math.pi / 8), 30.0)], top=False)
    _russian_onion(s, bx, bz, 30.0, 1.1, 2.5, ("green",), "plain")
    # The chapel of St Basil himself, low, to the north-east.
    prism(s, RED_BRICK, rect(7.0, 7.0, 16.0, 16.0), -2.0, 11.0)
    prism(s, WHITE_TRIM, ngon(2.0, 8, 16.0, 16.0, math.pi / 8), 11.0, 13.0, top=False)
    _russian_onion(s, 16.0, 16.0, 13.0, 2.3, 4.2, ("green", "gold"), "check")
    return s


# ── Parthenon ───────────────────────────────────────────────────────────────

PENTELIC = Finish("Pentelic marble, weathered", (0.33, 0.28, 0.21), 0.85, family="stone")


def _doric(s: Sculpt, x: float, z: float, y0: float, height: float) -> None:
    """A Doric column with its entasis, echinus and abacus."""
    # Twenty flutes, as every Doric column of the Parthenon has.
    lathe(s, PENTELIC, [(0.955, y0), (0.97, y0 + height * 0.35), (0.76, y0 + height * 0.94),
                        (0.74, y0 + height * 0.95)], sides=16, cx=x, cz=z, crease=40.0,
          flutes=20, flute_depth=0.05)
    lathe(s, PENTELIC, [(0.74, y0 + height * 0.95), (1.02, y0 + height * 0.985),
                        (1.02, y0 + height)], sides=16, cx=x, cz=z, crease=40.0)
    box(s, PENTELIC, x, y0 + height, z, 2.1, 0.35, 2.1)


def parthenon() -> Sculpt:
    """The stylobate 69.5 x 30.9 m on three steps, the peristyle of 8 x 17
    Doric columns 10.43 m high, the entablature, the two pediments. The roof
    is gone and so, mostly, is the cella; the west porch still stands.
    Recipe +x runs east along the axis (OSM way 910010406: 77.1°)."""
    s = Sculpt()
    prism(s, PENTELIC, rect(73.8, 35.2), -3.0, 0.0)
    for k, (w, d) in enumerate(((72.3, 33.7), (70.9, 32.3), (69.5, 30.9))):
        prism(s, PENTELIC, rect(w, d), 0.55 * k, 0.55 * (k + 1))
    y0, height = 1.65, 10.43
    xs = [-33.8 + 67.6 * i / 16 for i in range(17)]
    zs = [-14.5 + 29.0 * i / 7 for i in range(8)]
    ring = [(x, z) for x in xs for z in (-14.5, 14.5)] + [(x, z) for x in (-33.8, 33.8)
                                                           for z in zs[1:-1]]
    for x, z in ring:
        _doric(s, x, z, y0, height)
    top = y0 + height + 0.35
    _rect_frame(s, PENTELIC, 69.2, 30.8, 2.6, top, top + 3.3)
    _doric_frieze(s, PENTELIC, 69.2, 30.8, top, 1.35, 1.35, 0.6, 16, 7)
    for x in (-34.0, 34.0):
        _gable(s, PENTELIC, (x, 0.0), (0.0, 1.0), 30.6, top + 3.3, 3.46, 1.4)
    for z in [-9.5 + 19.0 * i / 5 for i in range(6)]:
        _doric(s, -24.8, z, y0, height)
        lathe(s, PENTELIC, [(0.85, y0), (0.8, y0 + 3.2), (0.0, y0 + 3.2)], sides=12, cx=25.4, cz=z)
    box(s, PENTELIC, -24.8, y0 + height + 0.35, 0.0, 2.2, 1.35, 21.0)
    box(s, PENTELIC, -22.3, y0, 0.0, 1.2, 10.5, 20.8)
    for z in (-9.8, 9.8):
        box(s, PENTELIC, 0.5, y0, z, 45.0, 2.2, 1.2)
    return s


# ── Tokyo Tower ─────────────────────────────────────────────────────────────

AVIATION_ORANGE = Finish("International orange paint", (0.52, 0.09, 0.02), 0.6)
AVIATION_WHITE = Finish("White paint", (0.33, 0.33, 0.32), 0.6)
DECK = Finish("Observation deck cladding", (0.26, 0.27, 0.28), 0.5, metallic=0.3)
FOOT_TOWN = Finish("Foot Town cladding", (0.27, 0.27, 0.26), 0.8, family="concrete_panel")

# The aviation bands, white between these heights and orange elsewhere.
_TOKYO_WHITE = ((95.0, 110.0), (175.0, 190.0), (212.0, 226.0), (253.0, 270.0), (290.0, 305.0))
_TOKYO_HALF = ((0.0, 44.0), (48.0, 28.0), (140.0, 15.8), (152.0, 15.0), (223.0, 8.2),
               (253.0, 5.4), (300.0, 1.4))


def _tokyo_paint(y: float) -> Finish:
    return AVIATION_WHITE if any(a <= y < b for a, b in _TOKYO_WHITE) else AVIATION_ORANGE


def tokyo_tower() -> Sculpt:
    """332.6 m. Four lattice legs meeting at 48 m over Foot Town, the Main
    Deck at 145-152 m, the Top Deck at 250 m, the antenna mast; painted in
    aviation bands. Recipe +x runs along one face (OSM: 34.4°)."""
    s = Sculpt()
    _lattice_legs(s, _TOKYO_HALF, ((0.0, 12.0), (48.0, 28.0)),
                  [0.0, 8.0, 16.0, 24.0, 32.0, 40.0, 48.0], _tokyo_paint, chord0=1.4)
    _lattice_shaft(s, _TOKYO_HALF, [48.0 + 11.5 * k for k in range(9)] + [140.0], _tokyo_paint,
                   chord0=1.6)
    prism(s, DECK, rect(31.0, 31.0), 140.0, 141.5)
    prism(s, DARK_GLASS, rect(29.5, 29.5), 141.5, 150.0, top=False)
    prism(s, DECK, rect(31.5, 31.5), 150.0, 152.5)
    _lattice_shaft(s, _TOKYO_HALF, [152.5 + 11.2 * k for k in range(9)] + [253.0], _tokyo_paint,
                   chord0=1.2, inner=0.0)
    prism(s, DARK_GLASS, rect(12.0, 12.0), 245.0, 250.5, top=False)
    prism(s, DECK, rect(12.8, 12.8), 250.5, 253.0)
    _lattice_shaft(s, _TOKYO_HALF, [253.0 + 11.75 * k for k in range(5)], _tokyo_paint,
                   chord0=0.8, inner=0.0)
    lathe(s, AVIATION_ORANGE, [(1.2, 300.0), (0.9, 316.0), (0.5, 316.0)], sides=8, crease=30.0)
    lathe(s, AVIATION_WHITE, [(0.5, 316.0), (0.3, 332.6), (0.0, 332.6)], sides=6)
    prism(s, FOOT_TOWN, rect(52.5, 76.5), -2.0, 20.0)
    return s


# ── Petronas Towers ─────────────────────────────────────────────────────────

PETRONAS_SHADE = Finish("Stainless sunshade", (0.30, 0.30, 0.31), 0.25, metallic=0.9)
PETRONAS_STEEL = Finish("Stainless steel and laminated glass", (0.26, 0.27, 0.28), 0.3,
                        metallic=0.7)


def _rub_el_hizb(radius: float, lod: int = 0):
    """Two squares turned 45° to each other, their inner corners filled with
    arcs: the eight-pointed star with sixteen lobes of the Petronas plan."""
    points = []
    for k in range(8):
        a = k * math.pi / 4
        points.append((radius * math.cos(a), radius * math.sin(a)))
        mid = a + math.pi / 8
        for d, r in ((-0.13, 0.86), (0.0, 0.92), (0.13, 0.86)) if lod < 2 else ((0.0, 0.9),):
            points.append((radius * r * math.cos(mid + d), radius * r * math.sin(mid + d)))
    return positive(points)


def petronas_towers() -> Sculpt:
    """451.9 m to the pinnacles. Two towers 100 m apart on the Rub el Hizb
    plan, six setbacks to the crown, 73 m pinnacles, the skybridge on the
    41st and 42nd floors (170 m) with its hinged legs. Recipe +x runs from
    one tower to the other (OSM way 279944536: 49.5°)."""
    s = Sculpt()
    prism(s, DARK_GLASS, rect(60.0, 36.0), -2.0, 16.0)
    for cx in (-50.0, 50.0):
        tower = Sculpt()
        stages = [(-2.0, 250.0, 23.0), (250.0, 266.0, 21.4), (266.0, 285.0, 19.8),
                  (285.0, 305.0, 18.2), (305.0, 330.0, 16.1), (330.0, 355.0, 13.8),
                  (355.0, 378.0, 11.5)]
        for y0, y1, radius in stages:
            prism(tower, PETRONAS_STEEL, _rub_el_hizb(radius, s.lod), y0, y1)
            # Stainless sunshades every two floors: the horizontal grain of
            # the towers, and the reason they glitter.
            y = y0 + 8.0
            while y < y1 - 1.0 and s.lod == 0:
                prism(tower, PETRONAS_SHADE, _rub_el_hizb(radius + 0.45), y, y + 0.35,
                      bottom=True)
                y += 8.0
        lathe(tower, PETRONAS_STEEL, [(8.0, 378.0), (8.0, 386.0), (6.5, 386.0), (6.5, 392.0),
                                      (5.0, 392.0), (5.0, 398.0), (2.2, 398.0), (1.2, 420.0),
                                      (0.9, 440.0), (0.3, 451.9), (0.0, 451.9)], sides=12,
              crease=30.0)
        ellipsoid(tower, PETRONAS_STEEL, (0.0, 408.0, 0.0), (2.8, 2.8, 2.8), rings=6, sides=10)
        ellipsoid(tower, PETRONAS_STEEL, (0.0, 425.0, 0.0), (1.8, 1.8, 1.8), rings=5, sides=8)
        s.merge(tower, lambda p, cx=cx: (p[0] + cx, p[1], p[2]))
    box(s, PETRONAS_STEEL, 0.0, 170.0, 0.0, 57.0, 9.0, 5.5)
    for sign in (-1, 1):
        for z in (-2.0, 2.0):
            beam(s, PETRONAS_STEEL, (sign * 3.0, 170.0, z), (sign * 28.0, 118.0, z), 1.1)
    return s
