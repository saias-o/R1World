# R1World game project

This directory is the playable SaidaEngine project. Its one scene,
`scenes/earth.scene` (the project's `mainScene`), answers one question: can you
pick any point on Earth and walk there?

## The world

```powershell
powershell -File game\Play.ps1
```

A map of the Earth opens. Click a place, or type coordinates, or take one of the
five shortcuts, then press **Go**. The starting tile is downloaded, cooked and
mounted first; you land as soon as it is safe, while surrounding tiles and
decorative objects continue streaming during play:
The animated character is controlled in third person.
**ZQSD / WASD** to walk, **Maj** to run, **Space** to jump, the mouse to orbit, **M** or **Échap**
for the map again, **Reprendre** to return where you were. A car is parked
beside you at every arrival and the streets have traffic on them: **F** gets in
and out of *any* car within reach, the same keys drive it and **Space** is the
handbrake — see [The car](#the-car) and [The traffic](#the-traffic).

The globe uses a geodetic controller over streamed elevation and footprint data;
it drives the character's idle/run/jump animations and an orbiting camera.
Camera obstruction tests use cached footprint bounds, and tile eviction retains
the live character's skeleton and clips. `--smoke --spawn2 2.356 48.858` exercises
walking, jumping, landing and map resume before/after a teleport. This is not a
Jolt controller: the streamed world does not yet supply solid colliders for
street furniture or precise sidewalk steps.

### Street and terrain revision v10 (10 September 2026)

The v10 generator fixes the mean-elevation gap under buildings with a solid,
untextured foundation below the sampled perimeter, keeping floors and roofs
horizontal. Ground-connected buildings receive it; tagged elevated structures
are excluded. Perimeter samples are spaced at most four metres apart.

For eligible mainland France/Corse tiles, the worker now requests an IGN RGE
ALTI bare-earth grid at the terrain's 41×41 resolution. At the Rue de Lobau
inspection point, the old Copernicus grid gave approximately 49.3 m and the IGN
service returned 34.63 m: conforming streets to the old grid alone could not
repair that difference. Invalid or uncovered IGN samples fall back as a whole
tile to Copernicus; `ground-elevation.json` persists both successes and fallbacks
and `elevationSource` records the answer. Cached ready tiles never revalidate.
The eligibility box is not a coverage assertion. Global high-resolution ground
coverage and vertical-datum harmonisation remain future work.

Streets use polygon unions and differences to connect intersections, remove
overlapping sidewalks and cut out buildings. Their top surfaces are clipped to
the **same terrain triangles** as the ground. Sidewalks rise 15 cm above the
carriageway; widths and sides follow OSM where tagged. Defaults for urban road
classes are labelled as inferred in each tile's `streets` manifest. Separately
mapped sidewalks are respected. Explicit zebra markings are read from OSM
crossing nodes and drawn on the intersected carriageway; unmarked crossings do
not become invented zebras. Query revision 4 now requests those nodes.

Continuous surface normals share vertices, while a quantised normal weld key
removes floating-point duplicates without moving geometry. Original decorative
models and progressive entry are retained. The previous nine-tile Paris measurement
used **875,014 / 900,000 resident vertices**, maximum **119,649 / 120,000** per
tile. This is measured geometry capacity, not a 60 FPS guarantee.

Authoring dependencies (shipped offline as wheels):

`Play.ps1` uses the bundled Windows CPython runtime at
`generated/python-runtime/python.exe`. It does not search PATH or require a
Python installation. Its `python312._pth` enables isolation, ignores
`PYTHONHOME`/`PYTHONPATH`, and includes only the standard library, game tools and
project dependencies. This replaces the unsuccessful terminal-runtime selection
approach. The runtime comes from the official Python 3.12.10 embeddable package;
its executable's Python Software Foundation signature was verified.

Verified under Windows PowerShell 5.1 with invalid Python environment variables
and no Python on PATH: offline spawn/walk/resume passes. The isolated runtime
also runs the complete test suite.

Before any Shapely import, `r1/runtime_packages.py` verifies the pinned wheel
archives in `generated/runtime-wheels` and repairs missing/corrupt files under
`generated/runtime-packages-v1` using their CRCs. This occurs in the player's
process, with no network or pip. A successful top-level package import alone
is not considered proof of a complete installation. The repair path was tested
by removing `shapely/geometry/__init__.py` and launching `Play.ps1` under Windows
PowerShell: the file was restored and the offline game smoke test completed.
The earlier `generated/python-deps` directory is no longer used by Play.

```powershell
python -m pip install -r tools/requirements-world.txt --target generated/python-deps
python tools/cook_neighborhood.py 2.3522 48.8566 --rebuild
$env:PYTHONPATH = 'tools'
python -m unittest discover -s tools/r1/tests
```

The 178 tests cover foundations, intersecting streets, building cut-outs,
terrain conformance, observed markings, geometry sharing, IGN cache reuse and
no-data fallback. Visual QA exposed defects that these tests did not: successive
captures are under `generated/paris-*.png`. Capture mode now waits for all nine
tiles and their props, honours the engine's camera flags, and fixes the sun to
the scene epoch so load duration does not change the lighting. `Play.ps1` forwards
extra arguments to the game. Example, from `game/`:

```powershell
./Play.ps1 --smoke --spawn 2.3522 48.8566 --screenshot generated/paris-final.png --camera-pos '70.448,1.745,83.405' --camera-look '45,2,50' --after-frames 12
```

Known limits: landmarks still use generic procedural façades; municipal Paris
road polygons are not yet integrated; inferred sidewalk widths need not match
the survey; crossing ramps/islands and proper grade-separated bridges/tunnels
remain unsupported. Such bridge/tunnel ways are counted in the manifest rather
than painted on the ground. Detail outside the measured Paris sample has not
received the same visual verification. The v10 cache key keeps earlier visual
iterations from being silently reused.

Sources: [OpenStreetMap crossing tags](https://wiki.openstreetmap.org/wiki/Tag:highway%3Dcrossing),
[IGN elevation service](https://cartes.gouv.fr/aide/fr/guides-utilisateur/utiliser-les-services-de-la-geoplateforme/calcul-altimetrique/),
[Paris surveyed crossings](https://opendata.paris.fr/explore/dataset/plan-de-voirie-passages-pietons/table/),
[Paris sidewalk boundaries](https://opendata.paris.fr/explore/dataset/plan-de-voirie-trottoirs/api/).

### Progressive entry measurements before v10

Go no longer waits for all nine tiles. The current tile has first priority;
neighbours follow by distance, including across the date line. Selecting a
destination on the map already prefetches it. Polling is immediate after Go,
then every 16 ms while waiting and 100 ms during play. If the selected point is
blocked and no safe point is found on the available terrain, the search waits
for neighbours before refusing the spawn. Walking into missing terrain remains
blocked until it is mounted.

The data service already runs in a separate process, concurrently with the
game. Network requests remain serial and rate limited. A dedicated heartbeat
thread now stays responsive during downloads and cooking, so a long request
does not falsely report a dead service. Destination changes replace pending
work between tiles; an in-flight download is not forcibly cancelled.

Tile integration now separates geography from decorative scene children.
Terrain, buildings and collision data arrive first. Original trees and street
objects are then instantiated on the main thread with a **soft 2 ms per-frame
budget**, yielding between objects. No assets are downgraded. Scene and GPU APIs
are not assumed thread safe: a single model import can exceed that budget.
The existing cache format is retained (first scene child = geography/ocean;
subsequent children = decorative props), and eviction also discards unfinished
prop work with its tile.

Measured with the existing local cache, network disabled, on this development
machine: Go-to-play was **37.9 ms in Paris**, then **56.6 ms on teleport to
Amsterdam**, with respectively one and two resident tiles (the latter includes
the tile being left). Geometry integration measured **20–41 ms**. Before
splitting props, the first progressive-entry trial measured **1,375 ms** to
spawn and **1,344–1,572 ms** per tile; its walk check failed. After the split,
the Paris → Amsterdam walk/resume run passed, moving 7.25 m at the destination,
with an empty worker log. These are internal steady-clock measurements, not
input-to-present latency or a guarantee on other machines. Logs contain
`[World streaming] go_to_play_ms` and `mount_ms` for repeatable diagnosis.

While driving or sailing, the request queue now covers the surrounding nine
tiles plus a corridor projected up to 45 seconds ahead (at most 25 tiles in
total). The imminent road tiles are prepared first; all immediate neighbors
remain in the queue. Each projected neighborhood gets its own bounded OSM
query so prefetch never mistakes observations from another area for local data.
The game mounts the next prefetched tile as the vehicle reaches its edge. The
resident geometry remains limited to the nearby ring, independent of the
larger disk prefetch queue. New areas can first appear as explicitly marked
simplified terrain when their detailed observations are unavailable.

Full-tile imports can still exceed a 16.7 ms frame. CPU asset decoding jobs
and incremental GPU uploads remain necessary for consistently smooth 60 FPS.

Verification: `PYTHONPATH=tools python -m unittest discover -s tools/r1/tests`
(166 tests), then the offline smoke command below with
`--spawn2 4.8952 52.3702` to exercise eviction and teleport as well as entry.

`Play.ps1 -Build` relinks the executable first. Nothing contacts a data service
except the tile worker, and only for tiles you actually asked for.

Four processes' worth of moving parts, each with one job:

| Piece | Job |
|---|---|
| `ui/world.html` | the selectable map, drawn over a Natural Earth basemap baked offline by `r1/prepare_world.py` |
| `native/world.cpp` | the game: map input, floating origin, streaming, collision, the walk |
| `r1/world_service.py` | the worker: downloads OSM and Copernicus, cooks one tile at a time, publishes `ready.json` only when every asset exists |
| `r1/world_tiles.py` | the grid: metre-sized latitude rings, global, with no Mercator polar cutoff |

`tools/play_world.py` owns both processes together, so there is no worker left
running after the game exits.

### A place you have been is yours

Everything downloaded stays on disk under `cache/world/<tile key>/`, and the key
is the tile and the generator version — no session id, no spawn point, no clock.
Quit the game, come back, return somewhere already visited, and **nothing
touches the network**: `cook` returns `ready.json` before it can open a socket.

Each surveyed tile keeps its raw observations (`osm.json` and elevation), its
geometry (`world.glb` and `tile.scene`), and a `ready.json` published **last**.
A run killed mid-cook can therefore rebuild from saved observations. A missing
shared neighborhood query never prevents reuse of a tile's own cached data.

If no observations exist and the network is unavailable, Go still opens a
playable, simplified tile. Its land/sea outline comes from the bundled Natural
Earth map; elevation is flat and local streets and buildings are absent. The
HUD says **Hors ligne : terrain simplifié** and `ready.json` records
`offlineApproximation: true`. The temporary geometry lives in `offline/` so a
later network sync can build detailed geometry beside it and replace the
manifest only when complete. The next visit loads those detailed tiles.

Measured on the real cache: about **1 MB per tile**, nine tiles per spawn, so a
visited place costs roughly 10 MB and is free forever after. Raw OSM responses
shared across a neighbourhood live in `cache/world/sources/`.

The cached path and the simplified path can both be exercised with closed
proxies:

```powershell
$env:HTTP_PROXY = "http://127.0.0.1:9"; $env:HTTPS_PROXY = "http://127.0.0.1:9"
python tools\play_world.py --smoke --spawn 2.3522 48.8566    # visited: saved terrain
python tools\play_world.py --smoke --spawn -58.3816 -34.6037 # new: simplified terrain
```

`tests/test_world_cache.py` holds the same guarantee where it does not need a
GPU: it makes `urlopen` raise and then asks for a cached tile anyway. Any future
edit that re-validates a cached tile against a server, or refreshes it on a
schedule, fails there.

Superseded tiles from earlier generator versions (`v1_`, `v2_`) are kept rather
than deleted — `cook` copies their raw observations forward into a new version
instead of downloading them again — so the directory grows across a version
bump. Deleting a `v1_`/`v2_` folder costs only a re-download.

### A frozen tile counter is a dead worker

`os.replace` on Windows refuses to replace a file another process has open, and
the game reads `status.json` four times a second while it waits for a spawn. On
a nine-tile teleport the two crossed after the eighth tile, the PermissionError
propagated out of `serve()`, and **the worker died**. Nothing cooked another
tile after that. The player saw the counter stop at "1 / 9" with no error
anywhere — the worst shape a failure can take, because it reads as slowness.

Three changes, and the third is the one that matters most:

1. `atomic_json` retries the replace (12 × 50 ms). The reader holds its handle
   for microseconds, so the next attempt succeeds.
2. The cooking loop publishes through `report()`, which swallows a write that
   fails anyway. A status line is something a player reads; it is not something
   the world may depend on.
3. The game now reads `heartbeat.json`, which the worker had been writing since
   the beginning and **nothing had ever read**. A worker silent for more than
   15 s puts a sentence on screen saying so instead of leaving a counter to sit
   there. A stall that says so is a different bug from one that does not.

`tests/test_worker_status.py` holds all three, including a check that the
cooking loop never writes the status file directly again.

### When the data services say no

The tiles come from two free, shared services while the player waits, and both
answer 429, 502 or 503 under load and recover within seconds. One such answer
used to become a bare `HTTP Error 503` in front of the player, with a
60-second cooldown behind it on every other tile — a hiccup read as an outage.

`_request_json` now separates a server saying *not now* from one saying *no*:
408, 425, 429 and 5xx are retried with a doubling backoff that honours
`Retry-After` up to 20 s, and anything else (400, 403, 404) is raised at once so
`fetch_osm` moves to the next mirror instead of waiting on a refusal. Nine tests
in `tests/test_fetch_retry.py` hold that distinction with `urlopen` replaced;
none opens a socket.

The mirror list is ordered by measurement, not preference, and it is short on
purpose:

| Endpoint | Measured |
|---|---|
| `overpass-api.de` | 0.4 s, correct |
| `maps.mail.ru/osm/tools/overpass` | 8.4 s, correct |
| ~~`overpass.private.coffee`~~ | removed — times out at 25 s |
| ~~`overpass.kumi.systems`~~ | not listed — times out at 25 s |
| ~~`overpass.osm.ch`~~ | **never list it** — 200 with zero elements outside Switzerland |

That last row is the one that matters. A mirror that fails is harmless; a
mirror that answers `200` with an empty element list is not, because `cook`
would store it as a valid, building-less tile and the cache cannot tell that
apart from genuinely empty countryside. The city would be gone until someone
deleted the cache by hand. A test refuses any `osm.ch` entry in the list.

Overpass allows two concurrent queries per client; `world_service.py` runs one
at a time and spaces its region downloads 20 s apart, which is well inside that.
`https://overpass-api.de/api/status` reports the slots you have left.

**The origin floats.** Plan §4 I1 says no ECEF coordinate ever reaches the
engine, and this is where that is enforced: the player's position is WGS84, each
tile owns its own tangent frame, and the renderer only ever sees metres relative
to an origin that is rebased every 350 m. Walking across a tile boundary, the
date line or a pole is therefore not a special case.

**The world tiles are still a preview LOD, and the manifest says so.** No
modelled openings, no waves on the water, no clutter beyond what OSM mapped as a
point. What they are no longer is *styleless*, no longer one green, and no
longer empty — see below. The detailed building generator, with modelled
openings, would exhaust the engine's vertex arena on a single city tile, so the
reduction stands; it is a scope reduction (§4 I0: reduce the scope, not the
correctness), not an approximation dressed up as detail.

## The region decides what the survey did not say

Until now an untagged building was three storeys, a flat grey roof and one
masonry colour, from Chamonix to Tunis. Ranks 5, 6 and 7 of the fidelity
hierarchy — heights and gabarits, facade materials, roof form — were answered
with a constant, and a constant is not a cheap approximation of a region. It is
the absence of one, and it is what made every city the same town with different
terrain under it.

The world's buildings now go through the Atlas (`r1/atlas.py`), which holds **twelve named regions and twenty-two
continental bands**:

| Tier | What it claims | Written |
|---|---|---|
| `region` | a place someone described: Paris's legislated cornice line, Tunis's whitewash and parapet, Amsterdam's narrow steep-gabled plots | 12 |
| `band` | the smallest honest thing that can be said about a continent — a Nordic town is painted timber under a steep roof, a Saharan one is flat-roofed earth render | 22 |
| `none` | nothing. Open ocean, the ice sheets, the seams between rectangles | the fallback |

Every tile's `ready.json` records which tier answered and how many buildings owe
it their height and their roof rather than owing them to a survey, because §4 I5
requires the game to know what it is guessing:

```json
"region": "Amsterdam — ceinture des canaux", "regionTier": "region",
"inference": {"count": 1381, "heightMeasured": 1249, "heightInferred": 132,
              "roofTagged": 3, "roofInferred": 1378,
              "roofShapes": {"flat": 429, "gabled": 777, "hipped": 156, ...}}
```

**Nothing measured changed.** `plan_gabarit` puts a tagged `height`, a tagged
`building:levels` and a tagged `roof:shape` ahead of anything a profile offers,
and Amsterdam is the proof: the Netherlands' OSM import means 1 249 of those
1 381 buildings never consult the Atlas for their height at all. It is the 132
that do — and the roofs, which almost nobody tags anywhere — that the region is
for.

**Bands exist because the alternative is worse.** With named regions and a
characterless fallback alone, ninety-nine per cent of the planet is the same
grey box, and risk n°1 of §14 — *le monde est vaste et ennuyeux* — is met not by
the Atlas but by the absence of one. A band is a modest claim, it is labelled as
one, and §9.2's similarity propagation is what eventually replaces it with
something said about a place.

Rectangles overlap — Sicily sits inside any box drawn around North Africa, the
Alps sit inside France and inside Italy at once — and **ordering is the only
disambiguation**: the table is written most-specific first and the first match
wins. Adding a region means putting it above the band it refines.

### Geometry budgets (original default-capacity measurements)

Giving the world its roofs cost geometry, and it found the real ceiling. Saida's
`GeometryRegistry` default arena is 1 048 576 vertices; the capacity can now be
configured at engine startup. The following measurements used that default. Amsterdam's canal
belt — nine tiles, 8 368 buildings, the densest urban fabric the project has
cooked — first came to **1 129 742 vertices**, and the failure it produced was
the worst shape a failure can take: eight tiles mounted, the ninth threw
`failed to allocate vertex space` inside the scene loader on every frame, the
message went nowhere a player could see it, and the spawn sat at "8 / 9" until
the three-minute smoke timeout blamed the network.

Three changes, and the first is the one that bought the roofs:

1. **The preview LOD's roofs have no fascia and no soffit.** `_add_slab` with a
   zero thickness emits the roof surface and nothing else — six vertices where a
   slab costs twenty-four. On a dense gabled tile the slab edges were 46% of
   every vertex in the file, more than the walls under them. What it gives up is
   rank 12; the ridge, the pitch, the overhang and the colour all survive, and
   they are what carries the silhouette. Amsterdam: 1 129 742 → **840 351**.
2. **The resident budget is derived from the configured arena:** 85% for tiles,
   with the rest left to shared models and other scene resources. The historical
   measurements below used the previous 900 000 vertex budget.
3. **A refused tile says so.** It logs one line naming the tile and the
   arithmetic, it fails the smoke test immediately with that reason instead of a
   data timeout, and in play it cancels the spawn with a sentence and hands the
   map back rather than leaving a counter to sit there. Same lesson as the
   frozen tile counter, learned in a different place: a stall that says so is a
   different bug from one that does not.

Measured after, on the three cities the smoke tests walk in:

| Neighbourhood | Buildings | Worst tile | Resident | Of 900 000 |
|---|---|---|---|---|
| Amsterdam, canal belt | 8 368 | 106 522 | 840 351 | 93% |
| Paris, rue de Rivoli | 3 653 | 102 839 | 764 705 | 85% |
| Tunis, médina | 567 | 22 439 | 165 905 | 18% |

Tunis is cheap for the reason that makes it Tunis: its roofs are flat. The
region model pays for what a place actually has.

The per-tile budget in `world_service.py` (120 000) is the sanity bound
underneath the runtime one — a single tile approaching a sixth of the planet's
entire geometry budget is a generator that has gone wrong, not a city that is
unusually dense. `tests/test_world.py` builds a denser tile than any that has
been cooked, out of footprints that are all rectangular so every one of them
gets its pitched roof, and holds it under that bound.


## The ground stops being one green

Every square metre of the planet used to be `Material("Ground", (.30,.38,.24))`.
The Sahara was that green. So was the Amazon, the tarmac of a Paris courtyard,
and the ice on a Chamonix glacier — and Amsterdam's canal belt had no canals,
because a tile only got water when it was *entirely* ocean.

§2.1 puts *sols : revêtements* at rank 9 and marks it "OSM partiel → règles",
which is the shape of the answer: `r1/ground.py` classifies what OSM mapped, and
where OSM mapped nothing the region says what the ground around here usually is.
The Overpass query already had to widen to ask — see below — but the mesh did
not, because **the terrain already has the vertices**.

**Nothing is laid over the ground; the ground is partitioned.** Draping a
landcover polygon on a 90 m elevation grid from its own outline would cut it
through every slope it crosses, and it would cost geometry the arena does not
have. Instead each of the terrain's 3 200 triangles is classified by where its
centre falls and the mesh is split into one part per class. Splitting a mesh
whose triangles already carry their own face normals duplicates nothing, so the
whole of rank 9 costs **zero vertices** — the densest tile in each of the three
cities weighs exactly what it weighed before.

The resolution follows from that and is worth stating plainly: 41×41 over 555 m,
so **a class narrower than about 14 m does not exist here**. An Amsterdam canal
does, at 25 m. A brook does not, and it reads as the ground around it rather
than as a thin blue line in the wrong place.

| Neighbourhood | Ground measured | The tile, by area |
|---|---|---|
| Amsterdam, canal belt | **76%** | 60% made ground, 15% water, 24% inferred |
| Tunis, médina | 18% | 10% made ground, 6% grass, 82% inferred |
| Paris, rue de Rivoli | 16% | 10% made ground, 5% grass, 84% inferred |

Amsterdam is what a fully mapped city looks like; Paris is what most of the
world looks like, and the 84% is the region talking. Every tile's `ready.json`
carries `ground.measuredFraction`, so how much of what you are standing on
anybody actually looked at is a number and not a feeling (§4 I5).

### They are albedos, and the first version of them was not

The table was first written as *colours*, picked the way a painter picks them,
and it gave asphalt 0.38 and dry sand 0.78. Under the world's own sunlight — a
peak intensity of 4.6, a Sun 50° above Paris — a horizontal surface at 0.38
renders at **230/255** and one at 0.78 renders at **242**, so the entire palette
collapsed into a single white and the Sahara looked like a car park in the snow.

The published figures are nothing like those numbers: water 0.03–0.06, asphalt
and concrete 0.10–0.15, forest 0.08–0.15, grass 0.18–0.25, cropland and rock
0.15–0.25, dry sand 0.30–0.40, fresh snow 0.80–0.90. The table carries those
now, and each entry keeps only its *hue* from what a painter would have chosen.
Paris's sunlit ground went from 223 to 196 and stopped being clipped; the
`Albedo` tests hold every class inside its published range, and hold made ground
at less than half the luminance of desert sand — the pair the table exists to
keep apart.

The building palettes have not been re-derived the same way. They are lit
obliquely rather than face-on so they are nowhere near saturating, but the same
question is open for them, and it belongs to the Atlas rather than to this pass.

### You can no longer walk on a canal

A canal you stroll across is a worse lie than a canal that is not there, and
`blocked()` knew only about building footprints. Rather than ship the polygons a
second time in another coordinate system — and let the collision and the picture
disagree about where the bank is — each tile ships the **grid it was classified
on**, as forty rows of forty characters. It is a kilobyte, it is exactly what
the eye sees, and testing it is two divisions instead of a walk over every ring
in the neighbourhood.

Spawning in a canal or the open sea starts the player swimming at the chosen
coordinate. A spawn inside a building still searches for nearby clear ground
and reports a refusal if none exists.

### The query widened, so the cache had to learn what it had asked

A cached response is only as good as the question that produced it, and the
cache key — a bounding box — does not carry the question. Asking for landcover
meant asking a wider question (`way[landuse]`, `way[natural]` rather than a
short list), and without a version every previously visited place would have
gone on answering the new question with the old answer, forever, with a symptom
indistinguishable from a city OSM has not mapped.

`OSM_QUERY_VERSION` now travels inside the cached document. A cache answering
the current question is used untouched — the offline guarantee is unchanged. A
cache answering an older one is re-asked **once**, and if nobody replies the
stale document is used rather than raised, with the tile recording
`osmQueryVersion` so the manifest says which question built it. An older
observation set is a worse answer than a fresh one and a far better answer than
no world at all.

### What this did not fix

The fog. `prepare_world.py` writes `fogStart: 160, fogDensity: 0.0035` as
literals — the only lighting numbers in the project that are not derived from an
atmosphere — and that is a half-distance of 200 m, roughly a hundred times
thicker than clear air. It is why everything beyond the first block washes to
pale blue, and why the new ground materials read for one street and then stop.

It has not been touched, and the reason is not oversight: with a 3×3
neighbourhood the world *ends* at about 800 m, and the fog is what hides the
edge. Thinning it without somewhere to see is trading a wrong atmosphere for a
visible void. It is the same knob the horizon chantier will have to take, and it
should be taken there.

## The world gets its furniture

OSM has been handing this generator a `natural=tree` node since its very first
query, and not one of them was ever planted. §2.1 puts vegetation at rank 8 and *mobilier urbain régional* at rank
10 — "forte signature culturelle" — and both were simply absent: a Paris street
had buildings, a road surface and nothing standing on it.

**Why this is affordable, and it is the whole design.** Saida's `MeshCache` keys
meshes by `AssetID`, so six hundred nodes pointing at `tree_oak.glb` upload that
oak *once*. Measured on one alpine valley: sixty-four trees, one hundred and twenty-eight
primitive loads, and twenty-one distinct mesh ids for the entire scene. The
vertex arena — the thing that decides everything else here — therefore charges
per prop *kind*, not per prop:

> **9 390 vertices for the whole prop library. Once, for the planet.**

That is 0.9% of the arena for eleven models, against 841 035 for one
neighbourhood of Amsterdam. (The arena is the only thing instancing makes free:
a model's *file* is re-parsed per node, which is what later ruled the
photoreal trees out — see below.) Props are therefore scene nodes in `tile.scene` and
are never baked into `world.glb`; baking them would charge the arena six hundred
times over and would not fit.

What a prop does cost is a node, and nodes are drawn. §12.4 caps L5 detail on
the reference machine, so the count is capped per tile — **260** — rather than
left to OSM, which maps about twelve hundred placeable points per Paris tile and
three per Tunis tile.

**The budget is shared out, not raced for.** Filling it by priority would give a
Paris tile six hundred trees and not one bench, because there are more trees.
Each kind claims a share and whatever it does not use falls through, so a street
keeps its lamps *and* its trees, and a village with forty mapped features keeps
all forty:

| Neighbourhood | Placed | Refused by budget | Steeples | What it is |
|---|---|---|---|---|
| Paris | 2 263 | 2 103 | 18 | 1 617 lamps, 298 benches, 154 bins, 86 bus stops |
| Amsterdam | 1 486 | 316 | 19 | 1 028 lamps, 245 benches, 128 bins, 36 bus stops |
| Tunis | 16 | 0 | 6 | 15 bus stops, 1 fountain |

**There are no trees in that table, and that is the honest part.** See below.

Tunis is the honest half of this. **Nothing here is invented** — every prop
stands on a point somebody surveyed, there is no scattering and no "a village
would have benches" — so where OSM is thin the world is thin, and `ready.json`
says by how much. Tunis will look emptier than Amsterdam until somebody maps
Tunis.

### Vegetation is measured, decimated — and still switched off

The project ships photoscanned Poly Haven trees. They are the best asset in the repository, and rule 1 of
[CLAUDE.md](../CLAUDE.md) exists because this feature first reached for a
low-poly kit instead of using them. It does not any more: there is no kit tree
anywhere, and a test refuses one.

What replaced that shortcut is `r1/decimate.py`, the §11.4 answer — the model
enters the game *decimated*, never exchanged. It works, it is deterministic, it
keeps whole needle clusters so no UV is ever averaged across an atlas, and it
spends its budget by water-filling so a trunk is not cut to 0.46% alongside a
canopy. And the measurement it produced is why the tree share is currently
**zero**:

| | |
|---|---|
| `island_tree_01` as scanned | 1 303 928 vertices, 927 528 of them canopy |
| streamable size | ~12 000 vertices, ~500 kB per species |
| canopy that survives | **0.43%** — about a hundred leaf clusters out of 25 000 |

A hundred clusters is not a canopy. The tree keeps its trunk, its bark and its
proportions and loses the thing that made it read as a plant; a street of
skeletons is not an improvement on a street of nothing, so the world plants
nothing until this is solved properly.

**Why the budget cannot simply be raised.** Saida deduplicates the *mesh* an
instance points at, but the scene loader opens and parses the referenced file
once per node — measured, 476 loads of one `broadleaf.glb` in a single
neighbourhood. A canopy that survives needs about 185 000 vertices and a 6 MB
file, which is 15% of the arena per species *and* thirty-six seconds a tile.

This is M5. §12.3 lists impostors as its third lever — *« un arbre à 200 m
devient deux triangles… la différence entre praticable et impraticable »* — and
§12.2 gives vegetation the largest triangle budget of any item in the frame.
Everything else is already in place and waiting for it: the per-biome species
table in the Atlas (pines in Chamonix and Oslo, quiver trees in the Sahara,
broadleaf in Paris), the placement, the budget, the decimator. Turning trees
back on is one number.

### The church is built, not downloaded

**No CC0 low-poly church exists.** Four Kenney kits were searched; the nearest
thing on offer is a fantasy crypt. That turned out not to matter, because
downloading one would have been the wrong answer anyway:

- OSM gives a church its **real outline** — rank 3, measured, exact, "c'est la
  forme des villes". Dropping a fixed model on top of it throws that away and
  stands a generic shape where a surveyed one exists.
- What makes a village read as a village at any distance is the **silhouette of
  the spire above the roofline**, not the tracery. That is forty vertices
  derived from the footprint the survey already drew.
- A generated tower is drawn into the same meshes as the walls and the roof it
  belongs to, so it **inherits the region**: a Paris church is limestone under
  zinc and a Nordic one is painted timber under red tile, with no palette of its
  own. A downloaded mesh would be Kenney-coloured on every continent.

The tower goes at one end of the nave, as wide as the nave allows and capped at
7 m, and its cap follows what the survey says the building is: a **spire** for a
church, a **minaret** — slender, much taller, small cap — where `religion=muslim`
or `building=mosque`, and a plain **tower** for a faith that does not build
spires. The religion tag is only ever read, never guessed.

### Two bugs the first render found

**A street of magenta lamp posts.** Kenney's newer kits do not embed their
texture: every model in a kit points at one shared `Textures/colormap.png`.
Extracting the `.glb` alone extracts a dangling URI, and the engine draws
missing-texture magenta with nothing in the log. The atlas is eleven kilobytes,
so it is now **embedded into each prop's own binary chunk** rather than copied
beside it — a prop is one file that cannot be half-installed.

**A turquoise blob the size of a house.** Kenney paints foliage
`(0.16, 0.79, 0.67)` — a turquoise at four times the albedo of a real leaf — and
bark `(0.89, 0.51, 0.34)`, a salmon brighter than most snow. At this world's
light level anything above roughly 0.35 saturates, so the first tree planted in
Paris rendered as a pale cyan lollipop. This is the same lesson the ground table
learned the same day, and §11.4 already had the answer: *un asset non conforme
n'entre pas*. `normalize.repaint_kit_model` now repaints every prop into albedo
on the way out of the archive, and **refuses any material name the palette does
not know** — a kit that renames `leafsGreen` stops the build instead of shipping
turquoise.

### The tile you are leaving no longer competes with the ones you arrive on

Adding props found a real bug in yesterday's budget, and the guard written
yesterday is what found it: a Paris → Amsterdam teleport refused its own ninth
tile. During a teleport the tile under the player is retained so they do not
stand on nothing, and it was being counted against the *destination's* 900 000.
It is leaving; the arena has the room for it (841 k for the densest
neighbourhood, 107 k for the tile being left, 9 k for every prop model on the
planet, inside 1 048 576). It is now excluded from the count, and the teleport
passes.

### Provenance

Five CC0 kits — City Kit (Roads), Graveyard Kit, Fantasy Town Kit, Nature Kit
and Car Kit — are pinned by SHA-256 in `external_assets.py` and live in
`data/source-assets/` as authoring inputs. Kenney publishes zips and no
per-model URL, so the archive is what gets pinned and twenty named models are
mined out of it; the archives are never opened at run time. What ships is
**556 kB** of extracted, texture-embedded, repainted GLB, recorded in
`assets/THIRD_PARTY_ASSETS.json` with its licence.

## The car

You arrive on foot and there is a car beside you. **F** gets in, the same
ZQSD/WASD drive it, **Space** is the handbrake, **F** gets out again — and the
door refuses to open above 7 km/h, out loud, which is the first thing the E2E
driver hit.

Water is not a road: entering it ejects the driver into a swim while the car
sinks below the surface. Walking into water also starts swimming. The swimmer
keeps only their head above water and can move back to a bank or board a boat.
Choosing a point in the water on the map starts directly in this swimming state.

At the wheel the mouse does not steer, so it turns the head instead: a free yaw
around the car that eases back behind it once the mouse stops and the car is
rolling. The first version locked the view to the heading and that was simply
wrong — you could look up and down but never at the street you were turning
into.

### Geographic arcade driving

The game keeps its arcade ground-following controller in geographic coordinates.
Saida's `VehicleBehaviour` handles physical raycast vehicles; floating origins
are now supported by `Scene::rebaseOrigin` / `rebaseSubtree`, including live body
poses and velocities. R1World uses `rebaseSubtree` when placing streamed tiles.
Its driving controller still uses the elevation grid and building footprints,
not Jolt terrain colliders, and does not simulate suspension or rolling bodies.

### The handling is arcade, and the first version was not

The numbers started as a real saloon's: 4.2 m/s² of pull, a 5 m turning circle,
7.5 m/s² of tyre grip. The first person to drive it said it felt like
manoeuvring a bus, and he was right — at 70 km/h a real grip limit allows about
21° of heading a second, which is the truth about cars and is not the game.

So they are arcade now and the comment in `world.cpp` says so rather than
dressing them as physics: 7 m/s² of pull (0–100 in about 4 s), a 3.6 m circle,
and 16 m/s² of lateral acceleration, which no tyre has. What the grip term still
buys is the shape of the curve rather than a cap: about 90° a second at town
speed, 27° at 130 km/h, so a hairpin at full speed is still not one key press.

**The world stays measured; the car crossing it does not have to be.** Every
number that describes the Earth here — albedos, heights, footprints, the Sun —
is a measurement or it does not ship. A vehicle's feel is not a fact about the
world, and treating it as one made the world less pleasant to be in without
making it truer.

### The car is the player's, not the tile's

It is a member of `earth.scene` beside the player and the camera — never a prop
of the tile it stands on. A prop is evicted with its neighbourhood, and a
teleport would leave the car in the city you just left. It is disabled at load
(a car enabled at the scene origin stands in the middle of the Atlantic) and
every spawn parks it: three metres to the player's right, then a golden-angle
spiral out to 14 m looking for ground that is loaded, dry and not inside a
building — the spawn's own search, reused.

**When there is no such spot, it says so.** Rule 3 of `CLAUDE.md` was earned
three times in `native/world.cpp`, and every refusal here follows it: no kerb
within 14 m, no room to step out, too fast to step out, the edge of the loaded
world, an obstacle. Each writes a log line naming the reason and a line the
player can read, and the E2E fails on the reason rather than on a timeout.

### The detail follows the speed

§12.4 puts a vehicle in the L0–L4 band and drops mobilier, clutter and facade
detail above 15 km/h, and this is where the plan pays for itself rather than
being quoted. Above 4.2 m/s the frame stops spending its 2 ms importing street
furniture the player is about to leave behind, and the vegetation radii shrink
to 55% — 550 m of trees becomes 300, 65 m of grass becomes 36. Slow down and
both come straight back; standing still costs nothing.

### 1.80 m wide, and which measurement that gives up

Kenney models the saloon 2.55 m long and 1.50 m wide: a length over width of
1.7, where a real saloon is 2.45. **No uniform scale makes both right**, and a
non-uniform one would restyle the model rather than normalise it. Width is what
decides whether a car belongs between two real kerbs, so width is what is made
real — 1.80 m, a scale of 1.2 — and the length that comes out is 3.06 m, a real
city car and shorter than the saloon the kit drew. `test_vehicle.py` holds both
numbers so the trade-off cannot drift into a bus while nobody is looking.

Its four wheels turn and the front two steer, by name. The importer wraps each
named node around a mesh node that inherits the name, so the collector takes the
outermost match only — descending found each wheel twice and would have applied
every turn to it twice. Eight wheels on a saloon is what said so.

### Rule 1, from the other side

A Kenney model entering a repository whose best assets are photoscans is exactly
the substitution rule 1 of `CLAUDE.md` exists to stop, and it was checked before
the kit was downloaded rather than after: **there is no vehicle in this project
of any grade**, photoscanned or otherwise, and Poly Haven publishes none that is
drivable. So this widens coverage instead of displacing anything, which is the
case the rule explicitly allows — and a car is a manufactured object, the same
category as the lamp posts and benches it already leaves to Kenney.
`test_vehicle.py::DisplacedNothing` is what keeps that true: the car kit ships no
vegetation, and the five tree species still point at the decimated photoscans.

The car is repainted by the same `normalize.repaint_kit_model` as every other
kit model. It declares one material, `colormap`, the shared palette atlas the
props already dim by half; a re-export that renamed it would stop the build
rather than ship paint at four times a real albedo (§11.4).

### What the driver actually tests

`--smoke` now walks, jumps, lands, **walks back to the car**, gets in, drives
five seconds under full throttle, is refused the door at 76 km/h, brakes, gets
out, and only then reopens the map. Measured on Paris, offline, from the cached
tiles: 54 m covered, 21.2 m/s peak, exit at 1.9 m/s.

Two of those steps exist because the test found them. The walk phase leaves the
player eleven metres from where the car was parked, and the first run failed on
`could not enter the car, 11.7m away` — which is the right failure: a door has a
handle, not a radius, so the driver walks back to it the way a player does. The
second run then failed on `could not step out of the car at 21.21 m/s`, which is
the refusal working; the driver now brakes first, and the refusal itself is
asserted rather than avoided.

### Not there yet

No traffic, no parked cars in the world, no passengers, no fuel, no damage. OSM
maps no individual vehicles and nothing here invents any — the same bargain the
props make (§4 I5). One car, the player's, is what "se déplacer en voiture"
needs; the fleet of ~40 archetypes §6 budgets belongs to the document §13's
phase 6 says should be written when the world exists.

## The traffic

Streets have cars on them, everywhere on the planet, and **F takes any of
them** — the one parked beside you at the spawn, one you abandoned in a side
street, or one that was driving past a second ago.

### Two halves, and the seam is the point

The simulation is not in this project. It is
[`engine/plugins/traffic`](../engine/plugins/traffic/README.md), a header-only
add-on that knows nothing about R1World: no geodesy, no tiles, no scene, no
glm. It owns a lane graph, a population of agents on it, car following, give
way at junctions, and spawning around an observer. It is compiled into this
game's binary and never into the engine library, so rule 5 still holds — R1World
links against the engine, it does not rebuild it.

What lives here is everything only this project can do:

| Here | There |
| --- | --- |
| `r1/traffic.py` turns the tile's OSM ways into a lane graph in engine metres | `Graph` drives on it |
| The tile's density decides how many cars it deserves | `Flow::setPopulation` spends it |
| Which side of the road this place drives on | `Rules::leftHand` mirrors the lane |
| A scene node per agent, painted, on the terrain | `Flow::pose` says where |
| The player's car, as an obstacle | agents queue behind it |

The add-on has its own tests, which run without an engine build or a project:
`g++ -std=c++20 -O2 -Wall -I../include test_traffic.cpp` in
`plugins/traffic/tests`, then run it.

### A graph per tile, cooked with the street

The lane graph is built by the worker at the same moment as the street mesh it
belongs to, from the same clipped ways, and it ships in the tile's `ready.json`.
That is why a car cannot drive where no road was drawn: bridges and tunnels are
skipped by the street mesh, so they are skipped here too, and `service` ways —
driveways, parking aisles, alleys — carry no traffic at all.

Each tile gets its own graph and its own flow. A tile is evicted as a unit, so
traffic that outlived its tile would be driving on roads that are no longer
there; giving the flow the same lifetime as the roads makes that impossible by
construction rather than by bookkeeping. A car that reaches the edge of its
tile's roads retires there.

Nothing is invented, in the sense `props.py` means it: every lane is a way
somebody surveyed, every one-way street is tagged `oneway`, and a speed limit is
the `maxspeed` tag where there is one — 328 of them against 32 estimates on the
tile under the rue de Rivoli, and the tile says which is which (§4 I5).

### How busy a street is

Two terms, because either alone is wrong. Buildings are the density signal the
player actually sees, so a tile gets a car per 28 of them; road length caps it,
because sixty cars on one village street is not density, it is a car park. A
Paris tile asks for its ceiling of 22, a village for two or three, an empty
motorway tile for one or two.

**Then the cars have to be on the street you are looking at.** The first version
put them on the road network evenly and Paris still looked empty — the cars were
all there, one street away, because a city has far more back streets than
avenues and an even draw follows the count. Lanes now carry a weight from their
road class, and the add-on spawns in proportion to weight × length, so a
boulevard carries several times what an alley does. That one change is the
difference between "there is traffic somewhere" and "there is traffic here".

The neighbourhood shares a budget of eighty cars, spent nearest-tile-first. A
car is five primitives, so that is four hundred draws beside the two thousand
seven hundred a Paris neighbourhood already draws.

### One model, ten paints

One car model, so the fleet is told apart by colour alone. The ten colours are
in `r1/traffic.py`, shipped as `assets/world/traffic_paints.json` and read at
startup — **not** written in the C++ that uses them, because rule 2 of
`CLAUDE.md` applies to a colour whatever produced it, and a colour the tests
cannot see is a rule that is not held. They are albedos: 0.05 for black through
0.30 for silver, none above 0.35, which is where a sunlit surface goes white at
this world's light level.

Tinting is one `MaterialDesc` per colour through the engine's material cache, on
a shared mesh. Ten materials for the whole planet's traffic.

### Taking a car

The car the player is in is a *pointer*, not the one node the scene shipped.
Taking a traffic car hands over that very node — it leaves the flow
(`Flow::retire`), leaves its tile, and joins the world — and the car he was
using stays exactly where he stepped out of it. Copying a model into place would
have been less code and would teleport the car he just parked to wherever he is
standing, which is the one thing a player notices immediately.

Six abandoned cars are kept; the oldest is cleared after that, and a teleport
clears them all, because they are half a planet from where they were left.

The E2E drives all of it: it walks, jumps, walks back to the car, drives, is
refused the door at 100 km/h, brakes, steps out, then stands beside a live
traffic car, takes it over, and checks that the flow no longer owns it and that
the previous car is still standing where it was left.

### What it is not

No traffic lights, no lane changing, no pedestrians, no parked cars along the
kerb, no collisions between traffic cars and the world beyond the lane they are
on. Junction give-way is nearest-first: it keeps two cars off the same square of
tarmac, which is all a player at street level can actually check, and it is not
a priority system.

### One bug this found on the way

Cooking an avenue in the 8th arrondissement crashed the worker in
`street_surfaces.py`: the union of pavements came back as a non-empty
`GeometryCollection`, GEOS gives that type no boundary, and the kerb pass called
`.intersection` on `None`. The existing guard tested for *emptiness*, which the
non-empty case walks straight past. On screen it read as a three-minute data
timeout — the exact failure shape §3 of `CLAUDE.md` exists to prevent — and the
fix is to ask only the polygonal part of each surface for its boundary.

## Streaming and engine ownership

Saida maintains scene membership and resource ownership incrementally. Traffic
slots use `setVisible` in place; there is no underground parking coordinate and
no dependency on a global hierarchy version. The engine retains hidden resources.

Street furniture imports use a 2 ms per-frame budget. Distance culling remains a
gameplay choice (including the smaller detail radius at driving speed), while
near/far vegetation selection uses the engine's `LODGroupBehaviour`, projected
coverage and hysteresis. This supports trees whose near representation contains
several meshes. Static prototype flattening still reduces transform and node
costs and only composes uniform-scale TRS trees without shear.

Shared imported prototypes belong to a disabled branch of the world. Tile
trimming uses `Scene::resourceUsage()`, including LOD resources and player
animation resources, without a second game-specific asset walker. Tile geometry
reserves 85% of `ResourceManager::geometryCapacity().vertices`; the remaining
capacity is available for shared assets. Engine capacity is configurable at
startup, so this game no longer copies an internal allocator constant.

The smoke log reports indexed nodes per frame, accumulated across simulation and
render refreshes. It does not infer a full-scene traversal from a global counter.
The explicit end-of-smoke traversal only reports scene composition; transform
updates still traverse the hierarchy and this change is not a 60 FPS guarantee.

Verified on 2026-09-23 against the rebuilt engine: offline Paris-to-Tunis smoke
(walk, drive, take over traffic, teleport, resume) passed. A Paris capture with
3,248 plants also completed. During the first smoke phase, the scene held 21,578
nodes; the measured membership work averaged 56.8 indexed nodes/frame over the
82-frame whole-run sample. This is a count of index updates, not an FPS benchmark.


## What things are made of

Every surface used to be a flat measured colour — right on average, wrong
everywhere up close. `r1/surfaces.py` gives the ground, the streets, the walls
and the roofs photographed, seamless CC0 materials (Poly Haven, ambientCG; 44
families from 43 scans), and three rules keep them honest:

- **A texture brings structure, never albedo.** Each colour map is normalised
  per channel, and the glTF colour factor is the palette's albedo divided by
  that level, so the rendered mean is the measured albedo the palette already
  had (CLAUDE.md, rule 2) and the photograph only decides how it varies.
- **Real size.** UVs are metres; each material divides them by its scan's
  published size, so a brick, a slab or a tile is its own size. Three
  ambientCG sets publish none, and the table says their size is estimated.
- **Which ground, where.** Each region has a climate (`RegionProfile.climate`),
  made colder by latitude. A park is lawn in Paris, withered grass in Rome,
  lush in Bangkok; a forest floor is leaf litter, moss and needles in the taiga,
  dark humus in the rainforest; bare ground is cracked earth in the Maghreb and
  laterite in the savanna. Above the snowline (5 500 m in the tropics, ~2 750 m
  in the Alps, sea level near 72°) it is snow, just below it frost, and polar
  land is frosted.

Streets are asphalt, sidewalks concrete slabs, and a way OSM tags `sett`,
`cobblestone` or `paving_stones` is laid in cobbles. Roofs get UVs across and
up their own slope, so rows of tiles follow the eave. Walls are baked sheets:
one bay by one storey, the family's material tiled a whole number of times,
with a window on top.

None of it costs a vertex: UVs are functions of the position within a face, so
the weld keeps every vertex it kept before (a Paris tile went from 119 645 to
119 594). Texture memory does cost: about 90 MB for a city neighbourhood, so
the game's GPU texture budget went from 256 to 512 MB (`native/world.cpp`),
against the 4.5 GB the reference GTX 1060 allows. Ground and street colour
maps are 1024², everything else 512².

```powershell
python -m r1.surfaces        # from game\tools: download (pinned), normalise, bake
```

## Harbours and the open sea

Until now the sea was whatever OSM mapped as water inside a tile — which is
nothing, because OSM does not map the sea: it maps the **coastline**, a line
with the land on its left. `r1/harbours.py` rebuilds the sea from it, per tile:
the tile's box is cut by every coastline way that crosses it, and each piece is
sea or land by which side of the line it lies on. A tile with no coastline is land, or open ocean
when the world map says so.

- **Cells.** The 40 × 40 water grid now has three codes: land, inland water,
  sea-level water. A mapped basin within 2.5 m of sea level joins the sea when
  a coastline crosses the tile; a reservoir up the hill stays a lake. Estuaries
  and docks are cut off from the coast by OSM itself, which closes the
  coastline across a river mouth, and the elevation model reads Rotterdam's
  basins at 5 m — so there the tags decide: `tidal=yes`, a harbour, dock or
  lagoon water, or water inside a mapped port is the sea's. Under the sea
  the terrain is sunk to −4 m beneath the animated surface; dry land beside it
  is never drawn below it.
- **Works, as OSM mapped them.** Piers are decks on piles at the water's
  freeboard (floating pontoons sit low), breakwaters and groynes are rock
  mounds, quays are walls, and a lighthouse is a tower banded in the
  colours its `seamark:landmark:colour` tag gives, at its tagged height. One
  OSM traced as a footprint (Cap Ferret) stands on it, at its surveyed
  radius, instead of being extruded as a grey block. Every way is clipped to the
  tile first. Decks are walkable (`ready.json` → `decks`).
- **Boats, inferred and labelled so.** Berths are laid along piers, quays and
  the shores of a port's docks,
  each hull checked to float and to clear every other. The fleet follows the
  harbour — a cluster of piers is a marina (sail, motor, fishing), a port area
  brings tugs, bulk carriers and container ships (the kit has none: its
  bare-decked freighter is loaded with the yard's forty-foot boxes, as children
  of the boat so they sail with it) beside stacked yards, a ferry
  terminal a ferry — and the country: rowing boats in hot countries,
  houseboats on Dutch canals. Kenney's Watercraft Kit (CC0; rule 1 read both
  ways: nothing in the project covered a vessel).
- **Budgets.** 70 boats, 4 ships and 60 containers per tile, all scene nodes.
  Piles are the first geometry dropped if a tile would pass its vertex budget
  (`harbour.pilesDropped`).

**Taking the helm.** `F` boards the nearer of the closest car and the closest
boat. A boat keeps its hull length's handling (`ready.json` → `boats`): drag
grows with the square of speed, the rudder needs way on, and the bow refuses
land — a boat runs aground rather than climbing a beach. Below 1.5 m/s, `F`
steps onto a bank or deck when one is alongside, or drops the player into the
water. ZQSD/WASD swim toward shore; `F` boards the same boat again when its
hull is within reach. A faster boat refuses the exit out loud. The sea is
continuous across tiles, so a boat can leave its harbour for the open ocean.

**Ships at sea.** The worker places up to 40 vessels from the shipped AIS prior,
the game's date and cached weather; optional live AIS observations refine the
prediction when available. They keep moving after appearing. A player can take
the helm with `F` from the water, or transfer from a nearby boat when their
speeds match. The same steering and throttle controls then drive that hull.

```powershell
python tools\play_world.py --smoke --sail --spawn 5.3698 43.2951   # Vieux-Port
python -m unittest r1.tests.test_harbours                           # from game\tools
```

## The Sun follows the player

A world you teleport across breaks an assumption every earlier phase held
without saying it: the anchor. `scripts/sun_cycle.js` used to read its observer
once at load, which is right for one place and wrong the moment you leave it —
Sydney lit at Paris's hour, the beam on the wrong side of the sky, the season
inverted.

So the observer is state, and the game moves it:

```
native/world.cpp  ──setObserver(lon, lat, altitude)──▶  scripts/sun_cycle.js
                     ScriptBehaviour::callExport
```

Called at every spawn and every origin rebase. The C++ says *where*, never
*which light*: the model is not ported a third time, because the second port is
already only kept honest by a test.

`scenes/earth.scene` therefore contains no hand-written lighting value at all.
Direction, colour, intensity, ambient, horizon and the sky come out of one
instant and one air column, and `r1/prepare_world.py` writes them from
`r1/solar.py` and `r1/skies.py`.

```powershell
$env:PYTHONPATH = (Resolve-Path game\tools).Path
python -m unittest r1.tests.test_sun_cycle_parity -v
```

Nine tests hold this. Six were already there and pin the JS port to the Python
at 1e-9 over a year of instants and five latitudes. Three are new and check the
thing that only matters once the player moves: after `setObserver`, the script
must compute the **destination's** sun and keep nothing of the anchor's. The
five sites are chosen for what they break — Sydney and Cape Town invert the
season, Tokyo sits half a revolution of longitude away where an equation-of-time
sign error shows, Quito removes the seasonal swing, Tromsø keeps the polar
branches in — and a fourth test requires a non-finite coordinate to be refused
rather than propagated into the light's direction, where it would black the
frame out with nothing on screen to explain why.

**The default clock is the real UTC clock.** At each location the Sun is computed
from that instant and the player's latitude and longitude; one real day takes
one real day. `WORLD_EPOCH` in `r1/prepare_world.py` only lights the generated
scene before the runtime script starts. The scene can opt into a fixed instant
with `useSystemClock: false` and can deliberately accelerate time with
`secondsPerSecond`.

The HUD shows the destination's civil time when Open-Meteo supplies its time
zone. Without a recent saved zone or a connection it labels a longitude-based
time-zone estimate, rather than showing the computer's local time as if it
belonged to the destination. The worker also fetches a local current forecast
in the background: cloud cover softens the Sun and blends in a CC0 overcast
sky; precipitation increases haze. A forecast older than two hours is marked
unavailable. Tile loading and offline exploration never wait for weather.

## The sky follows the hour

The sky used to be one photograph, a veiled afternoon with its own Sun baked
in, turned down as the real Sun set — so a night was a daytime sky, dimmed.
It is now a day of photographs: Poly Haven's **Qwantani series**, eleven
"pure sky" HDRIs of one sky from one hilltop in South Africa, from before dawn
to after dusk (CC0). `r1/skies.py` prepares them and `scripts/sun_cycle.js`
drives them every frame; a parity test holds the two to 1e-9.

- **Each sky is placed by measurement.** Every photograph carries the instant
  and place it was taken, so `solar.py` gives the Sun's elevation in it; the
  visible Sun agrees within 0.7° on every daytime frame. The runtime shows the
  two photographs the real Sun stands between and crossfades them, morning
  photographs on the way up and evening ones on the way down.
- **Each sky faces the real Sun.** Every image is turned so its photographed
  Sun sits at the real Sun's azimuth.
- **Brightness comes from the model, not the files.** Poly Haven normalises
  each photograph, so their night is as bright as their noon. The band from 0°
  to 4° of each is measured and the sky is scaled so it equals the model's
  horizon colour — which is also the fog — so the dome meets the fog without a
  seam at every hour and keeps the photograph's gradient above it.
- **One Sun.** Crossfading two photographs would show two Suns, neither where
  the light comes from, so each photograph's disc, aureole and lens streaks are
  removed and the engine draws one disc where the beam comes from
  (`SceneSettings::skySunDirection`). Faint traces of the removed aureole remain
  in the midday skies.

The engine gained what this needs, as general features: a second sky and a
crossfade (`skyboxBlendTexture`, `skyboxBlend`, `skyboxBlendRotation`), a Sun
disc, and `scene.setSkybox(path[, blendPath])` for scripts. A sky path with no
file behind it is refused with a logged reason rather than drawn magenta.

```powershell
python -m r1.skies        # from game\tools: rebuild the skies and the table
```

The downloads are pinned by md5 and kept in `data/source-assets/skies/`; what
ships is the normalised sky in `assets/skies/` and `assets/skies/skies.json`.

## Testing it without a window

Every check below runs with the window hidden, so it does not steal the screen:

```powershell
$env:SAIDA_WINDOW_HIDDEN = "1"
python tools\play_world.py --smoke --spawn 2.3522 48.8566
```

The driver clicks the map, presses Go, waits for the tiles, walks, jumps, walks
back to the car, drives it, is refused the door at speed, brakes, steps out,
takes over a passing traffic car, and reopens the map with **Reprendre**. Exit code 0 is a pass; the failing line is in the log
whose path is printed at the end. Pass `--screenshot <png>` to also capture the
frame after the spawn.

`--spawn2 <lon> <lat>` adds a second leg: it walks, reopens the map, types the
destination and presses Go again. Teleporting out of a place you are standing in
is a different path from a cold spawn — tiles are evicted, the resource arena is
trimmed, the origin moves half a planet — and it is the path a player takes
every time after the first. It went untested until it broke, and it is where the
frozen tile counter was finally reproduced:

```powershell
python tools\play_world.py --smoke --spawn 2.3522 48.8566 --spawn2 10.1815 36.8065
```

Three of its assertions exist because of bugs that shipped:

- **The press, not the click.** Hovering a `ui-hit` element makes the engine
  consume the mouse, which clears the button's previous state, so
  `isMouseButtonReleased()` is never true over the menu and RmlUi never receives
  the `Up` that makes a `click`. A menu bound to `click` alone does nothing at
  all under a real mouse. VerticalSlice answers this by acting on `mousedown` as
  well (`engine/VerticalSlice/ui/main_menu.js`), and so does this one, with the
  pair deduplicated so one press cannot zoom twice. The driver therefore fires a
  press *without* a release — the path a player actually takes — and separately
  requires a full press-and-release to act exactly once.
- **The Sun took the observer.** The spawn fails the run if `setObserver` was not
  accepted, because the failure mode otherwise is a scene that quietly keeps
  lighting the wrong hemisphere.
- **The second Go.** A teleport must reach a complete spawn, not a stalled
  counter. See below for what was stalling it.

```powershell
$env:PYTHONPATH = (Resolve-Path game\tools).Path
python -m unittest discover -s game\tools\r1\tests -v
```

161 contract tests, none of which need the engine or a network. Seventeen are
props — that a bench survives a tile of a thousand trees, that a lamp is five
metres because lamps are, that a palm does not grow in Chamonix, and three that
exist because of bugs the first render showed: every prop the table can place is
on disk, no prop references an image it did not bring, and no prop keeps a
colour the kit chose. Fourteen are ground materials — that a lake beats the wood it sits in, that partitioning the
terrain creates no vertices, that every class is inside its published albedo
range, and that the water bitmap the collision reads is the same grid the eye
sees. Nine are the Atlas seam: that a named region beats the band it sits in, that a band
declares itself a band, that mid-Atlantic and the Antarctic plateau admit to
knowing nothing, that every point on the ellipsoid resolves to a profile with a
palette a generator can actually draw from, and that no profile can name a roof
shape the generator does not build.

The three cities above are each a smoke test, and all three run offline against
the cache — including across the v3 → v4 generator bump, because a tile's raw
observations are carried forward and the shared regional OSM source is keyed by
bounding box rather than by version:

```powershell
$env:HTTP_PROXY = "http://127.0.0.1:9"; $env:HTTPS_PROXY = "http://127.0.0.1:9"
python tools\play_world.py --smoke --spawn 4.8900 52.3730 --spawn2 10.1815 36.8065
```

Amsterdam to Tunis: the densest neighbourhood the project has cooked, then one
of the sparsest, in one run, with an empty worker log at the end of it.

## Attribution

Map data is © OpenStreetMap contributors (ODbL). Elevation comes from the
Copernicus DEM GLO-90 through Open-Meteo. The selection map is Natural Earth
(public domain) and is used for selection only — never as terrain. Asset
provenance and checksums are recorded in `assets/THIRD_PARTY_ASSETS.json`, and
attribution is visible in-game.
