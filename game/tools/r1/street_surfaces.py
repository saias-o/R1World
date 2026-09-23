"""Topological street surfaces clipped to the actual terrain triangles."""
import math
import sys
from pathlib import Path

from .runtime_packages import ensure_dependencies
dependency_path = ensure_dependencies()
try:
    import shapely
    from shapely.geometry import LineString, Polygon, GeometryCollection
    from shapely.ops import unary_union
    from shapely import constrained_delaunay_triangles
except ImportError as error:
    raise RuntimeError("Impossible de charger les dependances embarquees. Python="
                       + sys.executable + "; packages=" + str(dependency_path)
                       + "; shapely=" + str(getattr(sys.modules.get("shapely"),"__file__",None))) from error
from .mesh import Mesh, MeshPart
from . import surfaces as surface_materials

# OSM `surface` values laid as cobbles or setts.
COBBLED = frozenset({"sett", "cobblestone", "unhewn_cobblestone", "cobblestone:flattened",
                     "paving_stones"})
from .streets import MOTOR, width, length_tag, sidewalk_sides, smooth_surface
from .terrain import ground_point, TERRAIN_MESH_SIZE


def pieces(geometry):
    if geometry.is_empty:
        return
    if hasattr(geometry,"geoms"):
        for g in geometry.geoms:
            yield from pieces(g)
    else:
        yield geometry


def surfaces_only(geometry):
    """The polygonal part of a union, as a geometry that has a boundary."""
    polygons = [g for g in pieces(geometry) if g.geom_type == "Polygon"]
    return unary_union(polygons) if polygons else GeometryCollection()


def build_surfaces(roads,features,elevations,anchor,footprints=()):
    stats = {"sidewalkSidesTagged":0,"sidewalkSidesInferred":0,
             "zebraCrossingsTagged":0,"widthsTagged":0,"widthsInferred":0,
             "unsupportedGradeSeparatedWays":0}
    road_polys, cobble_polys, walk_polys, motor_lines = [],[],[],[]
    for road in roads:
        tags = road.tags
        if tags.get("area")=="yes" or tags.get("footway")=="crossing":
            continue
        if tags.get("tunnel") not in {None,"no"} or tags.get("bridge") not in {None,"no"}:
            stats["unsupportedGradeSeparatedWays"] += 1
            continue
        pts = [anchor.geodetic_to_engine(lo,la,0.) for lo,la in road.points]
        line = LineString([(p[0],p[2]) for p in pts])
        if line.length < .1:
            continue
        half = width(tags)*.5
        strip = line.buffer(half,cap_style=2,join_style=2)
        stats["widthsTagged" if "width" in tags else "widthsInferred"] += 1
        if tags.get("highway") not in MOTOR:
            walk_polys.append(strip)
            continue
        # A surveyed sett or cobbled street is laid in cobbles, everything
        # else motorised in asphalt: the surface tag is a measurement (rule 4).
        (cobble_polys if tags.get("surface") in COBBLED else road_polys).append(strip)
        motor_lines.append((road.osm_id,line,half))
        for side,inferred in sidewalk_sides(tags):
            stats["sidewalkSidesInferred" if inferred else "sidewalkSidesTagged"] += 1
            sw = length_tag(tags.get("sidewalk:"+side+":width"),length_tag(tags.get("sidewalk:width"),1.8))
            # Engine Z points south, so geographic left has negative buffer.
            sign = -1 if side=="left" else 1
            walk_polys.append(line.buffer(sign*(half+sw),single_sided=True,
                                          cap_style=2,join_style=2).difference(strip))
    buildings = unary_union([Polygon(r).buffer(0) for r in footprints if len(r)>=3])
    asphalt = unary_union(road_polys).difference(buildings)
    cobbles = unary_union(cobble_polys).difference(buildings).difference(asphalt)
    paving = (unary_union(walk_polys).difference(asphalt).difference(cobbles)
              .difference(buildings))
    # Precision snapping removes submillimetre slivers, not surveyed detail.
    asphalt = shapely.set_precision(asphalt,.001)
    cobbles = shapely.set_precision(cobbles,.001).difference(asphalt)
    paving = shapely.set_precision(paving,.001).difference(asphalt).difference(cobbles)
    carriageway = unary_union([asphalt, cobbles])
    stripes = []
    seen = set()
    for f in features:
        if f.osm_id in seen or f.tags.get("highway")!="crossing":
            continue
        seen.add(f.osm_id)
        if f.tags.get("crossing:markings")!="zebra" and f.tags.get("crossing_ref")!="zebra":
            continue
        x,_,z = anchor.geodetic_to_engine(f.lon,f.lat,0.)
        from shapely.geometry import Point
        point = Point(x,z)
        matches = [(line.distance(point),oid,line,half) for oid,line,half in motor_lines]
        if not matches:
            continue
        distance,_,line,half = min(matches,key=lambda m:(m[0],m[1]))
        if distance>1.:
            continue
        along = line.project(point)
        a = line.interpolate(max(0.,along-.2)); b = line.interpolate(min(line.length,along+.2))
        size = a.distance(b)
        if size<1e-6:
            continue
        dx,dz = (b.x-a.x)/size,(b.y-a.y)/size
        centre = line.interpolate(along)
        span = length_tag(f.tags.get("width"),3.)/2
        offset = -half+.25
        while offset+.5<half:
            def p(s,t):
                return (centre.x+dx*s-dz*t,centre.y+dz*s+dx*t)
            stripes.append(Polygon([p(-span,offset),p(span,offset),p(span,offset+.5),p(-span,offset+.5)]))
            offset += 1.
        stats["zebraCrossingsTagged"] += 1
    paint = unary_union(stripes).intersection(asphalt)

    # Planar UVs in metres, so a pattern runs on across triangles of different
    # slope; the materials scale them by their scans' real sizes.
    road_mesh,cobble_mesh,walk_mesh,paint_mesh,kerb_mesh = (
        Mesh(uv_mode="planar"),Mesh(uv_mode="planar"),Mesh(uv_mode="planar"),Mesh(),Mesh())
    surfaces = [(asphalt,road_mesh,.06),(cobbles,cobble_mesh,.06),(paving,walk_mesh,.21),
                (paint,paint_mesh,.075)]
    bounds = elevations.bounds
    size = TERRAIN_MESH_SIZE-1
    grid = []
    for row in range(size+1):
        line = []
        for col in range(size+1):
            lo = bounds.west+(bounds.east-bounds.west)*col/size
            la = bounds.south+(bounds.north-bounds.south)*row/size
            line.append(anchor.geodetic_to_engine(lo,la,elevations.sample(lo,la)))
        grid.append(line)

    # A surface triangle never crosses a terrain triangle edge. Every road
    # therefore has the same underlying plane as the visible ground beneath it.
    for row in range(size):
        for col in range(size):
            sw,se,ne,nw = grid[row][col],grid[row][col+1],grid[row+1][col+1],grid[row+1][col]
            for a,b,c in ((sw,se,ne),(sw,ne,nw)):
                cell = Polygon([(p[0],p[2]) for p in (a,b,c)])
                denom = (b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2])
                def vertex(x,z,lift):
                    wa = ((b[2]-c[2])*(x-c[0])+(c[0]-b[0])*(z-c[2]))/denom
                    wb = ((c[2]-a[2])*(x-c[0])+(a[0]-c[0])*(z-c[2]))/denom
                    return (round(x,6),round(wa*a[1]+wb*b[1]+(1-wa-wb)*c[1]+lift,6),round(z,6))
                for surface,mesh,lift in surfaces:
                    if not surface.intersects(cell):
                        continue
                    clipped = surface.intersection(cell)
                    for part in pieces(clipped):
                        if part.geom_type!="Polygon" or part.area<1e-6:
                            continue
                        for triangle in shapely.constrained_delaunay_triangles(part).geoms:
                            mesh.add_up_triangle(*(vertex(x,z,lift) for x,z in list(triangle.exterior.coords)[:3]))

    # Kerbs only at the contact between pavement and carriageway; no arbitrary
    # vertical walls across junctions, no doubled borders under separate ways.
    # A union of no roads/sidewalks is an empty GeometryCollection, and a
    # union that had to keep a degenerate sliver is a *non-empty* one. GEOS has
    # no boundary for that type either way (None), so neither can participate
    # in intersection or line_merge -- and the non-empty case is the one that
    # got past the emptiness test and crashed the worker on an avenue in Paris,
    # where it read on screen as a three-minute data timeout. Only the
    # polygonal part of each surface has a kerb, so only it is asked for one.
    contact = GeometryCollection()
    kerb_paving, kerb_asphalt = surfaces_only(paving), surfaces_only(carriageway)
    if not kerb_paving.is_empty and not kerb_asphalt.is_empty:
        contact = shapely.line_merge(
            kerb_paving.boundary.intersection(kerb_asphalt.boundary)).simplify(.02)
    for line in pieces(contact):
        if line.geom_type!="LineString":
            continue
        coords = list(line.coords)
        for a,b in zip(coords,coords[1:]):
            n = max(1,math.ceil(math.dist(a,b)/8.))
            def p(t,lift):
                x,z = a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t
                lo,la,_ = anchor.engine_to_geodetic(x,0,z)
                return (x,ground_point(lo,la,elevations,anchor)[1]+lift,z)
            for i in range(n):
                kerb_mesh.add_quad(p(i/n,.06),p((i+1)/n,.06),p((i+1)/n,.21),p(i/n,.21))

    parts = []
    for name,mesh,color,family in (("Carriageway",road_mesh,(.10,.11,.12),"asphalt"),
                                   ("Cobbled carriageway",cobble_mesh,(.16,.155,.145),"cobbles"),
                                   ("Sidewalks",walk_mesh,(.22,.215,.20),"pavement"),
                                   ("Kerbs",kerb_mesh,(.26,.25,.23),None),
                                   ("Surveyed zebra crossings",paint_mesh,(.50,.49,.46),None)):
        if mesh.indices:
            if name!="Kerbs":
                mesh = smooth_surface(mesh)
            parts.append(MeshPart(name,mesh,surface_materials.material(
                name,color,.88,family,double_sided=name=="Kerbs")))
    stats["carriagewayAreaM2"] = round(asphalt.area,2)
    stats["cobbledAreaM2"] = round(cobbles.area,2)
    stats["sidewalkAreaM2"] = round(paving.area,2)
    stats["surfaceOverlapM2"] = round(asphalt.intersection(paving).area,6)
    return parts,stats
