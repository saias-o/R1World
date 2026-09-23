"""The measured ground of a tile: relief, and the road ribbons laid on it.

Every world tile is built on this grid, and everything that stands on the
ground samples it through `ground_point` so that it sits on the rendered
triangles rather than on the smoother source surface.
"""

from __future__ import annotations

import math

from .geodesy import Anchor
from .mesh import Mesh
from .sources import Bounds, ElevationGrid, OsmWay

TERRAIN_MESH_SIZE = 41

ROAD_WIDTHS = {
    "motorway": 13.0,
    "trunk": 11.0,
    "primary": 9.0,
    "secondary": 8.0,
    "tertiary": 7.0,
    "residential": 5.5,
    "living_street": 5.0,
    "service": 3.5,
    "pedestrian": 3.0,
    "cycleway": 1.8,
    "footway": 1.5,
    "path": 1.2,
    "steps": 1.2,
}


def build_terrain(
    bounds: Bounds,
    elevations: ElevationGrid,
    anchor: Anchor,
    classify=None,
) -> dict[str, Mesh]:
    """The measured relief of one box, as one mesh per ground class.

    `classify(lon, lat) -> str` names the material a triangle is made of; with
    no classifier every triangle lands in one unnamed mesh.

    **Partitioning the grid is free.** Each triangle carries its own face
    normal, so no two of them ever shared a vertex to begin with: splitting the
    mesh into a dozen meshes moves vertices between buckets and creates none.
    That is the whole reason `ground.py` classifies the terrain rather than
    laying polygons over it (see its docstring).
    """
    meshes: dict[str, Mesh] = {}
    points = []
    coordinates = []
    for row in range(TERRAIN_MESH_SIZE):
        lat = bounds.south + (bounds.north - bounds.south) * row / (TERRAIN_MESH_SIZE - 1)
        line = []
        line_coordinates = []
        for column in range(TERRAIN_MESH_SIZE):
            lon = bounds.west + (bounds.east - bounds.west) * column / (TERRAIN_MESH_SIZE - 1)
            line.append(anchor.geodetic_to_engine(lon, lat, elevations.sample(lon, lat)))
            line_coordinates.append((lon, lat))
        points.append(line)
        coordinates.append(line_coordinates)

    def mesh_for(a, b, c) -> Mesh:
        if classify is None:
            return meshes.setdefault("", Mesh())
        # The centre of the triangle, in longitude and latitude. A cell is about
        # fourteen metres across, so which of its three corners is inside a park
        # matters far less than where its middle is, and the middle cannot fall
        # outside the triangle the way a corner test can.
        name = classify((a[0] + b[0] + c[0]) / 3.0, (a[1] + b[1] + c[1]) / 3.0)
        return meshes.setdefault(name, Mesh())

    for row in range(TERRAIN_MESH_SIZE - 1):
        for column in range(TERRAIN_MESH_SIZE - 1):
            southwest = points[row][column]
            southeast = points[row][column + 1]
            northeast = points[row + 1][column + 1]
            northwest = points[row + 1][column]
            csw = coordinates[row][column]
            cse = coordinates[row][column + 1]
            cne = coordinates[row + 1][column + 1]
            cnw = coordinates[row + 1][column]
            uv = lambda point: (point[0] / 14.0, point[2] / 14.0)
            mesh_for(csw, cse, cne).add_up_triangle(
                southwest, southeast, northeast,
                (uv(southwest), uv(southeast), uv(northeast)),
            )
            mesh_for(csw, cne, cnw).add_up_triangle(
                southwest, northeast, northwest,
                (uv(southwest), uv(northeast), uv(northwest)),
            )
    return meshes


def ground_point(
    lon: float,
    lat: float,
    elevations: ElevationGrid,
    anchor: Anchor,
    lift: float = 0.0,
) -> tuple[float, float, float]:
    # Match the rendered terrain triangles, rather than the smoother bilinear
    # source surface: roads on the latter can disappear through the former.
    b = elevations.bounds
    size = TERRAIN_MESH_SIZE - 1
    u = max(0., min(1., (lon-b.west)/(b.east-b.west))) * size
    v = max(0., min(1., (lat-b.south)/(b.north-b.south))) * size
    col, row = min(size-1, int(u)), min(size-1, int(v))
    u, v = u-col, v-row
    corners = ((0,0), (1,0), (1,1)) if u >= v else ((0,0), (1,1), (0,1))
    weights = (1-u, u-v, v) if u >= v else (1-v, u, v-u)
    points = []
    for cx, cy in corners:
        lo = b.west+(col+cx)/size*(b.east-b.west)
        la = b.south+(row+cy)/size*(b.north-b.south)
        points.append(anchor.geodetic_to_engine(lo, la, elevations.sample(lo, la)))
    y = sum(p[1]*w for p,w in zip(points,weights))
    # Full OSM building footprints may cross the tile boundary. Clamp only
    # elevation sampling; clamping X/Z would squash neighbouring walls.
    x, _, z = anchor.geodetic_to_engine(lon, lat, elevations.sample(lon, lat))
    return x, y + lift, z


def build_roads(roads: tuple[OsmWay, ...], elevations: ElevationGrid, anchor: Anchor) -> Mesh:
    mesh = Mesh()
    for road in roads:
        width = ROAD_WIDTHS.get(road.tags.get("highway", ""), 2.5)
        half = width * 0.5
        points = [ground_point(lon, lat, elevations, anchor, 0.08) for lon, lat in road.points]
        for start, end in zip(points, points[1:]):
            dx, dz = end[0] - start[0], end[2] - start[2]
            length = math.hypot(dx, dz)
            if length < 0.1:
                continue
            px, pz = -dz / length * half, dx / length * half
            mesh.add_up_quad(
                (start[0] + px, start[1], start[2] + pz),
                (end[0] + px, end[1], end[2] + pz),
                (end[0] - px, end[1], end[2] - pz),
                (start[0] - px, start[1], start[2] - pz),
            )
    return mesh
