"""Probe the actual native geometry dependency, not just its package name."""
from r1.street_surfaces import Polygon, constrained_delaunay_triangles

triangles = constrained_delaunay_triangles(Polygon([(0,0),(1,0),(0,1)]))
if len(triangles.geoms) != 1:
    raise RuntimeError("GEOS triangulation probe failed")
