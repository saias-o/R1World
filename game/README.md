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
The player, a scanned and rigged Rocketbox avatar, is controlled in third person, among a crowd drawn from the same library (see [The people](#the-people)).
**ZQSD / WASD** to walk, **Maj** to run, **Space** to jump, the mouse to orbit, **M** or **Échap**
for the map again, **Reprendre** to return where you were. A car is parked
beside you at every arrival and the streets have traffic on them: **F** gets in
and out of *any* car within reach, the same keys drive it and **Space** is the
handbrake — see [The car](#the-car) and [The traffic](#the-traffic). Airports
have their aircraft parked and military bases a helicopter: **F** takes those
too — see [Airports, aircraft and the bases](#airports-aircraft-and-the-bases).

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

For eligible mainland France/Corse tiles, the game requests an IGN RGE
ALTI bare-earth grid at the terrain's 41×41 resolution. At the Rue de Lobau
inspection point, the old Copernicus grid gave approximately 49.3 m and the IGN
service returned 34.63 m: conforming streets to the old grid alone could not
repair that difference. Invalid or uncovered IGN samples fall back as a whole
tile to Copernicus; `ground-elevation.json` persists both successes and fallbacks
and `elevationSource` records the answer. Cached elevation never revalidates.
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

### What `Play.ps1` needs

The executable, and nothing else: the world is generated inside it
(`native/gen`), so playing starts no Python and no worker. `Play.ps1` also
builds it, with the engine's own flags read from `engine/build-rel/build.ninja`,
through GCC response files so that no path with a space goes through
PowerShell's quoting. It needs MSYS2's UCRT64 `g++` and `cmake` and puts them
first on `PATH` (otherwise the link fails with a bare `ld returned 116`).

The engine is built with the game. `engine/build-rel` is an optimized Saida
build (RelWithDebInfo, `-O2 -g`). `Play.ps1` configures it when it is missing
and runs Ninja on it before every game build: that does nothing when the engine
has not changed and rebuilds only what an engine change touched. The engine's
own `engine/build` is its Debug tree, and the game no longer links against it.
It used to, and a Paris frame cost 35 ms of CPU where it now costs 4.5
(below).

It rebuilds **by itself** whenever a source, a generator header or the engine
library is newer than the executable, recompiling only what is stale (all 23
units take about 35 s, nothing to do takes 0.2 s): an executable older than its
sources is a game without the change you just made. `world.cpp`, the one unit
that includes the engine's headers, is recompiled whenever the engine library
changes, so that the two cannot disagree about a class's layout. `-Rebuild` recompiles
everything, `-BuildOnly` stops after building — which is how
`tools/play_world.py` makes sure a test never runs an old binary. There is one
build and one output folder, `generated/world-windows`.

The game's output, stderr included, goes to `cache/sessions/<id>/game.log` in
UTF-8. That line is the one that had broken: the engine writes its warnings to
stderr, Windows PowerShell turns each stderr line of a native program into an
error record, and under `$ErrorActionPreference = 'Stop'` the first one
(*validation layers requested but not available*) ended the script in 0.2 s,
before the game had drawn a frame, with an empty log. When the game exits with
an error, its `[error]` and `FAIL` lines are repeated in the console.

The generator's C++ tests (`native/tests`) cover foundations, intersecting
streets, building cut-outs, terrain conformance, observed markings, geometry
sharing, IGN cache reuse and no-data fallback. Visual QA exposed defects that these tests did not: successive
captures are under `generated/paris-*.png`. Capture mode now waits for all nine
tiles and their props, honours the engine's camera flags, and fixes the sun to
the scene epoch so load duration does not change the lighting. `Play.ps1` forwards
extra arguments to the game. Example, from `game/`:

`--profile <trace.json>` profiles the whole run (the engine's
`Engine::profileTo`): the trace opens in `chrome://tracing` or Perfetto, and the
log ends with the most expensive scopes, per frame.

```powershell
./Play.ps1 --smoke --spawn 2.3522 48.8566 --screenshot generated/paris-final.png --camera-pos '70.448,1.745,83.405' --camera-look '45,2,50' --after-frames 12
```

Known limits: twenty landmarks have their own models (see *The landmarks*), every other
building uses generic procedural façades; municipal Paris
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

### Progressive entry

Go does not wait for all nine tiles. The current tile has first priority;
neighbours follow by distance, including across the date line. Selecting a
destination on the map already prefetches it. If the selected point is
blocked and no safe point is found on the available terrain, the search waits
for neighbours before refusing the spawn. Walking into missing terrain remains
blocked until it is mounted.

The world service (`native/gen/service.cpp`) runs inside the game, on its own
threads: downloads and cooking never block the frame, and destination changes
replace pending work between tiles (an in-flight download is not cancelled).
Terrain, buildings and collision data are mounted first; trees and street
objects follow on the main thread with a **soft 2 ms per-frame budget**. No
assets are downgraded. Logs contain `[World streaming] go_to_play_ms` and
`mount_ms` for repeatable diagnosis.

While driving or sailing, the request queue covers the surrounding nine
tiles plus a corridor projected up to 45 seconds ahead (at most 25 tiles in
total). The imminent road tiles are prepared first; all immediate neighbors
remain in the queue. Each projected neighborhood gets its own bounded OSM
query so prefetch never mistakes observations from another area for local data.
The game mounts the next prefetched tile as the vehicle reaches its edge. The
resident geometry remains limited to the nearby ring, independent of the
larger prefetch queue. New areas can first appear as explicitly marked
simplified terrain when their detailed observations are unavailable.

Full-tile imports can still exceed a 16.7 ms frame. CPU asset decoding jobs
and incremental GPU uploads remain necessary for consistently smooth 60 FPS.

Nothing contacts a data service except the world service, and only for tiles
you actually asked for.

| Piece | Job |
|---|---|
| `ui/world.html` | the selectable map, drawn over a Natural Earth basemap baked offline by `r1/prepare_world.py` |
| `native/world.cpp` | the game: map input, floating origin, streaming, collision, the walk, the vehicles |
| `native/gen/` | the generator: `sources` fetches and caches OSM and elevation, `service` schedules, `cook` turns one tile's observations into meshes and a manifest |
| `native/gen/common.hpp` | the grid: metre-sized latitude rings, global, with no Mercator polar cutoff |

### A place you have been is yours

Everything downloaded stays on disk under `cache/world/`: each tile's own
observations (`osm.json`, `ground-elevation.json`) in `v<N>_<row>_<col>/`, and
the Overpass answers a neighbourhood shares in `sources/`. The key is the tile
and the version — no session id, no spawn point, no clock. Geometry is never
stored: the game cooks it again on every visit, so a generator change reaches
every place already visited. Quit the game, come back, return somewhere
already visited, and **nothing touches the network**.

If no observations exist and the network is unavailable, Go still opens a
playable, simplified tile. Its land/sea outline comes from the bundled Natural
Earth map; elevation is flat and local streets and buildings are absent. The
HUD says **Hors ligne : terrain simplifié** and the manifest records
`offlineApproximation: true`. When the network answers again, the tile is
cooked from real observations and replaces it.

Measured on the real cache: about **1 MB per tile**, nine tiles per spawn, so a
visited place costs roughly 10 MB and is free forever after.

The cached path and the simplified path can both be exercised with closed
proxies:

```powershell
$env:HTTP_PROXY = "http://127.0.0.1:9"; $env:HTTPS_PROXY = "http://127.0.0.1:9"
python tools\play_world.py --smoke --spawn 2.3522 48.8566    # visited: saved terrain
python tools\play_world.py --smoke --spawn -58.3816 -34.6037 # new: simplified terrain
```

`Service.a_visited_place_never_touches_the_network` in
`native/tests/test_world.cpp` holds the same guarantee where it does not need a
GPU: it points every source at a closed port and asks for a cached tile anyway.
Any future edit that re-validates a cached tile against a server fails there.

Folders from earlier generator versions are kept rather than deleted — their
raw observations are read by any later version instead of being downloaded
again. Deleting one costs only a re-download.

### When the data services say no

The tiles come from two free, shared services while the player waits, and both
answer 429, 502 or 503 under load and recover within seconds. One such answer
used to become a bare `HTTP Error 503` in front of the player, with a
60-second cooldown behind it on every other tile — a hiccup read as an outage.

`net::requestJson` (`native/gen/net.cpp`) separates a server saying *not now*
from one saying *no*: 408, 425, 429 and 5xx are retried with a doubling backoff
that honours `Retry-After` up to 20 s, and anything else (400, 403, 404) is
raised at once so the fetch moves to the next mirror instead of waiting on a
refusal. A mirror that fails is asked last for the next five minutes, and a
server that has not accepted the connection in ten seconds counts as failed.

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
deleted the cache by hand.

Overpass allows two concurrent queries per client, and the world service stays
inside that. `https://overpass-api.de/api/status` reports the slots you have
left.

**Go never waits for Overpass.** On 24 September 2026 a first visit took 53 to
94 s before the player could move, and the cook took 2 ms of it. Two reasons,
both fixed:

- The game asked for `Accept: application/json`. Overpass answers
  `application/osm3s+json`, so its main server ran the whole query and then
  refused it with a 406. That was twenty seconds of every first visit, before
  the mirror was even asked. It now accepts any JSON.
- The player waited for the answer. A first cook no longer waits for it at
  all. With no answer on disk, the tile is cooked from the ground alone
  (`quickGround`: Copernicus through Open-Meteo, one request, four seconds at
  most, a fifth of a second in practice), with Natural Earth's coast and
  nothing built on it. The manifest says `provisional`, the HUD says the streets
  and buildings are on their way, and the tile waits in `awaiting` for its
  query, then is cooked again the moment the answer lands. That second cook
  also brings IGN's finer ground in France, and the player is set back on it.
  A building that lands on the player moves him to the nearest free ground.
  First visits to Nice, Porto and Oslo: playable 0.36 s after Go.
  Tromsø's 481 buildings came 87 s later, with Overpass under load. With
  Open-Meteo's minutely quota spent (HTTP 429), the tile is flat until the
  network answers again, still playable in 0.36 s.

The urgent tile no longer sends its own query beside its neighbourhood's.
Nobody waits for it any more, and the two only slowed each other down on the
same server (86 s for the one tile, 75 s for all nine). A query that fails
says so in the log (`OSM-QUERY-FAILED`), since nobody may be waiting on it,
and its tiles are asked for again once the network is.

**The origin floats.** Plan §3 I1 says no ECEF coordinate ever reaches the
engine, and this is where that is enforced: the player's position is WGS84, each
tile owns its own tangent frame, and the renderer only ever sees metres relative
to an origin that is rebased every 350 m. Walking across a tile boundary, the
date line or a pole is therefore not a special case.

**The world tiles are still a preview LOD, and the manifest says so.** No
modelled openings, no waves on the water, no clutter beyond what OSM mapped as a
point. What they are no longer is *styleless*, no longer one green, and no
longer empty — see below. The detailed building generator, with modelled
openings, would exhaust the engine's vertex arena on a single city tile, so the
reduction stands; it is a scope reduction (§3 I0: reduce the scope, not the
correctness), not an approximation dressed up as detail.

## The region decides what the survey did not say

Until now an untagged building was three storeys, a flat grey roof and one
masonry colour, from Chamonix to Tunis. Ranks 5, 6 and 7 of the fidelity
hierarchy — heights and gabarits, facade materials, roof form — were answered
with a constant, and a constant is not a cheap approximation of a region. It is
the absence of one, and it is what made every city the same town with different
terrain under it.

The world's buildings now go through the Atlas (`assets/world/atlas.json`, read by
`native/gen/palette.cpp`), which holds **twelve named regions and twenty-two
continental bands**:

| Tier | What it claims | Written |
|---|---|---|
| `region` | a place someone described: Paris's legislated cornice line, Tunis's whitewash and parapet, Amsterdam's narrow steep-gabled plots | 12 |
| `band` | the smallest honest thing that can be said about a continent — a Nordic town is painted timber under a steep roof, a Saharan one is flat-roofed earth render | 22 |
| `none` | nothing. Open ocean, the ice sheets, the seams between rectangles | the fallback |

Every tile's manifest records which tier answered and how many buildings owe
it their height and their roof rather than owing them to a survey, because §3 I5
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

The per-tile budget (`kTileVertexBudget` in `native/gen/cook.hpp`, 120 000) is the sanity bound
underneath the runtime one — a single tile approaching a sixth of the planet's
entire geometry budget is a generator that has gone wrong, not a city that is
unusually dense. `Cook.a_dense_paris_tile_fits_its_budget_and_says_what_it_inferred`
(`native/tests/test_world.cpp`) holds a dense tile under that bound.


## The ground stops being one green

Every square metre of the planet used to be `Material("Ground", (.30,.38,.24))`.
The Sahara was that green. So was the Amazon, the tarmac of a Paris courtyard,
and the ice on a Chamonix glacier — and Amsterdam's canal belt had no canals,
because a tile only got water when it was *entirely* ocean.

§2 puts *sols : revêtements* at rank 9 and marks it "OSM partiel → règles",
which is the shape of the answer: `native/gen/terrain.cpp` classifies what OSM mapped, and
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
world looks like, and the 84% is the region talking. Every tile's manifest
carries `ground.measuredFraction`, so how much of what you are standing on
anybody actually looked at is a number and not a feeling (§3 I5).

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
query, and not one of them was ever planted. §2 puts vegetation at rank 8 and *mobilier urbain régional* at rank
10 — "forte signature culturelle" — and both were simply absent: a Paris street
had buildings, a road surface and nothing standing on it.

**Why this is affordable, and it is the whole design.** Saida's `MeshCache` keys
meshes by `AssetID`, so six hundred nodes pointing at `bench.glb` upload that
bench *once*. Measured on one alpine valley: sixty-four trees, one hundred and twenty-eight
primitive loads, and twenty-one distinct mesh ids for the entire scene. The
vertex arena — the thing that decides everything else here — therefore charges
per prop *kind*, not per prop:

> **9 390 vertices for the whole prop library. Once, for the planet.**

That is 0.9% of the arena for eleven models, against 841 035 for one
neighbourhood of Amsterdam. (The arena is the only thing instancing makes free:
a model's *file* is re-parsed per node, which is what later ruled the
photoreal trees out — see below.) Props are therefore scene nodes and are never
baked into the tile's meshes; baking them would charge the arena six hundred
times over and would not fit.

What a prop does cost is a node, and nodes are drawn. §5 caps L5 detail on
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

Tunis is the honest half of this. **Nothing here is invented** — every prop
stands on a point somebody surveyed, there is no scattering and no "a village
would have benches" — so where OSM is thin the world is thin, and the manifest
says by how much. Tunis will look emptier than Amsterdam until somebody maps
Tunis.

### Vegetation is measured, decimated — and drawn as cards

The project ships photoscanned Poly Haven trees. They are the best asset in the repository, and rule 1 of
[CLAUDE.md](../CLAUDE.md) exists because this feature first reached for a
low-poly kit instead of using them. It does not any more: there is no kit tree
anywhere, and a test refuses one.

What replaced that shortcut is `r1/decimate.py`, the §4 answer — the model
enters the game *decimated*, never exchanged. It works, it is deterministic, it
keeps whole needle clusters so no UV is ever averaged across an atlas, and it
spends its budget by water-filling so a trunk is not cut to 0.46% alongside a
canopy. And the measurement it produced is why a decimated scan alone is not
enough:

| | |
|---|---|
| `island_tree_01` as scanned | 1 303 928 vertices, 927 528 of them canopy |
| streamable size | ~12 000 vertices, ~500 kB per species |
| canopy that survives | **0.43%** — about a hundred leaf clusters out of 25 000 |

A hundred clusters is not a canopy. The tree keeps its trunk, its bark and its
proportions and loses the thing that made it read as a plant; a street of
skeletons is not an improvement on a street of nothing.

**Why the budget cannot simply be raised.** Saida deduplicates the *mesh* an
instance points at, but the scene loader opens and parses the referenced file
once per node — measured, 476 loads of one `broadleaf.glb` in a single
neighbourhood. A canopy that survives needs about 185 000 vertices and a 6 MB
file, which is 15% of the arena per species *and* thirty-six seconds a tile.

So trees are planted as impostors: `tools/bake_nature.py` bakes each
photoscanned species into layered cards (`assets/models/external/nature_cards/`)
that keep its scanned albedo and alpha, and `native/gen/scatter.cpp` plants them
where OSM surveyed a tree or a tree row, then fills mapped woods inside the
budget — surveyed trees always first. A city tree also carries a near model
(`nature_selected/urban_tree.glb`), switched by `LODGroupBehaviour`.

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
learned the same day, and §4 already had the answer: *un asset non conforme
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

§5 puts a vehicle in the L0–L4 band and drops mobilier, clutter and facade
detail above 15 km/h, and this is where the plan pays for itself rather than
being quoted. Above 4.2 m/s the frame stops spending its 2 ms importing street
furniture the player is about to leave behind, and the vegetation radii shrink
to 55% — 550 m of trees becomes 300, 65 m of grass becomes 36. Slow down and
both come straight back; standing still costs nothing.

### Seven vehicles, drawn by the project

`r1/vehicle_fleet.py` authors seven original, unbranded road vehicles in metres
— city car, saloon, SUV, off-roader, sports car, lorry and bus — each with a
near model and a far one without trim, switched by `LODGroupBehaviour`.
`assets/models/vehicles/fleet.json` lists their dimensions and vertex counts
(27 732 for the whole fleet) and the game reads it at start-up, drawing them
and their dimensions at 0.8 (see [Smaller, on purpose](#smaller-on-purpose)). The player's
car is the city car; traffic draws from the whole fleet, mostly cars, with
lorries and buses only on faster through roads.

This widens coverage rather than displacing anything (rule 1 of `CLAUDE.md`):
the project had no vehicle better than a Kenney saloon, which stays on disk,
unused. `test_vehicle.py` holds the provenance and keeps the tree species
pointing at the decimated photoscans.

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

No parked cars along the kerb, no passengers, no fuel, no damage. OSM maps no
individual vehicles and nothing here invents any beyond the traffic — the same
bargain the props make (§3 I5).

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
| `native/gen/scatter.cpp` (`buildLaneGraph`) turns the tile's OSM ways into a lane graph in engine metres | `Graph` drives on it |
| The tile's density decides how many cars it deserves | `Flow::setPopulation` spends it |
| Which side of the road this place drives on | `Rules::leftHand` mirrors the lane |
| A scene node per agent, painted, on the terrain | `Flow::pose` says where |
| The player's car, as an obstacle | agents queue behind it |

The add-on has its own tests, which run without an engine build or a project:
`g++ -std=c++20 -O2 -Wall -I../include test_traffic.cpp` in
`plugins/traffic/tests`, then run it.

### A graph per tile, cooked with the street

The lane graph is built by the generator at the same moment as the street mesh
it belongs to, from the same clipped ways, and it ships in the tile's manifest.
That is why a car cannot drive where no road was drawn: bridges and tunnels are
skipped by the street mesh, so they are skipped here too, and `service` ways —
driveways, parking aisles, alleys — carry no traffic at all.

Each tile gets its own graph and its own flow. A tile is evicted as a unit, so
traffic that outlived its tile would be driving on roads that are no longer
there; giving the flow the same lifetime as the roads makes that impossible by
construction rather than by bookkeeping. A car that reaches the edge of its
tile's roads retires there.

Nothing is invented, in the sense the props mean it: every lane is a way
somebody surveyed, every one-way street is tagged `oneway`, and a speed limit is
the `maxspeed` tag where there is one — 328 of them against 32 estimates on the
tile under the rue de Rivoli, and the tile says which is which (§3 I5).

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

### Ten paints

The ten body colours are listed under `carPaints` in `assets/world/atlas.json`
and read at startup — **not** written in the C++ that uses them, because rule 2 of
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

Budgets count vertices and indices, but an upload needs them in one piece.
After a teleport from Paris to the pole, the arena held 828 000 free indices
and its largest free range was 141 234. The next tile's surface asked for
153 570 and the game stopped on *failed to allocate index space*. The
evicted city had left its free space in pieces between the shared models
reloaded on arrival. The engine now packs resident geometry together when an
upload fits in total but not in one piece (`GeometryRegistry::compact`, SPEC
§4), and says so in the log. Each `mounted` line in the streaming log gives
the arena's use and its largest free ranges (`arena=…v/…i (largest free …)`).

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
with the land on its left. `native/gen/harbours.cpp` rebuilds the sea from it, per tile:
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
  tile first. Decks are walkable (manifest → `decks`).
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
boat. A boat keeps its hull length's handling (manifest → `boats`): drag
grows with the square of speed, the rudder needs way on, and the bow refuses
land — a boat runs aground rather than climbing a beach. Below 1.5 m/s, `F`
steps onto a bank or deck when one is alongside, or drops the player into the
water. ZQSD/WASD swim toward shore; `F` boards the same boat again when its
hull is within reach. A faster boat refuses the exit out loud. The sea is
continuous across tiles, so a boat can leave its harbour for the open ocean.

**Ships at sea.** The generator places up to 40 vessels from the shipped AIS prior,
the game's date and cached weather; optional live AIS observations refine the
prediction when available. They keep moving after appearing. A player can take
the helm with `F` from the water, or transfer from a nearby boat when their
speeds match. The same steering and throttle controls then drive that hull.

```powershell
python tools\play_world.py --smoke --sail --spawn 5.3698 43.2951   # Vieux-Port
```

## Airports, aircraft and the bases

An airport used to be whatever OSM's `landuse` said about its ground: grass,
with a terminal extruded on it in the stone of its region. It is now paved,
painted and parked, and every aircraft on it can be flown — an airliner, a
wide-body, a business jet or a helicopter, each handling as itself.

### What the airport is made of

The Overpass question is now version 6: it asks for `aeroway` ways (aerodromes,
runways, taxiways, taxilanes, aprons, helipads, stand lines), `military` areas,
stand and helipad nodes, and — with a box of its own, five kilometres wide —
the runways around the neighbourhood, because a terminal's stands are a mile
or two from the runways that say what the airport can receive.

A place visited before that is **not** re-asked. The first build did re-ask it,
and every Go in Paris then waited on ten-megabyte Overpass downloads: 21.7 s to
the first tile for 0.4 s of cooking, minutes for its neighbours. An answer from
version 5 on is now cooked at once (`kOsmBaseVersion`), and what version 6 added
comes as a separate *aero layer* (`<answer>.aero.json`, a few kilobytes) fetched
in the background; the tile says `airportsPending` meanwhile, and is cooked again
when the layer lands only if it holds an airport or a base, so no other tile
flashes. Measured on Amsterdam's cache, online: Go to play in 0.97 s.

Two network fixes came with it. A server that does not accept a connection in
ten seconds is treated as down, instead of waiting the answer's two-minute
timeout three times over; and an Overpass mirror that fails is asked last for
five minutes. While `overpass-api.de` was unreachable, every query paid that
wait again before reaching the mirror that answered.

`gen/airports.cpp` lays what OSM traced on the terrain with the streets' own
`Drape` — each triangle cut to the terrain triangle beneath it — eight
centimetres up so that a surface seen from a cockpit does not shimmer into the
ground:

- **Runways** at their tagged `width`, otherwise 45 m from 2.4 km long, 30 m from
  1.2 km, 18 m below (counted in `airports.widthsTagged`). Grass, earth and
  gravel strips stay the ground they are.
- **Taxiways** 18 m and **taxilanes** 12 m unless tagged, **aprons** and
  **helipads** as traced, in photographed concrete; none of them under a
  building, none of them on top of another.
- **Paint**, the zebras' 0.50 white and a 0.44/0.35/0.07 yellow: threshold bars,
  the designator (the `ref` tag's half within 20° of the landing bearing, since
  runways are named magnetically, otherwise the bearing), aiming points 300 m
  in, a dashed centre line, edge lines on wide runways, taxi centre lines, stand
  lead-in lines and a helipad's H.
- **The field** between them is a new ground class, `airfield` (mown grass),
  last in the Atlas's order so anything mapped inside the fence still wins.
- **Terminals and hangars** — `aeroway=terminal|hangar`, `building=terminal|hangar`
  — are a glass hall and a steel shed wherever they stand, 15 m and 12 m high
  under a flat metal roof unless their tags say otherwise. A tagged height stays
  the total height (rule 4); a test holds it.

Every footprint now also carries its top (`footprintTops`), which is what an
aircraft clears, stops against, or — a helicopter — lands on.

### What is parked there, and how it knows

Nothing in OSM says which aircraft stands where, so every one is inferred and
its manifest entry says so, with the element it was inferred from:

| What decides | How |
|---|---|
| What the airport receives | its longest runway within 4 km: from 2 800 m wide-bodies, from 1 500 m airliners, from 900 m business jets |
| Where | stand lines (`aeroway=parking_position` ways), the nose at the end nearer a building; stand nodes, facing the nearest building; aprons nobody drew stands on, in rows across their long side |
| What fits | the stand's room to its neighbour, clear of every building and every other aircraft, off the runways, out of the water; a business jet only on stands too small for an airliner |
| How many | three stands in ten stay empty, twelve aircraft a tile at most — a draw-call budget, since an aircraft is ten materials |

Measured on Roissy's nine central tiles: 16 airliners, 8 business jets and 2
wide-bodies; on Le Bourget's, business jets in rows.

**Military bases get one helicopter each and nothing else**, wherever the base is
(`landuse=military`, `military=base|barracks|airfield|naval_base`): on its
helipad if it has one, on an apron if not, otherwise on the first clear ground
from its middle. A barracks drawn inside a base is the same base; a range or a
danger area is not one; a military airfield parks no airliner. Bases are to get
an update of their own.

### Flying

**F** takes the nearest of a car, a boat and an aircraft, from its stand or
wherever it was left. The HUD gives speed, height above the ground and, for a
plane, the throttle.

| | Plane (airliner, wide-body, business jet) | Helicopter |
|---|---|---|
| Z / W, S | throttle up, down; S at idle brakes, then pushes back | forward, back |
| Q / A, D | nose wheel on the ground, bank in the air | turn on the spot |
| Space / ↑ | pull up — above rotation speed it lifts off | climb |
| Maj / Ctrl / ↓ | push down | descend |
| F | out, on the ground and nearly stopped | out, set down |

The handling is arcade and says so, as the car's is, with its numbers in
`assets/models/aircraft/fleet.json`:

| | top | rotate | stall | spool | roll | bank |
|---|---|---|---|---|---|---|
| Wide-body | 317 km/h | 58 m/s | 46 m/s | 0.22 | 24°/s | 32° |
| Airliner | 324 km/h | 55 m/s | 43 m/s | 0.30 | 32°/s | 36° |
| Business jet | 349 km/h | 42 m/s | 34 m/s | 0.80 | 95°/s | 70° |
| Helicopter | 162 km/h | — | — | 3 s spin-up | 75°/s yaw | climbs 10 m/s |

An airliner's engines take seconds to answer the lever and it needs half a
kilometre to rotate; a business jet does everything at once and turns on a
wingtip; a helicopter holds its height by itself when both climb keys are let
go. Turns are coordinated and half again as quick as real ones; a climb costs
half the speed gravity would take.

**Nothing crashes.** A building stops an aircraft where its nose, tail or a tip
touches it, and the HUD and the log say so; a plane stopped in the air loses its
lift and comes down; a hard landing is a stop. Water takes a ditching and a
helicopter sets down on it; a taxiing plane stops at the water's edge. The door
is refused in the air, at speed and on a roof, each out loud.

Streaming follows the aircraft like the car: the queue looks 45 s ahead of it.
At 350 km/h a jet crosses the ring of nine tiles in under twenty seconds, and
an unvisited place waits on Overpass; in the air it flies on over the last
surface it saw, and the HUD says *relief inconnu sous l'appareil* rather than
invent a ground.

### Drawn, not downloaded

Rule 1 was read before anything was made: the project had no aircraft of any
grade, and Poly Haven publishes none. So the fleet is authored in metres by
`tools/r1/aircraft_fleet.py` on the road fleet's writer: a wide-body (64 m, 60 m
span), an airliner (37.6 m), a business jet with a T-tail and a military
utility helicopter, unbranded, with a near and a far model each (1 000 to 3 800
vertices). White livery is 0.34, the brightest a sunlit surface can be here
without going white; `test_aircraft.py` holds every material to 0.35, the models
to their manifest's dimensions, and the named `gear` and `rotor-*` nodes the
game animates (the gear disappears above 30 m, the rotors spin up and down).

### What the driver tests

```powershell
python tools\play_world.py --smoke --fly --spawn 2.5700 49.0060   # Roissy: a jet
python tools\play_world.py --smoke --fly --spawn 2.1900 48.7725   # Villacoublay: a helicopter
```

`--fly` takes a business jet where there is one (an airliner otherwise), lines
it up on its longest clear run, takes off, is refused the door in the air, is
flown low at the nearest tall building and must stop against it, comes down and
steps out; then the nearest helicopter spins up, climbs, flies forward, sets
down and lets its pilot out — or, set down on a roof, keeps him in. Measured
offline: the jet lifted off at 43 m/s, 257 m from its stand, and stopped 2.4 m
up against a building; the helicopter climbed 20 m in 4.8 s and covered 56 m.
`R1WORLD_FLY_SHOT=<png>` with `R1WORLD_FLY_SHOT_AT=stand|climb|stop|heli-stand|heli`
photographs that moment from the chase camera.

### Not there yet

No air traffic, no taxi routing and no tower. Runways and aerodromes mapped as
multipolygon relations are not read. The world is still nine tiles around the
player and the fog that hides its edge (see *What this did not fix*): from a few
hundred metres up the ground ends in haze, beyond which only the far landmarks
stand.

## The North Pole, and the cold

The Pole is a place like any other: *Pôle Nord* on the map, or
`--spawn 0 90`, and the player stands on the pack ice at 90°N, on foot (no road
has ever reached it, so no car is parked there).

### Where the sea is frozen is measured

`native/gen/seaice.cpp` reads NOAA CoastWatch/PolarWatch's daily ASCAT ice
classification (Metop-C, 4.28 km, open to reuse): open water, first-year,
mixed or multi-year ice. A 48 × 48-cell window (205 km) around the 16-cell
block holding the tile is fetched once and kept in
`cache/world/seaice/ascat_<row>_<col>.json`, like every observation: a
place already visited never asks again. Only polar tiles with sea-level ground
ask at all (`|lat| ≥ 60`), and only on a first visit.

The satellite sees nothing within about 35 km of the pole. Those cells, and the
land mask, take the class of the nearest cell read (a breadth-first flood, in
index order), and the manifest counts them (`seaIce.cellsFilled`: 319 of the
2 304 around the pole on 22 September 2026). With no reading, from the
network or the disk, a static climatology answers (ice north of 80°N, south of
70°S) and says `inferred`. The reading is fetched on a thread of its own: the
tile is cooked from the climatology at once and cooked again when the reading
lands, like the aero layer, so a slow server never holds up an arrival.
PolarWatch answered in 3 s one hour and failed with 502s the next while this
was written.

### What the pack looks like is synthesised

Floes, leads, pressure ridges, snow dunes and melt ponds are one deterministic
function of a position on a polar stereographic plane (`iceAt`): no seam at
the pole, none between tiles (a test holds two neighbours equal along their
edge, and the pole equal from both sides). Floes are the cells of a warped
Voronoi at 320 m. The boundary between two floes is a lead, a ridge or a closed
crack, drawn by the pair's hash. Leads are open in summer, a third open at
freeze-up and mostly frozen over in winter. Ridge sails are 0.9–3.8 m high,
rounder on multi-year ice, heaped with slabs as thick as the ice they broke
from. Melt ponds are blue in summer, frozen and veiled by the first snow in
autumn. Concentration thins the floes out towards the ice edge, and age
decides freeboard, snow, hummocks and ridge height. The season comes from the
reading's date, never from a clock (§3 I3). The manifest's `seaIce.pack` says
that all of this is drawn, not observed.

A frozen tile is a 161 × 161 grid (3.5 m: a lead is metres wide) drawn as
one mesh with the clean snow. Each vertex is tinted to what its point is, so a
pond's shore or a lead's edge is a gradient rather than a staircase of
triangles. Only the floor of an open lead is its own mesh, under a calm water
node. That grid is also what the player walks on (`elevations`), and the
leads are water to swim in (`water`). A tile is 42 700 vertices and 179 000
indices, cooked in 30 ms. Twelve stand around the pole (the three rows around
a player no longer cover what he sees there, so `nearby` takes the tiles
nearest in metres), which is 513 000 vertices and 2.1 million indices. The
arena's index limit (3 145 728) is now counted beside its vertex limit, and a
tile past either is refused out loud.

The surfaces are derived from the photographed snow already on disk
(`tools/r1/sea_ice_textures.py`, rule 1). `snow_clean` is Snow014 without the
meadow's grass tips and roughened to 0.85, because at 0.5 a clear sky's
zenith mirrored off it and it read as water. `sea_ice` is the same scan at a
third of its contrast, glazed. Albedos are measured (`atlas.json` →
`ground.seaIce`): snow 0.83–0.89, bare ice 0.50–0.71, young grey ice 0.21–0.28,
frozen ponds 0.36–0.53, melt ponds 0.10–0.33 (Perovich et al. 2002; Brandt et
al. 2005).

### To the horizon

Past the tiles the pack goes on: a 40 km disc (`buildFarPack`), snow or water
as the reading says, curved with the Earth (I2). It sinks half a metre a
kilometre below the floes, so the tiles always win where they are, and is
rebuilt after 2 km. Over it the camera's far plane follows the horizon (4.7 km
at eye height, 5 km everywhere else, as before), and the fog is the measured
visibility (Open-Meteo, Koschmieder). That was 23 km at the pole on the day
this was written. Elsewhere the visibility only ever thickens the 5 km haze:
clearer air would only show where the streamed world ends.

### Weather you can see

The local conditions now carry visibility, wind, snowfall and snow depth. When
it snows, flakes fall around the camera, carried by the measured wind. When
the wind passes 6 m/s over snow, it drifts along the ground. Both are dimmed
with the daylight, which the particles do not receive. When Open-Meteo says 3
cm or more of snow lies, the ground and the roofs of every resident tile wear
the photographed snow, and take their own surfaces back when it melts. Streets
stay clear, and so do water, glacier, snow and ice.

### Walking over the pole

Headings are angles in the origin's frame, the frame the camera draws in, and
each step is turned into the local east and north where it starts
(`onward`). Far from a pole this changes nothing measurable. Near one, a
heading kept against the local north walked in circles around the pole and
turned back at it. When the origin moves every 350 m, every heading the player
owns turns with it. `advance` is now a rotation of the unit vector rather
than an `asin`: at the pole, `cos(d)` of a 5 cm step rounded to exactly 1 and
the player could not step off it.

Two bugs outside the ice turned up on the way. Open-Meteo answers `deflate`
when allowed, in a form WinHTTP aborts on, which took the elevation and the
weather offline everywhere outside France. `net.cpp` now accepts gzip only.
Overpass refuses a longitude past ±180 (HTTP 400), which every tile at the
antimeridian, and every tile at the pole, asked for in its wider runway box.

```powershell
python tools\play_world.py --smoke --spawn 0 90                              # the Pole, on foot
python tools\play_world.py --smoke --spawn 2.3522 48.8566 --spawn2 0 90      # Paris, then the Pole
generated\tools\r1test.exe SeaIce
```

### Not there yet

Sea ice in the sea cells of a coastal tile (the fast ice of a winter fjord),
the Antarctic's measured ice (the climatology answers there), and relief
seen from afar on land. The sky shows a faint vertical seam where the
equirectangular photograph wraps. It is the engine's, turns with the Sun, and
shows most on an even grey sky.

## The people

The player and everyone in the street are people now, not a kit figure: the
Kenney block character is gone, and in its place are thirteen scanned and
rigged avatars from Microsoft's **Rocketbox** library (MIT), everyday clothes
only, six men and six women in the crowd and one for the player.

```powershell
python -m r1.humans        # from game/tools, with Blender 4.2 installed
```

### One library for the player and the crowd

Rocketbox is 115 characters on one 3ds Max biped and 400-odd motion-capture
clips for that biped: walks, runs, waits, a telephone, a conversation, a
chair. `r1/humans.py` fetches what it needs at one pinned commit into
`cache/downloads/rocketbox/`. `r1/humans_blender.py` then does the rest in
Blender: metres, feet on the ground, facing +Z. It folds the 28 face bones into
the head (nobody in a street is close enough to see a lip move) and retargets
every clip by copying each bone's world rotation. The pelvis position is scaled
by the two skeletons' legs, measured as thigh plus shin, because a clip's first
frame may already be sitting. `assets/models/humans/humans.json` says what came
out.

The library has no jump. The player's is the flight phase of the sprint (the
frame where the lower foot is highest), held for the 0.74 s the game's jump
lasts.

| | triangles | exported vertices | textures |
|---|---:|---:|---|
| Player | 7 364 (the whole scan) | 4 584 | 1024 / 512 px |
| Passer-by, near | 2 400 | 1 571–2 010 | 512 / 256 px |
| Passer-by, far | 500 | 432–617 | same |

A street pedestrian of the PS2's last years was 1.5–3 k triangles. The thirteen
together are 33 181 shared vertices, which `test_humans.py` holds under
40 000, the share of the arena that the trees, props, boats and fleets leave.
The colour maps are the scans' own, reduced. Their mean albedos (skin and cloth
0.05–0.28) are measured, and the bake refuses a map above 0.35 (rule 2).

### Smaller, on purpose

People are drawn at 0.8 of their scanned size, and so are the road vehicles
(`kVehicleScale` in `native/world.cpp`). The player now stands 1.46 m and the
city car is 2.9 m long. This was the player's call, made looking at them in
the streets. The fleet's dimensions are scaled with the models, so doors,
cameras, parking and the traffic's gaps agree with what is drawn. A clip
drawn at 0.8 covers ground at 0.8 of its captured speed. The player's run and
sprint are retimed in the bake, so 2.8 m/s and 7 m/s (Shift) keep the feet on
the ground. Passers-by walk at exactly their clip's pace, 0.81 m/s for the men
and 0.97 m/s for the women.

### Where they walk is surveyed

`native/gen/crowd.cpp` cooks a walking network into every tile (the manifest's
`crowd`):

- **Sidewalks.** It uses the ones the streets are drawn with, tagged or
  inferred exactly as `streets.cpp` infers them, walked down their middle.
- **Footways.** OSM's footways, pedestrian streets, paths and steps.
- **Cuts.** Every stretch that enters a building or a carriageway is cut out.
- **Joins.** Loose ends are joined to the nearest walk of another street:
  round the corner, or across the road. That is where people cross, stepping
  down the kerb.
- **Benches.** The benches the props actually placed seat two each, at the
  bench's own seat height.

### How many is inferred, and says so

The tile asks for a number from what it holds: pavement, shops and offices,
bus stops, crossings and signals, and buildings. The number is capped at 40
people per tile, and the manifest keeps the inputs and says that the number is
inferred (I5). At run time it is multiplied by the local solar hour. There is
nearly nobody before dawn; the morning rush, lunch and the evening peak are the
busiest. Rain or falling snow halves it. The whole neighbourhood shares 60
people, and the tiles nearest the player fill first, as the traffic does. A
commercial avenue at noon is busy, a village is quiet, and the open country at
night is empty.

### What they do

- **Walk.** People walk, keeping to the right so that two meeting pass each
  other, and choose a street at every junction.
- **Stand, phone, talk.** They stop to stand, wait or look at a telephone.
  Some stand in pairs, facing each other, talking.
- **Sit.** Some sit on the benches for half a minute to a minute and a half.
- **Make way.** They stop for the player rather than walk through him, and run
  for it when his car comes through at speed.
- **Appear out of sight.** Newcomers appear out of sight: behind the camera, or
  further than 70 m. People leave beyond 150 m.

The simulation (`r1::Crowd`) needs no engine and `test_crowd.cpp` runs it
headless.

### What they cost

- **Shared parts.** Each person is a pooled node with its own animator over
  its avatar's shared meshes, rig and clips.
- **Levels of detail.** The near model is drawn while a person stands taller
  than 3.5% of the screen, about 35 m away, and the far one beyond.
- **Animation.** Within 12 m a person is posed every frame. Further out the
  pose is resampled at 15, 8 and 4 Hz and held in between. That is the
  engine's `Animator::setPoseRate(hz, PoseRateMode::Hold)`, added for this and
  for any game with a crowd. The engine's older reduced rate still blended
  every bone on every frame, and so saved almost nothing.

Measured on the Paris smoke with `--profile`, which any Saida executable now
accepts: it writes a Chrome trace and a scope-by-scope summary to the log. A
frame is 4.5 ms of CPU work plus 12.8 ms waiting for the frame cap. The 63
animators cost 0.09 ms, and 4 frames out of 569 went over 33 ms on arrival.
Two engine changes got there:

| | before | after |
|---|---|---|
| Engine build the game links | Debug (`-O0`) | RelWithDebInfo |
| CPU per frame, Paris, 60 people | 37.3 ms | 4.5 ms |
| Animators (60 people and the player) | 2.97 ms | 0.09 ms |
| Arrival frames over 33 ms | 216 of 278 | 4 of 569 |

### Not there yet

People do not yet look both ways: a traffic car does not stop for someone on a
crossing. Nobody is a child, nobody dresses for the region, and nobody
enters a building (the Interior update).

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
belonged to the destination. The game also fetches a local current forecast
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
every time after the first. It went untested until it broke:

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
  counter.

```powershell
sh native/build_tools.sh; generated\tools\r1test.exe        # from game: the generator
python -m unittest discover -s tools\r1\tests -t tools      # from game: the authoring tools
```

None of these needs the engine or a network. The C++ tests hold the world:
props (a thousand lamps do not squeeze out five benches, no region plants a kit
tree), ground (water beats everything it sits inside, every class is a
plausible albedo, partitioning the terrain creates no vertices), the Atlas seam
(a named region beats the band it sits in, outside every rectangle admits it),
buildings, streets, harbours, traffic, airports and the cache. The Python tests hold the
assets the project authors: skies, the Sun, landmarks, the road fleet and the
aircraft, and that every prop and tree points at an asset on disk.

The three cities above are each a smoke test, and all three run offline against
the cache — including across the v3 → v4 generator bump, because a tile's raw
observations are carried forward and the shared regional OSM source is keyed by
bounding box rather than by version:

```powershell
$env:HTTP_PROXY = "http://127.0.0.1:9"; $env:HTTPS_PROXY = "http://127.0.0.1:9"
python tools\play_world.py --smoke --spawn 4.8900 52.3730 --spawn2 10.1815 36.8065
```

Amsterdam to Tunis: the densest neighbourhood the project has cooked, then one
of the sparsest, in one run.

## Attribution

Map data is © OpenStreetMap contributors (ODbL). Elevation comes from the
Copernicus DEM GLO-90 through Open-Meteo, the weather from Open-Meteo, and the
sea ice from NOAA CoastWatch/PolarWatch (Metop-C ASCAT ice classification). The people are
Microsoft Rocketbox avatars and motion capture (MIT, `assets/licenses/Microsoft-Rocketbox-MIT.txt`). The selection map is Natural Earth
(public domain) and is used for selection only — never as terrain. Asset
provenance and checksums are recorded in `assets/THIRD_PARTY_ASSETS.json`, and
attribution is visible in-game.


## The landmarks

Everything in this world is generated from a description, and until this
section that included the Eiffel Tower: OSM maps it as `building=tower`,
`height=330`, and the building chain extruded a 125 m square 330 m into the
sky. Twenty places now have their own models (`r1/landmarks.py`): the Eiffel
Tower, the Statue of Liberty, Big Ben, the Colosseum, the Taj Mahal, the Giza
pyramids, Christ the Redeemer, the Sydney Opera House, the Burj Khalifa, the
Empire State Building, the Leaning Tower of Pisa, the Arc de Triomphe,
Notre-Dame, the Sagrada Família, the Brandenburg Gate, St Peter's, St Basil's,
the Parthenon, Tokyo Tower and the Petronas Towers.

They are still descriptions. Each is a recipe of a few dozen lines in the
vocabulary of `r1/sculpt.py` -- a lathe for a dome, a lattice for a tower, a
wall with its arcade cut through -- regenerated identically on every machine
(§3 I3). No downloaded model and no photogrammetry, so no licence to carry and
no asset of a lower grade (CLAUDE.md rule 1). The anchor and bearing are
measured on the OSM element found by its `wikidata` tag, the height is the
official one, the ground under the anchor is read from the same terrain source
the tile draws; three bearings OSM cannot give (Liberty, Christ, St Basil's) are
inferred and the manifest says so. The OSM trace of the landmark is not
extruded; its neighbours are.

**Three levels of detail, one recipe.** A sculpture built inside
`sculpt.detail(1)` or `detail(2)` asks each primitive for less: fewer sides,
no opening or member thinner than a pixel at that distance, no texture (from a
kilometre a photograph is its average, which the albedo already is). Only
where a shape changes character does a recipe intervene: a lattice tower is a
solid silhouette from afar, a stepped pyramid a smooth one.

| Level | Drawn | Budget (vertices) |
|---|---|---|
| 0 | by its own tile, while that tile is resident | 32 768 |
| 1 | beyond the resident tiles, to 1.8 km | 12 288 |
| 2 | to 5 km | 4 096 |

`python -m r1.landmarks` (from `game/tools`) bakes the three levels into
`assets/world/landmarks/` with `landmarks.json`; `native/world.cpp` shows a far model exactly
when the near one is not there, places it like a tile at every rebase, and
counts it in the resident budget. `tools/landmark_preview.py` draws a contact
sheet of any level without the engine.

**The haze.** Nothing of this could be seen: the fog was a 1 km visibility
(`fogDensity 0.0035`), chosen to hide the edge of the 3x3 neighbourhood (see
*What this did not fix*). It is now a 5 km visibility (`0.00078`, Koschmieder's
3.912 / V), an ordinary city day, and the camera's far plane stops at 5 km with
it. The trade was taken knowingly: beyond the resident neighbourhood the world
still ends, and that edge is now fainter haze rather than a wall of it. The
horizon rings of §5 (L1/L2) are what will fill it.

Known limits: where the terrain falls back to Copernicus GLO-90 through a 7x7
grid, summits are shaved (Corcovado reads 565 m instead of about 700 m), and
the Christ stands on the terrain the game draws, not on the real summit.
Bridges (Golden Gate, Tower Bridge) are not on the list: roads are draped on
the ground and a deck would not carry the car.
