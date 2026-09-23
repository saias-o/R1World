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
from .sources import (fetch_osm, fetch_elevation_grid, normalize_osm,
                      ElevationGrid, SourceUnavailable, OsmWay, Bounds)
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
from . import harbours
from . import offline_world
from .polygons import point_in_polygon
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
    """Rows of '0' and '1', south to north, west to east, for a tile with no sea.

    A cell is water when its centre is, which is the same question the terrain
    triangles were classified by and therefore the same answer. At about
    fourteen metres a cell, a canal is water and a stream is not; the module
    docstring of `ground.py` argues why that is the right failure. A tile that
    touches the sea marks its sea-level water '2' (`harbours.Cells`).
    """
    return harbours.Cells(bounds, WATER_GRID, None, landcover, lambda lon, lat: 0.0).rows()


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


def cook(tile, osm_cache=None, *, offline=False, upgrade=False):
    root = CACHE / tile.key
    ready = root / "ready.json"
    if ready.exists() and not upgrade:
        return json.loads(ready.read_text(encoding="utf-8"))
    folder = root / "offline" if offline else root
    folder.mkdir(parents=True, exist_ok=True)
    bounds = tile.bounds
    lon, lat = tile.center
    if offline:
        elevations = ElevationGrid(bounds, 2, ((0., 0.), (0., 0.)))
        elevation_source = "flat offline approximation"
        document = {"elements": [], "r1QueryVersion": 0}
    else:
        # Reuse raw observations across generator revisions, never old geometry.
        for older in range(VERSION - 1, 0, -1):
            previous = CACHE / f"v{older}_{tile.row}_{tile.col}"
            for name in ("osm.json", "elevation.json", "ground-elevation.json"):
                if not (folder / name).exists() and (previous / name).exists():
                    shutil.copy2(previous / name, folder / name)
        refresh = False
        if (folder / "elevation.json").exists():
            saved = json.loads((folder / "elevation.json").read_text(encoding="utf-8"))
            refresh = any(abs(saved["bounds"][k]-v)>1e-9 for k,v in asdict(bounds).items())
        # ~90 m spacing matches GLO-90: more samples only oversample the source.
        elevations, elevation_source = fetch_ground(bounds, folder)
        document = fetch_osm(bounds, osm_cache or folder / "osm.json",
                             refresh=refresh and osm_cache is None)
    if any(not math.isfinite(h) for row in elevations.values for h in row):
        raise ValueError("Elevation source returned non-finite data")
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
    # A lighthouse is drawn as a tower on its node (harbours.py), so the
    # building OSM may also have traced around it is not extruded a second time.
    # One traced with no node becomes the node, at its footprint's centre.
    light_features = tuple(osm.features) + tuple(
        harbours.traced_lighthouse(w) for w in buildings
        if harbours.is_lighthouse(w.tags)
        and not any(n.tags.get("man_made") == "lighthouse" and point_in_polygon((n.lon, n.lat), list(w.points))
                    for n in osm.features))
    lights = harbours.lighthouse_points(light_features)
    if lights:
        buildings = tuple(
            w for w in buildings
            if w.tags.get("building") != "lighthouse" and w.tags.get("man_made") != "lighthouse"
            and not any(point_in_polygon(p, list(w.points)) for p in lights))
    profile = profile_for(lon, lat)
    climate = surfaces.climate_at(profile.climate, lat)
    parts, footprints, stats = compact_buildings(buildings, ground, profile)
    # Rank 9. The terrain mesh is partitioned by what OSM says the ground is,
    # and by what the region says it usually is where OSM says nothing. This
    # adds no vertices at all -- see ground.py -- and it is the difference
    # between a planet that is one green and a planet that has deserts.
    landcover = ground_materials.Landcover(osm.landcover)
    # The sea, rebuilt from the coastline, and the tile's cells: land, inland
    # water, sea-level water (harbours.py). The terrain under the sea is sunk
    # beneath the animated surface that covers it.
    harbour_stats = harbours.HarbourStats()
    if offline:
        sea = offline_world.sea_in(bounds)
        harbour_stats.coastline = "Natural Earth 1:110m approximation"
    else:
        sea, harbour_stats.coastline = harbours.sea_geometry(
            osm.coastlines, bounds, elevations.sample)
    cells = harbours.Cells(bounds, WATER_GRID, sea, landcover, elevations.sample,
                           harbours.tidal_water(osm.landcover, osm.maritime))
    harbour_stats.sea_cells = sum(line.count(2) for line in cells.codes)

    # Which ground, where: the class OSM mapped (or the region's own), turned
    # to snow above the snowline and to frost just below it (`surfaces.py`).
    def classify(x, y):
        if cells.sea_at(x, y):
            return "water"
        name = landcover.at(x, y) or ground_materials.INFERRED
        if name == "water":
            return name
        return name + surfaces.cold_suffix(y, elevations.sample(x, y), climate)

    terrain_meshes = build_terrain(bounds, elevations, anchor, classify=classify,
                                   adjust=cells.adjust)
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
    water_grid = cells.rows()
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
    # The sea's works, as OSM mapped them: piers, breakwaters, quays and
    # lighthouses. Tile geometry, like the streets they belong to.
    harbour_parts, decks = harbours.build_works(osm.maritime, light_features, ground, anchor,
                                                cells, profile, harbour_stats)
    parts = terrain_parts + street_parts + harbour_parts + parts
    vertices = sum(len(p.mesh.positions) for p in parts)
    if vertices > TILE_VERTEX_BUDGET and harbour_stats.piers:
        # The one detail given up before a tile is refused: the piles under the
        # piers, a rank-12 detail between a deck and the water. Said in the
        # manifest (`pilesDropped`), never silent.
        harbour_parts_bare, decks = harbours.build_works(
            osm.maritime, osm.features, ground, anchor, cells, profile,
            harbours.HarbourStats(), piles=False)
        parts = terrain_parts + street_parts + harbour_parts_bare + parts[len(terrain_parts)
                                                                           + len(street_parts)
                                                                           + len(harbour_parts):]
        harbour_stats.piles_dropped = True
    # Resource budget is enforced per tile, never silently truncate a city.
    vertices = sum(len(p.mesh.positions) for p in parts)
    if vertices > TILE_VERTEX_BUDGET:
        raise ValueError(f"Tile exceeds geometry budget ({vertices} vertices): " +
                         str({p.name: len(p.mesh.positions) for p in parts}))
    ocean = (sea is not None and sea.area >=
             (bounds.east-bounds.west)*(bounds.north-bounds.south)*(1-1e-9)) if offline else (
             not buildings and not osm.roads and
             all(abs(h)<.01 for row in elevations.values for h in row))
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
    boat_manifest = []
    if ocean:
        # The plane covers this tile and no more (harbours.sea_node).
        children = [dict(harbours.sea_node(bounds, anchor, "Ocean"), amplitude=.05, wavelength=12.)]
        prop_nodes, prop_stats = [], street_props.PropStats()
        nature_stats = {"revision":nature.REVISION,"placed":0}
    else:
        children = [{"type": "Node", "name": "Geography", "enabled": True,
                     "importedFrom": f"cache/world/{tile.key}/{'offline/' if offline else ''}world.glb"}]
        in_tile = tuple(f for f in osm.features
                        if bounds.west <= f.lon <= bounds.east
                        and bounds.south <= f.lat <= bounds.north)
        prop_nodes, prop_stats = street_props.plan_props(
            in_tile, ground, profile, road_segments, GAME)
        nature_nodes, nature_stats = nature.plan_nature(osm,tile,anchor,ground,GAME)
        children.extend(nature_nodes)
        children.extend(prop_nodes)
        # The harbours' boats and their container yards: inferred, scene nodes,
        # every boat listed below so the game can board it.
        if cells.has_sea:
            children.append(harbours.sea_node(bounds, anchor))
        berths = harbours.plan_boats(osm.maritime, osm.coastlines, osm.landcover, osm.features,
                                     anchor, cells, profile, climate, GAME, harbour_stats)
        boat_children, boat_manifest = harbours.boat_nodes(berths, anchor, GAME)
        children.extend(boat_children)
        children.extend(harbours.plan_containers(osm.maritime, osm.landcover, anchor, cells,
                                                 footprints, ground, GAME, harbour_stats))
    scene = {"schema": 2, "version": 2, "scene": {
        "type": "Node", "name": tile.key, "enabled": True, "children": children}}
    atomic_json(folder / "tile.scene", scene)
    result = {"key": tile.key, "row": tile.row, "col": tile.col,
              "lon": lon, "lat": lat, "bounds": asdict(bounds),
              "elevations": elevations.values, "footprints": footprints,
              "vertices": 0 if ocean else vertices, "buildings": len(buildings),
              "surface": "ocean" if ocean else "land",
              "source": ("Natural Earth 1:110m; " if offline else "OpenStreetMap; ") + elevation_source,
              "elevationSource": elevation_source,
              "offlineApproximation": offline,
              # I5 in the file the game reads: the region that answered, how
              # much authority it had, and how many buildings owe it their
              # height and their roof rather than owing them to a survey.
              "region": profile.name, "regionTier": profile.tier,
              "climate": climate,
              "osmQueryVersion": osm_query_version,
              "ground": {"measuredFraction": round(measured_ground, 4),
                         "trianglesByClass": ground_stats},
              "water": water_grid,
              # Walkable decks over the water (piers), in this tile's frame.
              "decks": decks,
              "boats": boat_manifest,
              "harbour": harbour_stats.as_json(),
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
    # Ships at sea: planned from the local base on the game's requests, and
    # the base realigned whenever the network answers (r1/sea_traffic.py).
    # Its own threads and its own log: tiles never wait for the sea.
    from .sea_traffic import Sea
    from .local_conditions import LocalConditions
    sea = None
    conditions = LocalConditions(session, stopped, atomic_json)
    try:
        sea = Sea(session)
        sea.start()
        conditions.start()
        serve_requests(session)
    finally:
        if sea is not None:
            sea.close()
        stopped.set()
        conditions.close()
        pulse.join()


def _cached_sources(tile):
    """Can this tile be rebuilt without contacting either remote source?"""
    folders = (CACHE / f"v{version}_{tile.row}_{tile.col}"
               for version in range(VERSION, 0, -1))
    have_osm = have_elevation = False
    for folder in folders:
        have_osm |= (folder / "osm.json").exists()
        have_elevation |= ((folder / "ground-elevation.json").exists() or
                           (folder / "elevation.json").exists())
        if have_osm and have_elevation:
            return True
    return False


def _shared_source(tiles):
    """The neighborhood query is optional when local observations suffice."""
    if not tiles:
        return None, None
    bb = [tile.bounds for tile in tiles]
    region = Bounds(min(b.south for b in bb), min(b.west for b in bb),
                    max(b.north for b in bb), max(b.east for b in bb))
    if region.east-region.west >= .15:
        return None, None
    key = hashlib.sha256(json.dumps(asdict(region), sort_keys=True).encode()).hexdigest()[:20]
    return region, CACHE / "sources" / (key + ".json")


def serve_requests(session):
    request = session / "request.json"
    failures = {}
    ready_states = {}
    offline_until = 0.
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
        desired = [Tile(int(item[0]), int(item[1])) for item in doc.get("tiles", [])[:25]]
        groups = [[Tile(int(item[0]), int(item[1])) for item in group]
                  for group in doc.get("groups", [])]
        if not groups:
            groups = [desired]  # requests from older game binaries
        def source_for(tile):
            return _shared_source(next((group for group in groups if tile in group), [tile]))
        work = False
        approximate = []
        for tile in desired:
            ready = CACHE / tile.key / "ready.json"
            if ready.exists():
                try:
                    stamp = ready.stat().st_mtime_ns
                    if ready not in ready_states or ready_states[ready][0] != stamp:
                        approximate_ready = json.loads(ready.read_text(encoding="utf-8")).get(
                            "offlineApproximation", False)
                        ready_states[ready] = (stamp, approximate_ready)
                    if ready_states[ready][1]:
                        approximate.append(tile)
                except (OSError, ValueError):
                    pass  # A concurrent atomic replacement; retry next poll.
                continue
            if time.monotonic() < failures.get(tile.key, 0):
                continue
            work = True
            report(session, {"key": tile.key, "state": "preparing"})
            try:
                region, shared = source_for(tile)
                cached = _cached_sources(tile)
                if time.monotonic() < offline_until and not cached:
                    result = cook(tile, offline=True)
                else:
                    # Shared Overpass saves eight queries, but is never a
                    # prerequisite for a tile with its own saved observations.
                    if shared and not shared.exists() and not cached:
                        fetch_osm(region, shared)
                    result = cook(tile, shared if shared and shared.exists() else None)
            except SourceUnavailable as error:
                offline_until = time.monotonic() + 60
                print("OFFLINE", tile.key, str(error), flush=True)
                try:
                    result = cook(tile, offline=True)
                except Exception as fallback_error:
                    failures[tile.key] = time.monotonic() + 60
                    report(session, {"key": tile.key, "state": "error", "error": str(fallback_error)})
                    print("ERROR", tile.key, str(fallback_error), flush=True)
                    traceback.print_exc()
                    break
            except Exception as error:
                failures[tile.key] = time.monotonic() + 60
                cooldown = time.monotonic() + 60
                report(session, {"key": tile.key, "state": "error", "error": str(error)})
                print("ERROR", tile.key, str(error), flush=True)
                traceback.print_exc()
                break
            if ready.exists():
                report(session, {"key": tile.key, "state": "ready",
                                 "buildings": result["buildings"],
                                 "offlineApproximation": result.get("offlineApproximation", False)})
                print("OFFLINE-READY" if result.get("offlineApproximation") else "READY",
                      tile.key, flush=True)
            break
        if not work and approximate and time.monotonic() >= offline_until:
            # A provisional tile remains playable while detailed observations
            # are fetched into separate files. ready.json changes only last.
            tile = approximate[0]
            try:
                region, shared = source_for(tile)
                if shared and not shared.exists():
                    fetch_osm(region, shared)
                cook(tile, shared if shared and shared.exists() else None, upgrade=True)
                print("UPGRADED", tile.key, flush=True)
            except SourceUnavailable:
                offline_until = time.monotonic() + 60
            except Exception as error:
                failures[tile.key] = time.monotonic() + 60
                offline_until = time.monotonic() + 60
                print("UPGRADE-FAILED", tile.key, str(error), flush=True)
                traceback.print_exc()
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
