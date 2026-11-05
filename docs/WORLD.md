# R1World — technical reference

This file describes the implemented world and its runtime contracts. Paths are
relative to `game/` unless stated otherwise. See [the player guide](../game/README.md)
for controls, [DEVELOPMENT.md](DEVELOPMENT.md) for builds and checks, and
[PLAN.md](PLAN.md) for open work. Change history is kept in Git.

## Runtime architecture

`native/world.cpp` initializes the engine and callbacks. The public session API
is `native/world/world.hpp`; `session.hpp` holds its private state. The session
is neither copyable nor movable and is destroyed before the engine.
Domain implementations compile separately under `native/world/`.

- `geography`, `tile_preparation`, `menu_tile_cache` and `options_store` provide
  coordinates, CPU mesh preparation, map caching and persisted options.
- `streaming`, `tile_streaming`, `terrain`, `assets` and `distant` handle
  residence, uploads, queries, shared prototypes and the horizon.
- Player, vehicles, population, combat and presentation have separate source
  files but still share the private session state.
- `loaded_tile.hpp` owns tile runtime data. Its traffic, walking and habitat
  graphs must keep stable addresses while simulations reference them.

`update` orders UI, session/streaming, scene, player, camera, capture, HUD and
smoke phases. Early returns must stop the same frame. `TravelState` represents
one exclusive Foot/Swim/Car/Boat/Aircraft mode; `transitionTravel` detaches the
bike rider before updating possession, character activation, position and camera.

## Photography and persistence

`player_profile.*` owns wallet, items, ammunition and photo metadata.
`photography.cpp` connects input/UI and capture; `photo_animals.cpp` gathers
visible loaded animals. `photo_detection.*` applies the actual exposure
projection and resolution, rejects off-frame/subpixel subjects and samples
visibility against physics colliders after scene/physics updates. It does not
perform pixel segmentation; foliage without colliders and animated silhouettes
remain approximations. Each species is saved once per photo.

Originals live in `saves/photos/`, the profile in `saves/player.json`.
Version 2 reads version 1 without retroactively classifying old photographs.
Atomic writes follow photos/menu actions, ammunition changes (at most every two
seconds), and clean shutdown. Corrupt/future profiles are protected against
replacement. Missing images are reported. If metadata cannot be saved, the PNG
is retained and opening the menu retries. Tests use isolated generated profiles.

## Streaming, cache and network

### The tile grid and the floating origin

Tiles are metric latitude rings with no polar cutoff (`native/gen/common.hpp`):
36 000 rows of 0.005° and `72 000 · cos(lat)` columns per row, so a tile is
about 556 m on a side everywhere, keyed `v<version>_<row>_<col>`. The
generator version is defined by `kVersion` in `native/gen/common.hpp`. Within 18 km of a pole the
neighbourhood is the tiles nearest in metres, twelve at the pole.

The player's position is WGS84; each tile owns its tangent frame, and the
renderer only sees metres relative to an origin rebased every 350 m
(`native/world/`, PLAN §3 I1). Crossing a tile edge, the date line or a pole
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
  to the nearest free ground.
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

### Observation cache

Everything downloaded stays under `cache/world/`: each tile's observations
(`osm.json`, `ground-elevation.json`) in `v<N>_<row>_<col>/`, and the Overpass
answers a neighbourhood shares in `sources/`. Geometry is never stored: the
game cooks it on every visit, so a generator change reaches every place already
visited. A visited place costs about 10 MB (about 1 MB a tile) and never
touches the network again. Folders from older generator versions are kept:
their raw observations are read by later versions.

Each cached Overpass answer records the query schema that produced it. Answers from `kOsmBaseVersion` on are
cooked at once; what later questions added arrives as separate, small layers
fetched in the background and never revalidated once on disk:

| Layer | Brings | Tile flag while missing |
|---|---|---|
| `<answer>.aero.json` | aeroways, military areas, a 5 km runway box | `airportsPending` |
| `<answer>.retail.json` (v3) | shops, entrances, parking, fuel stations, civic amenities, offices, crafts | `retail.observationsQueried`, `interiorStreaming` |

A layer is found beside any neighbourhood query covering the tile. A tile is
cooked again when its layer lands only if the layer holds something for it.

### Network failures

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

### Geometry budget

Saida's `GeometryRegistry` arena is 1 048 576 vertices and 3 145 728 indices by
default (configurable at engine startup). Tiles get 85% of
`ResourceManager::geometryCapacity()` (`kTileGeometryShare`), minus the police
reserve of 45,000 vertices / 135,000 indices; the rest is for shared models and
private runtime geometry. `kTileVertexBudget` (120 000, `native/gen/cook.hpp`) is a soft
generation target. The [density policy](PLAN.md) keeps dense tiles
visitable. A neighbourhood past either resident arena
limit is **refused, not truncated**:
one log line names the tile and the arithmetic, the smoke test fails on that
reason, and in play the spawn is cancelled with a sentence.

### Memory

The runtime limits cooking to two concurrent tiles and targets twelve prepared
tiles in its cache. Accepted local requests stay pinned, with at most twelve
polar tiles plus three forecasts. Cold tiles are released under pressure.
Admission reserves 512 MiB for the frame and 256 MiB per cook. The generator's
mesh copy is released after CPU preparation; prepared geometry remains resident.
A cook allocation failure logs OUT-OF-MEMORY, reduces concurrency, retries and
fails a smoke explicitly. The decoded-asset budget is 512 MiB, separate from
the GPU texture budget and geometry arena.

## Runtime admission and frame contracts

Ground stitching records the exact tiles read by `joinedGround`, including
neighbour seams and longitude wrapping. Each source change advances dependent
revisions even during a first cook; publication acknowledges only the revision
that cook started with. Newer revisions schedule another cook. Ground cache
reads quarantine malformed observations with a diagnostic and try older valid
versions before installed/network fallback. Checked temporary writes preserve
the last complete destination until replacement succeeds.

Mesh proxies expose Pending/Ready/Failed plus a reason. Terrain and Far building
failures reject arrival explicitly and fail a smoke immediately. A failed,
timed-out or over-budget replacement retains its prior terrain; an incomplete
replacement is never promoted by a timeout. A rejected serial is not retried
on every frame; a newer source or leaving/revisiting the tile can retry.

All local tiles precede up to three forecast tiles. The service's request bound
(12 polar tiles + 3 forecasts) is separate from its cache target of 12. `want`
returns accepted/refused tiles, and the runtime checks every local admission.
Accepted tiles remain pinned; total cooked residency is bounded by the larger
of the request limit and cache target. Host-memory admission still applies.

Gameplay actors share one accepted frame duration, at most 250 ms, split into
steps no longer than 50 ms. Player/combat, crowd, fauna, police, traffic and sea
actors consume the same steps; mouse motion is sampled once per frame. Longer
stalls discard time for all these actors together, reported in
`World/DroppedSimulationMs`. World clock/UI retain frame time.

`GenerationProfile` revision 1 identifies the geometry target independently of
observations. Manifests, service logs, `r1cook` JSON and exported generation
sidecars record it. To reproduce a runtime cook, pass its `targetVertices` as
`r1cook --tile-target`.

## Ground and terrain

### Elevation

The terrain mesh starts with a 41 × 41 grid over a tile (`kTerrainMeshSize`),
and every source is read at that grid (`native/gen/sources.cpp`). Its north and
south boundary strips also contain the regular longitude knots of both
adjacent rows. Those rows have different tile widths; resampling a shared
curve separately would leave cracks on steep terrain. Both sides are
triangulated at the shared knots. Ground queries,
grass height/coverage, peak shaping and bridge excavation use those refined
strips too; the interior grid remains unchanged.

Elevation sources, in priority order:

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
Where OSM mapped nothing, `native/gen/ground.cpp` measures local building
footprint coverage in a **200 × 200 m** window, including neighbouring tiles.
Coverage is weighted by the **completed predicted building height**, after
mapped heights, storeys and neighbour inference have been resolved by the same
planner used for rendering. Height contributes `max(1, log2(1 + height / 6))`:
towers influence the ground more than low houses without one exceptional tower
dominating its neighbourhood. The intermediate settlement score requires three
buildings; a very high weighted density can also qualify a large isolated tower.
A low farm shed stays below that dense threshold. Footprints are rasterized
as a union on a globe-aligned sampling grid (about 4 × 6 m at Vannes), so
overlaps do not inflate coverage and tile boundaries do not reset the estimate.

The Atlas's `ground.inference` table chooses the sparse, settled and dense
fallbacks, with regional then country overrides. The default weighted coverage
thresholds are 0.18 and 0.45: France keeps the regional natural ground in sparse
areas and uses made ground around substantial development. Arid regions and
the sub-Saharan profile prefer exposed soil in intermediate settlements;
Burkina Faso's country override starts with bare soil and reserves made ground
for a much denser score (0.55). These are adjustable priors, not surveyed
surfaces. Grass, water, parks, forests and other recognized OSM polygons always
win. The same classes choose both the ground texture and grass blades, so
inferred paving and bare soil grow no blades. The v42 key regenerates geometry.

`ground.inference` records country, profile, window, thresholds and height
weight. Classes such as `inferred:urban` and `inferred:bare` remain estimates
in `ground.measuredFraction`.

Each tile also ships the grid it was classified on (40 rows of 40 characters:
land, inland water, sea-level water). It says where the water is; its depth
decides how the player moves. He swims, head above the surface, when half his
height or more is under the water, and walks on the bed otherwise. That holds
when walking in or out, spawning, landing from a jump, an aircraft or a boat,
and when a car sinks. A cell is 25 m wide, so a rock, a sandbar or the swash
inside a water cell is ground where its ground is above that depth.

His bed is what his feet rest on, as the character solver found it: a
groyne, a breakwater or a slipway is a collision mesh, absent from the ground
grid and from `height`, and it still holds him above the water. Elsewhere the
bed is the tile's ground grid, but the relief sources stop at the shore:
open sea reads exactly 0.0 m at every grid point, and inland water is drawn on
its own ground. Where a grid quad's four corners all read 0.0 m, or the water
is inland, no bed is known and the water counts as deep. Only the beach
generator's shore profile, rocks and shoals give the sea a measurable depth.
The log gives the depth on each entry (`[World swim] entered water ...
depth=`, `standing at ... in ... m of water`, `spawned ... wading in ...`).

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
or `quay` (Poly Haven `concrete`, 4 m). UVs cost no vertex. The GPU resource budget is 3072 MiB in Standard and 512 MiB in Toaster
(`native/world/options.cpp`, `applyGpuBudget`). Ground and street colour maps
are 1024², the rest 512².
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
where the blades thin out the ground under them is the same colour. The blades
carry the ground material's light and dark patches (its macro variation), and
the engine shades them, away from the eye, exactly as that ground: at 40-80 m
a capture with blades and one without measure the same, band for band, within
3 %. Grassy ground is a canopy and is drawn matt (`palette.cpp`, `isCanopy`):
Grass004's scan averages a roughness of 0.26, and at grazing angles it mirrored
the sky, a pale blue band where the blades ended. Nothing grows under a face laid
within a metre of the ground (streets, pavements, car parks, quays, runways) or
inside a building. The player and the car push the blades aside.

## Buildings

### Architectural priors

Fallback heights, façade materials and roof forms come from the Atlas
(`assets/world/atlas.json`, read by `native/gen/palette.cpp`): **twelve named
regions and twenty-two continental bands**. Rectangles overlap and the table is
written most-specific first: the first match wins, so a region goes above the
band it refines.

| Tier | What it claims |
|---|---|
| `region` | a place someone described: Paris's cornice line, Tunis's whitewash and parapet, Amsterdam's narrow steep-gabled plots |
| `band` | the smallest honest thing about a continent: a Nordic town is painted timber under a steep roof, a Saharan one flat-roofed earth render |
| `none` | open ocean, ice sheets, the seams between rectangles |

A tagged positive `height` wins, without the former 90 m wall / 300 m total
height caps. Tagged storeys and roof heights likewise have no architectural
maximum. Flat-roof parapets fit inside a mapped total; a mapped place-of-worship
height is not multiplied by a synthesized steeple. Near, Mid, Far and physical
shells preserve the same planned total height. Above 128 storeys or 1,024
estimated window bays, existing facade textures replace modelled openings;
the building's dimensions and floor count remain intact.

Without its own height or storeys, an ordinary closed building uses the
arithmetic mean of the three nearest buildings with mapped heights within
250 m. One or two references suffice when fewer are available. If no mapped
height exists nearby, mapped storey counts provide estimated reference heights;
otherwise the Atlas remains the fallback. Sheds, garages, open canopies,
religious towers and airport structures do not supply ordinary street heights.
Homes use comparable residential references: public halls, apartment buildings,
footprints under 45 m² and footprints over three times larger or smaller are
excluded. Only homes classified by the low-rise residential rule exclude
references with a mapped floor count above two; ordinary urban houses retain
their taller residential references.
The full cached OSM neighbourhood participates, including adjacent tiles;
inferred heights never become references. Distance ties are resolved by OSM ID.
No additional data source or download is needed.

Attached French homes
receive the regional home profile when an observed village or hamlet lies
within 700 m and no town or city lies within 1.5 km. These rows do not invent
gardens. Back-row village homes without a road within 45 m keep their home
architecture without an inferred garden or driveway. Untyped footprints below
45 m² beside a recognized home become low
annexes, unless their use or mapped dimensions says otherwise. Untyped
`wall=no` light constructions stay low instead of becoming towers; this
cadastre tag does not prove a fully open roof (see the
[OSM tag documentation](https://wiki.openstreetmap.org/wiki/Tag%3Awall%3Dno)). The Breton fallback favors
two wall levels (80%); this is an adjustable
architectural prior, not a measurement. Mapped heights and levels still win.
Pitched home roofs are partitioned into rectangular wings when possible;
otherwise their planes are clipped to the actual irregular footprint. Near,
Mid and collision geometry retain the same dimensional plan, also used for
ground inference. Residential classifications apply to adjacent-tile height
references as well.

### Three building levels

Each closed building has three deterministic representations. **Near** adds
window reveals and glazing, roof thickness, trim and regional roof details;
its doors match the generated interiors. **Mid** keeps the surveyed footprint
and a simplified roof, with closed façades and no interior openings or added
detail. **Far** is a closed oriented box with at most **12 triangles per
building**, regional wall and roof colours, no texture maps and no cast shadows.
Open structures such as bus shelters and canopies keep their existing geometry.

Saida selects levels by projected screen coverage (Near 0.08, Mid 0.008),
with a 0.3-second dither fade. Near is eligible within 75 m of the building
bounds and remains eligible until 85 m. A ready level stays visible while its
replacement uploads. Far stays resident as a small fallback; unused Near and
Mid meshes are released after their fade. The tile streamer and closer-level
admission share the arena budget. Physics uses the original building shell
on the CPU, so a distant box cannot change walls, doorways or collision shapes.
Interiors load only when their building's Near is ready.

The permanent density policy still uses **120,000 vertices per tile**, counted
on the original geometry. A degraded tile uses the original envelope for Near,
without the added façade and roof details, and blocks new interiors. Manifest
`vertices`/`indices` retain that baseline; `residentVertices`/`residentIndices`
reserve the static tile plus Far, and `buildingLods` records each variant's
counts. Diagnostic GLB exports retain the original full shells. The three
variants never get added together to trigger the density threshold.

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

The street generator samples nearby building frontage every 40 m to distinguish
settled streets from rural roads. Explicit OSM sidewalk tags (including
`sidewalk:both=separate`) override that inference. Rural sides without a
sidewalk receive a narrow, varying aggregate shoulder and a low grass verge,
clipped around other streets, buildings and water. Both are textured, draped
surfaces sharing materials; their inferred areas are recorded in `streets`.

French detached-home priors are defined in the Atlas `residential`
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
budget. The persistent 120,000-vertex density rule also applies. Geometry shares
material batches.

OSM query v12 includes fences, walls and gate/entrance nodes. Mapped
boundaries silence a guessed enclosure and preserve measured gate openings.
Older observation caches remain usable; `residential.observationsQueried`
reports their missing boundary query. The manifest records the rule, confidence,
accepted plots and refusal reasons. Small untyped footprints beside French
rural bus stops become open timber shelters with a bench; substations remain
utility huts. Classification and dimensions remain inference where untagged.
The `lasne` view in `tools/gallery.py` keeps the comparison camera and lighting
reproducible at (-2.71559, 47.56272). It is a visual regression reference, not a
claim of exact photographic fidelity: unobserved house dimensions, property
boundaries, foliage and furniture remain inferred. Dense central Paris is
excluded from these rural frontage rules.

Low-rise priors also cover rural US homes (with a New England
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

Narrow gaps between parallel opposing expressways become inferred grass
medians while retaining the mapped separation. Raised carriageways within
0.75 m in height share an interpolated median instead of overlapping talus
faces; embankment slopes are cut out of neighbouring road platforms. Roads
at different bridge levels do not join their decks. These defaults follow
[Cerema ICTAAL](https://dtrf.cerema.fr/pdf/pj/Dtrf/0008/Dtrf-0008477/DT9041.pdf)
and the [IISR marking patterns](https://equipementsdelaroute.cerema.fr/IMG/pdf/IISR_7ePARTIE_VC_20151208_cle2c9c2c.pdf)
as inference, not a survey of the existing road's equipment.

### Predictive signage

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

## Props and vegetation

Props are **scene nodes**, never tile geometry: Saida's `MeshCache` keys meshes
by asset, so six hundred nodes on one bench upload it once. Placement is capped per tile (260 by default, `scatter.hpp`), shared out by
kind so a street keeps its lamps *and* its trees. Every prop stands on a point
somebody surveyed; where OSM is thin, the world is thin:

Benches align their long edge with the nearest road segment and face the
carriageway, on either side and independently of the road's mapped direction.

### Trees

The generator places non-cherry trees 30 cm below the sampled ground to bury
their roots. Sakura trees retain their existing ground-level placement.

The generator uses the selected Innerscene flowering cherry
(`nature_selected/sakura*.glb`). Observed `Prunus serrulata`, `Prunus [x/×]
yedoensis`, `Prunus subhirtella` and `Cerasus` select it worldwide, including
rural areas and the southern hemisphere. For trees without botanical identity,
deterministic urban planting selects it with probability 15% in Japan or 5%
elsewhere north of the equator; the rates do not stack. Both rates
require a nearby OSM `place=city/town` (less than 1500 m); a village/hamlet
at least as close vetoes inference. Unknown settlement status is ineligible.
This conservative local evidence is not a municipal boundary: a city's name
does not authorize planting across its surrounding countryside. In addition,
the tree must lie in mapped residential/commercial/retail/industrial land use
or have at least 25 principal buildings within a 350 m square. Density and
residential land use alone no longer turn villages such as Theix into cities.
Mapped woodland and
agricultural areas, rural roadside planting and known non-cherry species stay
excluded from these rates. The model has fixed blossom, without a seasonal
cycle. See [JAPAN_UPDATE.md](JAPAN_UPDATE.md) for the selection policy and asset provenance.

Observed conifers worldwide use the adult tall pine:
`leaf_type=needleleaved`, legacy `wood=coniferous`, and conifer genera in
`genus`, `species` or `taxon` (Pinus, Abies, Picea, Cedrus, Larix, etc.).
The source is TerrainTale's **Pine Tree Tall**, published under CC0 1.0,
with ambientCG materials and ez-tree credited. The existing photoscanned
saplings remain on disk; the adult pine widens the supported tree forms.
`tools/r1/pine.py` pins the source SHA-256, normalizes its planting pivot and
height, preserves its PBR materials, and derives all LODs from that same tree:
Near 6481 vertices / 5128 triangles, Mid 5735 / 3969, Far 4 / 2. Mid
keeps every LOD0 needle card, UV and material; only bark and branches undergo
UV-aware QEM simplification (no proxy or sloppy fallback). This improves the
crown while reducing the former Mid's 4028 triangles. Far is a camera-facing
billboard using the same engine path as the classic tree, baked directly from
LOD0 into 32 azimuths × 5 elevations at 192 px per view, with relightable normals
and visibility. Its normal response matches the classic billboard (0.5).
The three shared meshes occupy about 1.17% of the vertex arena,
independently of instance count. This is an asset budget, not an FPS measurement.

Within OSM `landuse=forest` or `natural=wood` polygons explicitly identifying
conifers, **every tree uses this pine**: area planting, surveyed points/rows
and measured canopy fill. The lookup uses each tree's position, not the whole
tile; the forest rule also overrides contradictory individual tree identities
inside the polygon, as requested. Urban cherry percentages cannot enter these
forests. `leaf_cycle=evergreen` or `leaf_type=mixed` alone do not imply conifers.
Unspecified boreal/Alpine forest inference also uses the adult pine, rather
than mixing the former fir and pine saplings. Observed heights always win;
otherwise adult pines use 18 m (22 m in forests), with the existing ±20% variation.
An untyped forest elsewhere is not automatically classified as coniferous.

The Atlas fills missing botanical identities using sourced Atlas
`treeAreaObservations`, limited to existing OSM woodland contours. Brural's
park has no botanical tags, and its two woods (ways 1085310430 and 1085310431)
only say `natural=wood`. The user's 2026-10-16 pine observation supplies
`genus=Pinus` for those two areas; it neither extends across the whole park nor
overrides an explicit botanical identity on a woodland.

Other trees include the project's photoscanned Poly Haven species (`CLAUDE.md` §1; a
test refuses any kit tree). A full scan does not fit (`island_tree_01` is
1 303 928 vertices; at a streamable 12 000 only 0.43% of the canopy survives),
and the scene loader parses a referenced file once per node. So trees are
planted as impostors: `tools/bake_nature.py` bakes each species into layered
cards (`assets/models/external/nature_cards/`) keeping its scanned albedo and
alpha, and a city tree carries a near model (`nature_selected/urban_tree.glb`)
switched by `LODGroupBehaviour`. Urban trees keep Near within 15 m (10%
hysteresis), Mid until screen coverage drops below 0.09, then a camera-facing
Far made from that same Mid: roughly 80 m rather than 250 m for a 9 m tree at
60° vertical FOV. Transitions retain the existing 0.5 s screen-door fade.
`tools/r1/tree_impostor.py` bakes 8 azimuths × 5 elevations at 256² per view,
supersampled twice, into shared albedo/alpha and normal/visibility atlases and a
4-vertex quad. The albedo is unchanged; mapped normals come from the same Mid.
32 equal-area directions measure cosine-weighted hemisphere visibility against
the source's alpha-tested geometry at 512². This static occlusion preserves
canopy cavities and contrast without baking a particular sun direction.
Adjacent azimuths blend; elevation selects the nearest view. No sun is baked.
Saida shades this Far with the same flat ambient fill as Mid, occluded diffuse
sky SH and live Lambert directional light using normals in each instance's
authored frame. Static hemisphere visibility attenuates sky SH, but never the
flat ambient fallback or the sun. The game's sun cycle supplies diffuse sky
light via ambient with diffuse IBL off; applying atlas visibility to that fill
made shaded Far trees almost black. Diffuse sky SH follows the scene's IBL
settings, rotations and sky crossfade, with no extra texture reads. It performs
two albedo and two normal/visibility reads, and skips MR maps, full PBR,
specular IBL, GI and runtime shadow
sampling. Billboards without a normal atlas keep the former broad volume
lighting and its two-read cost.
Far does not cast shadows; Near/Mid and trunk collisions are retained.
Other species keep their existing cards.

The shared urban Far prototype uses the engine's existing normal/volume
lighting blend at 0.5. Filtered atlas normals cannot retain the distribution
of leaf orientations within a canopy pixel: a strict surface Lambert response
can leave an entire backlit crown dark. Half the volume response keeps this
representation lit while the atlas normals retain contrast. This is set once
on the shared material in code; source textures, geometry and albedo stay intact.

Surveyed OSM trees and rows are planted first,
then mapped woods are filled within the budget.

### Measured canopy

`gen/canopy.*` is a world layer any system may ask (`storedCanopy`,
`Canopy::classAt`, `Canopy::heightAt`): per tile, a 48 × 48 grid (about 12 m a
cell) of trees (3 m or taller over at least 5 % of the cell) or no tree, and
the median tree height of each 8 × 8 block.

The source is Meta and WRI's High Resolution Canopy Height Maps (1 m, global,
CC BY 4.0, imagery 2009–2020): zoom-9 Web Mercator BigTIFFs on AWS.
`fetchCanopyBand` reads the header, the row offsets of one tile row and those
rows (a few MB), and converts the hundred-odd tiles that band covers. The raw
map is never written. The grid is coded by an adaptive binary range coder with
a JBIG-like 10-cell context: a dense tree-cover tile is about 200 bytes, sea
2 bytes, about **7 GB for all the land of the planet**, in one file per square
degree (`cache/world/canopy/<lat>_<lon>.r1c`). The game fetches a missing band
in the background and cooks again when it lands.

With the canopy, an inferred tree survives only in a tree cell, and every empty
tree cell gets a tree at the measured height (trunk clear of roads and roofs);
OSM trees keep their place. The budget is 640 trees per tile with canopy data
and 320 without it. Spare capacity adds clusters instead of isolated miniature
trees. The manifest's `nature.canopy` counts
cells, trees placed and inferred trees removed. Trunk colliders are created
when the tile mounts.

## Photographed low vegetation

`native/gen/understory.cpp` adds shared scene instances, independently of the
existing photographed trees. Poly Haven / Rico Cilliers supplies four shrub
forms, nettles, fern and meadow grass under CC0; an eighth variant retains the
grass scan's veins and coverage with a senescent straw recolour. Download URLs,
MD5/SHA-256 and authors are pinned in `tools/r1/understory_sources.json` and the
runtime assets carry individual `.source.json` provenance.

Each clump assembles one to five individual photographed plants. The bake keeps
22 directional depth slabs with separate front/back views and overhead layers,
source albedo, leaf normals and 16-direction hemisphere visibility. It contains
**176 triangles Near, 44 Far**, with one material/draw per clump. The two levels
share external atlases (1152 x 768), including edge padding and alpha masks.
There are 15 shared RGBA8 atlases, approximately 67.5 MiB including full mip
chains; dry grass reuses the fresh grass normal atlas. Full scanned source
geometry is an authoring input, never a runtime dependency.

The tile budget is 512 clumps in addition to the tree budget. Mapped hedges
take priority, then two thirds of the remaining budget are reserved for verges
and eligible detached-home massifs; mapped scrub and woodland margins fill the
remainder. Road verges have a short maintained shoulder, herbs/grass in front and
irregular woody cover behind. Inferred rural shrubs are 0.65–1.35 m, motorway
shrubs 1.25–2.25 m; explicit hedge height/width win. All roads, crossing paths,
buildings, driveways, water, beach sand and airport surfaces veto overlap.
Bridges/tunnels and paved urban sidewalks do not acquire inferred wild verges;
polar/arid climates retain surveyed hedges but no invented temperate scrub.
Water and beach sand are rejected before allocating instances. Separate paths
retain their clearance and shift the fringe toward the field. Detailed plants
thin the procedural grass directly underneath, preserving its ground colour.
Garden priors follow the existing country/region detached-home rules, preserve
the entrance and are silenced by surveyed plot boundaries.

The generic engine material supplies mapped foliage lighting, transmission,
root-anchored vertex wind and angular rejection of grazing slices. Instances
use 0.5 s screen-door LOD transitions, Near within 22 m in both presets and
visibility limits of 170 m standard / 110 m toaster. No shrub collider or
per-bush CPU animation is created. `python -m r1.understory_assets --download
--bake` (from `game/tools`, NumPy/Pillow/ModernGL installed for authoring)
rebuilds the assets; running the game requires none of those packages.

Automated captures can set `R1WORLD_SMOKE_NO_UNDERSTORY=1` to omit this layer
while retaining the same executable, observations, shaders and camera. This
benchmark hook is absent from saved settings and the game UI. Compare warm
profile tails with `tools/analyze_profile.py --tail 180 --skip-last 2`.

## Shops, interiors and fuel stations

### Shops

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
  the 2.5 m shelf) and up to twelve checkouts facing the gallery.
- **Parking** (`gen/retail.*`): mapped `amenity=parking` gets asphalt,
  synthesized 2.6 × 5 m bays, circulation and a painted pedestrian spine,
  draped on the terrain; buildings, roads, water and parks are subtracted.
  Ownership by a nearby shop is inferred and labelled. An inferred forecourt
  needs a mapped retail (or asphalt commercial) parcel. Underground,
  multistorey and private lots are excluded.
- **Parts**: a tile's shopfronts are one part per material (shell, frames,
  glazing reflections, one fascia per paint), rather than one group per shop.
  Each part requires a mesh upload, draw and static body, so grouping limits
  both render and physics overhead.

Glazing is frames and reflection strips: the renderer has no sorted transparent
pass. Shop walls use clean, warm pine cladding from the CC0 WoodSiding009 scan:
native 1024 × 512 colour, tangent-space normals and roughness, with a 2 × 1 m
repeat (eight horizontal boards). A stable choice from the shop identity alternates
light pine and dark brown finishes. Shops share the same three maps and batch
by finish. `python -m r1.retail_materials` rebuilds that finish, the plaster used
by interior floors and the fascia letter coverage; it uses local sources only.

### Interiors

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

Inland-water multipolygons join reversed shoreline members, preserve inner land rings and reject incomplete rings. Water geometry is clipped to each resident tile, and inferred vegetation is excluded from the lake. Distant water beyond resident tiles still uses the coarse relief.

### Coastal beaches

The coastal apron follows OSM coastlines, with a fallback from the offline sea
outline that ignores tile clipping edges. Unmapped coast gets a narrow sand
apron; mapped beach/sand/dune polygons retain their wider extent. A gentle
shore profile and low dunes blend back into surveyed relief. Mapped roads,
paths and buildings keep their ground. Roads and paths split the sand region:
only components connected to the waterline remain beaches, and landward
components lose the beach material and dune inference. Perpendicular beach
accesses preserve the shore on both sides. Short inferred access trails connect
to real paths/roads only through clear corridors.

Ports take priority over beach inference: marinas, harbour/dock polygons and
port land use, plus buffered quays, piers, breakwaters and harbour points,
exclude sand, dune grading and visitors. Harbour geometry and berth generation
continue through the existing port pipeline. The manifest reports sand area,
port protection, excluded landward area and access area as inferred geometry.

A single shaped WaterNode per coastal tile carries signed vertex depths.
Its swash follows the curved waterline; wet sand darkens gradually, foam breaks
into moving patches and the offshore wave spectrum matches sea tiles. This is
visual surf, with no fluid simulation or tide model. Geometry is cooked once;
there is no per-frame shoreline search in the fragment shader.

Beach attendance uses the live measured temperature, clouds, rain, snow, wind,
local solar hour and sun elevation. It grows smoothly between 18 and 30 C,
peaks during daytime and declines with clouds/wind. Below 18 C, during rain,
snow, thunderstorms, strong wind, night, or unknown temperature/weather, all
beach visitors and their equipment are disabled. A clear-sky override preserves
the measured temperature; it cannot turn an 8 C day into a beach day.

Stable spots carry shared towel/umbrella meshes and existing human rigs/LODs.
Visitors rest, rise, walk to the water, bathe, return and lie down again on
staggered cycles. The cap is 24 live beach visitors within 100 m of the camera,
12 per tile, sharing the existing 60-person crowd budget; only one new visitor
is created per frame and distant poses update at lower frequency. Beach spots
and attendance are inferred, not observations of real people. Separate deck
chairs and dedicated swimsuit assets remain outside this delivery.

### Road grading

Roads on the ground are graded into the relief (`native/gen/grading.*`): the
ground under each road is averaged along it over a Gaussian window (12 m for a
service road to 45 m for a motorway), continued through junctions along the
straightest way on, so a real climb stays and a bump goes; knots above the
road's mean count less, so it cuts into a bump. Cut and fill are capped per
class (8/4 m for expressways down to 2.5/1.5 m for service roads). The
carriageway and 0.85 of a terrain cell beside it take the road's level; a
smooth bank, three times as wide as it is high (10-30 m), meets the survey.
Bridges, embankments, tunnels, digs under bridges, footpaths and sea-level
ground are left alone. Two tiles do not read the same roads near their seam,
so grading fades in over 30 m from every tile edge and the seam keeps the
joined survey.

Where a road stands at its own level (an embankment or a low deck from
`gen/bridges`) the drawn terrain is cut out under it wherever the ground comes
within 0.6 m, each triangle keeping its plane, and a rock-coloured cutting face
joins the road's edge to higher ground beside it.

The manifest's `roadGrading` gives the nodes moved, deepest cut, highest fill,
the ride along the tile's roads before and after (`rollPer10m`, `acrossSlope`)
and the cuttings. Measured on Paris centre: across slope 1.7 % -> 0.5 %,
roll 0.12 -> 0.06 m per 10 m. `r1cook` also prints `groundEdges` to compare a
seam's two sides.

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

**Flight handling.** A building stops an aircraft where it touches it, and the
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
wind, cloud cover, precipitation, rain, showers, snowfall and snow depth. Snow falls around
the camera with the measured wind, and drifts along the ground above 6 m/s.
From 3 cm of measured snow, ground and roofs of resident tiles wear the
photographed snow (streets stay clear). Cloud cover softens the Sun and blends
in an overcast sky; precipitation thickens the haze. A forecast older than two
hours is unavailable, and nothing waits for weather.

Liquid rain activates Saida's Rain particle preset around the camera, with a
bounded budget and the measured wind. An upward physics query suppresses the
emitter when the camera is beneath a roof. Rain plus showers is converted from
the forecast's reported aggregation interval (`precipitationIntervalSeconds`,
normally 900 seconds for current conditions) to mm/h; legacy cached totals are
used only outside snow conditions (`native/weather.hpp`). Explicit zero liquid
rain remains authoritative. The local weather check wraps the date line, and
teleports reset the weather pointer and accumulated wetness.

The engine's generic `rain_ripples.glsl` is enabled on carriageways, cobbles,
sidewalks, kerbs, markings, bridge concrete, made ground and parking surfaces.
It adds animated expanding impacts, wet roughness and irregular puddle patches
under the existing PBR lighting. Interior floors, vegetation and facades are
not opted in. Wetness rises over eight seconds and dries over two minutes after
rain stops; impacts stop with rainfall. Water also receives the engine's
impact shading. These are visual water films, not simulated water accumulation.

Rain raises the cloudy sky blend and activates the engine's procedural rain
bases, cumuliform towers and anvils over the existing HDR skies. The cloud
overlay is lit from those images and stays dark at night. Droplet simulation,
world-space impact placement and cloud directions are shared by stereo eyes;
native multiview and Web shader variants use the same equations. This does not
claim a headset validation or a new reference-machine frame-rate measurement.

## Sun and sky

`native/world/environment.cpp` calls `setObserver(lon, lat, altitude)` on
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

**Options → Forcer l'heure à** holds the whole world at one hour of the local
day: 14:00 is 14:00 on the HUD clock wherever the player goes (the place's
zone, or the longitude estimate). The game turns that hour into an instant on
the place's current day (`native/forced_time.hpp`) and hands it to
`setForcedTime`; the Sun, the sky, the crowd and the ships all read that one
clock, and time stops flowing until **Heure réelle** hands `null` back. A
capture's `--at` still wins. The choice is kept in `cache/options.json`, which
smoke tests and captures neither read nor write; a bad hour is refused on
screen and in `game.log`.

**Options → Météo dans le monde** lets the player choose **Dégagé**,
**Petite pluie** or **Forte pluie / orage** everywhere, including after a
change of destination. **Météo réelle** restores each place's local weather.
This choice is independent of the forced hour; the selected button and the
weather status show which mode is active. It is saved alongside the hour in
`cache/options.json` and works even without a local forecast. Smoke tests and
captures keep their own atmosphere and never read or overwrite these choices.

**The sky** is eleven Qwantani pure-sky HDRIs (Poly Haven, CC0), one sky
photographed from before dawn to after dusk (`r1/skies.py`):

- each photograph is placed by the Sun's elevation at its instant (within 0.7°),
  and the runtime crossfades the two the real Sun stands between;
- each is turned so its Sun sits at the real Sun's azimuth;
- brightness comes from the model: the 0–4° band of each is scaled to the
  model's horizon brightness;
- the renderer matches the lower sky's RGB to the fog in linear HDR after
  clouds, fully below 1° and fading out by 6°; finite terrain fades before its
  far clip. The separate Sun disc stays visible through this photographic
  horizon correction;
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

### Installed relief

`native/gen/relief.*` stores a grid per square degree: 400 intervals north/south
(about 278 m), longitude intervals reduced with latitude, metre heights from
−500 to 9 000. A median edge predictor and the shared binary range coder keep
the grid compact and lossless after source sampling/rounding. One indexed
`.r1relief` file holds a complete 10° × 10° block; absent cells within a valid
pack mean sea. There are 648 packs between 85° south and 85° north. The reader
keeps at most 64 decoded cells (about 20 MB at the equator), with shared access
protected across the world's workers.

Latitude edges share the northern cell's curve so grids of different widths
join continuously. The complete layer is about 859 MB. Missing packs are
unavailable, never interpreted as sea; diagnostics name the missing layer.
The package requires all 648 packs; --partial-relief is an explicit test override.
Build/resume options are in [DEVELOPMENT.md](DEVELOPMENT.md#distribution).
Web/console delivery still needs asynchronous reads through the engine asset
system instead of preloading the whole layer into MEMFS.

## Road vehicles and traffic

### The car

In solo, a car is parked beside the arrival: three metres to the player's right, then
a golden-angle spiral out to 14 m for ground that is loaded, dry and not inside
a building. It belongs to `earth.scene`, not to a tile, so a teleport does not
leave it behind. Each refusal (no kerb within 14 m, no room to step out, too
fast, edge of the loaded world, an obstacle) writes a log line and a line the
player can read.

`native/vehicle_runtime.hpp` installs the engine's `SurfaceDrive` controller,
three raised chassis collision volumes, four rounded wheel contacts and
`DeformableMesh` on each car. The two motorcycles instead use one narrow
chassis and two inline wheel contacts, with lower mass, tighter steering,
quicker response, less reverse speed and a freer lean response. They use light
arcade handling distinct from cars; the cross bike gets more grip and less top
speed than the city bike. Driving uses
forces on a Jolt body, with simplified ground adhesion and no suspension
requirement. Profiles for all nine models specify mass, acceleration, braking,
grip and damage energy as game approximations, not manufacturer measurements.
Ground support samples the centre and both axles. Powered slope assistance
preserves responsive arcade acceleration uphill; releasing the accelerator
restores the full grade load. Drive force stays available below the top-speed
band in the requested direction, including torque opposing rollback. Braking
counteracts gravity along the slope. The body has no separate linear damping:
the controller alone applies rolling and aerodynamic resistance. Acceleration,
braking and grip profiles favour keyboard-friendly action driving, while the
original model-specific speed limits remain in place.
Small upward wheel contacts are excluded from body damage through the engine's
contact policy; severe landings and chassis collisions remain eligible.
Contacts determine crash motion. Cosmetic energy below half the profile's
damage budget leaves power intact; further body damage progressively reduces
power by at most 35%, rather than disabling the engine through accumulated
bodywork energy. This is a gameplay penalty, not simulated engine failure.
Localized mesh dents affect grip and
front-chassis asymmetry biases steering. Wheels/glazing are excluded from the
cage. Near/Far geometry retains permanent damage while shared source assets stay
intact. The deformation model has bounded plastic offsets, not tearing or full
structural crash simulation.

The mouse turns the head and eases back once the car rolls. Stepping out is
allowed at any speed; taking a moving traffic car preserves solver momentum.
Water ejects the driver and the car sinks. A terrain guard brakes before an
unloaded region. Physics determines position, including the geographic position
used for streaming/rebasing, instead of translating the car along a 2D path.
At rest the terrain guard checks the requested driving direction, and either
direction input first brakes opposing motion before engaging the requested gear.
Holding a direction while overturned progressively rights the car on nearby
ground. F also works on the roof or in mid-air: exit points account for the
rotated chassis, with an above-chassis fallback when its sides are blocked.
The abandoned body keeps its velocity and brakes through the solver; an airborne
character falls from the exit height. The chase camera follows the completed
physics pose before rendering, avoiding the previous one-frame camera/body lag.
Set `R1WORLD_SMOKE_VEHICLE_DAMAGE=1` with `--smoke` to exercise driving/braking
with an actual deformed car mesh and collider in the streamed world.

### Motorcycles and local radio

`moto` and `rocket` now create empty bikes. Only traffic instances receive a
crowd rider. Taking one retires its traffic slot and removes that rider permanently;
stepping off leaves an empty saddle. Mounting reparents the real player body, with
its original rig, weapon and modifiers, directly below the physics body. It follows
the completed solver pose without a second avatar or a frame of geographic lag.
Teleport and sinking return that same body to the pedestrian hierarchy.

Both motorcycles are authored facing +Z and rendered/controlled facing -Z. Human
rigs are rotated to match; left/right arm targets no longer cross. Hand and foot
IK targets are measured at the bars and pegs in the model's frame, then transformed
into each rider's rig space. Traffic riders use a stable 80/20 male/female draw,
independent of the number of available outfits. Two female variants are preloaded
alongside the existing male selection, rather than loading the whole wardrobe.
Existing crowd models and helmets are reused.

Both palms contact the rubber grips when driving, with closed fingers. The
chair source is held at a stable seated frame; its long fidgets cannot pull a
shoulder out of bar reach. Wrist rotations account for the palm offset,
shorter rigs sit closer to the tank, and the head looks ahead
without rotating Rocketbox's shoulder-bearing Neck bone. During pistol aim,
the chest turns only as far as the supporting arm can reach; the left palm
remains on its grip. The right arm follows the camera and clears the shoulder
for rearward shots. The pistol is stowed again when aiming ends aboard a bike.

Handling now has quicker acceleration, progressive steering response, steering lock
that decreases with speed, supported-turn banking, road-aligned damping, effective
braking and a walking-speed reverse. City/cross masses include a rider. The engine's
same force controller and Jolt contacts still own the motion; this is accessible
arcade handling, not a complete tyre or motorcycle suspension simulation. With the
pistol selected (`2`), hold right mouse to aim and press left mouse to fire; `E`
reloads. The existing ammo, effects, damage and muzzle obstruction rules are shared.
Shots ignore the ridden bike's collider; the left hand stays on the handlebar.

The pinned source GLBs are rebuilt by `r1/motorcycles_blender.py`, then finalised by
`r1/motorcycle_detail.py`. Smooth curved surfaces, source textures and mechanical
silhouettes are retained; small fasteners are reduced before the main forms. Near
city geometry falls from 17,724 to **8,564 triangles**, cross from 10,192 to **6,498**;
Far uses **1,798 / 1,797**. Embedded cross textures are bounded to 512 pixels Near
and 128 Far, instead of nine 2K images in each LOD. Combined decoded base images
fall from about 288 MiB to 9.6 MiB before mipmaps, preserving the loader's existing
budget. The game now configures a bounded **512 MiB CPU decoded-asset cache**
through the engine's existing API: the 256 MiB engine default rejected existing
scanned vegetation while measured process use was only 1.4–3 GiB. This is separate
from VRAM and the unchanged 1,048,576-vertex geometry arena. Material roles and wheel
pivots remain explicit, and the fleet metadata
counts every exported vertex seam.

In cars and motorcycles, `B` enables radio, `N` / `Shift+N` changes stations and
Page Up/Down changes volume. The HUD shows the station, distance, volume and
connection/error state. `gen/radio.*` queries the public
[Radio Browser API](https://api.radio-browser.info/) through the engine's pollable
HTTP service, without requiring Web threads. It discovers API
mirrors, requests real geolocated stations in an expanding radius and ranks them
locally by great-circle distance, rejecting broken, unlocated and unsupported rows
and duplicate names/streams. Local entries come first; when the settlement has none,
the nearest listed station is selected, without restricting the search to a country.
Locations are the directory's coordinates, not a claim of municipal broadcast coverage.

Station metadata is cached in `cache/radio/`, reused offline and refreshed after a
week when the service is reachable. Live programmes are streamed only while radio
is enabled aboard a road vehicle, stop on exit/teleport and resume on re-entry.
Failure/buffering times out to the next station, with a bounded pass through the list.
The engine includes native libcurl/Mbed TLS, CA certificates and MP3 decoding in
the player, with a WebAudio/HTML media implementation for browser exports. The
catalogue selects MP3 stations so playback uses the same codec baseline across
exports. Native live playback, real browser sound and an Android ARM64 shared
library are verified. Browser CORS/HTTPS restrictions can make particular stations
unavailable; the normal station fallback applies. A browser awaiting permission
to start sound displays “Toucher pour écouter” and resumes on the next interaction.
Geographic caching remains independent; an offline cache cannot supply a live
programme. Full mobile/Quest export and physical-device validation remain engine
platform work; the radio service no longer depends on Windows APIs.

### The fleet

`r1/vehicle_imports.py` converts nine licensed source vehicles to GLB — compact,
estate, SUV, off-roader, sports car, lorry, bus, police and gendarmerie — and preserves two sourced
motorcycles in the shared fleet manifest. The city bike and motocross bike are
CC0 models from Innerscene, stored with pinned SHA-256 hashes in
`data/source-assets/vehicles/motorcycles/sources.json`. Both have independent
wheel pivots and Near/Far LODs. The near car models retain the source
geometry and normals **without decimation**; only distance models are reduced.
The source ZIPs and SHA-256 hashes are pinned in `data/source-assets/vehicles`,
so rebuilding the car models needs Blender 4.2, not a live asset server. The
source motorcycle GLBs and their download links are pinned alongside them.

### The traffic

The simulation is [`engine/plugins/traffic`](../engine/plugins/traffic/README.md),
a header-only add-on that knows nothing about R1World (lane graph, car
following, give way, spawning around an observer), compiled into the game's
binary. Its tests run alone:
`g++ -std=c++20 -O2 -Wall -I../include test_traffic.cpp` in
`plugins/traffic/tests`.

| Here | There |
| --- | --- |
| `native/gen/traffic_graph.cpp` (`buildLaneGraph`) turns the tile's OSM ways into a lane graph | `Graph` drives on it |
| The tile's density decides how many vehicles | `Flow::setPopulation` spends it |
| Which side of the road this place drives on | `Rules::leftHand` mirrors the lane |
| A scene node per agent, painted, on the terrain | `Flow::pose` says where |
| The player's car or motorcycle, as an obstacle | agents queue behind it |

- **A graph per tile**, cooked with the street from the same ways, with the
  tile's lifetime. Tunnels and `service` ways carry no traffic; bridges are
  driven at their solved level. Speeds are `maxspeed` where tagged (328 against
  32 estimates on the rue de Rivoli tile), and the tile says which.
- **How busy**: a vehicle per 28 buildings, capped by road length (22 for a Paris
  tile, two or three for a village). Lanes are weighted by road class and
  spawning follows weight × length, so the cars are on the avenue you are
  looking at. Eighty vehicles per neighbourhood, nearest tile first. Motorcycles
  make up 5% of traffic by default and 50% across Africa, in India, Central
  America, South America and the Caribbean; stable agent seeds make the mix
  repeatable when a tile is visited again.
- **Ten paints**, listed as albedos under `carPaints` in `atlas.json` (0.05 to
  0.30), one material each through the engine's cache.
- **F takes any car or motorcycle.** The traffic node leaves the flow (`Flow::retire`) and
  joins the world; the vehicle you left stays where you left it. Six abandoned vehicles
  are kept; a teleport clears them.

Generator v47 / traffic schema 3 preserves short road connectors, directional lane counts, offsets,
road identity, turn masks and layer separation. Speed tags accept km/h and mph.
Observed traffic-signal, stop and give-way nodes attach to nearby lanes; their
assignment and signal timing are explicitly inferred. Static via-node `no_` and
`only_` restrictions are read; conditional, exception and via-way restrictions
are unsupported. The old OSM query cache is refreshed under query version 15.

Signal observations attach to the incoming junction approach rather than every
nearby shape segment. Repeated observations are deduplicated; adjacent junction
nodes linked within their carriageway mouths share a program, and internal
connectors have no second signal. Posts share one assembly per program and
approach group, use the outermost incoming stop position and belong to one
streamed tile. A roadside farther than 10 m from the lane is refused explicitly.
All incoming approaches of an observed controlled junction share its program,
including heads not individually tagged in OSM; these assignments remain inferred.
Each controlled lane carries a stop setback and `stopPosition`, inferred from
the surveyed control, intersecting carriageway widths and observed crossings.
Setbacks trace preceding lane segments; `stopDirection` and `stopWidth` keep
the pole and painted line aligned with the actual road through short bends.
The same position places the pole, drives braking and paints a 40 cm stop line
across the lane, clipped to actual asphalt/cobbles and draped above the surface.
The producer records `stopLineDrawn`; a post without drawable road marking is
omitted explicitly rather than shown without its line.
Observed zebra crossings remain separate; the generator does not invent them.
`r1cook --glb` also exports `.traffic.json` for auditing controls and positions.

Drivers anticipate controls and queues across short segments and bends, and clear a junction once
committed on green or an amber too close to stop. At road ends they brake;
offscreen completed physical trips retire instead of becoming permanent queue
leaders. A region transfer still keeps its live node and identity. The fleet
supplies actual dimensions before spawn clearance through `Flow::setSpawnSize`;
a recycled slot uses the model matching its new trip's dimensions. Long buses
therefore cannot appear using a compact car's clearance. A stable `vehicleSeed`
keeps model selection aligned with those dimensions while the routing seed changes.
Birth clearance uses the resolved curve pose and excludes junction mouths and
stop-line areas; short straight shape segments remain driveable and spawnable.
Only a queue's eligible head competes for junction priority.
Region handoffs follow the current road's exit, so an early-adopted body is not
returned to its old region before its centre crosses the boundary. Neighbouring
regions supply live vehicle footprints to each other while the body crosses;
changing its owner does not make it invisible to the upstream queue.
`R1WORLD_TRAFFIC_VALIDATE=1` reports individual waiting vehicles and fails if a
physical car within 60 m of the observer stays below 0.1 m/s for over 60 seconds.
`R1WORLD_TRAFFIC_SECONDS` extends the test duration (60 to 300 seconds); the
smoke timeout includes loading and frame-rate overhead.

Signal meshes share a cylindrical galvanized pole, mounting brackets, a rounded
black housing, three domed circular lenses under visors and a lower repeater.
Heads face their incoming approach; parallel lanes share the outermost post.
The generator retains the asphalt/cobble contours used by the rendered street.
Posts are placed outside those contours with 45 cm perpendicular clearance,
including rounded junction mouths and median islands. Placement runs once at
tile activation; an unavailable safe roadside omits the post with a diagnostic.
Visible lamp heads use the same phase as the flow. Opposing approaches share
green; perpendicular traffic waits. Green/amber/all-red phases
include a shared pedestrian phase; engaged pedestrians finish their crossing,
and cars treat them as obstacles. At unsignalled crossings pedestrians also
wait for a safe gap. Curved junction poses, lateral-speed limits, length-aware
following, adjacent-lane changes and a reused spatial sweep improve road following.
Intersection admission is conservative, without full country-specific traffic
law, route destinations or movement-conflict scheduling.

Remote cars follow the graph. Nearby cars are promoted to dynamic bodies with
their current pose/velocity, aligned to the road grade; AI supplies driving
commands across multiple short segments. Severely damaged cars brake
and retain physical ownership. Clean offscreen cars may return to graph control
beyond the hysteresis distance. Region handoff/replacement moves live vehicle
nodes with their identity and damage; an unmappable replacement vehicle remains
as a braked, reachable car. A teleport clears abandoned cars. This does not
provide unlimited world persistence for wrecks.

The engine implementation is built for native and Web players and shares geometry
between mono/multiview views. R1World remains a native Windows application; these
changes do not complete its Web port or qualify headset performance.

## People

The player and the crowd are scanned, rigged avatars from Microsoft's
**Rocketbox** library (MIT): nineteen in the crowd and one for the player.
`r1/humans.py` fetches what it needs at a pinned commit; `r1/humans_blender.py`
converts in Blender 4.2 (metres, feet on the ground, 26 face bones folded into
the head but the eyes kept) and retargets the motion-capture clips. The jump is
the flight phase of the sprint, held 0.74 s.

| | triangles | exported vertices | textures |
|---|---:|---:|---|
| Player | 7 364 | 4 584 | 1024 / 512 px |
| Player, AutoLOD levels 1-3 | 4 504 / 2 363 / 912 | 2 929 / 1 704 / 777 | same |
| Passer-by, near | 2 400 | 1 571–2 010 | 512 / 256 px |
| Passer-by, far | 500 | 432–617 | same |

The player has three AutoLOD reductions in the same GLB (`MSFT_lod`),
including the skin: `python -m r1.humans --lods`. Their coverage thresholds
are 0.05, 0.012 and 0.003; copies retain the chain through `MeshNode::duplicate`.
The runtime scale is 0.68; walk/run playback follows the 2.8/7 m/s movement.
On supported ground, floor sticking maintains contact without a forced downward
velocity. Unsupported feet still fall; jump remains separate. Crowd selection
is seeded per tile and slot, with country weights; these are gameplay defaults.
For distant online avatars, see Multiplayer below.

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

## Animals

The catalogue places 67 ecological profiles: 55 land-animal profiles and 12
bird profiles (14 bird models/variants), from `assets/animals/fauna.json`.
Bats, fish and sea animals are not placed yet. Each species names its
range as countries narrowed by boxes (wolves in the French Alps and the
Pyrenees' bears, never Paris or Brocéliande), its climates, its habitats, a
density per km² of optimal habitat, the share of it a walker can meet, its
group size, its hours (diurnal, nocturnal, crepuscular, cathemeral, pastoral),
its months (the temperate calendar flipped south of the equator, ignored in
the tropics), how it takes rain, and its home range.

- **Where** (`native/gen/fauna.cpp`, generator v60): each tile samples its
  ground on an 8 m grid -- forest, scrub, meadow, park, farmland, farmyard,
  orchard, wetland, beach, desert, savanna, rock, ice, urban, bare, open --
  from the ground class the terrain is drawn with, its photographed family and
  the land cover's own tags (a park is not a pasture), with buildings, the
  distance to water and the built share within 64 m. Wild animals leave as
  the built share grows, livestock keeps to fields, rats and cats need houses.
- **How many**: density × habitat × meetable share over the tile, drawn from
  the tile key, so every player meets the same residents; at most 10 groups of
  a species and 48 in all on a tile. The manifest's `fauna` entry is inferred
  and says so, with the expected individuals per species.
- **Their lives**: the engine's wildlife add-on (`engine/plugins/wildlife`).
  A group is at one point of its home range at a time (a roe deer's 250 m, a
  rabbit's 50 m), redrawn every 45 s while unseen, and appears when that
  point comes into view range and its hour, season and the weather send it
  out -- never inside its flight distance, never out of nowhere close in front.
  It grazes, wanders, keeps together and flees from a walker (further from a
  sprinter or a car); small animals dart off and are gone. Sixty-four animals at
  most around the player, nearest tiles first.
  Since 27 October 2026, appearance ranges extend to about 250 m for roe deer
  and 322 m for red deer (previously 100 m and 129 m). The size-based range
  also allows at least three flight distances, within 24–450 m, so groups
  are present before a walker reaches them instead of appearing only in a
  narrow band just outside their flight distance.
- **Indoors**: a furnished home sometimes shelters a spider on a wall (about
  8 % of homes, more in autumn), a trail of ants in the kitchen (April to
  September), a mouse at night, a wasp in summer or a cat; drawn from the
  building and the day, never a crowd.
- **Drawing**: `tools/r1/animals.py` measures every placed model from its
  GLB: where its feet touch the ground, which way its head points (tail to
  head), which clip plays what, and each gait's speed from its planted feet
  (inferred from leg length by dynamic similarity where the feet do not tell,
  and said so). It retargets the 0 A.D. clips that carry Collada faults --
  a body drawn at another size than its rest (a fox at 2.5), a root turned or
  standing off its place -- onto each model's own skeleton, leaves every
  other clip as its author made it, and refuses clips that stay broken (a
  goat idle with a key 4 m off, a sheep walk whose feet push forwards). One pooled node per animal with its own animator over shared
  meshes, LOD0/LOD1, pose rate by distance and a kinematic hit box at the measured body size.
  The engine now draws a skinned mesh in its root bones' parent frame, as
  glTF requires (`GLTFLoader::skinnedPrimitiveFrame`): an armature scaled to
  size no longer doubles the animal.

### Birds

All 14 bird models/variants in `assets/animals/catalog.csv` now occur through
`fauna.json`: pigeon, robin, crow, gull, both hawk coats, bald eagle, eagle owl,
macaw, yellow-billed shrike, hen, rooster, peacock and turkey. This adds 12
profiles; no bird asset is replaced. Crow, gull and hawk are explicitly broad
visual proxies, not claims that the same exact species inhabits every listed
country. Other profiles use narrower country/climate envelopes; northern robin
and eagle ranges have seasonal occupancy. Density and all numerical flight
parameters are inferred game tuning, not measured population counts or aerodynamics.

Resident population seeds and identities are versioned independently of terrain,
so a geometry recook preserves saved death identities. Birds share
the local animal budget, per-tile slots, shared meshes and both LODs.

Each tile pools 48 animal slots, with 64 active animals around the player;
nearest groups receive the budget first.

`Species::bird` reads speed, acceleration, climb/turn limits, altitude band,
flight/rest durations, flap frequency, glide share, ground/perch preferences,
maximum wind and twelve monthly effort values from JSON. The host maps local
solar time to approximate sunrise/sunset for the latitude and month (including
polar day/night), reverses the seasonal effort calendar in the south, and
reduces activity in rain and strong wind. Seasonal absence uses the existing
range calendar. Present birds rest at night rather than being removed solely
because their flight activity falls; owls forage at night. This models local
seasonal occupancy, not individual transcontinental migration routes.

Saida's optional wildlife add-on supplies continuous takeoff, cruise, approach
and perch states with bounded acceleration/climb, banked turns, loose group
cohesion, separation, wind drift and flap/glide spells. Songbirds make short,
low flights; raptors spend longer aloft and glide more; turkey and peacock
mostly forage on the ground and make short flights. Hens and roosters keep
their source ground locomotion: their delivered rigs do not have independently
usable wing joints, so full winged flight is not claimed for them.

Perches come from actual upward-facing opaque bark triangles of mounted tree
models, excluding cutout foliage and distant cards. Branch candidates are
cached per shared geometry and sampled across heights, then transformed with
the real tree. Birds reserve supports within their species' height band,
brake on approach and attach at the support elevation; absent support triggers
takeoff. A tree with no suitable bark surface contributes no invented perch.
Building footprints/roof heights and real trunk positions constrain flight;
leaf volume is permeable. A safe ground landing is the fallback when no branch
fits. The algorithm does not simulate talon contacts or deforming branches.

`native/bird_animation.hpp` authors procedural wing, tail, head and leg poses
on the delivered skeletons through Saida's existing `PoseModifier` interface.
Original fly/glide clips remain in use for the legacy birds; resting blends
back to a folded pose. The control-only rigs receive locomotion poses rather
than playing diagnostic clips as flight. Geometry, weights, UVs, materials and
source textures are preserved exactly. Their simple folded-wing rigs remain
a content limitation: feather deployment and close-up anatomy need visual
review and possibly targeted rig/weight work; these are not motion-captured
production flight animations. Procedural modifiers run continuously; the
existing clip pose cadence is not a claim of equivalent procedural CPU savings.

Flying deaths inherit horizontal and vertical motion, stop live wing control
and enter the persistent ragdoll/save system. The physics fixture now covers
133 used models, including birds falling from 12 m: 129 articulated profiles,
automatic rest, exact saved-pose restore, repeated blasts and origin rebasing.
`[World birds]` reports alive/flying/perched birds and available bark supports.

### Persistent animal remains

Pistol hits are lethal to animals, including small and indoor residents.
The displayed pose and current movement pass directly to Saida's ragdoll;
bullet momentum is limited per struck part, so a bullet collapses an animal
instead of launching it like an explosion. `native/animal_physics.hpp` fits
the shipped skeleton families with torso, neck and limb capsules and joint
limits. Mass is inferred from measured bounds and species scale, rather than
claimed as measured anatomy. The four insect rigs retain their authored limb
pose on a compact physical body; 129 of the 133 resident model variants have
articulated profiles. No animal mesh or texture is replaced; source-clip frame repairs are documented above.

Rockets apply an outward and upward blast with distance falloff and bounded,
mass-dependent velocity; off-axis rotation starts the tumble. Direct hits and
exposed animals inside the 14 m blast are killed and charred. Cover queries
are resolved before creating the victims' ragdolls, so a new corpse cannot
shield the next animal from the same explosion. Carbonized material variants
retain texture detail, normal maps and alpha cutouts on both LODs; shared
materials on living animals remain unchanged. A later bullet or rocket can
move an existing corpse again.

Bodies belong to independent geographic anchors outside living population slots
and room/tile ownership. Deaths and poses persist indefinitely; at most 32
bodies mount within 2 km, with at most eight articulated simulations. Distant
bodies are snapshotted and unmounted, including their colliders, and restored
on return. At the simulation cap an older tumble freezes at its current pose.
Settled bodies retain a compound static collider until another weapon impact.
The physics budget includes these bodies and up to 64 joints per live animal.
Floating-origin rebases carry the mounted bodies and colliders.

`cache/fauna-remains/<resident-sha256>.json` stores independent records with
stable resident/member ID, model, charred state, geographic frame, transforms,
bone names, pose and pending motion. Startup retains a geographic metadata
index and death IDs; historical skeleton poses stay on disk. Death IDs prevent
wildlife respawns after tile resets. Legacy `cache/fauna-remains.json` is read
and migrated, with the original retained as a recovery copy. Existing per-body
records win when a migration is resumed. Invalid saves block writes and are
reported; player saves are never quarantined or deleted automatically.

Snapshots of mounted bodies are checked at most twice per second. A joined
worker serializes and atomically replaces only changed records, coalescing
pending changes per body in a 64-record queue. Unmount snapshots are enqueued
before scene teardown; a full or failed queue retains the body and reports a
refusal when no new slot can be made. Shutdown drains accepted snapshots. A
crash can lose changes since the last completed write. Metadata and disk usage
still grow with history; scene nodes, poses in memory, colliders and queued
writes have explicit limits. Smoke saves use isolated generated directories.

Limits, from the assets: cats, rats, lizards, frogs, the gorilla and the pond
turtle have no gait clip and stand; sheep have no usable walk; gazelles graze
where they stand (the 0 A.D. deer clips they carry were made on another
skeleton and fold their legs, so only the standing ones are played); the
indoor spider is a wasp spider standing in for house spiders; roe, fallow and
white-tailed deer and the chital are the CC0 hind at their size, without
their own coats. 0 A.D. animals are CC BY-SA
3.0 (Wildfire Games), credited in `assets/animals/ATTRIBUTION.md`.

## Combat

Fists, a pistol and a rocket launcher, in the spirit of GTA III. **Click**
punches or fires, **right click** aims over the right shoulder, **1 / 2 / 3**
select fists, pistol and rocket launcher, **E** reloads (an empty magazine
reloads by itself).

### Rockets

- **Launcher model:** a shared 32-sided olive tube with closed bevelled rims,
  an interior bore, flared exhaust, metal fittings, rubber shoulder pad and
  grips, trigger guard, optical sight and identification bands. Separate PBR
  finishes distinguish painted tube, machined metal, rubber and glass. The
  player holds it at the shoulder, with both palm contacts solved from the rig;
  downward aim keeps the support hand within reach. The muzzle position is
  shared by the model and the projectile launch code.
- **On foot:** select **3**, aim with the mouse/right click and fire with left
  click. A 1.5 m rocket flies at 85 m/s; one loaded rocket and twelve in reserve,
  independent of pistol ammunition. Reloading takes 2.4 seconds. Arrest
  confiscates both ammunition stocks. The existing `rocket` cheat still spawns
  the motocross bike; `buzzoff` spawns a helicopter.
- **Helicopter:** taking the controls selects the launcher. Left click fires
  from beside the belly toward the camera crosshair; **3** selects it again and
  **E** reloads. The rocket ignores the aircraft that launched it. Aircraft
  hulls now use Saida kinematic collision bodies, including police air support.
- **Impact:** continuous segment ray queries prevent fast rockets skipping thin
  walls. A hit produces one explosion, with damage falling from 180 at its centre
  to zero at 14 m; physical cover blocks blast damage. Direct vehicle hits and
  exposed vehicles within 6 m are destroyed. Occupants of the player's car or
  aircraft are ejected before ownership and collision are removed, taking 55
  damage and a knockdown. Invincibility preserves health, but still ejects.
- **SaidaFX:** pooled GPU particle systems layer an expanding fireball, rising
  smoke, streaking sparks and an outward dust ring. A brief light pulse,
  distance-scaled camera shake (disabled in VR), exhaust trail and two original
  synthesized sounds complete the effect. These use supported SaidaFX nodes;
  the dust ring is particles, not screen-space distortion. Twelve projectiles
  and six explosion groups bound the effect budget. Rockets expire harmlessly
  after six seconds; geographic travel and pursuit reset clear active rockets.
- **Police:** below 100% alert, patrols, officers and helicopters start with
  normal gunfire. Rockets become an option from 60% only after twelve seconds
  of unsuccessful gunfire; their share then rises over ninety seconds, weighted
  by the fourth power of the wanted fraction. A bullet hitting the player resets
  that pressure; losing contact reduces it. At **100% alert, every attack uses
  rockets**, including against a player on foot. Weapon choices persist through
  the aiming delay and never reroll each frame. Launch requires
  continuous sight for 1.4 s, at least 24 m separation, a clear predicted path
  and no friendly unit inside the predicted blast. Reloads take 7 s on the ground
  and 5.5 s in the air. Losing sight or changing target vehicle resets the lock.
  Launches across all units are spaced by at least 3 s (1.5 s at 100%) to prevent
  simultaneous salvos. Destroyed air support waits 20 s before replacement.
- **Motorcycle exposure:** a police bullet hitting the compact motorcycle
  chassis also hits its exposed rider for 12 health. A knockdown detaches the
  rider before activating the ragdoll and carries over the motorcycle's velocity.
  Closed cars retain their bodywork protection; the invincibility cheat still
  protects health intentionally.

## Police

The game owns dispatch, offences and pursuit (`native/police.hpp` and
`native/world/police.cpp`). Saida supplies the existing character controller,
physical vehicles, ray queries, animation, attachments and ragdolls.

- The HUD shows **Recherche 0–100%**, without stars. Witnessed assaults,
  killings, gunshots, theft, collisions and repeated dangerous driving raise
  it. Civilian witnesses and officers need line of sight to identify the player.
- Up to four two-officer patrols follow the resident OSM lane graph, preferring
  mapped police/gendarmerie stations. Quiet patrols also walk nearby pavements.
  Pursuing cars brake for obstacles; officers dismount near a stopped suspect,
  chase on foot and can return to their vehicle. Off-road destinations use the
  nearest reachable roadside point, where officers dismount however far the
  suspect stands from the road. A pursuit spawns the car with the shortest
  drive to the closest reachable roadside; a refused dispatch is logged with
  its reason and shown on the HUD. Officers on a foot patrol re-board when
  called to a distant pursuit. Routing joins streamed graph
  seams and keeps different bridge heights separate.
- Minor offences lead to arrest after 2.5 seconds stopped within reach. The
  player is released outside the nearest resident station, when one exists,
  with ammunition confiscated. Violent offences or 40% alert authorize gunfire;
  physical raycasts resolve hits, cover and vehicle protection. Officers use
  the crowd/player humanoid factory and shared knockdown/recovery policy.
- At **100%**, a helicopter approaches, circles the last known position and
  searches with a spotlight and shares the progressive weapon selection of
  ground officers: exclusively rockets at 100%, then gunfire and occasional
  rockets as the wanted level falls. New visual contact updates dispatch; losing sight
  starts a search of the recorded area. After 12 seconds unseen the bar falls
  at 0.8 points/second; aerial support withdraws below 60%.
- France uses Scopia's textured Police nationale and Gendarmerie cars, imported
  through the existing vehicle pipeline: four independent wheels and Near/Far
  LODs, **7,755 shared vertices** together. Other countries use the existing
  estate with localized markings; labels, equipment colours and siren profiles
  are game content. Each car owns a spatial siren; rotor sound follows the
  helicopter. Original synthesized sirens/rotor audio are CC0.
- Population, planning frequency and geometry are bounded. The game reserves
  45,000 vertices for response assets/equipment and private car geometry;
  optional building LOD uploads also respect actual and pending arena use.
  Death or completed geographic travel clears the local pursuit.

`R1WORLD_POLICE_SMOKE=1` with `--smoke` runs the physical patrol, escalation,
helicopter, dismount, shooting, officer ragdoll/recovery and arrest scenario.
`R1WORLD_POLICE_CAPTURE=<png>` captures the armed response after its first hit.
The pursuit test keeps the player's original position, escalates gradually,
and requires both ground and aerial gunfire to reach him. The earlier Vannes
fixture placed the player beside a patrol and did not prove dispatch arrival.
Sustained frame times and country-specific pursuit qualification remain open.

## Physics

All runtime collision is the engine's (Jolt through Saida); the game has no
obstacle tests of its own. Each uploaded tile mesh gets a `StaticBodyNode` with
a `Mesh` `CollisionShapeNode`; interior shells and doors are mesh bodies;
furniture and vehicles have box colliders; a tile's trunks are one compound
body, and a trunk joins it when its tree is mounted, never before. A tree
still waiting to stream was an obstacle nobody could see, on tiles cooked before OSM
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

## Multiplayer

The engine owns discovery/transport (`plugins/p2p`) and owned-state replication
(`plugins/netcode`). R1World owns geographic topics, messages and gameplay rules
in `native/world/online*`. Multiplayer is disabled by default.

Meeting cells are 0.02 degrees wide, joined within 700 m and left beyond 900 m.
The overlay targets 32 nearest neighbours. Up to 96 players are tracked, 48
drawn, and 16 presented with full combat/aim detail. Distant `sightings` are not
yet rendered. The engine uses encrypted direct links, the public Mainline DHT,
LAN beacons and introductions by peers. A third reachable peer is necessary
for relay when two NATs cannot be punched.

Each player owns their avatar. The 32-byte snapshot is sent at 20 Hz to relevant
nearby peers, 5 Hz within 400 m and 1 Hz beyond. `netcode::Replica` interpolates
in the past, extrapolates at most 250 ms and hides a player silent for 3 s.
Standing players use walk/idle impostors beyond 60 m, returning to their mesh
below 54 m. The hidden mesh keeps its active hit capsule. `r1.human_impostor`
bakes the scan facing -Z; the atlases retain live relighting and world-distance
walk phase. Frame cost with a real crowd remains unqualified.

The victim judges a shot or punch against their own recorded position, range
and cadence at the instant the shooter rendered. Each machine simulates rockets
and judges its own blast. Refusals give a reason. Local witnesses can hide a
player whose observed snapshots and claims disagree; they neither vote nor
provide comprehensive cheat prevention.

`kApplication` stays `r1world/online`; `kProtocol` identifies message compatibility.
Other versions remain linked, are named as incompatible and exchange no state.
A session invite code joins `arenaTopic`, identifies its owner via Welcome and
uses ordinary travel to land four metres beside them. The friend link has no
permanent priority over nearer players.

Online mode removes traffic, passers-by, beach visitors and police.
`parking_rules` derives shared places from each tile's street graph: streets up
to 50 km/h, 6.5 m spacing, 12 m clearance at junctions, 35% occupancy. The nearest
96 within 130 m receive nodes. A taken car belongs to its driver and broadcasts
at 10 Hz, then 1 Hz after abandonment. Take count then identity breaks concurrent
claims. After 30 s without its owner the car is forgotten, its place remaining
empty for the session. Boats, aircraft, fauna, weather and rocket damage to cars
remain local; players aboard boats/aircraft are not drawn remotely.

`[World online]` logs link/relay counts, versions, estimated population,
snapshot tiers, worst network-thread wait and median/p95/p99/worst frame each
minute and at shutdown. See [the two-PC qualification procedure](QUALIFICATION_MULTIJOUEUR.md).

## Known limits

- **Dense neighbourhoods**: the 120,000-vertex per-tile refusal is removed;
  the total resident vertex/index arena can still be exhausted. The density
  policy reads the tile's own geometry and reduces people and interiors, never
  trees or surveyed road structures. Street trees have three levels of detail:
  the model, the same tree thinned by `tools/r1/tree_lod.py` (run it after
  changing a tree model), and a Far atlas billboard derived from Mid.
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
- **Traffic**: signal timing and missing lane rules are inferred, rather than
  surveyed municipal programs. Junction reservations remain conservative;
  national priority laws, roundabout semantics, destination routing and kerb
  parking are not complete. OSM restrictions with conditional exceptions or
  via-way chains are not yet supported. Physical simulation is local; distant
  traffic follows its graph. Pedestrian crossing protection uses local gaps
  and the shared signal phase.
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
CC BY 3.0 (`assets/licenses/Road-vehicles-CC-BY-3.0.txt`). The parachute is
"Parachute" by Poly by Google, CC BY 3.0
(`assets/licenses/Poly-by-Google-Parachute-CC-BY-3.0.txt`), modified as
`tools/r1/parachute.py` says. The selection map
and offline coastline are Natural Earth (public domain). Asset provenance and
checksums are in `assets/THIRD_PARTY_ASSETS.json`, and attribution is visible
in game.
