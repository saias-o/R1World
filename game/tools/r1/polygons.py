"""Planar polygon primitives shared by the R1World generators.

They are pure 2D geometry with no notion of a scene, a
footprint or a material, and they are float64 and deterministic like everything
else the pipeline runs offline (§4 I3).

Every function here takes points as `(x, y)` pairs. The generators work in the
engine's horizontal plane and pass `(east, -north)`; nothing in this module
needs to know that.
"""

from __future__ import annotations

Point = tuple[float, float]


def cross2(a: Point, b: Point, c: Point) -> float:
    """Twice the signed area of the triangle abc; the sign is the turn."""
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def polygon_area(points: list[Point]) -> float:
    """Signed area. Positive is counter-clockwise in the numeric plane."""
    return 0.5 * sum(
        points[i][0] * points[(i + 1) % len(points)][1]
        - points[(i + 1) % len(points)][0] * points[i][1]
        for i in range(len(points))
    )


def centroid(points: list[Point]) -> Point:
    """Area centroid, falling back to the vertex mean on a degenerate ring."""
    area = polygon_area(points)
    if abs(area) < 1e-9:
        count = max(1, len(points))
        return (sum(p[0] for p in points) / count, sum(p[1] for p in points) / count)
    cx = cy = 0.0
    for i, current in enumerate(points):
        nxt = points[(i + 1) % len(points)]
        w = current[0] * nxt[1] - nxt[0] * current[1]
        cx += (current[0] + nxt[0]) * w
        cy += (current[1] + nxt[1]) * w
    return cx / (6.0 * area), cy / (6.0 * area)


def point_in_triangle(p: Point, a: Point, b: Point, c: Point) -> bool:
    ab = cross2(a, b, p)
    bc = cross2(b, c, p)
    ca = cross2(c, a, p)
    return (ab >= -1e-8 and bc >= -1e-8 and ca >= -1e-8) or (
        ab <= 1e-8 and bc <= 1e-8 and ca <= 1e-8
    )


def triangulate(points: list[Point]) -> list[tuple[int, int, int]]:
    """Deterministic ear clipping for simple OSM footprint polygons."""
    if len(points) < 3:
        return []
    order = list(range(len(points)))
    if polygon_area(points) < 0.0:
        order.reverse()
    triangles = []
    guard = len(order) * len(order)
    while len(order) > 3 and guard > 0:
        guard -= 1
        clipped = False
        for cursor in range(len(order)):
            i0 = order[cursor - 1]
            i1 = order[cursor]
            i2 = order[(cursor + 1) % len(order)]
            if cross2(points[i0], points[i1], points[i2]) <= 1e-8:
                continue
            if any(
                point_in_triangle(points[index], points[i0], points[i1], points[i2])
                for index in order
                if index not in (i0, i1, i2)
            ):
                continue
            triangles.append((i0, i1, i2))
            del order[cursor]
            clipped = True
            break
        if not clipped:
            break
    if len(order) == 3:
        triangles.append(tuple(order))
    if not triangles:
        triangles.extend((0, index, index + 1) for index in range(1, len(points) - 1))
    return triangles


def point_in_polygon(point: Point, polygon: list[Point]) -> bool:
    inside = False
    j = len(polygon) - 1
    for i, current in enumerate(polygon):
        previous = polygon[j]
        crosses = (current[1] > point[1]) != (previous[1] > point[1])
        if crosses:
            edge_x = (previous[0] - current[0]) * (point[1] - current[1]) / (
                previous[1] - current[1]
            ) + current[0]
            if point[0] < edge_x:
                inside = not inside
        j = i
    return inside


def convex_hull(points: list[Point]) -> list[Point]:
    """Monotone chain hull, counter-clockwise, without repeating the first point.

    Used to find a footprint's principal axis. Sorting makes it independent of
    the order OSM happened to store the way in, which matters: the same building
    must produce the same roof ridge whichever end of the way it was drawn from.
    """
    unique = sorted(set(points))
    if len(unique) < 3:
        return unique
    lower: list[Point] = []
    for p in unique:
        while len(lower) >= 2 and cross2(lower[-2], lower[-1], p) <= 0.0:
            lower.pop()
        lower.append(p)
    upper: list[Point] = []
    for p in reversed(unique):
        while len(upper) >= 2 and cross2(upper[-2], upper[-1], p) <= 0.0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def inset_polygon(points: list[Point], distance: float) -> list[Point]:
    """Shrink a ring toward its centroid by roughly `distance` metres.

    A parapet's inner face and a roof deck's edge need a slightly smaller ring,
    and a proper straight-skeleton offset is a chantier of its own. Pulling each
    vertex toward the centroid is exact only for a regular polygon, but the
    insets asked for here are a few tens of centimetres on a building footprint,
    where the error is under a centimetre and never self-intersects. Anything
    larger belongs to a real offset, not to this.
    """
    if len(points) < 3:
        return list(points)
    cx, cy = centroid(points)
    out: list[Point] = []
    for x, y in points:
        dx, dy = x - cx, y - cy
        length = (dx * dx + dy * dy) ** 0.5
        if length <= distance * 1.5:
            return list(points)  # too small to inset without folding
        scale = (length - distance) / length
        out.append((cx + dx * scale, cy + dy * scale))
    return out
