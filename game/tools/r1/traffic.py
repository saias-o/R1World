"""The lane graph a tile ships so `saida::traffic` has something to drive on.

The simulation is the engine add-on (`engine/plugins/traffic`), which knows
nothing about this project. This module is the other half: it turns the OSM
ways the tile already cooked into the graph that add-on wants, decides how busy
the tile should be, and says which side of the road this part of the planet
drives on.

**Nothing here is invented, in the same sense as `props.py`.** Every lane is a
way somebody surveyed, every one-way street is tagged `oneway`, and a speed is
the `maxspeed` tag when there is one. What is inferred says so in the tile's
manifest (§4 I5), and the only inferred numbers are a speed where the tag is
missing and the car count, which is a budget rather than a claim about a place.

**Why a graph per tile and not one graph for the region.** A tile is evicted as
a unit; traffic that outlived its tile would be driving on roads that are no
longer drawn. Giving each tile its own graph and its own `Flow` makes that
impossible by construction rather than by bookkeeping, and a car that reaches
the edge of its tile's roads retires there instead of crossing into geometry
nobody has loaded.
"""

from __future__ import annotations

import math

from .streets import MOTOR, length_tag

# Ways a car belongs on. `service` is deliberately absent: it is the tag for
# driveways, parking aisles and alleys behind buildings, and a city's worth of
# them would put more cars in car parks than on streets.
DRIVEABLE = MOTOR - {"service"}

# What a road of this class is driven at where nobody tagged a limit, in m/s.
# Inferred, and counted as inferred in the manifest.
CLASS_SPEED = {
    "motorway": 33.0, "trunk": 25.0, "primary": 16.7, "secondary": 13.9,
    "tertiary": 13.9, "residential": 8.3, "living_street": 5.6, "unclassified": 11.1,
}
DEFAULT_SPEED = 11.1

# How much of a tile's traffic a road of this class carries, per metre of it.
# A city has far more back streets than avenues, so spawning evenly over lanes
# puts the whole fleet in the back streets and leaves the boulevard empty --
# which is exactly what "pas assez de trafic dans Paris" looked like, with the
# cars present and all of them one street away. The add-on takes the weight and
# does the rest (`engine/plugins/traffic`).
CLASS_WEIGHT = {
    "motorway": 6.0, "trunk": 5.0, "primary": 4.0, "secondary": 3.0,
    "tertiary": 2.0, "residential": 0.7, "living_street": 0.25, "unclassified": 1.0,
}
DEFAULT_WEIGHT = 1.0

# Junctions and shape points closer than this are the same point. OSM ways that
# meet at a crossroads share a node exactly, but two ways digitised separately
# along the same street can miss each other by a few centimetres, and a graph
# that does not join them is a graph of dead ends.
WELD = 0.75

# A lane shorter than this carries no car and costs a node.
MIN_LANE = 2.0

# How busy a tile is. Buildings are the density signal the player actually sees
# — more of them means more people, means more cars — and the road length is
# what caps it, because sixty cars on one village street is not density, it is
# a car park. Both terms are needed: a motorway through empty country has few
# buildings and real traffic, a pedestrianised old town has the opposite.
CARS_PER_BUILDING = 1 / 28.0
CARS_PER_ROAD_KM = 1.6
TILE_CAR_CAP = 22

# Where the left-hand-driving world is, as coarse boxes (west, south, east,
# north). It is coarse on purpose: the alternative is a per-country polygon set
# this project has no other use for, and the failure mode of a wrong box is a
# car on the wrong side of an empty street. OSM does not tag the side of the
# road, so there is nothing measured to prefer here.
LEFT_HAND_BOXES = (
    (-11.0, 49.8, 2.1, 61.0),     # United Kingdom and Ireland
    (60.0, 5.0, 92.5, 37.0),      # Pakistan, India, Nepal, Bangladesh, Sri Lanka
    (127.0, 24.0, 146.5, 46.0),   # Japan
    (95.0, -11.0, 141.5, 21.0),   # Thailand, Malaysia, Indonesia
    (112.0, -48.0, 179.5, -8.0),  # Australia, New Zealand, Papua New Guinea
    (11.0, -35.0, 42.0, 5.0),     # Southern and eastern Africa
    (-78.5, 17.5, -76.0, 18.6),   # Jamaica
)


def drives_on_the_left(lon: float, lat: float) -> bool:
    return any(w <= lon <= e and s <= lat <= n for w, s, e, n in LEFT_HAND_BOXES)


def speed_of(tags: dict) -> tuple[float, bool]:
    """(m/s, measured). A tagged limit beats the class estimate — §4 I5."""
    raw = tags.get("maxspeed")
    if raw:
        text = str(raw).strip().lower()
        mph = text.endswith("mph")
        number = length_tag(text.removesuffix("mph").removesuffix("km/h").strip(), 0.0)
        if number > 0:
            return number * (0.44704 if mph else 1 / 3.6), True
    return CLASS_SPEED.get(tags.get("highway"), DEFAULT_SPEED), False


def oneway(tags: dict) -> int:
    """1 forward only, -1 backward only, 0 both ways."""
    value = str(tags.get("oneway", "")).lower()
    if value in {"yes", "true", "1"}:
        return 1
    if value in {"-1", "reverse"}:
        return -1
    # A roundabout is one-way whether or not anybody tagged it as one.
    if tags.get("junction") in {"roundabout", "circular"}:
        return 1
    return 0


def build_graph(roads, ground, buildings: int, lon: float, lat: float) -> dict:
    """The tile's lane graph in engine metres, plus how busy it should be.

    `ground(lon, lat)` is the tile's own geodetic-to-engine mapping, so a lane
    sits on the road surface that was actually drawn rather than on a second
    guess about where the ground is. The height travels with the node: the
    add-on's graph is planar, and only this side knows about terrain.
    """
    nodes: list[tuple[float, float, float]] = []
    index: dict[tuple[int, int], int] = {}
    lanes: list[tuple[int, int, float, float]] = []
    measured = inferred = 0
    metres = 0.0

    def node_at(point) -> int:
        x, y, z = point
        key = (round(x / WELD), round(z / WELD))
        found = index.get(key)
        if found is None:
            found = index[key] = len(nodes)
            nodes.append((x, y, z))
        return found

    for road in roads:
        tags = road.tags
        if tags.get("highway") not in DRIVEABLE or tags.get("area") == "yes":
            continue
        # The street mesh skips bridges and tunnels, so the traffic must too:
        # a car driving where no road was drawn is worse than no car.
        if tags.get("tunnel") not in {None, "no"} or tags.get("bridge") not in {None, "no"}:
            continue
        speed, is_measured = speed_of(tags)
        weight = CLASS_WEIGHT.get(tags.get("highway"), DEFAULT_WEIGHT)
        direction = oneway(tags)
        points = [ground(*point) for point in road.points]
        used = False
        for a, b in zip(points, points[1:]):
            span = math.dist((a[0], a[2]), (b[0], b[2]))
            if span < MIN_LANE:
                continue
            first, second = node_at(a), node_at(b)
            if first == second:
                continue
            if direction >= 0:
                lanes.append((first, second, speed, weight))
            if direction <= 0:
                lanes.append((second, first, speed, weight))
            metres += span
            used = True
        if used:
            if is_measured:
                measured += 1
            else:
                inferred += 1

    cars = 0
    if lanes:
        cars = min(TILE_CAR_CAP, round(buildings * CARS_PER_BUILDING
                                       + metres / 1000.0 * CARS_PER_ROAD_KM))
    return {
        "nodes": [[round(x, 2), round(y, 2), round(z, 2)] for x, y, z in nodes],
        "lanes": [[a, b, round(speed, 1), weight] for a, b, speed, weight in lanes],
        "cars": int(cars),
        "leftHand": drives_on_the_left(lon, lat),
        "roadMetres": round(metres, 1),
        "speedsTagged": measured,
        "speedsInferred": inferred,
    }


# ── paint ───────────────────────────────────────────────────────────────────
#
# One car model, so the fleet is told apart by colour alone, and rule 2 of
# `CLAUDE.md` applies to these exactly as it applies to a kit's materials:
# these are **albedos**, not paint chips. Real car paint measures 0.05 for
# black through 0.25 for a strong red to about 0.35 for white; anything above
# roughly 0.35 saturates to a white blob at this world's light level, which is
# how the ground table and the prop palette were both learned.
#
# They are ordered by how common the colour is on a European street, and the
# runtime draws from the list with a car's own seed, so a street is mostly
# silver, white and black with a red one in it — which is what a street is.
PAINTS = (
    ("silver", (0.30, 0.30, 0.31)),
    ("white", (0.34, 0.34, 0.33)),
    ("graphite", (0.09, 0.09, 0.10)),
    ("slate", (0.16, 0.16, 0.17)),
    ("midnight", (0.05, 0.05, 0.06)),
    ("navy", (0.06, 0.09, 0.19)),
    ("burgundy", (0.19, 0.05, 0.06)),
    ("moss", (0.07, 0.12, 0.08)),
    ("sand", (0.28, 0.24, 0.16)),
    ("ochre", (0.30, 0.22, 0.06)),
)


def paint_table() -> dict:
    """What the runtime reads to tint one shared car model into a fleet."""
    return {"schema": 1,
            "note": "Linear albedo, not paint chips. See r1/traffic.py and CLAUDE.md rule 2.",
            "paints": [{"name": name, "albedo": list(colour)} for name, colour in PAINTS]}
