"""Acquire and normalize the OSM and elevation data a world tile is built from.

Public services are used only by this offline authoring step. Runtime builds
contain generated geometry and visible attribution; they never query Overpass
or Open-Meteo while the player is moving.
"""

from __future__ import annotations

import errno
import json
import math
import socket
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .geodesy import R_MEAN


# Ordered by measured reliability, not by preference. A mirror is only listed
# here once it has answered a real bbox query with the ways actually in it: an
# endpoint that returns 200 with an empty element list is worse than one that
# fails, because the empty answer gets cached as a valid tile and the city is
# gone for good. `overpass.osm.ch` is exactly that trap -- it is a Swiss extract
# and answers 200/empty for anywhere else -- and it is deliberately absent.
OSM_ENDPOINTS = (
    "https://overpass-api.de/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
)


class SourceUnavailable(RuntimeError):
    """Remote observations are unavailable; callers may use local data."""


ELEVATION_ENDPOINT = "https://api.open-meteo.com/v1/elevation"
USER_AGENT = "R1World-generator/1.0"

# Statuses that mean "not now" rather than "not ever". A public Overpass mirror
# answers 429 when the two concurrent slots are taken and 502/503/504 when it is
# overloaded or restarting, and both clear on their own within seconds. Treating
# them as failures is what put a bare "HTTP Error 503" in front of the player
# and froze every tile behind a 60-second cooldown for one hiccup.
RETRYABLE_STATUS = frozenset({408, 425, 429, 500, 502, 503, 504})
RETRY_ATTEMPTS = 3
# Overpass may send Retry-After; a server asking for longer than this is one to
# leave for the next endpoint rather than one to wait for, with a player stood
# in front of an empty map.
MAX_RETRY_WAIT = 20.0


@dataclass(frozen=True)
class Bounds:
    south: float
    west: float
    north: float
    east: float


@dataclass(frozen=True)
class OsmWay:
    osm_id: int
    points: tuple[tuple[float, float], ...]
    tags: dict[str, str]


@dataclass(frozen=True)
class OsmNode:
    """A tagged point. Rank 10 arrives entirely through this."""

    osm_id: int
    lon: float
    lat: float
    tags: dict[str, str]


@dataclass(frozen=True)
class OsmData:
    buildings: tuple[OsmWay, ...]
    roads: tuple[OsmWay, ...]
    vegetation_areas: tuple[OsmWay, ...]
    trees: tuple[tuple[float, float], ...]
    waterways: tuple[OsmWay, ...]
    # Every closed way carrying a tag `ground.py` knows how to colour, in one
    # tuple and still tagged: the classification belongs to the module that owns
    # the palette, not to the normaliser, which should stay a reader of OSM.
    landcover: tuple[OsmWay, ...]
    # Every tagged node, untouched. `props.py` decides which of them is a thing.
    features: tuple[OsmNode, ...]
    tree_rows: tuple[OsmWay, ...] = ()
    # `natural=coastline`, complete ways: land is on their left, sea on their
    # right, which is how the sea of a coastal tile is rebuilt (harbours.py).
    coastlines: tuple[OsmWay, ...] = ()
    # Piers, breakwaters, groynes, quays, marinas and harbours, open or closed.
    maritime: tuple[OsmWay, ...] = ()


@dataclass(frozen=True)
class ElevationGrid:
    bounds: Bounds
    size: int
    values: tuple[tuple[float, ...], ...]

    def sample(self, lon: float, lat: float) -> float:
        """Bilinear elevation in metres, clamped to the grid's bounds."""
        u = (lon - self.bounds.west) / (self.bounds.east - self.bounds.west)
        v = (lat - self.bounds.south) / (self.bounds.north - self.bounds.south)
        u = max(0.0, min(1.0, u)) * (self.size - 1)
        v = max(0.0, min(1.0, v)) * (self.size - 1)
        x0, y0 = int(math.floor(u)), int(math.floor(v))
        x1, y1 = min(x0 + 1, self.size - 1), min(y0 + 1, self.size - 1)
        tx, ty = u - x0, v - y0
        low = self.values[y0][x0] * (1.0 - tx) + self.values[y0][x1] * tx
        high = self.values[y1][x0] * (1.0 - tx) + self.values[y1][x1] * tx
        return low * (1.0 - ty) + high * ty


def bounds_around(lon: float, lat: float, radius_m: float) -> Bounds:
    lat_delta = math.degrees(radius_m / R_MEAN)
    lon_delta = math.degrees(radius_m / (R_MEAN * math.cos(math.radians(lat))))
    return Bounds(lat - lat_delta, lon - lon_delta, lat + lat_delta, lon + lon_delta)


def _read_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _retry_after(error: urllib.error.HTTPError) -> float | None:
    """Seconds the server asked to be left alone for, if it said so and it is sane."""
    raw = error.headers.get("Retry-After") if error.headers else None
    if raw is None:
        return None
    try:
        seconds = float(raw)
    except ValueError:
        return None  # the HTTP-date form; the backoff below is a fine answer
    if not math.isfinite(seconds) or seconds < 0:
        return None
    return min(seconds, MAX_RETRY_WAIT)


def _request_json(
    url: str,
    data: bytes | None = None,
    timeout: float = 120.0,
    attempts: int = RETRY_ATTEMPTS,
    sleep=time.sleep,
    headers: dict | None = None,
) -> Any:
    """One JSON request, retried while the failure is a transient one.

    The distinction the retry rests on is between a server saying "not now" and
    a server saying "no": a 429 or a 503 clears by itself, a 400 never will, and
    retrying the second only delays the error the caller has to see anyway.
    """
    request = urllib.request.Request(
        url,
        data=data,
        headers={"User-Agent": USER_AGENT, "Accept": "application/json", **(headers or {})},
    )
    delay = 2.0
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as error:
            if error.code not in RETRYABLE_STATUS or attempt == attempts:
                raise
            wait = _retry_after(error)
            wait = delay if wait is None else wait
        except (TimeoutError, urllib.error.URLError, ConnectionError) as error:
            reason = error.reason if isinstance(error, urllib.error.URLError) else error
            no_network = isinstance(reason, socket.gaierror) or (
                isinstance(reason, OSError) and
                (reason.errno in {errno.ENETDOWN, errno.ENETUNREACH, errno.EHOSTUNREACH,
                                  errno.ECONNREFUSED} or
                 getattr(reason, "winerror", None) in {10051, 10061, 11001}))
            if attempt == attempts or no_network:
                raise
            wait = delay
        sleep(wait)
        delay = min(delay * 2.0, MAX_RETRY_WAIT)
    raise AssertionError("unreachable: the loop either returns or raises")


# What the Overpass query asks for, as a number.
#
# A cached response is only as good as the question that produced it, and the
# cache key -- a bounding box -- does not carry the question. Widening the query
# without this would leave every previously visited place answering the new
# question with the old answer, forever, and the symptom would be a city with no
# ground materials and no way to tell that from a city OSM has not mapped.
#
#   1  buildings, highways, a little vegetation, waterways
#   2  + the landcover classes rank 9 is made of: farmland, orchard, vineyard,
#      residential and industrial land, sand, bare rock, scree, glacier, beach,
#      wetland, heath, quarry, cemetery, pitch
#   3  + the point features rank 10 is made of: street lamps, benches, bus
#      stops, fountains, pylons, post boxes, hydrants, playgrounds. Trees were
#      already asked for at v1.
#   4  (unchanged question, a cache-format revision)
#   5  + the sea's works: piers, breakwaters, groynes, quays, marinas,
#      harbour and seamark points. The coastline itself was always asked for
#      (`way[natural]`) and never used.
OSM_QUERY_VERSION = 5


def fetch_osm(bounds: Bounds, cache_path: Path, refresh: bool = False) -> dict:
    """The OSM observations for a box, from cache when the question matches.

    The node clauses are rank 10 of §2.1, "mobilier urbain régional — forte
    signature culturelle". Each is a point with a tag and nothing else to say,
    which is exactly what an instanced prop wants. The list is as narrow as it
    is on purpose: `node[amenity]` alone would return every restaurant, bank and
    cash machine in the neighbourhood, none of which is a thing to put on the
    ground, and all of which would be paid for in query time on a shared
    service.

    A cached document written by an older query is *used* rather than refused
    when the network cannot be reached: an older observation set is a worse
    answer than a fresh one and a far better answer than no world at all. It
    keeps its own version number, so `cook` can record in the manifest that the
    tile was built from an older question (§4 I5).
    """
    stale = None
    if cache_path.exists() and not refresh:
        document = _read_json(cache_path)
        if document.get("r1QueryVersion") == OSM_QUERY_VERSION:
            return document
        stale = document

    bbox = f"{bounds.south:.8f},{bounds.west:.8f},{bounds.north:.8f},{bounds.east:.8f}"
    query = f"""
[out:json][timeout:90][bbox:{bbox}];
(
  way[building];
  way[highway];
  way[landuse];
  way[natural];
  way[leisure~\"park|garden|golf_course|pitch\"];
  way[waterway];
  way[water];
  way[amenity=grave_yard];
  way[man_made~"^(pier|breakwater|groyne|quay)$"];
  way[leisure=marina];
  way[harbour];
  node[leisure=marina];
  node[harbour];
  node["seamark:type"~"^(harbour|mooring|light_major|light_minor|landmark)$"];
  node[natural=tree];
  node[highway~"^(street_lamp|bus_stop|crossing|traffic_signals)$"];
  node[amenity~"^(bench|fountain|waste_basket|drinking_water|post_box|telephone|clock)$"];
  node[emergency=fire_hydrant];
  node[power~"^(tower|pole)$"];
  node[man_made~"^(water_tower|windmill|lighthouse|mast)$"];
  node[natural~"^(rock|stone)$"];
);
out body;
>;
out skel qt;
""".strip()
    encoded = urllib.parse.urlencode({"data": query}).encode("ascii")
    failures = []
    for endpoint in OSM_ENDPOINTS:
        try:
            document = _request_json(endpoint, encoded)
            document["r1QueryVersion"] = OSM_QUERY_VERSION
            _write_json(cache_path, document)
            return document
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
            failures.append(f"{endpoint}: {error}")
            time.sleep(1.0)
    if stale is not None:
        return stale
    raise SourceUnavailable("all Overpass endpoints failed: " + " | ".join(failures))


# Keys whose presence makes a closed way a statement about the ground. The
# values are `ground.py`'s business; what belongs here is only the question of
# whether OSM said anything about this patch of earth at all.
_LANDCOVER_KEYS = ("landuse", "natural", "leisure", "water", "waterway", "amenity")


def normalize_osm(document: dict) -> OsmData:
    nodes: dict[int, tuple[float, float]] = {}
    raw_ways = []
    explicit_trees = []
    features = []
    for element in document.get("elements", []):
        if element.get("type") == "node":
            point = float(element["lon"]), float(element["lat"])
            nodes[int(element["id"])] = point
            tags = {str(key): str(value) for key, value in element.get("tags", {}).items()}
            if tags.get("natural") == "tree":
                explicit_trees.append(point)
            if tags:
                features.append(OsmNode(int(element["id"]), point[0], point[1], tags))
        elif element.get("type") == "way":
            raw_ways.append(element)

    buildings = []
    roads = []
    vegetation = []
    tree_rows = []
    waterways = []
    landcover = []
    coastlines = []
    maritime = []
    for element in raw_ways:
        points = tuple(nodes[node_id] for node_id in element.get("nodes", []) if node_id in nodes)
        if len(points) < 2:
            continue
        tags = {str(key): str(value) for key, value in element.get("tags", {}).items()}
        way = OsmWay(int(element["id"]), points, tags)
        if tags.get("natural") == "tree_row":
            tree_rows.append(way)
        if tags.get("natural") == "coastline":
            coastlines.append(way)
        if (tags.get("man_made") in {"pier", "breakwater", "groyne", "quay"}
                or tags.get("leisure") == "marina" or "harbour" in tags):
            maritime.append(way)
        if "building" in tags and len(points) >= 4 and points[0] == points[-1]:
            buildings.append(way)
        if "highway" in tags:
            roads.append(way)
        if (
            tags.get("landuse") in {
                "forest", "meadow", "grass", "village_green", "recreation_ground", "orchard"
            }
            or tags.get("natural") in {"wood", "scrub", "grassland", "shrubbery", "wetland"}
            or tags.get("leisure") in {"park", "garden"}
        ) and len(points) >= 4 and points[0] == points[-1]:
            vegetation.append(way)
        if "waterway" in tags or tags.get("natural") == "water" or "water" in tags:
            waterways.append(way)
        closed = len(points) >= 4 and points[0] == points[-1]
        if closed and "building" not in tags and any(k in tags for k in _LANDCOVER_KEYS):
            landcover.append(way)

    key = lambda way: way.osm_id
    return OsmData(
        tuple(sorted(buildings, key=key)),
        tuple(sorted(roads, key=key)),
        tuple(sorted(vegetation, key=key)),
        tuple(sorted(set(explicit_trees))),
        tuple(sorted(waterways, key=key)),
        tuple(sorted(landcover, key=key)),
        tuple(sorted(features, key=key)),
        tuple(sorted(tree_rows, key=key)),
        tuple(sorted(coastlines, key=key)),
        tuple(sorted(maritime, key=key)),
    )


def fetch_elevation_grid(
    bounds: Bounds,
    size: int,
    cache_path: Path,
    refresh: bool = False,
) -> ElevationGrid:
    if cache_path.exists() and not refresh:
        doc = _read_json(cache_path)
        cached_bounds = Bounds(**doc["bounds"])
        return ElevationGrid(
            cached_bounds,
            int(doc["size"]),
            tuple(tuple(float(value) for value in row) for row in doc["values"]),
        )

    points = []
    for row in range(size):
        lat = bounds.south + (bounds.north - bounds.south) * row / (size - 1)
        for column in range(size):
            lon = bounds.west + (bounds.east - bounds.west) * column / (size - 1)
            points.append((lon, lat))

    values = []
    for start in range(0, len(points), 100):
        batch = points[start:start + 100]
        query = urllib.parse.urlencode({
            "latitude": ",".join(f"{lat:.8f}" for _, lat in batch),
            "longitude": ",".join(f"{lon:.8f}" for lon, _ in batch),
        }, safe=",")
        try:
            response = _request_json(f"{ELEVATION_ENDPOINT}?{query}")
        except (OSError, TimeoutError, ValueError) as error:
            raise SourceUnavailable(f"elevation service unavailable: {error}") from error
        elevations = response.get("elevation")
        if not isinstance(elevations, list) or len(elevations) != len(batch):
            raise SourceUnavailable("Open-Meteo returned an incomplete elevation batch")
        values.extend(float(value) for value in elevations)

    rows = tuple(
        tuple(values[row * size:(row + 1) * size]) for row in range(size)
    )
    _write_json(cache_path, {
        "schema": 1,
        "source": "Copernicus DEM GLO-90 via Open-Meteo",
        "bounds": {
            "south": bounds.south,
            "west": bounds.west,
            "north": bounds.north,
            "east": bounds.east,
        },
        "size": size,
        "values": rows,
    })
    return ElevationGrid(bounds, size, rows)
