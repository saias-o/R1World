"""Ground materials — rank 9 of the fidelity hierarchy, for no vertices at all.

Until this module existed the terrain of the entire planet was one hardcoded
colour, `(.30, .38, .24)`, a mid green. The Sahara was that green. The Amazon
was that green. So was the tarmac of a Paris courtyard and the ice of a Chamonix
glacier. §2.1 puts *sols : revêtements* at rank 9 and marks it "OSM partiel →
règles", which is exactly the shape of the answer here: OSM has mapped a great
deal of the world's landuse, and where it has not, the region says what the
ground around here usually is.

**The terrain mesh already has the vertices.** A landcover polygon is not
triangulated and laid over the ground — draping a polygon on a 90 m elevation
grid from its own outline alone would cut it through every slope it crosses, and
it would cost geometry the arena does not have (see `native/world.cpp`). Instead
the terrain grid is *partitioned*: each of its 3 200 triangles is classified by
where its centre falls, and the mesh is split into one part per class. The
terrain costs exactly what it cost before, to the vertex, because splitting a
mesh whose triangles already carry their own face normals duplicates nothing.

What that buys is bounded by the grid, and the bound is worth stating plainly:
the terrain is 41×41 over a tile of about 555 m, so **a class smaller than
roughly 14 m does not exist here**. A canal in Amsterdam does, at 25 m. A brook
does not, and it will read as whatever surrounds it rather than as a thin blue
line in the wrong place — which is the failure this resolution should have, and
not the other one.

**Order is the classification.** A lake inside a forest is a lake; a park inside
a residential district is a park. Real ground has this structure and OSM records
it by overlapping polygons rather than by cutting them, so the table below is
read in order and the first class that contains the point wins. Nothing here
resolves ties by area, because area is not what makes water beat wood.

**What is measured and what is guessed.** A triangle inside an OSM polygon is
rank 9 *measured* — someone drew that field. A triangle inside no polygon takes
the region's default, which is a guess about a continent, and the tile manifest
counts both (§4 I5). In Chamonix that split is about a third measured; in a
mapped city centre it is most of the tile; in the Sahara it is none of it, and
the honest consequence is that the Sahara is sand because the Atlas says the
Sahara is sand, not because anyone surveyed that dune.
"""

from __future__ import annotations

from .atlas import RegionProfile, Swatch


def _sw(name: str, color: tuple[float, float, float], roughness: float) -> Swatch:
    return Swatch(name, color, roughness)


# **These are albedos, not colours.** The distinction is the difference between
# a ground that reads and a ground that does not, and it was learned by getting
# it wrong: picked by eye as colours, the first version of this table gave
# asphalt 0.38 and dry sand 0.78. Under the world's own sunlight -- a peak
# intensity of 4.6, a Sun 50° up over Paris -- a horizontal surface at 0.38
# renders at 230/255 and one at 0.78 renders at 242, so the whole table
# collapsed into the same white and the Sahara looked like a car park in the
# snow. The published figures are not close to those numbers:
#
#   water 0.03-0.06 · asphalt and concrete 0.10-0.15 · forest 0.08-0.15
#   grass 0.18-0.25 · bare soil and cropland 0.15-0.25 · rock 0.15-0.25
#   dry sand 0.30-0.40 · fresh snow and ice 0.80-0.90
#
# So the table below carries those, and each entry keeps only its *hue* from
# what a painter would have chosen. It is the same discipline §11.4 asks of the
# asset normaliser and §2.2 of the light: the numbers are measurements of the
# world, and the render is what happens to them.


# One class is a name, its material, and the OSM tags that select it. The tags
# are `(key, values)`; an empty value set means "any value for this key".
GroundClass = tuple[str, Swatch, tuple[tuple[str, frozenset[str]], ...]]


def _tags(*pairs: tuple[str, str]) -> tuple[tuple[str, frozenset[str]], ...]:
    grouped: dict[str, set[str]] = {}
    for key, value in pairs:
        grouped.setdefault(key, set()).add(value)
    return tuple((key, frozenset(values)) for key, values in grouped.items())


# Read in order; the first match wins. See the module docstring.
GROUND_CLASSES: tuple[GroundClass, ...] = (
    # Water first and unconditionally. It is the only class that changes what a
    # place *is* rather than what it is made of, and a lake rendered as the wood
    # around it is the single most visible error this module can make.
    ("water", _sw("Water", (0.021, 0.034, 0.047), 0.08), _tags(
        ("natural", "water"), ("natural", "bay"), ("natural", "strait"),
        ("waterway", "riverbank"), ("waterway", "dock"), ("waterway", "canal"),
        ("landuse", "reservoir"), ("landuse", "basin"), ("landuse", "salt_pond"),
        ("water", "*"),
    )),
    ("glacier", _sw("Glacier ice", (0.81, 0.85, 0.90), 0.45), _tags(
        ("natural", "glacier"),
    )),
    ("rock", _sw("Bare rock", (0.225, 0.213, 0.195), 0.95), _tags(
        ("natural", "bare_rock"), ("natural", "scree"), ("natural", "rock"),
        ("natural", "cliff"), ("natural", "shingle"),
    )),
    ("sand", _sw("Sand", (0.425, 0.372, 0.271), 0.94), _tags(
        ("natural", "sand"), ("natural", "beach"), ("natural", "dune"),
    )),
    ("wetland", _sw("Wetland", (0.126, 0.145, 0.104), 0.93), _tags(
        ("natural", "wetland"), ("natural", "mud"), ("natural", "marsh"),
    )),
    ("forest", _sw("Forest floor", (0.072, 0.108, 0.058), 0.96), _tags(
        ("landuse", "forest"), ("natural", "wood"),
    )),
    ("scrub", _sw("Scrub", (0.132, 0.148, 0.091), 0.95), _tags(
        ("natural", "scrub"), ("natural", "heath"), ("natural", "shrubbery"),
        ("natural", "tundra"),
    )),
    ("orchard", _sw("Orchard", (0.144, 0.183, 0.093), 0.94), _tags(
        ("landuse", "orchard"), ("landuse", "vineyard"), ("landuse", "allotments"),
        ("landuse", "plant_nursery"),
    )),
    ("grass", _sw("Grass", (0.152, 0.213, 0.107), 0.94), _tags(
        ("landuse", "grass"), ("landuse", "meadow"), ("landuse", "village_green"),
        ("landuse", "recreation_ground"), ("landuse", "cemetery"),
        ("natural", "grassland"), ("natural", "fell"),
        ("leisure", "park"), ("leisure", "garden"), ("leisure", "pitch"),
        ("leisure", "golf_course"), ("leisure", "dog_park"),
        ("amenity", "grave_yard"), ("landuse", "flowerbed"),
    )),
    # Farmland is deliberately not green. A ploughed or stubbled field is the
    # colour of its soil for most of the year, and a continent of emerald
    # cropland is the tell that gives a generated world away from the air.
    ("farmland", _sw("Farmland", (0.237, 0.213, 0.127), 0.94), _tags(
        ("landuse", "farmland"), ("landuse", "farmyard"), ("landuse", "field"),
        ("landuse", "greenhouse_horticulture"), ("landuse", "aquaculture"),
    )),
    ("bare", _sw("Bare ground", (0.271, 0.250, 0.219), 0.95), _tags(
        ("landuse", "quarry"), ("landuse", "landfill"), ("landuse", "brownfield"),
        ("landuse", "construction"), ("landuse", "greenfield"),
        ("natural", "bare_ground"),
    )),
    ("urban", _sw("Made ground", (0.139, 0.135, 0.128), 0.93), _tags(
        ("landuse", "residential"), ("landuse", "industrial"),
        ("landuse", "commercial"), ("landuse", "retail"), ("landuse", "railway"),
        ("landuse", "garages"), ("landuse", "port"), ("landuse", "military"),
        ("landuse", "institutional"), ("landuse", "education"),
        ("landuse", "religious"), ("landuse", "parking"),
        ("amenity", "parking"), ("amenity", "bus_station"),
    )),
)

_CLASS_INDEX = {name: index for index, (name, _s, _t) in enumerate(GROUND_CLASSES)}
SWATCHES = {name: swatch for name, swatch, _t in GROUND_CLASSES}

# The name a triangle gets when no polygon contains it. It is not a class in the
# table: its material comes from the region, so two tiles with no OSM landcover
# at all are still different if they are on different continents.
INFERRED = "inferred"


def classify_way(tags: dict[str, str]) -> str | None:
    """The ground class an OSM way declares, or None if it declares none."""
    for name, _swatch, selectors in GROUND_CLASSES:
        for key, values in selectors:
            value = tags.get(key)
            if value is not None and ("*" in values or value in values):
                return name
    return None


class Landcover:
    """The classified polygons of one tile, ready to be asked about a point.

    Built once per tile and queried a few thousand times, so each polygon keeps
    its bounding box: the overwhelming majority of the questions are answered by
    four comparisons, and only the polygons that could contain the point are
    walked. Without it a tile with a thousand landcover ways costs three million
    point-in-polygon crossings, and cooking a city stops being interactive.
    """

    __slots__ = ("_areas",)

    def __init__(self, ways) -> None:
        areas = []
        for way in ways:
            name = classify_way(way.tags)
            if name is None:
                continue
            ring = way.points[:-1] if way.points[0] == way.points[-1] else way.points
            if len(ring) < 3:
                continue
            lons = [point[0] for point in ring]
            lats = [point[1] for point in ring]
            areas.append((_CLASS_INDEX[name], name, ring,
                          min(lons), min(lats), max(lons), max(lats)))
        # Sorted by class priority, so the first containing polygon found is
        # also the winning one and the walk can stop there.
        areas.sort(key=lambda area: area[0])
        self._areas = tuple(areas)

    def __len__(self) -> int:
        return len(self._areas)

    def at(self, lon: float, lat: float) -> str | None:
        for _rank, name, ring, west, south, east, north in self._areas:
            if not (west <= lon <= east and south <= lat <= north):
                continue
            if _contains(ring, lon, lat):
                return name
        return None


def _contains(ring, lon: float, lat: float) -> bool:
    """Crossing number against a ring given in (lon, lat).

    `polygons.point_in_polygon` does the same job on the engine's horizontal
    plane; this one works on the ellipsoid's coordinates directly, because the
    terrain grid knows the longitude and latitude of every cell it builds and
    projecting them first would only add a step that can be wrong.
    """
    inside = False
    count = len(ring)
    previous = ring[-1]
    for index in range(count):
        current = ring[index]
        if (current[1] > lat) != (previous[1] > lat):
            span = previous[1] - current[1]
            if span != 0.0 and lon < current[0] + (previous[0] - current[0]) * (lat - current[1]) / span:
                inside = not inside
        previous = current
    return inside


# What the cold does to a class (`surfaces.cold_suffix`): above the snowline it
# is snow whatever it was, and just below it grass and fields are frosted.
# Albedos: fresh snow 0.80-0.90; frost on grass sits between the two.
SNOW = _sw("Snow", (0.83, 0.85, 0.89), 0.55)
FROST = _sw("Frosted ground", (0.44, 0.47, 0.50), 0.85)


def material_for(name: str, profile: RegionProfile) -> Swatch:
    """The swatch a class renders with, the region answering for the unmapped.

    `name` may carry a cold suffix, "@snow" or "@frost".
    """
    base, _, cold = name.partition("@")
    if cold == "snow" and base != "water":
        return SNOW
    if cold == "frost" and base not in ("water", "urban", "rock", "sand"):
        return FROST
    if base == INFERRED:
        return profile.ground
    return SWATCHES[base]
