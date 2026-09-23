"""Terrain-following streets, tagged sidewalks and surveyed zebra crossings.

Widths without tags are estimates, never survey data. Geometry remains batched
by material. No per-stripe scene nodes or textures are required.
"""
import math
from .mesh import Mesh, MeshPart, Material
from .terrain import ground_point, ROAD_WIDTHS

MOTOR = {"motorway", "trunk", "primary", "secondary", "tertiary",
         "residential", "living_street", "service", "unclassified"}
URBAN = {"primary", "secondary", "tertiary", "residential", "living_street"}


def length_tag(value, default):
    try:
        n = float(str(value).removesuffix(" m"))
        return n if math.isfinite(n) and 0 < n <= 50 else default
    except (ValueError, TypeError):
        return default


def width(tags):
    return length_tag(tags.get("width"), ROAD_WIDTHS.get(tags.get("highway"), 2.5))


def sidewalk_sides(tags):
    general = tags.get("sidewalk", "")
    out = []
    for side in ("left", "right"):
        tagged = tags.get("sidewalk:"+side, general)
        if tagged in {"no", "none", "separate"}:
            continue
        if tagged in {"yes", "both", side}:
            out.append((side, False))
        elif not tagged and tags.get("highway") in URBAN:
            out.append((side, True))
    return out


def smooth_surface(mesh):
    """Share street vertices across triangles, preserving real kerb creases.

    Only continuous top surfaces use this; the separate vertical kerb mesh
    keeps flat normals. This saves vertices without removing geometric detail.
    """
    normals = {}
    for position,normal in zip(mesh.positions,mesh.normals):
        total = normals.setdefault(position,[0.,0.,0.])
        for i in range(3):
            total[i] += normal[i]
    for position,total in normals.items():
        size = math.sqrt(sum(n*n for n in total)) or 1.
        normals[position] = tuple(n/size for n in total)
    out = Mesh()
    remap = [out._vertex(p,normals[p],uv) for p,uv in zip(mesh.positions,mesh.texcoords)]
    out.indices = [remap[i] for i in mesh.indices]
    return out


def build_streets(roads, features, elevations, anchor, footprints=()):
    from .street_surfaces import build_surfaces
    return build_surfaces(roads, features, elevations, anchor, footprints)
