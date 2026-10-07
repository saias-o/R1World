# R1World game project

This directory is the playable SaidaEngine project. Its one scene,
`scenes/earth.scene` (the project's `mainScene`), answers one question: can you
pick any point on Earth and walk there?

`docs/PLAN.md` holds the thesis, the invariants and the next updates;
`CLAUDE.md` holds the working rules. This file describes what exists today,
how it works and what was measured.

Documentation reviewed on 7 October 2026. The current generator is v35;
the validation records below are dated observations, not a claim that every
location or target machine is qualified.

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
minimap. `--weather <cloud fraction> <rain mm/h> <visibility metres>` overrides
inspection weather; it requires both `--at` and `--screenshot`. Cloud fraction
must be in [0,1], rain/visibility nonnegative and all values finite; zero
visibility keeps the clear-air default. The applied values are logged as
`[World inspection]`. A capture waits for OSM on tiles still provisional, up to two minutes,
then says so in the log. The picture's size is the window's: the hidden window
is 640 × 360 unless `SAIDA_WINDOW_SIZE=1600x900` (an engine variable) says
otherwise.

### The reference gallery

A visual regression is the one no test sees (`CLAUDE.md` §1). `tools/gallery.py`
takes the same ten pictures every time — same place, same camera and fixed
weather, 1600 × 900. City views use 10:30 local solar time on 21 September;
mountain views use the photograph's date, lens and aspect ratio. Each run records
its UTC instant, cloud fraction, rain and visibility; old captures without that
record are labelled accordingly. Weather inferred from the image is labelled
as estimated, not a historical station observation:

| View | Where |
|---|---|
| `paris` | Paris, the rue de Rivoli by Châtelet at eye height |
| `liberty` | the Statue of Liberty from Battery Park, 2.7 km away, at eye height |
| `sousse` | the médina of Sousse from 320 m up (Tunis's has few buildings in OSM) |
| `kyoto` | Higashiyama, Kyoto, towards the hills |
| `vannes` | the port of Vannes from the place Gambetta |
| `theix` | Theix: the fuel station, the car park and the Carrefour Market |
| `lasne` | Croix de Lasné, towards Saint-Colombier: rural verges and quiet frontage |
| `grenoble` | Grenoble, towards the Moucherotte and its limestone cliffs |
| `lecap` | Cape Town, towards Table Mountain |
| `rio` | Rio de Janeiro, the Corcovado from the Sugarloaf summit |

```powershell
python tools\gallery.py                 # every view
python tools\gallery.py kyoto theix     # some of them
python tools\gallery.py --offline       # proxies closed: the cache only
```

Each run is a folder of `generated/gallery/` (date and commit);
`generated/gallery/index.html` shows each view, the latest run beside the one
before. Run it after every update and look at it before calling the update
done.

Rio's EXIF says 17:53 on 22 May 2015 without a timezone. Reading it as civil
Rio time puts the Sun 8.7° below the horizon; the photo contains a low Sun on
the right. The test uses 19:53 UTC (16:53 civil time), an inferred one-hour
camera-clock correction: elevation 4.35°, azimuth 294.27°. The gallery states
that uncertainty. Open-Meteo's hourly reanalysis at 20:00 UTC gives 8% cloud and
no rain; visibility is visually estimated at 150 km. This is an approximate
lighting comparison, not a certified time/weather observation. The terrain
still lacks surveyed land cover and the distant city's buildings beyond the
resident tiles, and its silhouettes inherit the elevation source's resolution.

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
python tools\play_world.py --smoke --spawn -70.65 -33.45                       # new: installed relief (Santiago)
python tools\play_world.py --smoke --spawn 4.8900 52.3730 --spawn2 10.1815 36.8065  # Amsterdam, then Tunis
```

A smoke test that passes with closed proxies on a visited place is what proves
the cache's promise (`CLAUDE.md` §7).
The Santiago test was first run with neither OSM nor ground cached: arrival at
559.2 m in 234 ms, walking/jumping/driving passed, all nine terrain rings drawn.
The installed layer is never written to `cache/world/`; the finer survey wins
when it arrives. The service regression holds this upgrade with a delayed survey.

## Streaming, cache and network

### The tile grid and the floating origin

Tiles are metric latitude rings with no polar cutoff (`native/gen/common.hpp`):
36 000 rows of 0.005° and `72 000 · cos(lat)` columns per row, so a tile is
about 556 m on a side everywhere, keyed `v<version>_<row>_<col>`. The
generator version is `kVersion` (31 today). Within 18 km of a pole the
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
  The tile uses cached ground, then quick Terrain Tiles when they arrive,
  otherwise the installed relief, with Natural Earth's coast and nothing built
  on it. Terrain Tiles fall back to Copernicus through Open-Meteo. The manifest
  says `provisional`, the HUD says streets and buildings
  are on their way, and the tile is cooked again when the answer lands — with
  IGN's finer ground in France. A building that lands on the player moves them
  to the nearest free ground. First visits to Nice, Porto and Oslo: playable
  0.36 s after Go.
- **A failure from one source does not pause the others**; the HUD names the
  missing observation. With no observation and no network, Go opens a
  simplified tile (Natural Earth land/sea, installed relief, no streets), labelled
  **Hors ligne : terrain simplifié** and `offlineApproximation: true`.
  Flat ground remains the last resort where no usable relief is installed.
  Pending airport data no longer suppresses terrain or OSM upgrades; an empty
  airport response clears its pending flag. Tile uploads are ordered by distance
  from the player, so the spawn tile comes first.
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

- The summits come in the neighbourhood's own Overpass answer (question 11),
  by exact tags in the runways' wider box: not one Overpass call more, and a
  few hundred bytes. An older answer is raised to the square-degree lists an
  earlier generator kept in `cache/world/peaks/`, else not at all; nothing
  asks Overpass again for them.
- Under a summit the ground rises to a dome (30 m crown radius) that fades out
  150 m away, only where the relief stands below it. The raise reads nothing
  but the vertex, the summit and their distance, so neighbouring tiles agree
  on their shared edge.
- An `ele` that is not plain metres ("6234 ft", "1200-1300") is not read, and a
  summit more than 250 m above the relief under it is doubted, not drawn.
- The manifest's `peaks` says which summits raised the ground and by how
  much, which agreed with it, which were doubted, and what answered
  (`source`).

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
materials from Poly Haven and ambientCG scans:

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
one bay by one storey with a window, and their files say so
(`assets/textures/facades/<family>_and_window_*.jpg`): a wall family is a
façade, never a plain material for anything else. Entrance steps wear
`step_stone` (Poly Haven `marble_rock_02`, worn pale stone without joints, 2 m)
or `quay` (Poly Haven `concrete`, 4 m). UVs cost no vertex. Textures cost about
90 MB for a city neighbourhood; the GPU budget is 512 MB (`setGpuBudget` in
`native/world.cpp`). Ground and street colour maps are 1024², the rest 512².
Rebuild with `python -m r1.surfaces` from `game\tools`.
Pass family names to rebuild only those families, preserving the others, for
example `python -m r1.surfaces cliff`. The cliff scan is Poly Haven's
`rock_face_03`, a 27 m wall, separate from the existing 2 m rocky ground.

### Windows, relief and variation

A sheet's window is no longer painted. `r1/windows.py` fetches Poly Haven's
*Modular Urban Apartments Facade* (CC0, pinned in
`data/source-assets/models/`) and `r1/windows_blender.py` photographs three of
its window modules through an orthographic camera framed on their wall module,
once per map: colour, normal in the wall's frame, roughness and metalness,
depth in front of the wall, and the glazing with its film of dirt (through
which a dark room shows). `r1/surfaces.py` sets the window into each wall
family's tiled scan -- the tall window with its surround in plaster, stone and
brick, the bare opening in timber, earth and siding, two casements in
concrete -- at 1024² for the colour and 512² for the other maps. Rebuild with
`python -m r1.windows` then `python -m r1.surfaces <wall families>`; Blender
4.2 is needed, the photographs are cached in `cache/windows/`.

Each sheet also has a height map: the masonry's relief integrated from its own
normal map (high-passed, under 2 cm), the window 18 cm behind the wall and the
sill standing out of it. The engine draws it as parallax occlusion mapping up
close (`MaterialDesc::heightId`), so a window sits back in the wall seen along
a street; cobbles, pavement slabs and steps have one too. The glazing reflects
the sky (`MaterialDesc::environmentReflection`, 0.6 of it: from a street about
half of what a window faces is the buildings across it), and nothing else on
the sheet does. A wall a building gives no UVs to -- a gable, a parapet, the
storeys above a shop -- still gets its sheet's bays and storeys (`UvMode::
Facade`), so the floors above a Paris shop keep their windows.

Every textured surface varies across itself (`MaterialDesc::variation`,
`gen/palette.cpp`): natural ground warps so no two repeats line up and varies
over 64, 32 and 16 m; asphalt and open surfaces warp less; anything with
straight joints -- slabs, setts, bricks, planks, roof tiles -- never warps;
roofs and facades vary in patches of wear. The mean stays the measured albedo.

### Grass

Out to 70 m the ground grows knee-high grass, tufts of blades made by the
engine in the vertex shader (`saida::GrassNode`, its defaults), with gusts
running across it. The cook gives each tile a field (`gen/grass.cpp`): its
ground grid as drawn, and a 512² cover, about a metre a texel, whose density
follows the family the ground is drawn with (lawn and meadow full, fields of
crops nearly so, marsh, dry grass and savanna thinner, a wood's floor sparse)
and whose colour is that family's measured albedo -- straw on farmland -- so
where the blades thin out the ground under them is the same colour. Nothing grows under a face laid
within a metre of the ground (streets, pavements, car parks, quays, runways) or
inside a building. The player and the car push the blades aside.

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
are clipped to the same terrain triangles as the ground. Streets keep complete
nearby axes, buffered with round joins, so a bend keeps its outer edge (a strip
per segment left a notch at every bend, visible all over Paris). Where two ways
meet end to end, or one ends on another, the ends are rounded too; a free end
stays square. Only what lies inside the tile is laid; keeping the original
axes also preserves the phase of dashed markings and posts across tile edges.
They lie on the ground the terrain draws, sea-level adjustments included, and
never under the sea that ground meets. Sidewalks rise 15 cm;
widths and sides follow OSM where tagged, and inferred defaults are labelled in
the `streets` manifest. Separately mapped sidewalks are respected. Zebras are
drawn only where OSM maps a marked crossing. Normals are shared, with a
quantised weld key that removes floating-point duplicates without moving
geometry.

Generator v31 samples nearby building frontage every 40 m to distinguish
settled streets from rural roads. Explicit OSM sidewalk tags (including
`sidewalk:both=separate`) override that inference. Rural sides without a
sidewalk receive a narrow, varying aggregate shoulder and a low grass verge,
clipped around other streets, buildings and water. Both are textured, draped
surfaces sharing materials; their inferred areas are recorded in `streets`.

Generator v32 adds French detached-home priors in the Atlas `residential`
entries, with a Brittany override for lower houses and slate roofs. Country
uses the existing OSM/bundled jurisdiction rule; local coverage, building mass,
attached footprints and public/commercial land uses gate the prediction.
Mapped heights, storeys and roof tags still win. Dense street blocks keep their
existing urban generation, including their bus shelters.

Eligible homes receive an inferred front lawn, gravel approach, coated wire
fence and open gate leaves. The approach stays clear of planting; lawns mask
the surrounding tall grass. Roads, buildings, water and non-private land uses
clip the plots; neighbouring homes limit their extent. Only the frontage and
short returns are predicted, not a cadastral rear boundary. Wider visible
frontages precede small ones when a tile exhausts its 18,000-vertex frontage
budget. The absolute 120,000-vertex tile limit still applies. Geometry shares
material batches; hedges reuse the existing shrub asset and woody-plant budget.

OSM query v12 includes fences, walls, hedges and gate/entrance nodes. Mapped
boundaries silence a guessed enclosure and preserve measured gate openings.
Older observation caches remain usable; `residential.observationsQueried`
reports their missing boundary query. The manifest records the rule, confidence,
accepted plots and refusal reasons. Small untyped footprints beside French
rural bus stops become open timber shelters with a bench; substations remain
utility huts. Classification and dimensions remain inference where untagged.
See [the Lasné and Paris validation](../docs/VALIDATION_LASNE.md).

Generator v34 extends the low-rise priors to rural US homes (with a New England
override), European Russian cottages, Anti-Atlas mineral courts and Japanese
village houses. Historic Kiso valley rows have a bounded regional rule; observed
city/town centres silence it. Compact village architecture never authorizes
guessed gardens in dense blocks. Small untyped annexes beside eligible rural
homes stay one storey. Orthogonal houses with joined wings receive pitched roofs
over their actual wings; explicit flat roofs and the French/urban path remain
unchanged. Surveyed dimensions still beat every prior.

Tafraout's bounded small-town rule also covers low shop buildings, without
inventing private gardens or changing public institutions and observed cities.
Wadi ground, timber fences and stone boundaries use surface textures without
baked facade windows. Geographic inspection captures hide the player's mesh
when it overlaps the photo camera.

OSM query v13 and civic layer v4 add settlement nodes. Old caches can be enriched
without downloading their buildings again. Explicitly intermittent waterways
in southern Morocco receive a dry gravel-bed hypothesis, rather than permanent
swimming water; mapped permanent water is preserved. This does not model floods.

`tools/capture_rural_references.py` replays four geolocated photo views with EXIF
clocks and visually reconstructed weather. `--camera-geo <lon> <lat> <eye-AGL-m>`
fixes the capture camera independently of safe-spawn relocation; it requires
smoke capture and a viewpoint. Optical and bearing uncertainties are recorded,
including the stitched Moroccan panorama. `tools/rural_comparison.py` builds the
side-by-side review page. See [the registered rural study](../docs/VALIDATION_RURAL_REFERENCES.md)
for locations, remaining visual gaps, budgets and the unchanged Paris geometry.

Generator v30 fixes two missing road classes: `unclassified` country roads
used to fall back to 2.5 m, and `*_link` ramps were treated as pedestrian
surfaces at that same width. Country roads and residential streets now infer
6.5 m when no width or lane count is given. Every motor-road link is asphalt,
participates in the road graph and shares the same width calculation as bridge
decks, vegetation clearances and parking exclusions (`gen/road_profile.cpp`).
An OSM `width` always wins; otherwise `lanes` (or directional lane counts) and
hard shoulders determine the paved width. Expressway lanes infer 3.5 m each;
a one-way single-lane ramp has a 0.5 m left and 1 m right strip, 5 m total.
The cached Theix N165 observation contains these `trunk_link`, `lanes=1`,
`oneway=yes` ways, plus two- and three-lane mainline sections.

`gen/road_details.cpp` draws lane separators and edge lines on the actual
road surface, ground or raised; `lane_markings=no` suppresses them. French
defaults use T1 lane dashes (3/10 m) and T4 outside edge dashes (39/13 m) for
divided expressways, with continuous inner/ramp edges. Country comes from OSM
or the already bundled borders. At-grade intersections leave the lane markings
open; overlapping entry/exit pavement interrupts the edge line and guardrail.
Guardrails are folded galvanized steel beams and posts, inferred beside divided
expressways unless a road tag forbids them. Bridges keep their existing parapets.
The manifest separates tagged widths, lane counts and inferred markings/rails.

Narrow gaps between parallel opposing expressways become inferred grass
medians while retaining the mapped separation. Raised carriageways within
0.75 m in height share an interpolated median instead of overlapping talus
faces; embankment slopes are cut out of neighbouring road platforms. Roads
at different bridge levels do not join their decks. These defaults follow
[Cerema ICTAAL](https://dtrf.cerema.fr/pdf/pj/Dtrf/0008/Dtrf-0008477/DT9041.pdf)
and the [IISR marking patterns](https://equipementsdelaroute.cerema.fr/IMG/pdf/IISR_7ePARTIE_VC_20151208_cle2c9c2c.pdf)
as inference, not a survey of the existing road's equipment.

The v30 road changes were compiled without executing tests. Regression cases
for the Theix ramp profile, measured narrow widths, lane counts, raised paint
and rail openings are in `native/tests/test_roads.cpp`; they are not recorded
as passing. A new in-game Theix capture remains to validate the final picture.

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
woody plants per tile (trees and shrubs, 320 without canopy). Low vegetation
uses the existing shrub model near the camera and foliage cards at distance.
A surveyed low cell gets priority over inferred fill; spare capacity adds
clusters instead of isolated miniature trees. Low garden planting below the
survey's 1 m threshold is inferred beside rural homes, with entrance gaps,
and recorded separately in `nature.bySource`. On the Theix periurban tile the frame
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
  sliding doors. A door above the ground outside is reached by a flight of
  steps, never a slope: risers of at most 18 cm, 30 cm treads with a nosing,
  a 60 cm landing before the door, down to where the flight meets the
  sampled ground and founded below it (`buildEntranceStairs`). Cut stone where
  masonry is the regional tradition (Europe, the Mediterranean, Manhattan's
  stoops), concrete elsewhere and for shops, garages and warehouses. The flight
  is seen only; the walked slope stays inside it, under every tread. Measured
  roofs and heights are kept.
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

**No water on a street.** No water is drawn over a carriageway, cobbles or
pavement laid on the ground; water nobody sees (a culvert, a canal under a
boulevard, a covered reservoir: `tunnel`, `covered=yes`,
`location=underground`) is not drawn at all. A water cell is 25 m of water or
not; the streets crossing a water cell are published in the manifest
(`dryStreets`, rings in the tile's frame) and answer before the water, so a
car on a quay never sinks. Bridges are not streets on the ground: the river
under them stays water.

**How the water is drawn** (Saida's `WaterNode`, realistic style). The engine
uses continuous world-space waves, gradient-noise ripples, filtered sun highlights
and reflections of the current sky. Foam is evaluated per pixel, so it does not
expose the water mesh's triangles. Large procedural patches concentrate their
vertices around the camera; mapped river/lake coverage meshes stay fixed.
Beyond resident tiles, the far relief's water layer now uses the same shader,
with the sea's 0.12 m amplitude and 9 m wavelength, instead of opaque blue terrain.
The coarse classification of distant coasts is still a separate limitation.

The engine's `waveType` selects swell, wind sea or chop; `waveIntensity` controls
the overall agitation (0 is still, 1 is the default, up to 3), `windAngle` sets
the heading and `gustStrength` varies wave groups and wind patches over time.
These controls are available in the WaterNode inspector and scripting; R1World
currently uses the default wind sea, not a weather-driven sea-state simulation.
The reflection source is the sky; local buildings/boats are not reflected.

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

The haze is the measured visibility (Koschmieder's 3.912 / V); where none is
measured, in inspection captures, and from 24 km up (the weather models'
ceiling, fifteen miles), a clear day's 150 km (`fogDensity 0.0000261`). That
value is measured on the reference photographs: drawn without haze, Table
Mountain from Bloubergstrand stands 20% darker than the sky, as in the
photograph; 60 km made it whiter than the sky. The haze is the horizon's
colour, far brighter than a facade, so a little of it hides a lot.

The air is layered, not uniform (the engine's `fogRayleigh` and
`fogScaleHeight`). Clear air takes 13.6e-6 per metre at sea level at 550 nm
(Bucholtz), as λ⁻⁴ at the sky model's three wavelengths: blue goes about three
times faster than red, so far mountains turn blue, as in the photographs. It
thins over the pressure scale height (8.4 km). The rest of the measured
extinction, at the ground it was measured on, is aerosol: grey, thinning over
1.2 km, so summits stand in clearer air than valleys. Altitude along each ray
is taken over the sphere that fits the ground under the player (its Gaussian
radius), so the rays to the horizon climb out of the haze as they do. The
engine's fog reaches the far plane since its depth is reversed: before, it took
any depth over 0.9999 for sky and left everything past a kilometre unfogged.

## The relief to the horizon

Past the nine resident tiles the engine draws the relief out to 262 km
(Saida's `TerrainRingsNode`, geometry clipmaps): nine nested square rings of
128 × 128 cells, 16 m for the finest and 4 096 m for the last, each centred on
the player snapped to two of its cells. The grid is generated in the vertex
shader and the heights live in one storage buffer, so the horizon costs
nothing in the geometry arena; with reversed depth a metre at 100 km is still
resolved. The camera's far plane follows the farthest ring drawn.

- **Measured heights** (`native/gen/far_relief.*`): below 256 m spacing, the
  Terrain Tiles at each ring's zoom, then up to three coarser cached zooms,
  then the installed relief. From 256 m spacing, installed relief first,
  cached images second, without network. Sampling runs on a worker; a ring is
  sampled again when the player has moved two of its cells. A fine ring using
  installed fallback retries after 60 s, even when the player stands still.
  Missing rings retry on the same schedule. Its outer samples use the next
  ring's source policy, so where two rings meet they stand on the
  same heights and no crack opens.
- **Ringing filtered**: the images of zoom 11 and coarser overshoot by
  thousands of metres where a cliff meets the sea floor (4 109 m by Vidigal,
  Rio, where zoom 13 reads 137 m and Copernicus 418 m); a 3 × 3 median takes it
  out.
- **The curvature is content** (PLAN §3 I2): each sample is the true surface
  point under its grid position in the rings' tangent frame, which moves with
  the player every 20 km.
- **Inferred surfaces, measured albedos**: the sea where the model is at or
  below 0, snow above the latitude's snowline, rock past a 35° slope. Below the
  treeline (three quarters of the snowline: 2 140 m at 45°, 950 m at 60°), a
  slope past 15°, which nobody ploughs, is forest where the climate is wet
  (temperate, boreal, tropical) and scrub where it is mediterranean (the Cape's
  fynbos). The region's ground covers the rest. Slopes are judged as they
  would read over the finest ring's 16 m: terrain is self-affine (Hurst
  exponent 0.75, what mountain elevation models measure), so a slope read over
  a 128 m cell is steepened by (128 / 16)^0.25, and a cliff band the cell
  averages away is still rock. The log names each ring's zoom and how many
  cells were inferred as each.
- **Physical-scale materials**: cliffs use the 27 m photographed wall scan,
  snow and ground use the installed PBR surfaces. Forests select the climate's
  surface again when the rings reanchor, with 12 m filtered colour/normal
  variation for distant stands. Three-axis projection retains detail on
  vertical walls; continuous warping breaks texture repetition, and mip/normal
  filtering prevents distant shimmer. Cell materials and ring borders blend;
  the Atlas remains the albedo reference. This is inferred surface appearance,
  not a satellite image or a measured canopy map, and adds no geometry.
- **Their own shadows**: the engine marches from every sample toward the Sun
  over the rings' heights, out to the horizon. At dusk a range darkens the
  valley behind it while the summits stay lit. The march runs again only when
  the Sun has turned by 0.1° (about every 25 s) or a ring changed. The streets
  and buildings of the nine tiles receive them too: the engine keeps, over the
  finest ring's square, the height below which the Sun is hidden, so a roof
  keeps the last light the street has lost.
- **The tiles are holes** in the rings: the resident tiles draw their own,
  finer ground, and the rings neither overlap them nor leave a gap.
- **Offline**: images stay in `cache/world/terrain/<zoom>/`; fine rings prefer
  them to the installed fallback. Even a place never visited has its relief
  and horizon from the installed layer. Missing packs are unavailable, never
  interpreted as sea; malformed indices and truncated streams are refused and logged.
- Not drawn over the pack ice (its own far field is the horizon there) nor
  past 84.5° (the images are Web Mercator).

### The installed planet

`native/gen/relief.*` stores a grid per square degree: 400 intervals north/south
(about 278 m), longitude intervals reduced with latitude, metre heights from
−500 to 9 000. A median edge predictor and the shared binary range coder keep
the grid compact and lossless after source sampling/rounding. One indexed
`.r1relief` file holds a complete 10° × 10° block; absent cells within a valid
pack mean sea. There are 648 packs between 85° south and 85° north. The reader
keeps at most 64 decoded cells (about 20 MB at the equator), with shared access
protected across the world's workers.

The complete build on 5 October 2026 contains 22 856 land cells and weighs
858 961 885 bytes (859 MB, 819 MiB). Every pack and all 2 347 763 572 height
samples were decoded successfully before installation. Latitude edges share
the northern cell's edge curve, so grids of different widths join continuously.
Validation of the installed-relief integration on 5 October: 235 generator
tests, 110 Python tests and 90 engine tests passed; all nine offline reference
captures passed.
Offline walking/driving passed in Santiago and Paris, and Theix's retail check
passed entry, collisions, exit, eviction and regeneration.

From `game/`, build and install it:

```powershell
sh native/build_tools.sh
generated\tools\r1relief.exe --out assets/world/relief
```

`--threads <n>` controls concurrent packs; `--region <south> <west> <north>
<east>` selects whole intersecting packs; `--measure` writes nothing. An
interrupted build resumes by validating existing headers, spans and file sizes.
Each complete pack is published from a checked temporary file; failed packs
remain missing and make the command exit nonzero. The survey uses Terrain Tiles
zoom 7, the stored relief zoom 9, both with the same coastal-ringing correction
as the runtime. The source images are discarded after each pack.

The layer belongs under `assets/world/relief/`, is excluded from Git, and must
be built before distributing the game. Engine exports copy unknown extensions:
Windows and Web exports were checked with an actual pack and identical SHA-256.
On Web it is listed among the MEMFS boot files, which would preload the entire
layer. R1World currently builds as a Windows native application; a Web/console
port still needs asynchronous reads through the engine's asset system and an
explicit delivery policy for these packs. Those platforms are not claimed here.

The reference views of the gallery (Grenoble, Le Cap, Rio) are laid beside
real photographs: the Moucherotte, Table Mountain between Devil's Peak and
Lion's Head, and the Corcovado's ridge stand where the photographs have them.

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

`r1/vehicle_imports.py` converts seven licensed source vehicles to GLB — compact,
estate, SUV, off-roader, sports car, lorry, bus. The near models retain the source
geometry and normals **without decimation**; only distance models are reduced.
The source ZIPs and SHA-256 hashes are pinned in `data/source-assets/vehicles`,
so rebuilding needs Blender 4.2, not a live asset server:

```powershell
cd game/tools
python -m r1.vehicle_imports
```

Ruff's compact and the Scopia / Space Mushrooms vehicles are CC BY 3.0, credited
in the menu and `assets/licenses/Road-vehicles-CC-BY-3.0.txt`. Logos and hidden
interiors are removed. Paint, dark glazing, alloy wheels, mirrors and rubber
have distinct opaque metallic/roughness materials; detail textures ship inside
the GLBs. `assets/models/vehicles/fleet.json` counts the actual exported vertex
seams and both resident LODs (about 72k shared vertices for the whole fleet).
The engine switches to the distance mesh below about one fifth of the screen
height (coverage threshold 0.16),
with hysteresis; a nearby car keeps its full mesh, and distant traffic does
not keep drawing tens of thousands of triangles per compact.
The engine now binds the selected HDR sky to the PBR shader; previously a
descriptor rebuild replaced it with white, flattening every reflection. Diffuse
sky light remains supplied by the solar ambient, avoiding a second diffuse wash.
People and road
vehicles are drawn at **0.8** of their size (`kVehicleScale`), the player's
call made looking at them; dimensions are scaled with them so doors, cameras
and gaps agree.

### Validation recorded on 6 October

The integrated code passed 250 generator cases, 115 Python cases and 91
engine CTest cases. Native red/blue
environment captures and material sidedness checks passed. The Vannes-to-Paris
offline smoke passed walking, driving, traffic takeover and respawn; the
close car capture showed the restored sky reflections. These results are in
`generated/spawn-vehicle-main-*-tests.log`, `generated/pbr-engine-main-ctest.log`
and `generated/pbr-*-main-test.log` locally. The final optimized build completed
with `Play.ps1 -BuildOnly`. This documentation review records those existing
results; it does not constitute another test run.

The subsequent full reference gallery did **not** complete: Grenoble refused
`v29_27038_26176` at 124 952 vertices. The earlier nine-view gallery result
belongs to the 5 October integration and does not certify the later v29/v30
generators. Web and headset rendering were not validated for the PBR fix.

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
- **How many is inferred** from homes and active buildings near the paths,
  with a small outdoor share for dispersed households and larger shares for
  compact housing/apartments. Demand is capped at 40 per tile; only the share
  of demand within 110 m of the camera is simulated, then scaled by the local solar hour
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
furniture and vehicles have box colliders; a tile's trunks are one compound
body, and a trunk joins it when its tree is mounted, never before. A tree
still waiting to stream (all of them while driving above 15 km/h, when only
the signs stream) was an obstacle nobody could see, on tiles cooked before OSM
answered as much as after. Interior bodies are destroyed with their room. Arrivals wait
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
On 5 October, with the installed relief and both fixes merged into `main`,
the offline Rivoli gallery run reached play in 1.64 s and mounted all nine
tiles in 2.33 s, with the generator at version 27; batching compares every
material property, including textures, rather than only its name and paint.

Later measurements: Le Fourchêne hypermarket at 60 fps; the canopy tile at
60 fps (+0.8 ms scene update); a 600-frame Vannes home run at 0.750 ms/frame
for `Physics/SceneStep`. These were taken on an RTX 4070 host, not on the
reference i5 / GTX 1060. Full-tile imports can still exceed a 16.7 ms frame:
CPU asset decoding jobs and incremental GPU uploads remain to be done.

## Known limits

- **Dense spawn tiles**: the v29 Grenoble capture refused `v29_27038_26176`
  at the 120 000-vertex
  tile limit (124 952 total; 46 041 in embankments). It remained unmounted even
  though surrounding tiles can load. This is a separate geometry-budget issue
  from the fixed pending-airport upgrade bug. The 6 October diagnostic is in
  `cache/sessions/2692d72965094d67865fb59d5cbd36ab/game.log` locally.
  The v30 road changes have not requalified that tile or the full gallery.
- **Cars**: paint and metal now reflect the HDR sky, but glazing is opaque,
  there is no automotive clearcoat layer, and the reflection source is the sky,
  not nearby buildings. An automotive clearcoat and local reflection solution
  remain work for the renderer.
- **The streets and buildings end at about 800 m**: past the nine resident
  tiles only the relief is drawn, to the horizon. A summit OSM does not survey
  stays as rounded as the elevation model has it, and the far relief does not
  raise to surveyed summits yet. The installed planet supplies the relief of
  unvisited places offline at about 278 m; streets and buildings still need
  their surveyed observations from the network or an earlier visit.
- **Buildings**: preview façades without modelled openings; only ground floors
  are furnished (no upper floors, stairs or lifts); landmark interiors are not
  generated; no sorted glass pass. The Atlas building palettes are not yet
  re-derived as measured albedos.
- **Water from the air**: past the resident tiles, the far relief draws the
  sea as whole cells, so a coast far away is a staircase of squares; a tile
  cooked offline before its observations arrive takes Natural Earth's coarse
  coast and can show a flat square of sand at sea until OSM answers.
- **Landmarks**: their stone, concrete and brick finishes use the façade
  sheets, which carry a window in every bay.
- **Streets**: municipal road polygons (e.g. Paris) are not integrated;
  inferred sidewalk widths need not match the survey; crossing ramps and
  islands are not modelled; tunnels are skipped. Exact merge tapers, gore
  chevrons, turn arrows and separately mapped barrier ways are not yet read.
  Generated guardrails follow class defaults, not a complete equipment survey.
  Outside the measured Paris
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
capture (MIT, `assets/licenses/Microsoft-Rocketbox-MIT.txt`). The road vehicles
are by Ruff and Scopia / Space Mushrooms,
CC BY 3.0 (`assets/licenses/Road-vehicles-CC-BY-3.0.txt`). The selection map
and offline coastline are Natural Earth (public domain). Asset provenance and
checksums are in `assets/THIRD_PARTY_ASSETS.json`, and attribution is visible
in game.

Generator v35 reads inland-water multipolygons (OSM query v14), joins reversed shoreline members, preserves inner land rings and rejects incomplete rings. Kawaguchi relation 2313174 was refreshed from the official OSM API; the targeted check and offline capture passed. Water geometry is clipped to each resident tile, and inferred vegetation is excluded from the lake. Distant water beyond resident tiles still uses the coarse relief.
