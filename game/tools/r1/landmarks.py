"""The twenty places that must look like themselves.

Everything else in this world is generated from a description: a footprint, a
height, a roof shape, a region. That is the thesis and it is why Paris looks
like Paris. It is also why the Eiffel Tower, until this module, was a box: OSM
maps it as `building=tower`, `height=330`, and the building chain did exactly
what it does with every tower -- it extruded a 125 m square three hundred and
thirty metres into the sky. Nothing inferred can know that one footprint is a
lattice, one a robed woman holding a torch and one an onion-domed church.

**This is the exception, and it keeps the thesis.** A landmark is still a
description, not stored geometry: a recipe of a few dozen lines in the
vocabulary of `sculpt.py` -- this plinth, this drum, this dome profile -- that
every client turns into the same triangles (§4 I3). No photogrammetry (§2 rules
it out) and no downloaded model: the recipes are this project's own work, so
there is no licence to carry and no asset of a lower grade to smuggle in
(CLAUDE.md rule 1).

What is measured stays measured (CLAUDE.md rule 4). Each entry's anchor and
bearing come from the landmark's own OSM element, found by its `wikidata` tag,
and its height is the official one; the recipe supplies only the shape
between them. The manifest records all three and says which OSM element they
came from.

**Why a scene node and not tile geometry.** A landmark exists once on Earth, so
it is baked once into `cache/world/landmarks/` and placed by the tile whose
bounds hold its anchor. Its vertices are charged to that tile's residency
(`world.cpp` sums `vertices`) but not to the per-tile generator bound, which
exists to catch a generator gone wrong, not a monument going right. Every
model stays under `MAX_VERTICES`, a thirty-second of the arena (CLAUDE.md rule 5).

**The generic building is removed, never drawn twice.** A building way that
carries the landmark's `wikidata`, or whose centre lies inside the landmark's
`clearance`, is not extruded: the Eiffel Tower's box goes, the Palace of
Westminster next to Big Ben stays.
"""

from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import landmark_recipes as recipes
from . import surfaces
from .mesh import Material, MeshPart, write_glb
from .sculpt import Finish, Sculpt, detail

Vec2 = tuple[float, float]

GAME = Path(__file__).resolve().parents[2]
# Shipped with the game: the recipes run here, at authoring time, and the
# game only places what they drew (`native/gen/landmarks.cpp`).
MODEL_DIR = GAME / "assets" / "world" / "landmarks"

# Bumped whenever a recipe or the vocabulary changes what it draws. It is part
# of every model's file name, so an old model is never served for a new recipe
# (§4 I3: changing a generator invalidates, never silently).
REVISION = 3

# A thirty-second of the 1 048 576-vertex arena. The densest resident
# neighbourhood measured is Paris at 764 705 against a budget of 900 000, and
# Paris carries three landmarks (the tower, the arch, the cathedral: about
# 34 000 together), so even there a hundred thousand vertices stay free.
MAX_VERTICES = 32_768

# The far levels are drawn while their own tile is not resident, beside the
# nine tiles that are. The worst case is Paris -- the densest neighbourhood
# measured (764 705 of 900 000) and the only one with three landmarks within
# five kilometres of each other: three level-1 models are 36 864 vertices,
# which leaves that neighbourhood a hundred thousand of air. Level 2 is a
# silhouette across a city, a third of level 1.
LOD_BUDGET = (MAX_VERTICES, 12_288, 4_096)

# Where level 1 gives way to level 2, and where level 2 stops being drawn.
# The haze is a visibility of 5 km (`prepare_world.FOG_DENSITY`): past it a
# monument is more than 97% air, and the camera's far plane stops there too.
LOD1_UNTIL = 1_800.0
FAR_RANGE = 5_000.0


@dataclass(frozen=True)
class Landmark:
    slug: str
    name: str
    wikidata: str
    # The OSM element the anchor and the bearing were measured on.
    osm: str
    lon: float
    lat: float
    # Compass bearing, clockwise from north, of the recipe's +x axis.
    bearing: float
    # Official height of the highest point above the ground at the anchor.
    height: float
    recipe: Callable[[], Sculpt]
    # Recipe-frame polygon, or several: generic buildings centred inside are
    # the landmark's own OSM trace and are not extruded. As tight as the
    # monument: a security pavilion between the Eiffel Tower's pillars is a
    # real building, and stays.
    clearance: tuple
    # Recipe-frame rings the player cannot walk through.
    solids: tuple[tuple[Vec2, ...], ...] = ()
    # Where the preview looks from (compass bearing of the camera).
    view: float = 35.0
    # The ground under the anchor, metres above the geoid, and where it was
    # read: the same terrain source and grid the tile draws, so the far models
    # stand exactly where the near one does (see `ship`).
    ground: tuple[float, str] = (0.0, "none")
    # Where the bearing came from, when it is not the OSM element's own axis:
    # a statue's footprint is its plinth, and a plinth does not say which way
    # its figure looks (§4 I5).
    bearing_source: str = "osm"


def _square(half: float) -> tuple[Vec2, ...]:
    return ((-half, -half), (half, -half), (half, half), (-half, half))


def _box(hx: float, hz: float, cx: float = 0.0, cz: float = 0.0) -> tuple[Vec2, ...]:
    return ((cx - hx, cz - hz), (cx + hx, cz - hz), (cx + hx, cz + hz), (cx - hx, cz + hz))


def _ellipse(a: float, b: float, sides: int = 32) -> tuple[Vec2, ...]:
    return tuple((a * math.cos(math.tau * i / sides), b * math.sin(math.tau * i / sides))
                 for i in range(sides))


def _circle(radius: float, sides: int = 24) -> tuple[Vec2, ...]:
    return _ellipse(radius, radius, sides)


# The terrain sources `world_elevation.fetch_ground` answers from, sampled at
# each anchor on 2026-09-23 exactly as a tile samples them.
IGN = "IGN RGE ALTI via Geoplateforme"
GLO90 = "Copernicus DEM GLO-90 via Open-Meteo"

LANDMARKS: tuple[Landmark, ...] = (
    Landmark(
        "eiffel_tower", "Tour Eiffel", "Q243", "way/5013364",
        2.294501, 48.85826, bearing=recipes.EIFFEL_BEARING, height=330.0, ground=(33.9, IGN),
        recipe=recipes.eiffel_tower,
        clearance=tuple(_box(13.5, 13.5, sx * 49.9, sz * 49.9) for sx in (-1, 1) for sz in (-1, 1)),
        solids=tuple(_box(12.5, 12.5, sx * 49.9, sz * 49.9)
                     for sx in (-1, 1) for sz in (-1, 1)),
        view=150.0),
    Landmark(
        "statue_of_liberty", "Statue of Liberty", "Q9202", "way/32965412",
        -74.044537, 40.689243, bearing=125.0, height=93.0, ground=(6.9, GLO90),
        recipe=recipes.statue_of_liberty, clearance=_circle(49.0),
        solids=(_circle(47.0),), view=150.0,
        bearing_source="inferred: faces the Narrows; Fort Wood's star gives no axis"),
    Landmark(
        "big_ben", "Elizabeth Tower (Big Ben)", "Q41225", "way/123557148",
        -0.124573, 51.500701, bearing=6.3, height=96.0, ground=(12.3, GLO90),
        recipe=recipes.big_ben, clearance=_square(8.5), solids=(_square(7.1),), view=230.0),
    Landmark(
        "colosseum", "Colosseo", "Q10285", "relation/1834818",
        12.492392, 41.890317, bearing=114.9, height=48.5, ground=(28.0, GLO90),
        recipe=recipes.colosseum, clearance=_ellipse(96.0, 80.0),
        solids=(_ellipse(95.0, 79.0),), view=20.0),
    Landmark(
        "taj_mahal", "Taj Mahal", "Q9141", "way/375257537",
        78.042090, 27.175009, bearing=89.9, height=73.0, ground=(146.9, GLO90),
        recipe=recipes.taj_mahal, clearance=_square(52.0), solids=(_square(50.0),),
        view=200.0),
    Landmark(
        "pyramid_of_khufu", "Great Pyramid of Giza", "Q37200", "way/4420397",
        31.134216, 29.979171, bearing=0.0, height=138.5, ground=(66.1, GLO90),
        recipe=recipes.khufu, clearance=_square(117.0), solids=(_square(116.0),), view=210.0),
    Landmark(
        "pyramid_of_khafre", "Pyramid of Khafre", "Q208358", "way/4420396",
        31.130737, 29.975990, bearing=0.0, height=136.4, ground=(74.3, GLO90),
        recipe=recipes.khafre, clearance=_square(109.5), solids=(_square(108.6),), view=210.0),
    Landmark(
        "pyramid_of_menkaure", "Pyramid of Menkaure", "Q238623", "way/4420398",
        31.128261, 29.972510, bearing=0.0, height=61.0, ground=(76.6, GLO90),
        recipe=recipes.menkaure, clearance=_box(53.0, 54.0), solids=(_box(52.0, 53.3),),
        view=210.0),
    Landmark(
        "christ_the_redeemer", "Cristo Redentor", "Q79961", "node/4919986733",
        -43.210459, -22.951917, bearing=115.0, height=38.0, ground=(565.0, GLO90),
        recipe=recipes.christ_the_redeemer, clearance=_square(9.0), solids=(_square(4.9),),
        view=60.0, bearing_source="inferred: faces Guanabara Bay; a node has no axis"),
    Landmark(
        "sydney_opera_house", "Sydney Opera House", "Q45178", "relation/9596872",
        151.215159, -33.857117, bearing=14.0, height=67.0, ground=(0.7, GLO90),
        recipe=recipes.sydney_opera_house, clearance=_box(100.0, 53.0, -12.0),
        solids=(_box(76.0, 52.0, -4.0),), view=320.0),
    Landmark(
        "burj_khalifa", "Burj Khalifa", "Q12495", "way/446646206",
        55.274227, 25.197046, bearing=66.4, height=828.0, ground=(15.2, GLO90),
        recipe=recipes.burj_khalifa, clearance=_circle(40.0), solids=(_circle(30.0),),
        view=200.0),
    Landmark(
        "empire_state_building", "Empire State Building", "Q9188", "way/34633854",
        -73.985659, 40.748442, bearing=118.9, height=443.2, ground=(34.1, GLO90),
        recipe=recipes.empire_state_building, clearance=_box(65.5, 30.5),
        solids=(_box(64.75, 30.0),), view=60.0),
    Landmark(
        "leaning_tower_of_pisa", "Torre di Pisa", "Q39054", "relation/12982355",
        10.396632, 43.723007, bearing=180.0, height=56.67, ground=(6.6, GLO90),
        recipe=recipes.leaning_tower, clearance=_circle(9.5), solids=(_circle(8.0),),
        view=260.0, bearing_source="published: leans 3.97° to the south"),
    Landmark(
        "arc_de_triomphe", "Arc de Triomphe", "Q64436", "way/226413508",
        2.295039, 48.873779, bearing=25.2, height=50.0, ground=(58.6, IGN),
        recipe=recipes.arc_de_triomphe, clearance=_box(24.5, 13.0),
        solids=tuple(_box(7.6, 11.1, sx * 14.9, 0.0) for sx in (-1, 1)), view=140.0),
    Landmark(
        "notre_dame_de_paris", "Cathédrale Notre-Dame de Paris", "Q2981", "way/201611261",
        2.349918, 48.853006, bearing=116.4, height=96.0, ground=(34.2, IGN),
        recipe=recipes.notre_dame, clearance=_box(66.0, 25.0), solids=(_box(64.0, 21.0),),
        view=240.0),
    Landmark(
        "sagrada_familia", "Basílica de la Sagrada Família", "Q48435", "relation/9194723",
        2.174280, 41.403505, bearing=134.5, height=172.5, ground=(39.3, GLO90),
        recipe=recipes.sagrada_familia, clearance=_box(52.0, 36.0, 13.0),
        solids=(_box(50.0, 34.5, 13.0),), view=90.0),
    Landmark(
        "brandenburg_gate", "Brandenburger Tor", "Q82425", "way/518071791",
        13.377702, 52.516275, bearing=173.4, height=26.0, ground=(40.2, GLO90),
        recipe=recipes.brandenburg_gate, clearance=_box(17.5, 6.8),
        solids=tuple(_box(0.9, 5.2, x, 0.0) for x in (-14.8, -9.3, -3.7, 3.7, 9.3, 14.8)),
        view=60.0),
    Landmark(
        "st_peters_basilica", "Basilica di San Pietro", "Q12512", "way/244159210",
        12.453710, 41.902164, bearing=89.4, height=136.57, ground=(45.7, GLO90),
        recipe=recipes.st_peters_basilica,
        clearance=((-108.0, -30.0), (-72.0, -46.0), (-72.0, -70.0), (20.0, -70.0), (20.0, -48.0),
                   (108.5, -58.0), (108.5, 58.0), (20.0, 48.0), (20.0, 70.0), (-72.0, 70.0),
                   (-72.0, 46.0), (-108.0, 30.0)),
        solids=(_box(95.0, 46.0, 10.0),), view=60.0),
    Landmark(
        "st_basils_cathedral", "Собор Василия Блаженного", "Q129846", "relation/3030568",
        37.623097, 55.752469, bearing=0.0, height=65.0, ground=(145.2, GLO90),
        recipe=recipes.st_basils_cathedral, clearance=_circle(24.0), solids=(_circle(23.0, 16),),
        view=250.0, bearing_source="inferred: chapels on the cardinal axes"),
    Landmark(
        "parthenon", "Παρθενών", "Q10288", "way/910010406",
        # 13.72 m from the stylobate to the cornice; with the three steps
        # below and the pediment's apex above, 19.2 m from the rock.
        23.726618, 37.971503, bearing=77.1, height=19.2, ground=(129.4, GLO90),
        recipe=recipes.parthenon, clearance=_box(37.0, 17.6), solids=(_box(36.9, 17.6),),
        view=300.0),
    Landmark(
        "tokyo_tower", "東京タワー", "Q183536", "relation/4247312",
        139.745446, 35.658590, bearing=34.4, height=332.6, ground=(29.5, GLO90),
        recipe=recipes.tokyo_tower, clearance=_square(45.0), solids=(_box(26.25, 38.25),),
        view=200.0),
    Landmark(
        "petronas_towers", "Menara Berkembar Petronas", "Q83063", "way/279944536",
        101.711717, 3.158006, bearing=49.5, height=451.9, ground=(43.7, GLO90),
        recipe=recipes.petronas_towers, clearance=_box(76.0, 26.0),
        solids=tuple(tuple((x + cx, z) for x, z in _circle(22.0)) for cx in (-50.0, 50.0)),
        view=150.0),
)

BY_SLUG = {landmark.slug: landmark for landmark in LANDMARKS}


# ── the model ───────────────────────────────────────────────────────────────

def material_for(finish: Finish, lod: int = 0) -> Material:
    """A finish as the engine sees it: photographed when it has a family and
    is seen close enough for the photograph to be more than its average."""
    if finish.family is not None and lod == 0:
        return surfaces.material(finish.name, finish.colour, finish.roughness, finish.family,
                                 double_sided=finish.double_sided)
    return Material(finish.name, (*finish.colour, 1.0), finish.roughness,
                    metallic=finish.metallic, double_sided=finish.double_sided)


def model_parts(landmark: Landmark, lod: int = 0) -> list[MeshPart]:
    with detail(lod):
        return landmark.recipe().parts(landmark.bearing, lambda f: material_for(f, lod))


def model_path(landmark: Landmark, lod: int = 0) -> Path:
    suffix = f"-lod{lod}" if lod else ""
    return MODEL_DIR / f"{landmark.slug}-r{REVISION}{suffix}.glb"


def bake(landmark: Landmark, lod: int = 0) -> tuple[str, int]:
    """The model's path relative to the game, and its vertex count.

    Written once and atomically: two workers cooking neighbouring tiles must
    not see half a file, and the second one finds the first one's.
    """
    path = model_path(landmark, lod)
    parts = None
    key = (landmark.slug, lod)
    if key not in _vertex_counts:
        parts = model_parts(landmark, lod)
        count = sum(len(p.mesh.positions) for p in parts)
        if count > LOD_BUDGET[lod]:
            # CLAUDE.md rule 3: a refusal names its reason.
            raise ValueError(f"landmark {landmark.slug} LOD{lod} has {count} vertices, "
                             f"over its budget of {LOD_BUDGET[lod]}")
        _vertex_counts[key] = count
    if not path.exists():
        parts = parts or model_parts(landmark, lod)
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(f".{os.getpid()}.tmp")
        write_glb(temporary, parts)
        os.replace(temporary, path)
    return path.relative_to(GAME).as_posix(), _vertex_counts[key]


_vertex_counts: dict[tuple[str, int], int] = {}


# ── the list the game reads ─────────────────────────────────────────────────
# The game places the models (native/gen/landmarks.cpp): which buildings are a
# landmark's own trace, where its solids stand, when a far level shows.

def _rings(shape) -> tuple:
    """A clearance is one ring or several; always hand back several."""
    return (shape,) if isinstance(shape[0][0], (int, float)) else shape


# ── shipping ────────────────────────────────────────────────────────────────

def ship() -> Path:
    """Bake every level of every landmark into the game's assets, with the list
    the game places them from (`assets/world/landmarks/landmarks.json`).

    The game reads nothing else about a landmark: its anchor, bearing, height,
    clearance and solids, the ground it stands on, and each level's model and
    the distance it is drawn to. Run it after changing a recipe; the revision
    in the file names keeps an old model from answering for a new one.
    """
    from .world_tiles import tile_at
    entries = []
    for landmark in LANDMARKS:
        levels = []
        for lod, until in ((0, 0.0), (1, LOD1_UNTIL), (2, FAR_RANGE)):
            model, vertices = bake(landmark, lod)
            levels.append({"path": model, "until": until, "vertices": vertices})
        entries.append({
            "slug": landmark.slug, "name": landmark.name, "wikidata": landmark.wikidata,
            "osm": landmark.osm, "lon": landmark.lon, "lat": landmark.lat,
            "bearing": landmark.bearing, "bearingSource": landmark.bearing_source,
            "height": landmark.height,
            "alt": landmark.ground[0], "altSource": landmark.ground[1],
            "tile": tile_at(landmark.lon, landmark.lat).key,
            "clearance": [list(map(list, ring)) for ring in _rings(landmark.clearance)],
            "solids": [list(map(list, ring)) for ring in landmark.solids],
            "levels": levels,
        })
    document = {"revision": REVISION, "range": FAR_RANGE, "landmarks": entries}
    path = MODEL_DIR / "landmarks.json"
    path.write_text(json.dumps(document, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return path


if __name__ == "__main__":
    print(ship())
