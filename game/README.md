# R1World game project

This directory is the playable SaidaEngine project. Its one scene,
`scenes/earth.scene` (the project's `mainScene`), answers one question: can you
pick any point on Earth and walk there?

`docs/PLAN.md` holds the thesis, the invariants and the next updates;
`CLAUDE.md` holds the working rules. This file describes what exists today,
how it works and what was measured.

## Playing

```powershell
powershell -File game\Play.ps1
```

A map of the Earth opens. Search for a city or commune (Photon, while online),
pick a suggestion, click the map or type coordinates, then press **Go**. The
whole-Earth image is a 4096 × 2048 Natural Earth overview; zooming switches to
OpenStreetMap tiles at their native resolution, up to level 19 (**Vue rue**).
Map tiles are cached in `cache/map-tiles/osm/`.

| Key | On foot | In a car | In an aircraft |
|---|---|---|---|
| ZQSD / WASD | walk | drive | see [Flying](#flying) |
| Maj | run | | push down / descend |
| Space | jump | handbrake | pull up / climb |
| F | take the nearest car, boat or aircraft | get out (below 7 km/h) | get out, at any moment |
| Mouse | orbit the camera | turn the head | |
| M / Échap | the map; **Reprendre** returns | | |

A north-up minimap (about 900 m) sits at the bottom left, with a few named
streets from the cached OSM observations. Above it: the destination's local
time, city and country. City names come from OSM address tags; the country
from those tags or the weather time zone and the bundled IANA table. Missing
place data is labelled as such.

## Building and testing

### What `Play.ps1` needs

The executable, and nothing else: the world is generated inside it
(`native/gen`), so playing starts no Python and no worker. `Play.ps1` builds it
with the engine's own flags read from `engine/build-rel/build.ninja`, through
GCC response files so that no path with a space goes through PowerShell's
quoting. It needs MSYS2's UCRT64 `g++` and `cmake` and puts
`C:\msys64\ucrt64\bin` first on `PATH` (otherwise the link fails with a bare
`ld returned 116`).

- **The engine is built with the game.** `engine/build-rel` is RelWithDebInfo
  (`-O2 -g`); `Play.ps1` configures it when missing and runs Ninja on it before
  every game build. The game never links the Debug tree `engine/build`: a Paris
  frame cost 35 ms of CPU there against 4.5 ms optimized.
- **It rebuilds by itself** whenever a source, a generator header or the engine
  library is newer than the executable, recompiling only what is stale.
  `world.cpp`, the one unit that includes the engine's headers, is recompiled
  whenever the engine library changes. `-Rebuild` recompiles everything,
  `-BuildOnly` stops after building. Output: `generated/world-windows`.
- **The log** (stderr included) goes to `cache/sessions/<id>/game.log` in UTF-8.
  When the game exits with an error, its `[error]` and `FAIL` lines are repeated
  in the console.
- Extra arguments are forwarded to the game. `--profile <trace.json>` profiles
  the whole run (`Engine::profileTo`): the trace opens in `chrome://tracing` or
  Perfetto, and the log ends with the most expensive scopes per frame.

### Tests

From `game/`:

```powershell
sh native/build_tools.sh; generated\tools\r1test.exe        # the generator (C++)
python -m unittest discover -s tools\r1\tests -t tools      # the authoring tools (Python)
```

Neither needs the engine or a network. `r1test <Filter>` runs one group
(`Interior`, `Retail`, `Fuel`, `SeaIce`, `Airports`…). The C++ tests hold the
world: props and their budgets, ground classes and albedos, the Atlas seams,
buildings, streets, bridges, harbours, traffic, airports, interiors and the
cache. `Service.a_visited_place_never_touches_the_network` points every source
at a closed port and asks for a cached tile anyway. The Python tests hold the
assets the project authors (skies, Sun, landmarks, fleets, people, signage) and
that every prop and tree points at an asset on disk. `build_tools.sh` also
builds `r1cook`, which cooks tiles headless from the cache (`--fetch` lets it
download).

### Smoke tests

Every in-game check runs with the window hidden:

```powershell
$env:SAIDA_WINDOW_HIDDEN = "1"
python tools\play_world.py --smoke --spawn 2.3522 48.8566
```

`play_world.py` rebuilds the executable (`Play.ps1 -BuildOnly`) then runs it,
so a test never runs an old binary. The driver clicks the map, presses Go,
waits for the tiles, walks, jumps, walks back to the car, drives, is refused
the door at speed, brakes, steps out, takes over a passing traffic car, and
reopens the map with **Reprendre**. Exit code 0 is a pass; the failing line is
in the log whose path is printed at the end.

| Option | Adds |
|---|---|
| `--spawn2 <lon> <lat>` | a second leg: walk, reopen the map, type the destination, Go again — tiles evicted, arena trimmed, origin moved |
| `--sail` | board a boat and sail (e.g. Vieux-Port `5.3698 43.2951`) |
| `--fly` | take off in a jet, stop against a building, land; then a helicopter, hover, jump out (Roissy `2.5700 49.0060`, Villacoublay `2.1900 48.7725`) |
| `--screenshot <png>` | capture the frame after the spawn; with `--camera-pos`, `--camera-look`, `--after-frames` |
| `R1WORLD_RETAIL_SMOKE=1` | enter a shop, walk an aisle, leave, close, evict, regenerate |
| `R1WORLD_INTERIOR_SMOKE=home\|school\|office\|police\|garage` | the same for one ordinary-building recipe |
| `R1WORLD_RETAIL_SHOT=<png>`, `R1WORLD_RETAIL_SHOT_AT=outside\|inside\|anchor\|room` | photograph a store or a room and end the run |
| `R1WORLD_FLY_SHOT=<png>`, `R1WORLD_FLY_SHOT_AT=stand\|climb\|stop\|heli-stand\|heli\|jump` | photograph that moment of `--fly` |

Capture mode waits for all nine tiles and their props, honours the engine's
camera flags — in metres from where the player stands when the picture is taken
(x east, y up, z south) — and holds the Sun at the instant the run started, so
load time does not change the lighting:

```powershell
./Play.ps1 --smoke --spawn 2.3522 48.8566 --screenshot generated/paris-final.png --camera-pos '70.448,1.745,83.405' --camera-look '45,2,50' --after-frames 12
```

`--at <unix seconds>` makes a capture an inspection picture: lit at that
instant, under a clear sky (fetched weather is ignored), without the HUD or the
minimap. A capture waits for OSM on tiles still provisional, up to two minutes,
then says so in the log. The picture's size is the window's: the hidden window
is 640 × 360 unless `SAIDA_WINDOW_SIZE=1600x900` (an engine variable) says
otherwise.

### The reference gallery

A visual regression is the one no test sees (`CLAUDE.md` §1). `tools/gallery.py`
takes the same six pictures every time — same place, same camera, 10:30 local
solar time on 21 September, clear sky, 1600 × 900:

| View | Where |
|---|---|
| `paris` | Paris, the rue de Rivoli by Châtelet at eye height |
| `liberty` | the Statue of Liberty from Battery Park, 2.7 km away, at eye height |
| `sousse` | the médina of Sousse from 320 m up (Tunis's has few buildings in OSM) |
| `kyoto` | Higashiyama, Kyoto, towards the hills |
| `vannes` | the port of Vannes from the place Gambetta |
| `theix` | Theix: the fuel station, the car park and the Carrefour Market |

```powershell
python tools\gallery.py                 # every view
python tools\gallery.py kyoto theix     # some of them
python tools\gallery.py --offline       # proxies closed: the cache only
```

Each run is a folder of `generated/gallery/` (date and commit);
`generated/gallery/index.html` shows each view, the latest run beside the one
before. Run it after every update and look at it before calling the update
done.

Three assertions of `--spawn2` exist because of bugs that shipped:

- **The press, not the click.** Hovering a `ui-hit` element makes the engine
  consume the mouse, so RmlUi never receives the `Up` that makes a `click`. The
  menu acts on `mousedown` too (as `engine/VerticalSlice/ui/main_menu.js`
  does), deduplicated; the driver fires a press without a release, and
  separately requires a full press-and-release to act exactly once.
- **The Sun took the observer.** The spawn fails if `setObserver` was refused;
  otherwise the scene quietly lights the wrong hemisphere.
- **The second Go** must reach a complete spawn, not a stalled counter.

### Offline

```powershell
$env:HTTP_PROXY = "http://127.0.0.1:9"; $env:HTTPS_PROXY = "http://127.0.0.1:9"
python tools\play_world.py --smoke --spawn 2.3522 48.8566                       # visited: saved terrain
python tools\play_world.py --smoke --spawn -58.3816 -34.6037                    # new: simplified terrain
python tools\play_world.py --smoke --spawn 4.8900 52.3730 --spawn2 10.1815 36.8065  # Amsterdam, then Tunis
```

A smoke test that passes with closed proxies on a visited place is what proves
the cache's promise (`CLAUDE.md` §7).

## Streaming, cache and network

### The tile grid and the floating origin

Tiles are metric latitude rings with no polar cutoff (`native/gen/common.hpp`):
36 000 rows of 0.005° and `72 000 · cos(lat)` columns per row, so a tile is
about 556 m on a side everywhere, keyed `v<version>_<row>_<col>`. The
generator version is `kVersion` (25 today). Within 18 km of a pole the
neighbourhood is the tiles nearest in metres, twelve at the pole.

The player's position is WGS84; each tile owns its tangent frame, and the
renderer only sees metres relative to an origin rebased every 350 m
(`native/world.cpp`, PLAN §3 I1). Crossing a tile edge, the date line or a pole
is not a special case. Headings are angles in the origin's frame, turned into
local east/north at each step, and `advance` rotates a unit vector rather than
calling `asin`, so a player can walk over the pole and step off it.

### Progressive entry

Go does not wait for all nine tiles. The tile under the player comes first,
neighbours follow by distance (across the date line too), and choosing a
destination on the map already prefetches it.

The world service (`native/gen/service.cpp`) runs inside the game on its own
threads; downloads and cooking never block the frame. OSM and relief improve a
tile independently:

- **With no OSM answer on disk**, the first cook does not wait for Overpass.
  The tile is cooked from `quickGround` (Copernicus through Open-Meteo, one
  request, four seconds at most) with Natural Earth's coast and nothing built
  on it. The manifest says `provisional`, the HUD says streets and buildings
  are on their way, and the tile is cooked again when the answer lands — with
  IGN's finer ground in France. A building that lands on the player moves them
  to the nearest free ground. First visits to Nice, Porto and Oslo: playable
  0.36 s after Go.
- **A failure from one source does not pause the others**; the HUD names the
  missing observation. With no observation and no network, Go opens a
  simplified tile (Natural Earth land/sea, flat, no streets), labelled
  **Hors ligne : terrain simplifié** and `offlineApproximation: true`.
- **Mounting**: terrain, buildings and collision first; trees and street
  objects follow on the main thread within a soft 2 ms per-frame budget. Logs
  carry `[World streaming] go_to_play_ms` and `mount_ms`; each `mounted` line
  gives the arena's use and largest free ranges.
- **In a vehicle** the queue covers the nine tiles plus a corridor projected
  45 s ahead (25 tiles at most); each projected neighbourhood gets its own
  bounded OSM query. Resident geometry stays limited to the nearby ring.

### A place you have been is yours

Everything downloaded stays under `cache/world/`: each tile's observations
(`osm.json`, `ground-elevation.json`) in `v<N>_<row>_<col>/`, and the Overpass
answers a neighbourhood shares in `sources/`. Geometry is never stored: the
game cooks it on every visit, so a generator change reaches every place already
visited. A visited place costs about 10 MB (about 1 MB a tile) and never
touches the network again. Folders from older generator versions are kept:
their raw observations are read by later versions.

The Overpass question has grown (version 10 today), and a cached answer
records which question produced it. Answers from `kOsmBaseVersion` on are
cooked at once; what later questions added arrives as separate, small layers
fetched in the background and never revalidated once on disk:

| Layer | Brings | Tile flag while missing |
|---|---|---|
| `<answer>.aero.json` | aeroways, military areas, a 5 km runway box | `airportsPending` |
| `<answer>.retail.json` (v3) | shops, entrances, parking, fuel stations, civic amenities, offices, crafts | `retail.observationsQueried`, `interiorStreaming` |

A layer is found beside any neighbourhood query covering the tile. A tile is
cooked again when its layer lands only if the layer holds something for it.

### When the data services say no

`net::requestJson` (`native/gen/net.cpp`) separates *not now* from *no*: 408,
425, 429 and 5xx are retried with a doubling backoff honouring `Retry-After` up
to 20 s; anything else is raised at once so the fetch moves to the next mirror.
A failed mirror is asked last for five minutes, and a server that has not
accepted the connection in ten seconds counts as failed. Requests accept any
JSON (Overpass answers `application/osm3s+json`) and gzip only (Open-Meteo's
`deflate` aborts WinHTTP). Longitudes past ±180 are never sent (Overpass
answers 400). Overpass allows two concurrent queries per client and the
service stays inside that. A failed query logs `OSM-QUERY-FAILED` and its tiles
are asked again later.

| Endpoint | Measured |
|---|---|
| `overpass-api.de` | 0.4 s, correct |
| `maps.mail.ru/osm/tools/overpass` | 8.4 s, correct |
| ~~`overpass.private.coffee`~~, ~~`overpass.kumi.systems`~~ | not listed — time out at 25 s |
| ~~`overpass.osm.ch`~~ | **never list it** — 200 with zero elements outside Switzerland |

A mirror that fails is harmless; one that answers `200` with no elements is not,
because the cache cannot tell it from genuinely empty countryside.

### Geometry budget

Saida's `GeometryRegistry` arena is 1 048 576 vertices and 3 145 728 indices by
default (configurable at engine startup). Tiles get 85% of
`ResourceManager::geometryCapacity()` (`kTileGeometryShare`); the rest is for
shared models. A single tile is bounded by `kTileVertexBudget` (120 000,
`native/gen/cook.hpp`). A tile past either limit is **refused, not truncated**:
one log line names the tile and the arithmetic, the smoke test fails on that
reason, and in play the spawn is cancelled with a sentence.

During a teleport the tile under the player is retained and is not counted
against the destination's budget. When an upload fits in total but not in one
piece, the engine packs resident geometry (`GeometryRegistry::compact`, SPEC
§4) and logs it. Tile trimming uses `Scene::resourceUsage()`. Static prototypes
belong to a disabled branch of the world and are flattened where their
transforms are uniform-scale TRS.

| Neighbourhood (9 tiles) | Buildings | Worst tile | Resident vertices |
|---|---|---|---|
| Amsterdam, canal belt | 8 368 | 106 522 | 840 351 |
| Paris, rue de Rivoli | 3 653 | 102 839 | 764 705 |
| Tunis, médina | 567 | 22 439 | 165 905 |
| Vannes, dense tile with 708 interior plans | | 118 486 | |

These are geometry capacities, not frame-rate measurements.

## Ground and terrain

### Elevation

The terrain mesh is 41 × 41 over a tile (`kTerrainMeshSize`), and every
source is read at that grid (`native/gen/sources.cpp`):

1. **Mainland France and Corsica**: IGN RGE ALTI, a bare-earth grid.
2. **Elsewhere, or when IGN fails**: the Terrain Tiles (Mapzen's archive on
   AWS: SRTM, 3DEP, EU-DEM and national surveys), read at zoom 13 (19 m a
   pixel at the equator), bilinear between pixel centres. Past zoom 13 the
   images are the same data resampled: Lion's Head reads 605 m at 12 and 630 m
   at 13, 14 and 15. The images are kept in `cache/world/terrain/<zoom>/`; a
   place read before generator 26 (seven samples a side, nearest pixel) is read
   again from its zoom-12 images on disk, without the network.
3. **Last**: Copernicus GLO-90 through Open-Meteo, 7 × 7.

`ground-elevation.json` keeps successes and fallbacks, and the manifest's
`elevationSource` says which answered. At the rue de Lobau the Copernicus grid
read about 49.3 m and IGN 34.63 m.

### Surveyed summits

Every model of the relief rounds a summit off, and a steep one most: the
Sugarloaf (396 m) reads 298 m in the Terrain Tiles and 329 m in Copernicus at
30 m, the Corcovado (710 m) 584 m and 632 m. OSM's `natural=peak|volcano` nodes
carry the surveyed height where the summit is, and what is measured wins
(`native/gen/peaks.*`):

- The summits of a square degree are one Overpass answer, kept in
  `cache/world/peaks/<lat>_<lon>.json`. A tile is cooked on the relief as it is
  and again when its list lands.
- Under a summit the ground rises to a dome (30 m crown radius) that fades out
  150 m away, only where the relief stands below it. The raise reads nothing
  but the vertex, the summit and their distance, so neighbouring tiles agree
  on their shared edge.
- An `ele` that is not plain metres ("6234 ft", "1200-1300") is not read, and a
  summit more than 250 m above the relief under it is doubted, not drawn.
- The manifest's `peaks` says which summits raised the ground and by how
  much, which agreed with it, and which were doubted.

Buildings stand on a solid foundation below their sampled perimeter (samples
at most 4 m apart), so floors and roofs stay horizontal; tagged elevated
structures are excluded.

### The ground is partitioned, not draped

Each of the terrain's triangles is classified by where its centre falls
(`native/gen/terrain.cpp`) and the mesh is split into one part per class, so
the ground classes cost **zero vertices**. A class narrower than about 14 m does
not exist at this resolution: an Amsterdam canal (25 m) does, a brook does not.
Where OSM mapped nothing, the region says what the ground usually is, and the
manifest's `ground.measuredFraction` says how much was surveyed:

| Neighbourhood | Ground measured | The tile, by area |
|---|---|---|
| Amsterdam, canal belt | 76% | 60% made ground, 15% water, 24% inferred |
| Tunis, médina | 18% | 10% made ground, 6% grass, 82% inferred |
| Paris, rue de Rivoli | 16% | 10% made ground, 5% grass, 84% inferred |

Colours are measured albedos (`CLAUDE.md` §2): water 0.03–0.06, asphalt and
concrete 0.10–0.15, forest 0.08–0.15, grass 0.18–0.25, cropland and rock
0.15–0.25, dry sand 0.30–0.40, fresh snow 0.80–0.90. Under the world's sunlight
(peak intensity 4.6) anything above about 0.35 saturates. The `Albedo` tests
hold every class in its range. The building palettes have not yet been
re-derived the same way.

Each tile also ships the grid it was classified on (40 rows of 40 characters:
land, inland water, sea-level water). It is what decides water for walking:
walking or spawning into water starts swimming, with the head above the
surface.

### Photographed surfaces

`r1/surfaces.py` gives the ground, streets, walls and roofs seamless CC0
materials (Poly Haven, ambientCG; 44 families from 43 scans):

- **A texture brings structure, never albedo.** Each colour map is normalised
  per channel and the colour factor is the palette's albedo divided by that
  level, so the rendered mean is the measured albedo.
- **Real size.** UVs are metres divided by each scan's published size (three
  ambientCG sets publish none and are marked estimated).
- **Which ground, where.** Each region has a climate, colder with latitude: a
  park is lawn in Paris, withered grass in Rome, lush in Bangkok; bare ground
  is cracked earth in the Maghreb, laterite in the savanna; snow above the
  snowline (5 500 m in the tropics, about 2 750 m in the Alps, sea level near
  72°).

Streets are asphalt, sidewalks concrete slabs, `sett`/`cobblestone`/
`paving_stones` cobbles. Roof UVs follow the slope; walls are baked sheets of
one bay by one storey with a window. UVs cost no vertex. Textures cost about
90 MB for a city neighbourhood; the GPU budget is 512 MB (`setGpuBudget` in
`native/world.cpp`). Ground and street colour maps are 1024², the rest 512².
Rebuild with `python -m r1.surfaces` from `game\tools`.

## Buildings

### The Atlas decides what the survey did not say

Untagged heights, façade materials and roof forms come from the Atlas
(`assets/world/atlas.json`, read by `native/gen/palette.cpp`): **twelve named
regions and twenty-two continental bands**. Rectangles overlap and the table is
written most-specific first: the first match wins, so a region goes above the
band it refines.

| Tier | What it claims |
|---|---|
| `region` | a place someone described: Paris's cornice line, Tunis's whitewash and parapet, Amsterdam's narrow steep-gabled plots |
| `band` | the smallest honest thing about a continent: a Nordic town is painted timber under a steep roof, a Saharan one flat-roofed earth render |
| `none` | open ocean, ice sheets, the seams between rectangles |

A tagged `height`, `building:levels` or `roof:shape` always wins (`plan_gabarit`).
Each manifest says which tier answered and how much was inferred:

```json
"region": "Amsterdam — ceinture des canaux", "regionTier": "region",
"inference": {"count": 1381, "heightMeasured": 1249, "heightInferred": 132,
              "roofTagged": 3, "roofInferred": 1378,
              "roofShapes": {"flat": 429, "gabled": 777, "hipped": 156, ...}}
```

Roofs are drawn without fascia or soffit (six vertices where a slab costs
twenty-four); the ridge, pitch, overhang and colour carry the silhouette.
Facades are a preview LOD: no modelled openings except the doors of
interiors.

### Churches and mosques

A tower is generated from the OSM footprint at one end of the nave (as wide as
the nave allows, capped at 7 m), in the region's materials: a **spire** for a
church, a **minaret** where `religion=muslim` or `building=mosque`, a plain
**tower** otherwise. The religion tag is read, never guessed. No downloaded
model replaces the surveyed outline.

### Landmarks

Twenty places have their own models (`r1/landmarks.py`): the Eiffel Tower,
Statue of Liberty, Big Ben, Colosseum, Taj Mahal, Giza pyramids, Christ the
Redeemer, Sydney Opera House, Burj Khalifa, Empire State Building, Leaning
Tower of Pisa, Arc de Triomphe, Notre-Dame, Sagrada Família, Brandenburg Gate,
St Peter's, St Basil's, Parthenon, Tokyo Tower and Petronas Towers.

Each is a recipe of a few dozen lines in the vocabulary of `r1/sculpt.py`
(lathe, lattice, arcaded wall), regenerated identically everywhere. Anchor and
bearing are measured on the OSM element found by its `wikidata` tag, the height
is official, the ground is the tile's; three bearings (Liberty, Christ,
St Basil's) are inferred and the manifest says so. The OSM trace is not
extruded; its neighbours are.

| Level | Drawn | Budget (vertices) |
|---|---|---|
| 0 | by its own tile, while resident | 32 768 |
| 1 | beyond the resident tiles, to 1.8 km | 12 288 |
| 2 | to 5 km | 4 096 |

`python -m r1.landmarks` (from `game/tools`) bakes them into
`assets/world/landmarks/`; `tools/landmark_preview.py` draws a contact sheet
without the engine. The Christ stands on the drawn terrain, which the
Corcovado's surveyed summit raises ([Surveyed summits](#surveyed-summits)).

## Streets, signs and bridges

### Streets

Streets are polygon unions and differences (Clipper2) that connect
intersections, remove overlapping sidewalks and cut out buildings; their tops
are clipped to the same terrain triangles as the ground. Sidewalks rise 15 cm;
widths and sides follow OSM where tagged, and inferred defaults are labelled in
the `streets` manifest. Separately mapped sidewalks are respected. Zebras are
drawn only where OSM maps a marked crossing. Normals are shared, with a
quantised weld key that removes floating-point duplicates without moving
geometry.

### What the maps do not say: the predictive model

Regularities of a place are written as **rules** (`native/gen/predict.hpp`),
in four stages:

1. **Facts** — the neighbourhood's roads as a graph (`gen/roadnet.cpp`), welded
   on OSM's shared nodes, with directions of travel. An intersection is where a
   driver chooses: the merge of a split roundabout exit is not one.
2. **Rules** propose, each with a confidence and the OSM element it reasoned
   from: one rulebook per country plus `gen/rules_structure.cpp` for rules that
   hold everywhere. The country is measured (Overpass question 7, admin
   boundary); an older answer falls back to the bundled borders and the
   manifest says so. Adding a country is one `rules_<cc>.cpp` and one line in
   `rulebooks()`.
3. **Arbitration** — a surveyed sign silences a guess within 30 m (PLAN §3 I5);
   two guesses for the same traffic merge; 160 signs per tile.
4. **Emission** — scene nodes on shared models, streamed before the furniture
   and while driving.

France (`gen/rules_fr.cpp`, IISR):

| Rule | Says | Confidence |
|---|---|---|
| `fr.roundabout.give_way` | AB3a at every roundabout entry (R415-10) | 0.97 |
| `fr.roundabout.exit_priority` | AB6 60 m after an exit onto a départementale, in the country; no 80 | 0.80 |
| `fr.speed.limit` | a signed limit repeated after each junction and posted where it changes; the default (80 / 50) never is | 0.75 |
| `fr.zone30.gate` | B30 where a zone 30 is entered from a faster road | 0.85 |
| `fr.hump.warning` | A2b over a B14 30 before a hump, both ways, unless already at 30 | 0.80 |
| `fr.town.gates` | EB10 entering a town, EB20 leaving, named after the commune its buildings give as address | 0.85 |
| `any.grade_separation` | an expressway crossing a road without a shared node crosses on a bridge, the more important on top | 0.90 |

A town is its buildings on a 40 m grid grown by 80 m, at least fifteen; its
name comes from the `addr:city` its buildings carry, never guessed. A gate is
only posted where the OSM answer reaches past it.

United States (`gen/rules_us.cpp`, MUTCD):

| Rule | Says | Confidence |
|---|---|---|
| `us.roundabout.yield` | YIELD (R1-2) at every roundabout entry | 0.95 |
| `us.one_way.do_not_enter` | DO NOT ENTER (R5-1) at a one-way street's exit end | 0.90 |
| `us.stop.minor` | STOP (R1-1) on a minor road meeting a major one without lights; the stem of an equal T | 0.80 |
| `us.speed.limit` | SPEED LIMIT (R2-1) where the limit changes, in mph | 0.75 |
| `us.street.names` | two D3-1 blades crossed on a corner post, USPS abbreviations; 16 per tile, busiest crossings first | 0.60–0.84 |
| `us.stop.all_way` | STOP with ALL WAY (R1-3P) where two equal residential streets cross without lights | 0.60 |

No stop sign stands at a crossing with traffic lights. On Lawrence, Kansas,
53 of the 70 two-way stops proposed on one tile stand on a surveyed one.

**What OSM surveyed is drawn**, not only obeyed: a `traffic_sign`,
`highway=stop` or `give_way` node whose sign the catalogue has stands where it
was mapped, facing the traffic heading to the next junction (or as its
`direction` says). A stop on the crossing node itself is every approach's when
`stop=all`. The manifest's `predicted` section says, rule by rule, what was
proposed, silenced, merged and placed.

The signs are drawn by the project from the IISR and the MUTCD
(`tools/r1/signage.py`): no third-party artwork, faces in RA1 sheeting
luminance, lower edge at 1 m in the country and 2.30 m in town. Town names are
assembled from shared parts (posts, plate ends, a stretched middle, one model
per glyph). Prediction costs 5–15 ms for a tile that cooks in 60–400 ms.

### Bridges

Bridges, surveyed and predicted, are built by `native/gen/bridges.cpp`, and the
profile is solved on the neighbourhood's roads before anything else is cooked:

- A deck clears what passes under it (4.75 m over a road, 2.6 m over a path)
  plus its structure, and spans straight between its abutments.
- Roads climb to it on embankments at 5 % at most (8 % on foot), and every road
  joined to an embankment climbs with it.
- A predicted crossing, or anything over an expressway, may rise 7.5 m; a
  surveyed bridge 2.5 m. What it cannot rise to, the ground gives: the road
  beneath is dug, and the tile's relief is refined to take the dig.
- Decks mapped side by side are one deck at one level, parapets outside only.
- Drawn: carriageway, pavements and kerbs, parapets and cornices, underside,
  piers (never on the road below), abutments, 2-in-3 grass embankments.
- Walked and driven, on and under, by level (the manifest's `raised` pieces).
  Traffic drives bridges at their solved level. Each tile carries the pieces
  over it, whichever tile draws them, and solves on its neighbours' relief when
  it is on disk.

A tile still cooks in 280–380 ms; Paris along the Seine builds about 4 km of
deck around a tile.

## Props and vegetation

Props are **scene nodes**, never tile geometry: Saida's `MeshCache` keys meshes
by asset, so six hundred nodes on one bench upload it once. The whole prop
library is 9 390 vertices, once, for the planet. What a prop costs is a node,
so placement is capped per tile (260 by default, `scatter.hpp`), shared out by
kind so a street keeps its lamps *and* its trees. Every prop stands on a point
somebody surveyed; where OSM is thin, the world is thin:

| Neighbourhood | Placed | Refused by budget | Steeples |
|---|---|---|---|
| Paris | 2 263 | 2 103 | 18 |
| Amsterdam | 1 486 | 316 | 19 |
| Tunis | 16 | 0 | 6 |

Lamps, benches, bins, bus stops and fountains come from Kenney kits, extracted
from archives pinned by SHA-256 in `tools/r1/external_assets.py`, with the
shared colour atlas embedded in each GLB (a dangling URI renders magenta).
`normalize.repaint_kit_model` repaints every material into albedo and refuses
any material name the palette does not know.

### Trees

Trees are the project's photoscanned Poly Haven species (`CLAUDE.md` §1; a
test refuses any kit tree). A full scan does not fit (`island_tree_01` is
1 303 928 vertices; at a streamable 12 000 only 0.43% of the canopy survives),
and the scene loader parses a referenced file once per node. So trees are
planted as impostors: `tools/bake_nature.py` bakes each species into layered
cards (`assets/models/external/nature_cards/`) keeping its scanned albedo and
alpha, and a city tree carries a near model (`nature_selected/urban_tree.glb`)
switched by `LODGroupBehaviour`. Surveyed OSM trees and rows are planted first,
then mapped woods are filled within the budget.

### Measured canopy

`gen/canopy.*` is a world layer any system may ask (`storedCanopy`,
`Canopy::classAt`, `Canopy::heightAt`): per tile, a 48 × 48 grid (about 12 m a
cell) of trees (3 m or taller over at least 5 % of the cell), low vegetation
(1–3 m over a quarter: hedges, shrubs) or nothing, and the median tree height
of each 8 × 8 block.

The source is Meta and WRI's High Resolution Canopy Height Maps (1 m, global,
CC BY 4.0, imagery 2009–2020): zoom-9 Web Mercator BigTIFFs on AWS.
`fetchCanopyBand` reads the header, the row offsets of one tile row and those
rows (a few MB), and converts the hundred-odd tiles that band covers. The raw
map is never written. The grid is coded by an adaptive binary range coder with
a JBIG-like 10-cell context: a Breton bocage tile is about 200 bytes, sea
2 bytes, about **7 GB for all the land of the planet**, in one file per square
degree (`cache/world/canopy/<lat>_<lon>.r1c`). The game fetches a missing band
in the background and cooks again when it lands.

With the canopy, an inferred tree survives only in a tree cell, every empty
tree cell gets a tree at the measured height (trunk clear of roads and roofs),
every low cell a shrub; OSM trees keep their place. The budget is then 640
trees per tile (320 without canopy). On the Theix periurban tile the frame
stays at 60 fps (+0.8 ms scene update). The manifest's `nature.canopy` counts
cells, trees placed and inferred trees removed. Trunk colliders are created
when the tile mounts.

## Shops, interiors and fuel stations

### Shops (generator v23)

Retail buildings and buildings containing a shop node get a hollow shopfront,
framed display windows, a canopy and their OSM `name` / `brand` in the shared
Latin sign font (unsupported names are logged). No logo or unobserved
architecture is invented. A funeral home keeps its building.

- **The door** opens on a mapped public entrance (not `access=private`,
  emergency or exit-only); among several, the one whose aisle runs deepest
  (up to 12 m) wins. Without one, it is inferred toward the nearest parking
  (else road) on a 6 m aisle, and the manifest says `inferred`. Sliding double
  doors open from both sides, with colliders on the leaves.
- **Layouts by use**: market, bakery, fashion and mall recipes; up to 128
  fittings and two checkouts, four seeded merchandise variants on shared
  meshes; a clear central aisle.
- **The anchor**: a `shop=supermarket` node inside a mall keeps its measured
  position (`anchorSource: measured:tenant-node`); the floor nearer to it than
  to the door becomes its sales floor with 7.5 m gondolas (three instances of
  the 2.5 m shelf) and up to twelve checkouts facing the gallery. Le Fourchêne,
  Vannes: 76 gondolas, 52 kiosks, 12 checkouts, 22 792 vertices, 60 fps.
  Reference sizes: Carrefour City (Vannes), Carrefour Market (Theix), Carrefour
  in Le Fourchêne.
- **Parking** (`gen/retail.*`): mapped `amenity=parking` gets asphalt,
  synthesized 2.6 × 5 m bays, circulation and a painted pedestrian spine,
  draped on the terrain; buildings, roads, water and parks are subtracted.
  Ownership by a nearby shop is inferred and labelled. An inferred forecourt
  needs a mapped retail (or asphalt commercial) parcel. Underground,
  multistorey and private lots are excluded.
- **Parts**: a tile's shopfronts are one part per material (shell, frames,
  glazing reflections, one fascia per paint), never four per shop. Each part
  is a mesh upload, a draw and a static body: four per shop made the rue de
  Rivoli tile (`v26_27772_23992`, 205 shops) 849 parts, and the Paris capture
  timed out waiting for them. It is now 36.

Glazing is frames and reflection strips: the renderer has no sorted transparent
pass. `python -m r1.retail_materials` rebuilds the dedicated plaster finish and
the fascia letter coverage.

### Interiors (generator v25)

`gen/interiors.hpp` is the shared contract: an `InteriorPlan` (identity,
footprint, portal, floor, ceiling, recipe, provenance) travels with the tile;
`layoutInterior`, `buildInteriorShell` and `buildInteriorFixture` build the
room when the player approaches. `interior_uses.cpp` classifies homes,
police/gendarmerie, schools, offices, garages, clinics, prisons, worship,
restaurants and storage: tags win, tenant nodes complete anonymous buildings,
mapped campuses give an inferred association, and a home is the default
inference.

- **Homes**: open living/kitchen, separate bedrooms and bathrooms, 1.2 m
  doorways on a 1.8 m corridor. Rounded sofas and cushions, beds with
  headboards and pillows, wardrobes, bedside drawers and lamps, kitchen with
  oven, hob, basin, extractor and fridge, shower, lavabo, toilet, rugs, floor
  lamps, bookcases and plants where space permits.
- **Others**: classrooms with desks and boards; police reception, waiting,
  offices, archives and interview rooms; office workstations and meeting
  rooms; pews and prayer mats; garages with workbenches, tool cabinets and
  cars from the road fleet, plus up to two cars on synthesized forecourt bays
  (clear of buildings, roads, water, parks and slopes over 0.4 m).
- **Doors**: the doorway alone is cut in the regional façade (triangulated with
  a hole and eight welded corners); two leaves swing inward, stores keep
  sliding doors. A threshold joins floor and terrain; measured roofs and
  heights are kept.
- **Furniture** is shared prototypes (`interior_furniture.cpp`, including
  revolved ceramics), seeded by OSM id and region, fitted against the concave
  footprint without overlap, 128 fittings at most.
- **Streaming**: built within 65 m of the door, released beyond 85 m, two rooms
  at most with the player's first, 24 000 vertices each, checked against the
  arena's actual free space. A refusal is logged and retried on a later visit;
  an unloaded entrance has a closed-door collider.

Only the ground floor is furnished (`storeysFurnished: 1`). Footprints too
narrow for a door, raised volumes without access and buildings without
headroom are reported unavailable in `interiorStreaming`. Overpass question 10
and `.retail.json` v3 bring civic, office and craft observations.

### Fuel stations

A station (`amenity=fuel`, often also `shop=gas`) and a roof without walls
(`building=roof`) are never stores. `gen/fuel.*` finds the canopy: the station
way itself, the open roof or cadastre `wall=no` construction it stands in, the
one inside its area, or the nearest within 30 m. It is a slab on columns (4.7 m
clearance, 0.9 m fascia) that cars and walkers pass under. Other
`building=roof` are roofs on posts; other `wall=no` keep their walls.

Under it, synthesized pump islands (rows 8 m apart, islands 9 m apart,
4.6 × 1.2 m) with a column, two dispensers and bollards; islands are obstacles.
The fascia is in the brand's livery (an albedo table, neutral for an unknown
brand) and reads the country's word: STATION-SERVICE, TANKSTELLE, ESTACIÓN DE
SERVICIO, STAZIONE DI SERVIZIO, PETROL STATION, GAS STATION, STACJA PALIW…
(`fuelTitle`). An accent the font lacks is written as its plain letter; a
script it cannot spell leaves the brand, and `fuel.title` says why. A totem
stands 2 m off the nearest public road within 45 m, with the brand and no
price. Without a mapped canopy, an 18 × 9 m one is inferred along the road, or
the manifest says why none fitted.

## Harbours and the sea

OSM maps the **coastline**, not the sea. `native/gen/harbours.cpp` cuts the
tile by every coastline way crossing it, land on the left. A mapped basin
within 2.5 m of sea level joins the sea when a coastline crosses the tile; a
reservoir up the hill stays a lake; `tidal=yes`, harbour, dock or lagoon water
and water inside a mapped port are the sea's. Under the sea the terrain sinks
to −4 m below the animated surface.

- **Works, as mapped**: piers on piles at freeboard (pontoons low),
  breakwaters and groynes as rock mounds, quays as walls, lighthouses banded in
  their `seamark:landmark:colour` at their tagged height. Decks are walkable.
- **Boats, inferred and labelled**: berths along piers, quays and dock shores,
  each hull checked to float and clear the others. A cluster of piers is a
  marina; a port area brings tugs, bulk carriers and container ships with
  stacked yards; a ferry terminal a ferry; hot countries get rowing boats,
  Dutch canals houseboats (Kenney Watercraft Kit). 70 boats, 4 ships and 60
  containers per tile; piles are dropped first if a tile would pass its budget
  (`harbour.pilesDropped`).
- **Ships at sea**: up to 40 vessels from the shipped AIS density prior, the
  date and the cached weather; they keep moving.

**F** boards the nearer of the closest car and boat. A boat handles by its hull
length: drag grows with the square of speed, the rudder needs way on, and the
bow refuses land. Below 1.5 m/s, **F** steps onto a bank or deck alongside, or
into the water; a faster boat refuses the exit. Ships can be taken from the
water or from a boat at matched speed. The sea is continuous across tiles.

## Airports and aircraft

`gen/airports.cpp` lays what OSM traced on the terrain with the streets'
`Drape`, 8 cm up so it does not shimmer from a cockpit:

- **Runways** at their tagged `width`, else 45 m from 2.4 km long, 30 m from
  1.2 km, 18 m below (`airports.widthsTagged`). Grass, earth and gravel strips
  stay ground.
- **Taxiways** 18 m, **taxilanes** 12 m, **aprons** and **helipads** as traced,
  in photographed concrete.
- **Paint** (white 0.50, yellow 0.44/0.35/0.07): threshold bars, the
  designator (the `ref` half within 20° of the landing bearing, else the
  bearing), aiming points, centre and edge lines, taxi lines, stand lead-ins, a
  helipad's H.
- **The field** is the `airfield` ground class, last in the Atlas order.
- **Terminals and hangars** are a glass hall and a steel shed, 15 m and 12 m
  under a flat metal roof unless tagged (a tagged height stays the total).

Every footprint carries its top (`footprintTops`): what an aircraft clears,
stops against or lands on.

Every parked aircraft is inferred, with the element it was inferred from:

| What decides | How |
|---|---|
| What the airport receives | its longest runway within 4 km: from 2 800 m wide-bodies, from 1 500 m airliners, from 900 m business jets |
| Where | stand lines (nose at the end nearer a building), stand nodes (facing the nearest building), aprons without stands (rows across the long side) |
| What fits | room to the neighbour, clear of buildings, aircraft, runways and water |
| How many | three stands in ten empty, twelve aircraft per tile at most |

Roissy's nine central tiles: 16 airliners, 8 business jets, 2 wide-bodies.
**Military bases** (`landuse=military`, `military=base|barracks|airfield|naval_base`)
get one helicopter each: on a helipad, else an apron, else the first clear
ground from the middle. A barracks inside a base is the same base; ranges and
danger areas are not bases.

### Flying

| | Plane (airliner, wide-body, business jet) | Helicopter |
|---|---|---|
| Z / W, S | throttle up, down; S at idle brakes, then pushes back | forward, back |
| Q / A, D | nose wheel on the ground, bank in the air | turn on the spot |
| Space / ↑ | pull up — lifts off above rotation speed | climb |
| Maj / Ctrl / ↓ | push down | descend |
| F | out, at any moment | out, at any moment |

The handling is arcade, with its numbers in `assets/models/aircraft/fleet.json`:

| | top | rotate | stall | spool | roll | bank |
|---|---|---|---|---|---|---|
| Wide-body | 317 km/h | 58 m/s | 46 m/s | 0.22 | 24°/s | 32° |
| Airliner | 324 km/h | 55 m/s | 43 m/s | 0.30 | 32°/s | 36° |
| Business jet | 349 km/h | 42 m/s | 34 m/s | 0.80 | 95°/s | 70° |
| Helicopter | 162 km/h | — | — | 3 s spin-up | 75°/s yaw | climbs 10 m/s |

**Nothing crashes.** A building stops an aircraft where it touches it, and the
HUD and log say so; a plane stopped in the air loses lift and comes down; a
hard landing is a stop; water takes a ditching. **F** leaves at any moment: on
the ground beside the nose, in the air by jumping to open ground nearby (up to
80 m off), falling at most 55 m/s, into water too. The aircraft left behind
comes down without crashing. Streaming looks 45 s ahead; over unknown ground
the aircraft flies over the last surface it saw and the HUD says *relief
inconnu sous l'appareil*.

The fleet is authored in metres by `tools/r1/aircraft_fleet.py`: a wide-body
(64 m, 60 m span), an airliner (37.6 m), a T-tail business jet and a military
utility helicopter, unbranded, near and far models (1 000–3 800 vertices),
white livery at 0.34. The game animates the `gear` (hidden above 30 m) and
`rotor-*` nodes. Measured offline with `--fly`: the jet lifted off at 43 m/s,
258 m from its stand, and stopped 3.1 m up against a building; the helicopter
climbed 20 m in 4.9 s, its pilot jumped from 21 m and landed 2.1 s later.

## The North Pole, sea ice and weather

The Pole is a place like any other (`--spawn 0 90`): on foot, on the pack.

- **Where the sea is frozen is measured.** `native/gen/seaice.cpp` reads NOAA
  CoastWatch/PolarWatch's daily ASCAT ice classification (Metop-C, 4.28 km):
  open water, first-year, mixed or multi-year ice. A 48 × 48-cell window
  (205 km) is fetched once into `cache/world/seaice/`, only for polar tiles
  (`|lat| ≥ 60`) with sea-level ground, on a thread of its own. The satellite's
  polar hole and the land mask take the nearest class read
  (`seaIce.cellsFilled`: 319 of 2 304 around the pole on 22 September 2026).
  Without a reading, a climatology answers (ice north of 80°N, south of 70°S)
  and says `inferred`.
- **What the pack looks like is synthesised** (`seaIce.pack`) from one
  deterministic function on a polar stereographic plane, seamless at the pole
  and between tiles: floes as a warped Voronoi at 320 m, leads, pressure ridges
  (0.9–3.8 m sails, heaped slabs), snow dunes, melt ponds. The season comes
  from the reading's date. A frozen tile is a 161 × 161 grid (3.5 m), 42 700
  vertices and 179 000 indices, cooked in 30 ms; leads are water to swim in.
- **Surfaces** derive from the photographed snow already on disk
  (`tools/r1/sea_ice_textures.py`). Albedos (`atlas.json` → `ground.seaIce`):
  snow 0.83–0.89, bare ice 0.50–0.71, young grey ice 0.21–0.28, frozen ponds
  0.36–0.53, melt ponds 0.10–0.33.
- **To the horizon**: past the tiles, a 40 km disc of pack or water
  (`buildFarPack`), curved with the Earth (PLAN §3 I2), sunk half a metre per
  kilometre so the tiles win, rebuilt every 2 km. There, the fog is the
  measured visibility (23 km at the pole the day it was written).

**Weather you can see.** Open-Meteo's local conditions carry visibility,
wind, cloud cover, precipitation, snowfall and snow depth. Snow falls around
the camera with the measured wind, and drifts along the ground above 6 m/s.
From 3 cm of measured snow, ground and roofs of resident tiles wear the
photographed snow (streets stay clear). Cloud cover softens the Sun and blends
in an overcast sky; precipitation thickens the haze. A forecast older than two
hours is unavailable, and nothing waits for weather.

## The Sun and the sky

`native/world.cpp` calls `setObserver(lon, lat, altitude)` on
`scripts/sun_cycle.js` at every spawn and every rebase: the C++ says *where*,
never *which light*. The script is a port of `r1/solar.py` (NOAA/Meeus), held to
it at 1e-9 by a parity test over a year and five latitudes; three more tests
check that a new observer replaces the old one (Sydney and Cape Town invert
the season, Tokyo catches an equation-of-time sign, Quito and Tromsø the
extremes), and one that a non-finite coordinate is refused.

The clock is the real UTC clock: arriving at night in Tokyo is night in Tokyo.
`useSystemClock: false` fixes an instant, `secondsPerSecond` accelerates time.
The HUD shows the destination's civil time from Open-Meteo's time zone, or a
labelled longitude estimate.

**The sky** is eleven Qwantani pure-sky HDRIs (Poly Haven, CC0), one sky
photographed from before dawn to after dusk (`r1/skies.py`):

- each photograph is placed by the Sun's elevation at its instant (within 0.7°),
  and the runtime crossfades the two the real Sun stands between;
- each is turned so its Sun sits at the real Sun's azimuth;
- brightness comes from the model: the 0–4° band of each is scaled to the
  model's horizon colour, which is also the fog, so the dome meets the fog
  without a seam;
- each photograph's Sun is removed and the engine draws one disc where the
  light comes from (`SceneSettings::skySunDirection`).

The engine features this needed are generic: a second sky and a crossfade
(`skyboxBlendTexture`, `skyboxBlend`, `skyboxBlendRotation`), a Sun disc, and
`scene.setSkybox(path[, blendPath])`. `scenes/earth.scene` holds no hand-written
lighting value; `r1/prepare_world.py` writes them. Rebuild the skies with
`python -m r1.skies`.

The haze is a 5 km visibility (`fogDensity 0.00078`, Koschmieder's 3.912 / V)
and the far plane stops at 5 km with it: beyond the nine resident tiles the
world ends, and that edge is faint haze. Measured visibility may only thicken
it, except over the far pack.

## The car and the traffic

### The car

A car is parked beside every arrival: three metres to the player's right, then
a golden-angle spiral out to 14 m for ground that is loaded, dry and not inside
a building. It belongs to `earth.scene`, not to a tile, so a teleport does not
leave it behind. Each refusal (no kerb within 14 m, no room to step out, too
fast, edge of the loaded world, an obstacle) writes a log line and a line the
player can read.

The handling is arcade and says so (`native/world.cpp`): 100 km/h top speed,
7 m/s² of pull (0–100 in about 4 s), a 3.6 m turning circle and 16 m/s² of
lateral grip, about 90°/s of heading at town speed and 27°/s at speed. The
mouse turns the head, which eases back behind the car once it rolls. Stepping
out is refused above 2 m/s. Water ejects the driver into a swim and the car
sinks. Obstacles are engine scene queries.

**The detail follows the speed.** Above 4.2 m/s (15 km/h) street furniture
stops being imported and vegetation radii shrink to 55%; slow down and both
come back.

### The fleet

`r1/vehicle_fleet.py` authors seven unbranded vehicles in metres — city car,
saloon, SUV, off-roader, sports car, lorry, bus — with near and far models
(`assets/models/vehicles/fleet.json`, 27 732 vertices in all). People and road
vehicles are drawn at **0.8** of their size (`kVehicleScale`), the player's
call made looking at them; dimensions are scaled with them so doors, cameras
and gaps agree.

### The traffic

The simulation is [`engine/plugins/traffic`](../engine/plugins/traffic/README.md),
a header-only add-on that knows nothing about R1World (lane graph, car
following, give way, spawning around an observer), compiled into the game's
binary. Its tests run alone:
`g++ -std=c++20 -O2 -Wall -I../include test_traffic.cpp` in
`plugins/traffic/tests`.

| Here | There |
| --- | --- |
| `native/gen/scatter.cpp` (`buildLaneGraph`) turns the tile's OSM ways into a lane graph | `Graph` drives on it |
| The tile's density decides how many cars | `Flow::setPopulation` spends it |
| Which side of the road this place drives on | `Rules::leftHand` mirrors the lane |
| A scene node per agent, painted, on the terrain | `Flow::pose` says where |
| The player's car, as an obstacle | agents queue behind it |

- **A graph per tile**, cooked with the street from the same ways, with the
  tile's lifetime. Tunnels and `service` ways carry no traffic; bridges are
  driven at their solved level. Speeds are `maxspeed` where tagged (328 against
  32 estimates on the rue de Rivoli tile), and the tile says which.
- **How busy**: a car per 28 buildings, capped by road length (22 for a Paris
  tile, two or three for a village). Lanes are weighted by road class and
  spawning follows weight × length, so the cars are on the avenue you are
  looking at. Eighty cars per neighbourhood, nearest tile first.
- **Ten paints**, listed as albedos under `carPaints` in `atlas.json` (0.05 to
  0.30), one material each through the engine's cache.
- **F takes any car.** The traffic node leaves the flow (`Flow::retire`) and
  joins the world; the car you left stays where you left it. Six abandoned cars
  are kept; a teleport clears them.

## The people

The player and the crowd are scanned, rigged avatars from Microsoft's
**Rocketbox** library (MIT): nineteen in the crowd and one for the player.
`r1/humans.py` fetches what it needs at a pinned commit; `r1/humans_blender.py`
converts in Blender 4.2 (metres, feet on the ground, 26 face bones folded into
the head but the eyes kept) and retargets the motion-capture clips. The jump is
the flight phase of the sprint, held 0.74 s.

| | triangles | exported vertices | textures |
|---|---:|---:|---|
| Player | 7 364 | 4 584 | 1024 / 512 px |
| Passer-by, near | 2 400 | 1 571–2 010 | 512 / 256 px |
| Passer-by, far | 500 | 432–617 | same |

The twenty together are 49 530 shared vertices (`test_humans.py` holds them
under 55 000). Mean albedos 0.05–0.28; the bake refuses a map above 0.35. The
player stands 1.46 m at 0.8 scale; run and sprint are retimed (2.8 and 7 m/s)
so the feet stay planted. Each tile weights the light, medium and dark scans by
its country (`assets/world/countries.geojson`), stable per tile and slot,
balanced between men and women: gameplay defaults, not population statistics.

- **Where they walk is surveyed** (`native/gen/crowd.cpp`): the sidewalks the
  streets are drawn with, OSM footways, pedestrian streets, paths and steps,
  cut where they enter a building or a carriageway, loose ends joined across
  the road (where people cross), and two seats per placed bench.
- **How many is inferred** from pavement, shops, offices, bus stops, crossings
  and buildings, capped at 40 per tile, then scaled by the local solar hour
  (nearly nobody before dawn, peaks at rush hours and lunch) and halved by rain
  or snow. 60 per neighbourhood, nearest tiles first.
- **What they do**: walk on the right, choose a street at each junction,
  stand, wait, phone, talk in pairs, sit on benches, step aside for the player
  and run from a fast car. They appear out of sight (behind the camera or past
  70 m) and leave beyond 150 m. `r1::Crowd` runs headless in `test_crowd.cpp`.
- **Bodies**: everyone is a kinematic capsule in the engine's physics; the
  player is a `CharacterBodyNode` moved by `moveAndSlide`. A contact closing
  faster than 0.6 m/s is a bump: the person is carried off their line (20 cm
  at a jog, 70 cm at a sprint), their spine springs back (`ImpactModifier`),
  and they answer with a shrug, a telling-off or dusting themselves down.
- **Gaze**: within 7 m, most look up as the player passes; eyes, head, neck and
  spine follow him (`GazeModifier`) to 83° either side, then let go.

**Cost**: pooled nodes with their own animator over shared meshes and clips; a
person who has left is disabled and leaves the physics. Near model above 3.5%
of screen height (about 35 m). Within 12 m a pose every frame, further at 15, 8
and 4 Hz, held in between (`Animator::setPoseRate(hz, PoseRateMode::Hold)`).

## Physics

All runtime collision is the engine's (Jolt through Saida); the game has no
obstacle tests of its own. Each uploaded tile mesh gets a `StaticBodyNode` with
a `Mesh` `CollisionShapeNode`; interior shells and doors are mesh bodies;
furniture and vehicles have box colliders; trunks are authored from the cooked
tree positions. Interior bodies are destroyed with their room. Arrivals wait
for the destination's first physics sync. Character motion, ceilings,
spawn/exit occupancy and camera obstacles use the engine API. Water navigation,
the unloaded-tile limit and the globe's height sampling remain game rules.
Generation still uses polygons to place buildings, props, parking and
circulation: those are content constraints, not physics.

Engine fixes this game found, each with its regression test in Saida:

- a disabled character kept its inner physics body, and reactivation left it
  disconnected from the world (found on Paris → Tunis);
- scene raycasts and sphere overlaps now detect both sides of triangle walls;
- colliders are built in local space, so a wall first mounted millions of
  metres from the origin stays on its mesh after a rebase;
- `MaterialDesc::doubleSided` is honoured in scene draws, including mixed
  GPU-driven batches (facades and interior fixtures are double-sided).
- every shape of a compound walked its whole body for meshes every frame, a
  box included: a tile's ~600 trunk boxes made that N² visits, 89 ms of a
  110 ms Paris frame in `Physics/ResolveAutoShapes` (2 October 2026; 3.4 ms
  after). Box, sphere and capsule shapes now read no mesh.

## Performance

Measured on the Paris smoke with `--profile` (25 September 2026): a frame is
4.5 ms of CPU work plus 12.8 ms waiting for the frame cap; 63 animators cost
0.09 ms; 4 frames of 569 went over 33 ms on arrival.

| | before | after |
|---|---|---|
| Engine build the game links | Debug (`-O0`) | RelWithDebInfo |
| CPU per frame, Paris, 60 people | 37.3 ms | 4.5 ms |
| Animators (60 people and the player) | 2.97 ms | 0.09 ms |
| Arrival frames over 33 ms | 216 of 278 | 4 of 569 |

The Paris gallery capture (2 October 2026, 1600 × 900, rue de Rivoli) failed
four runs in five on the 180 s smoke limit: frames of 110 ms, and tiles
uploading about 20 parts a second. The profile named the trunk compound above
and 849-part tiles; with both fixed, every tile is in and the picture is
taken within 10 s of the spawn. Each memory-mesh upload still waits for the
GPU queue (about 1.7 ms a part), and the scene's two full transform walks
cost 6–7 ms each with nine Paris tiles resident.

Later measurements: Le Fourchêne hypermarket at 60 fps; the canopy tile at
60 fps (+0.8 ms scene update); a 600-frame Vannes home run at 0.750 ms/frame
for `Physics/SceneStep`. These were taken on an RTX 4070 host, not on the
reference i5 / GTX 1060. Full-tile imports can still exceed a 16.7 ms frame:
CPU asset decoding jobs and incremental GPU uploads remain to be done.

## Known limits

- **The world ends at about 800 m**, in a 5 km haze; from the air, only the far
  landmarks stand beyond it. A summit OSM does not survey stays as rounded as
  the elevation model has it.
- **Buildings**: preview façades without modelled openings; only ground floors
  are furnished (no upper floors, stairs or lifts); landmark interiors are not
  generated; no sorted glass pass. The Atlas building palettes are not yet
  re-derived as measured albedos.
- **Streets**: municipal road polygons (e.g. Paris) are not integrated;
  inferred sidewalk widths need not match the survey; crossing ramps and
  islands are not modelled; tunnels are skipped. Outside the measured Paris
  sample, detail has had less visual verification.
- **Traffic**: no traffic lights, no lane changing, no parked cars along the
  kerb, no collisions between traffic and the world beyond its lane; junction
  give-way is nearest-first. Traffic does not stop for pedestrians.
- **People**: no children, no regional clothing, passers-by never enter
  buildings.
- **Aircraft**: no air traffic, taxi routing or tower; runways and aerodromes
  mapped as multipolygon relations are not read.
- **Cold**: no fast ice in coastal sea cells, the Antarctic uses the
  climatology. The sky shows a faint seam where the equirectangular photograph
  wraps (the engine's).
- **Elevation**: IGN is used only in France; vertical-datum harmonisation
  between sources is not done.

## Attribution

Map data is © OpenStreetMap contributors (ODbL), summits included. Elevation:
IGN RGE ALTI (Licence Ouverte); the Terrain Tiles (Mapzen, with the credits
its sources require in `assets/licenses/Terrain-Tiles-attribution.txt`: USGS
SRTM, 3DEP and GMTED2010, NOAA ETOPO1, EU-DEM (Copernicus), Geoscience
Australia, Kartverket, LINZ, the Environment Agency, INEGI, the Government of
Canada, DGM Österreich, ArcticDEM); the Copernicus DEM GLO-90 through
Open-Meteo; weather from
Open-Meteo (CC BY 4.0); sea ice from NOAA CoastWatch/PolarWatch (Metop-C
ASCAT); canopy from Meta and WRI's High Resolution Canopy Height Maps
(CC BY 4.0); shipping density from the World Bank's Global Shipping Traffic
Density (CC BY 4.0). The people are Microsoft Rocketbox avatars and motion
capture (MIT, `assets/licenses/Microsoft-Rocketbox-MIT.txt`). The selection map
and offline coastline are Natural Earth (public domain). Asset provenance and
checksums are in `assets/THIRD_PARTY_ASSETS.json`, and attribution is visible
in game.
