"""R1World's desktop data worker. Saida itself remains unmodified.

The game atomically publishes a small priority list. This process downloads and
cooks one tile at a time, publishing ready.json only after all assets exist.
Requests are re-read between tiles, so teleporting supersedes old prefetch work.
"""
import argparse
import json
import math
import os
import time
import hashlib
import shutil
import threading
import traceback
from dataclasses import asdict
from pathlib import Path

from .world_tiles import Tile, VERSION
from .sources import fetch_osm, fetch_elevation_grid, normalize_osm, OsmWay, Bounds
from .geodesy import Anchor
from .atlas import RegionProfile, profile_for
from . import ground as ground_materials
from . import props as street_props
from . import nature
from .streets import build_streets, smooth_surface
from . import traffic as road_traffic
from .world_elevation import fetch_ground
from .mesh import MeshPart, write_glb
from . import surfaces
from .buildings import build_buildings
from .terrain import (
    TERRAIN_MESH_SIZE, build_terrain, build_roads, ground_point)

GAME = Path(__file__).resolve().parents[2]
CACHE = GAME / "cache" / "world"


# The world's buildings are the full building chain with its most expensive
# stage switched off. `detail_radius` is §12.4 — le détail suit la vitesse — applied
# at generation time, and a negative radius means *no building is near*: every
# facade is a plain textured wall rather than modelled openings. A city tile
# built with modelled openings exhausts Saida's vertex arena on its own, and
# openings are rank 11 of the fidelity hierarchy. Everything above them — the
# footprint, the height, the roof, the material — is unaffected by this line,
# which is exactly why it is the line that gives way.
WORLD_DETAIL_RADIUS = -1.0

# Roofs without a fascia or a soffit, for the same reason as the radius above
# and at a larger saving: the slab edges were 46% of every vertex in the
# densest tile the project has cooked. See `buildings.build_buildings`.
WORLD_ROOF_THICKNESS = 0.0

# §4 I4: the budget is a contract, not an objective. Exceeding it raises and
# the tile is not published — a city that will not fit is a bug to be seen,
# never a city quietly missing a third of its buildings.
#
# The real ceiling is not here. Saida's geometry arena holds 1 048 576 vertices
# for the whole scene, `native/world.cpp` keeps nine tiles resident inside it,
# and that runtime check is the one that owns the arena. This number is the
# per-tile sanity bound underneath it: a single tile approaching a sixth of the
# planet's entire geometry budget is a generator that has gone wrong, not a
# city that is unusually dense.
#
# Measured, once the Atlas gave the world its roofs and the preview LOD gave up
# their fascias: Amsterdam's canal belt, 1 381 buildings on one tile, 106 522
# vertices; Paris rue de Rivoli, 524 buildings, 82 000. The bound is the worst
# of those plus an eighth.
TILE_VERTEX_BUDGET = 120_000


def compact_buildings(buildings, ground, profile: RegionProfile):
    """The tile's building stock: measured where OSM says, inferred where not.

    Until this went through the Atlas the world was flat roofs, one masonry
    grey and three storeys everywhere — ranks 5, 6 and 7 of §2.1 answered with
    a constant. A constant is not a cheap approximation of a region, it is the
    absence of one, and it is what made Tunis, Tokyo and Chamonix the same town
    with different terrain under it.

    Nothing measured changed. `plan_gabarit` still puts a tagged height, a
    tagged storey count and a tagged roof shape ahead of anything the profile
    offers, and the manifest still records which of the two answered (§4 I5).
    """
    # Each wall and roof swatch is shown in the material its name says it is
    # made of (`surfaces.py`), at the albedo the region gave it.
    return build_buildings(buildings, ground, profile,
                           detail_radius=WORLD_DETAIL_RADIUS,
                           roof_thickness=WORLD_ROOF_THICKNESS,
                           wall_materials=lambda swatch, double_sided: surfaces.material(
                               swatch.name, swatch.color, swatch.roughness,
                               surfaces.wall_family(swatch.name), double_sided=double_sided),
                           roof_materials=lambda swatch, double_sided: surfaces.material(
                               swatch.name, swatch.color, swatch.roughness,
                               surfaces.roof_family(swatch.name), double_sided=double_sided))


# One cell per terrain quad, so the bitmap and the picture cannot drift apart.
WATER_GRID = TERRAIN_MESH_SIZE - 1


def _water_grid(bounds, landcover):
    """Rows of '0' and '1', south to north, west to east.

    A cell is water when its centre is, which is the same question the terrain
    triangles were classified by and therefore the same answer. At about
    fourteen metres a cell, a canal is water and a stream is not; the module
    docstring of `ground.py` argues why that is the right failure.
    """
    rows = []
    for row in range(WATER_GRID):
        lat = bounds.south + (bounds.north - bounds.south) * (row + .5) / WATER_GRID
        line = []
        for col in range(WATER_GRID):
            lon = bounds.west + (bounds.east - bounds.west) * (col + .5) / WATER_GRID
            line.append("1" if landcover.at(lon, lat) == "water" else "0")
        rows.append("".join(line))
    return rows


def clip_roads(roads, bounds):
    """Clip complete Overpass ways before elevation sampling (Liang–Barsky)."""
    result = []
    for road in roads:
        for a,b in zip(road.points,road.points[1:]):
            dx,dy=b[0]-a[0],b[1]-a[1]
            lo,hi=0.,1.
            valid=True
            for p,q in ((-dx,a[0]-bounds.west),(dx,bounds.east-a[0]),
                        (-dy,a[1]-bounds.south),(dy,bounds.north-a[1])):
                if abs(p)<1e-15:
                    if q<0: valid=False;break
                elif p<0: lo=max(lo,q/p)
                else: hi=min(hi,q/p)
            if valid and lo<hi:
                result.append(OsmWay(road.osm_id,((a[0]+lo*dx,a[1]+lo*dy),(a[0]+hi*dx,a[1]+hi*dy)),road.tags))
    return tuple(result)


# Windows refuses to replace a file another process currently has open, and the
# game opens status.json every 250 ms while it waits for a spawn. The two cross
# eventually -- measured: after eight tiles of a nine-tile teleport -- and the
# PermissionError used to propagate out of serve() and kill the worker outright.
# Nothing then cooked another tile ever again, and the player watched a counter
# sit at "1 / 9" with no error anywhere on screen. The replace itself is fine on
# the next attempt: the reader holds the handle for microseconds.
REPLACE_ATTEMPTS = 12
REPLACE_PAUSE = 0.05


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(value, separators=(",", ":"), allow_nan=False), encoding="utf-8")
    for attempt in range(1, REPLACE_ATTEMPTS + 1):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == REPLACE_ATTEMPTS:
                raise
            time.sleep(REPLACE_PAUSE)


def cook(tile, osm_cache=None):
    folder = CACHE / tile.key
    ready = folder / "ready.json"
    if ready.exists():
        return json.loads(ready.read_text(encoding="utf-8"))
    folder.mkdir(parents=True, exist_ok=True)
    # Reuse raw observations across generator revisions, never old geometry.
    # Every earlier version is searched, newest first, rather than only the one
    # immediately before: a player who skips a release would otherwise
    # re-download a tile whose observations are already sitting on their disk.
    for older in range(VERSION - 1, 0, -1):
        previous = CACHE / f"v{older}_{tile.row}_{tile.col}"
        for name in ("osm.json", "elevation.json", "ground-elevation.json"):
            if not (folder / name).exists() and (previous / name).exists():
                shutil.copy2(previous / name, folder / name)
    bounds = tile.bounds
    lon, lat = tile.center
    refresh = False
    if (folder / "elevation.json").exists():
        saved = json.loads((folder / "elevation.json").read_text(encoding="utf-8"))
        refresh = any(abs(saved["bounds"][k]-v)>1e-9 for k,v in asdict(bounds).items())
    # ~90 m spacing matches GLO-90: more samples only oversample the source
    # and consume public-service quota without adding measured detail.
    elevations, elevation_source = fetch_ground(bounds, folder)
    if any(not math.isfinite(h) for row in elevations.values for h in row):
        raise ValueError("Elevation source returned non-finite data")
    document = fetch_osm(bounds, osm_cache or folder / "osm.json",
                         refresh=refresh and osm_cache is None)
    # A tile built from observations answering an older question keeps saying so
    # rather than looking like a place OSM never mapped (see `fetch_osm`).
    osm_query_version = int(document.get("r1QueryVersion", 1))
    osm = normalize_osm(document)
    anchor = Anchor.at(lon, lat, 0)
    ground = lambda x, y: ground_point(x, y, elevations, anchor)
    # Assign each building to exactly one tile, using an actual footprint point.
    # Full ways (including outside vertices) are retained by Overpass.
    from .world_tiles import tile_at
    buildings = tuple(w for w in osm.buildings if tile_at(*w.points[0]) == tile)
    profile = profile_for(lon, lat)
    parts, footprints, stats = compact_buildings(buildings, ground, profile)
    # Rank 9. The terrain mesh is partitioned by what OSM says the ground is,
    # and by what the region says it usually is where OSM says nothing. This
    # adds no vertices at all -- see ground.py -- and it is the difference
    # between a planet that is one green and a planet that has deserts.
    landcover = ground_materials.Landcover(osm.landcover)
    # Which ground, where: the class OSM mapped (or the region's own), turned
    # to snow above the snowline and to frost just below it (`surfaces.py`).
    climate = surfaces.climate_at(profile.climate, lat)

    def classify(x, y):
        name = landcover.at(x, y) or ground_materials.INFERRED
        if name == "water":
            return name
        return name + surfaces.cold_suffix(y, elevations.sample(x, y), climate)

    terrain_meshes = build_terrain(bounds, elevations, anchor, classify=classify)
    # The terrain is continuous ground, not hard-edged masonry. Smooth normals
    # let adjacent triangles share vertices without changing the measured relief.
    terrain_meshes = {name: smooth_surface(mesh) for name,mesh in terrain_meshes.items()}
    # Where the water is, at the resolution it is drawn at. A canal you can
    # stroll across is a worse lie than a canal that is not there at all, and
    # the game has no way to know about one: `blocked()` reads building
    # footprints and nothing else. Rather than ship the polygons a second time
    # in another coordinate system -- and let the collision and the picture
    # disagree about where the bank is -- the grid the terrain was *classified*
    # on is shipped as a bitmap. It is 40x40 characters, it is exactly what the
    # eye sees, and testing it is two divisions instead of a walk over every
    # ring in the neighbourhood.
    water_grid = _water_grid(bounds, landcover)
    terrain_parts, ground_stats = [], {}
    for name in sorted(terrain_meshes):
        swatch = ground_materials.material_for(name, profile)
        ground_stats[name] = len(terrain_meshes[name].indices) // 3
        family = surfaces.ground_family(name, profile.ground.name, climate)
        terrain_parts.append(MeshPart(
            f"Ground — {swatch.name}", terrain_meshes[name],
            surfaces.material(swatch.name, swatch.color, swatch.roughness, family)))
    clipped_roads = clip_roads(osm.roads, bounds)
    street_parts, street_stats = build_streets(clipped_roads, osm.features, elevations, anchor, footprints)
    # The road centre lines in engine metres, so a bench can be turned to face
    # the street it belongs to rather than a seeded direction (props.py).
    road_segments = tuple(
        ((ground(*way.points[0])[0], ground(*way.points[0])[2]),
         (ground(*way.points[-1])[0], ground(*way.points[-1])[2]))
        for way in clipped_roads)
    # Rank 10's moving half. The lane graph is cooked with the street it
    # belongs to, so traffic cannot outlive the roads it drives on.
    lane_graph = road_traffic.build_graph(clipped_roads, ground, len(buildings), lon, lat)
    parts = terrain_parts + street_parts + parts
    # Resource budget is enforced per tile, never silently truncate a city.
    vertices = sum(len(p.mesh.positions) for p in parts)
    if vertices > TILE_VERTEX_BUDGET:
        raise ValueError(f"Tile exceeds geometry budget ({vertices} vertices): " +
                         str({p.name: len(p.mesh.positions) for p in parts}))
    ocean = not buildings and not osm.roads and all(abs(h)<.01 for row in elevations.values for h in row)
    # Fraction of the tile whose ground came from a survey rather than from the
    # region. It is the one number that says how much of what you are standing
    # on anybody actually looked at (§4 I5).
    triangles = sum(ground_stats.values()) or 1
    inferred = sum(n for key, n in ground_stats.items()
                   if key.partition("@")[0] == ground_materials.INFERRED)
    measured_ground = 1. - inferred/triangles
    if not ocean: write_glb(folder / "world.glb", parts)
    # Props are scene nodes and not tile geometry, and that is the whole reason
    # they are affordable: Saida's MeshCache keys meshes by asset, so six
    # hundred nodes pointing at one oak upload one oak. Baking them into
    # world.glb instead would charge the vertex arena six hundred times and
    # would not fit -- see props.py.
    if ocean:
        children = [{"type": "Water", "name": "Ocean", "size": 450., "amplitude": .05,
                     "wavelength": 12., "shoreMode": 0}]
        prop_nodes, prop_stats = [], street_props.PropStats()
        nature_stats = {"revision":nature.REVISION,"placed":0}
    else:
        children = [{"type": "Node", "name": "Geography", "enabled": True,
                     "importedFrom": f"cache/world/{tile.key}/world.glb"}]
        in_tile = tuple(f for f in osm.features
                        if bounds.west <= f.lon <= bounds.east
                        and bounds.south <= f.lat <= bounds.north)
        prop_nodes, prop_stats = street_props.plan_props(
            in_tile, ground, profile, road_segments, GAME)
        nature_nodes, nature_stats = nature.plan_nature(osm,tile,anchor,ground,GAME)
        children.extend(nature_nodes)
        children.extend(prop_nodes)
    scene = {"schema": 2, "version": 2, "scene": {
        "type": "Node", "name": tile.key, "enabled": True, "children": children}}
    atomic_json(folder / "tile.scene", scene)
    result = {"key": tile.key, "row": tile.row, "col": tile.col,
              "lon": lon, "lat": lat, "bounds": asdict(bounds),
              "elevations": elevations.values, "footprints": footprints,
              "vertices": 0 if ocean else vertices, "buildings": len(buildings),
              "surface": "ocean" if ocean else "land",
              "source": "OpenStreetMap; " + elevation_source,
              "elevationSource": elevation_source,
              # I5 in the file the game reads: the region that answered, how
              # much authority it had, and how many buildings owe it their
              # height and their roof rather than owing them to a survey.
              "region": profile.name, "regionTier": profile.tier,
              "climate": climate,
              "osmQueryVersion": osm_query_version,
              "ground": {"measuredFraction": round(measured_ground, 4),
                         "trianglesByClass": ground_stats},
              "water": water_grid,
              "props": prop_stats.as_json(),
              "nature": nature_stats,
              "streets": street_stats,
              "traffic": {"nodes": [], "lanes": [], "cars": 0,
                          "leftHand": lane_graph["leftHand"]} if ocean else lane_graph,
              "inference": stats.as_json()}
    atomic_json(ready, result)
    return result


def report(session, value):
    """Publish what the worker is doing. Best effort, on purpose.

    A status line is something the player reads, not something the world depends
    on. Losing one to a file race costs a stale sentence for 250 ms; letting the
    failure escape costs every tile from here on, which is the trade that made
    the counter freeze. The heartbeat carries the same reasoning.
    """
    try:
        atomic_json(session / "status.json", value)
    except OSError as error:
        print("STATUS-WRITE-FAILED", error, flush=True)


def heartbeat(session, stopped):
    """Keep liveness independent of slow downloads and CPU tile cooking."""
    while not stopped.is_set():
        try:
            atomic_json(session / "heartbeat.json", {"time": time.time()})
        except OSError as error:
            print("HEARTBEAT-WRITE-FAILED", error, flush=True)
        stopped.wait(1.)


def serve(session):
    stopped = threading.Event()
    pulse = threading.Thread(target=heartbeat, args=(session, stopped), daemon=True)
    pulse.start()
    try:
        serve_requests(session)
    finally:
        stopped.set()
        pulse.join()


def serve_requests(session):
    request = session / "request.json"
    failures = {}
    next_download = 0.
    cooldown = 0.
    while not (session / "stop").exists():
        try:
            doc = json.loads(request.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            time.sleep(.2)
            continue
        if time.monotonic()<cooldown:
            time.sleep(.2)
            continue
        work = False
        for item in doc.get("tiles", [])[:25]:
            tile = Tile(int(item[0]), int(item[1]))
            if (CACHE / tile.key / "ready.json").exists():
                continue
            if time.monotonic() < failures.get(tile.key, 0):
                continue
            work = True
            report(session, {"key": tile.key, "state": "preparing"})
            try:
                # One OSM query for the complete current neighborhood, rather
                # than nine almost-identical queries at every spawn.
                desired = [Tile(int(t[0]),int(t[1])) for t in doc.get("tiles",[])[:25]]
                bb = [t.bounds for t in desired]
                region = Bounds(min(b.south for b in bb),min(b.west for b in bb),
                                max(b.north for b in bb),max(b.east for b in bb))
                shared = None
                if region.east-region.west<.15:
                    key=hashlib.sha256(json.dumps(asdict(region),sort_keys=True).encode()).hexdigest()[:20]
                    shared=CACHE/"sources"/(key+".json")
                    if not shared.exists():
                        if time.monotonic()<next_download:
                            time.sleep(.2)
                            break
                        next_download=time.monotonic()+20.
                        fetch_osm(region,shared)
                result = cook(tile,shared)
                report(session, {"key": tile.key, "state": "ready",
                                 "buildings": result["buildings"]})
                print("READY", tile.key, flush=True)
            except Exception as error:
                failures[tile.key] = time.monotonic() + 60
                next_download = time.monotonic() + 60
                cooldown = time.monotonic() + 60
                report(session, {"key": tile.key, "state": "error", "error": str(error)})
                print("ERROR", tile.key, str(error), flush=True)
                traceback.print_exc()
            break
        if not work:
            time.sleep(.2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--session", type=Path)
    parser.add_argument("--tile", nargs=2, type=int)
    args = parser.parse_args()
    if args.tile:
        print(json.dumps(cook(Tile(*args.tile))))
    elif args.session:
        serve(args.session.resolve())
    else:
        parser.error("--session or --tile is required")


if __name__ == "__main__":
    main()
