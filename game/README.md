# R1World game project

This directory is the playable SaidaEngine project. Its one scene,
`scenes/earth.scene` (the project's `mainScene`), answers one question: can you
pick any point on Earth and walk there?

`docs/PLAN.md` holds the thesis, the invariants and the next updates;
`CLAUDE.md` holds the working rules. This file describes what exists today,
how it works and what was measured.

The current game version is **0.1**. Its release is being prepared; the in-game
Settings → Credits panel displays the version. The long-term animal photography
collection is planned in `docs/PLAN.md` and is not part of the current version.

Documentation reviewed on 19 October 2026. The current generator is v56;
the validation records below are dated observations, not a claim that every
location or target machine is qualified.

### Current verification — 20 October 2026, the arrival

The generator suite passes **447/447** (`Arrival` included). The menu smoke,
the Paris smoke and `--dive` (online at 1600 × 900, offline with the proxies
closed) pass. The Python checks run 82 here: 72 pass, 2 skip and 8 cannot import
numpy, which this machine's Python lacks; `test_parachute` passes. The Paris
smoke now reaches **Reprendre** through the settings' Destination tab, where
the globe menu put it.

### Current verification — 19 October 2026

The generator suite passes **427/427** from the repository root with
`game/generated/tools/r1test.exe --game game`. Engine native CTest passes
**100/100**. Python asset/tool checks pass **121/121**, with no skipped tests.
The GPU streaming suite, portable media lifecycle, vehicle runtime, motorcycle
poses and local radio lifecycle also pass.

The divided-bridge defect is fixed: overlapping inferred pavements no longer
leave parapets between parallel carriageways. Reversed axes, separate heights,
distant decks, crossing roads and overlapping carriageways have regression checks.
The generator is v56, bridge revision 3. The resident target test now uses a
deterministic 121-building scene with a feasible reduction target; immutable
surveyed geometry remains protected when a soft target cannot be met. Material
checks require the intentionally matte vegetation ground, and bundled-asset
checks respect `--game`. Pure tree-asset checks no longer require an OpenGL bake
dependency just to import their helpers.

The reference PC is the user's Ryzen 7 5700U / NVIDIA MX450 2 GiB laptop,
with about 7.4 GiB usable RAM, connected to AC. The performance target,
set on 9 October 2026, is sustained **30 fps at 1920 × 1080 everywhere in
the world**, a **33.33 ms whole-frame budget**, including streaming and
gameplay on foot, in vehicles and inside buildings. Dense cities, rural areas,
forests, coasts, mountains and polar regions all fall under this target.
This remains a design target, not a demonstrated whole-world guarantee.
On 9 October 2026 the user removed the dedicated fluidity/qualification
campaign from the backlog after the tree Far billboard and tighter Mid range
were delivered. Historical measurements below retain their actual hardware,
observed frame rates and limitations.

## Playing

On Windows, double-click `Play.cmd` in the repository root to build and play.
The launcher limits compilation to two jobs when Windows denies the system
memory query; `R1_BUILD_JOBS` can explicitly choose the compilation job count.

```powershell
powershell -File game\Play.ps1
```

The main menu plays `assets/audio/menu/life.mp3` once per application launch
through SaidaEngine's `AudioManager`, after a three-second delay, without looping.
Returning to the menu or reloading its UI does not replay it; restarting the game
does. The existing
background music continues independently. Three small white subtitles with a
black halo appear above Go/Settings at 0–2 s, 2–4.5 s and 4.5–8 s, measured from
the startup sound on the monotonic clock; they disappear after eight seconds.

A map of the Earth opens. Search for a city or commune (Photon, while online),
pick a suggestion, click the map or type coordinates, then press **Go**: the
globe flies down to the place, fades into the world and drops the player there
by parachute ([The arrival](#the-arrival-from-go-to-the-ground-20-october-2026)). The
whole-Earth image is a 4096 × 2048 Natural Earth overview; zooming switches to
OpenStreetMap tiles at their native resolution, up to level 19 (**Vue rue**).
Map tiles are cached in `cache/map-tiles/osm/`.

The zoom buttons advance one level per gesture, including a long hold. Menu
actions run on `mousedown`; their matching `click` is deduplicated without a
time limit. Saida preserves sampled mouse button history across UI consumption
so a held button is not mistaken for a fresh press on every frame.
The WebCanvas bridge also leaves click generation to RmlUi instead of adding a
second artificial click on release. Verified on 13 October 2026 with the real
menu zoom smoke, Saida's input/UI tests and native runtime contract. Native CTest now passes 100/100, including crash-reporter build identity checks.
The Web player and authoring WASM builds also pass, as does the authoring
Node smoke; these build checks do not claim browser menu interaction coverage.

### The arrival: from Go to the ground (20 October 2026)

**Go** no longer cuts to the street. For ten seconds the globe flies down to the
place chosen; at ten its satellite picture fades into the world it was showing,
the player falling through it, Fortnite-style; at twenty he opens his parachute
**fifty metres above the ground chosen** and lands it himself. He steers from
the moment the world shows, before the canopy too. The rules are
`native/arrival.hpp` (held by `r1test Arrival`); `native/world.cpp` draws them.

- **The flight.** Van Wijk and Nuij's optimal zoom path, the one map viewers
  fly: far places are flown to over the globe, near ones descended onto. It is
  eased in, and eased out so that the camera arrives at the fall's own speed
  (56 m/s) rather than stopping. The map layer is off: the globe shows the
  Sentinel-2 imagery only, credited on screen while it shows.
- **The hand-over.** At ten seconds the world's camera is the globe's: straight
  down, north up, the globe's lens (45° for the 1440 × 900 interface) and its
  off-centre framing (a 3.8° tilt). While the globe fades (2.5 s) it is moved
  to that camera every frame, so the picture going is the picture coming. The
  fading globe writes no depth, so the world's haze and shading do not change
  when it goes, and under complete imagery its base layer is skipped, so it
  dissolves as one layer. The camera then comes down behind him by fifteen
  seconds (64°, 7 m, looking down 62° high up and 40° near the opening).
- **The fall.** Belly to earth at 56 m/s, tracking from 17 s down to 32 m/s;
  the canopy opens at exactly 20 s, 50 m above the ground chosen (the fall is
  a function of time, never of the keys). Over higher ground — a hill, a tower
  — it opens earlier, higher the faster he still falls (86 m at terminal
  speed), so the wing always has the height it needs. He starts 250 m south of
  the opening point and an autopilot flies him to it, arriving as the canopy
  opens; the first key hands him over for good. WASD / ZQSD then track forward
  at 24 m/s, back at 8 and sideways at 12, the air taking 1.2 s to turn him;
  the mouse looks round him while the autopilot flies and turns him after.
- **The canopy.** "Parachute" by Poly by Google (CC BY 3.0, chosen by the
  project owner from poly.pizza), normalised by `tools/r1/parachute.py`: its
  kite bar removed and its lines brought onto his shoulders, its painted
  texture replaced by five flat measured albedos (red, yellow, blue and black
  ripstop at 0.30 or less, white lines at 0.30; `tools/r1/tests/test_parachute.py`).
  It opens out of the container as a snake of fabric in 1.6 s, never harder
  than 4.5 g, then flies at 9 m/s and sinks at 4.5: W / Z dives it (12.5 and
  6.5 m/s), S brakes it (4 and 3), A / D or Q / D turn it at 60°/s, the mouse
  turns it toward where he looks at 80°/s at most. Left alone it flies him to
  the ground chosen. Below 4 m he flares by himself: Paris, 8.5 s under the
  canopy, landed at 1.9 m/s, within 1.2 m of the point when nobody steers. His
  feet are the character body all the way down, so a roof is landed on and a
  wall slid along; deep water is swum in.
- **His body.** Engine IK on the Rocketbox rig: the box position in free fall
  (upper arms out, forearms up beside the head, knees bent, head lifted), then
  hanging under the wing, hands up on the toggles — the one of the side he
  turns to pulled down, both under brakes.
- **The sounds** are the project's own (`tools/r1/arrival_sounds.py`, seeded,
  CC0): the roar of free fall rising as the globe comes down, the rustle and
  crack of the opening, the fabric's flutter. The menu's music gives way to
  the wind two seconds before the world shows.

**The world behind the globe.** Choosing a point already prefetches it; Go
requests it again. As soon as the ground under the point is cooked he is put in
the air over it and the world comes alive behind the opaque globe: tiles,
buildings, props, traffic, crowd, the relief to the horizon. It streams from
the ground chosen while he falls within 450 m of it, steering or not (a row
further would evict three of the tiles cooked for him), from him beyond. The
fade never finds it unready: the zoom's last seconds slow smoothly to a stop
over the drop zone and resume as smoothly once the tiles the fade shows —
projected into the actual window, including its letterbox, and the one he lands on — are
mounted with their ground, buildings and collisions; then, for at most 25 s
more, their OpenStreetMap streets, trees and street furniture, textures and
the satellite picture's finest level. The rest of the neighbourhood starts
cooking during the fall. A
machine short of memory cooks fewer tiles at once. Visible tiles are requested
first (five in the Paris hand-over, rather than eight selected by the old
650 m radius); optional props, crowd and far relief start after their ground,
buildings and collisions are ready. If memory pauses cooking, the remaining
optional wait ends and the neighbourhood continues streaming during the fall.
The menu prewarms only models used by the selected tile, rather than retaining
the entire catalogue of every biome's vegetation, boats and signs. A network
exception while fetching map imagery remains a transient failure with its error
message; it never writes a permanent `.none` cache marker. What it waits
for is written under the caption and logged every five seconds
(`[World arrival] the fade waits … for …`); the log says what the fade went
without, if anything. While the globe covers it nothing of the world is drawn
(the far plane is a metre and the relief rings are hidden), until it is ready
and half a second before the fade.

Loading follow-up: cached Paris, offline, 640 × 360, completes the full dive
with five visible tiles mounted before the fade, and the other four requested
afterwards. The world appeared 32.68 s after Go on this run: the memory wait
no longer stalled indefinitely, but cooking the visible ground is still not
instantaneous. Menu smoke and all 19,872 projection/coverage checks pass;
the 1600 × 900 imagery capture still joins continuously. The offline run
created no permanent absence markers for network exceptions.

The map opened before the ground is ready calls the arrival off; after, it
pauses it, and **Reprendre** resumes it. A headset (`--vr`) and the smoke tests
still spawn at once; `--dive` flies the smoke through the whole arrival.

Measured on 20 October 2026 on the reference machine, cached Paris, hidden
window, with 2.9 GB of the 7.4 GB free before the game started (the world
service paused cooking for want of memory: the zoom waited 17.8 s online and
7.6 s offline). `--dive` passed both online at 1600 × 900, local noon, and
offline, proxies closed, at the hidden window's 640 × 360. Frames over 33 ms
of all frames, worst in brackets:

| Phase | 1600 × 900, noon | 640 × 360, offline |
|---|---|---|
| Globe flight and wait | 32 / 1 635 (493 ms) | 27 / 1 043 (505 ms) |
| Fade (2.5 s) | 66 / 70 (45 ms) | 2 / 107 (33 ms) |
| Free fall | 174 / 178 (88 ms) | 29 / 295 (52 ms) |
| Canopy | 113 / 261 (49 ms) | 3 / 357 (33 ms) |

The fall's original GPU bottleneck was the relief to the horizon
(`TerrainRingsFeature`): about 25 ms from the air and 12–15 ms near the ground.
Saida now rejects whole cells covered by resident-ground holes before
rasterization. A compute invocation per cell checks the morphed triangles,
with a screen-space guard for MSAA; the vertex shader reads its flag and emits
a degenerate cell. Boundary cells keep the original fragment clip, derivatives,
PBR, water and morph blending. The engine knows only terrain holes.

Before/after measurements on the same MX450, 1600 × 900, cached Paris at local
noon, with captures at 17 s and 26 s after Go: the last three wall-clock seconds
before each capture, excluding the final three capture/readback frames. These
windows avoid mixing the hidden globe into the relief's cost. Times are ms;
parentheses give p95 per call.

| View | Rings before | Rings after | Total GPU before → after |
|---|---:|---:|---:|
| Free fall, before the 17 s picture | 30.01 (33.42) | 4.26 (6.56) | 52.50 → 20.72 |
| Canopy, before the 26 s picture | 11.93 (15.17) | 2.20 (4.87) | 29.24 → 19.25 |

The feature pre-pass, including the new culling dispatches, costs 0.23–0.24 ms
in these after windows (0.06–0.07 ms before). Traces and PNGs are in
`cache/terrain-rings-perf/`: `before-dive` / `after-fall` and `before-canopy` /
`after-canopy`; `summary.json` holds the distributions. There were 8–9 resident
tiles and cached relief levels 0–2 in these runs. The live pictures were
inspected at the requested arrival times; loading, moving actors and the real
frame clock make them unsuitable for byte equality.

The engine's fixed-content 1.8 m eye-height benchmark also improves: rings
21.79 → 15.25 ms, total GPU 23.01 → 16.53 ms, with a 0.28 ms pre-pass. Its
500 m view's total GPU falls from 30.05 to 1.38 ms. Thirteen fixed-camera image
pairs compare **every pixel exactly** against fragment-only clipping, covering
both hole windings, morphing, relief, rain, water, grazing/subpixel cells and
hole removal between frames. Native CTest passes 105/105 and the generator
suite 447/447. The final offline `--smoke --dive` passes at 1600 × 900: canopy
open at 20.003 s, 50 m over the target, landing at 1.89 m/s. These GPU gains do
not qualify sustained 30 fps everywhere.

Behind the opaque globe the old rings cost 26 ms for no picture, which is why
they remain hidden there.

Detail loading, 21 October 2026: each resident satellite tile is displayed
immediately, with cached parents/children covering missing detail. A slow edge
tile no longer blocks an entire level. Hidden detail layers upload ahead of
the flight; lookahead follows the next 0.75/1.5 seconds of the existing path.
The whole-Earth view no longer queues a world-sized level-7 detail grid.
Recent GPU tiles survive gameplay and scene trims for immediate Escape/M
reopening; disk images remain permanent and cached menu views do not fetch
unused neighbouring levels. Standard targets 320 retained tiles and up to 96
lookahead tiles; toaster targets 128 and 24, with visible tiles taking priority.
The flight, world streaming budgets and existing toaster gameplay settings
are unchanged. `r1_menu_preview` accepts `cache-test` or `cache-test-toaster`
in place of `imagery` to verify hidden uploads, retention across a scene trim
and immediate reopening with the same GPU objects, using local images only.
The 19,882 projection/coverage checks and both cache presets pass. The full
Paris dive also passes at noon with closed HTTP/HTTPS proxies and
`R1WORLD_SMOKE_QUALITY=toaster` (which now applies to `--dive` too): canopy at
20.003 s, landing at 1.89 m/s. This run used an RTX 4070; it does not qualify
MX450 frame timing. Logs are in `generated/menu-cache-validation/`.

Magnified satellite tiles filter across their neighbours, including diagonal
corners and the longitude wrap. Separate clamped textures had stopped at
their own edge texels: the adjacent pixel centres are still one source pixel
apart (about 6 m at level 14 in Paris), so jumping between them made a visible
step during the last seconds of the flight. The globe now interpolates those
same four source texels on both sides of the boundary, in linear colour.
Minification keeps the existing mipmapped, anisotropic sampler; a neighbour
not yet resident falls back to the tile's clamped edge. The imagery-only
preview reproduces this view without loading the world:

```powershell
cmake --build generated/world-windows/build --target r1_menu_preview
generated/world-windows/r1_menu_preview.exe . cache/arrival-tiles.png main 48000 2.3522 48.8544 30 imagery
```

Verified on 20 October 2026: the Paris arrival at local noon, 9.9 seconds,
1600 × 900, joins continuously across the level-14 boundary at 2.35107° E.
The mean absolute RGB step at the former boundary fell from 7.86 to 0.72
in 8-bit output (rows 200–779), matching the surrounding pixel gradients.
The menu smoke and the complete `--smoke --dive --spawn 2.3522 48.8566`
both pass offline, proxies closed, at 640 × 360; all 3,960 globe projection
checks pass too. The dive opened at 20.02 s, slid 12.8 m east under the keys,
and landed at 1.91 m/s.

Two fixes came with it. A menu map tile not on disk is no longer looked for on
disk again every frame — only this cache's own download puts it there — which
was 0.3–1.4 s a frame on a synced folder while the globe swept levels of tiles
not yet fetched. And a model the decoder's budget refuses (the budget full of
decoded images waiting their turn, one prepared per frame) is asked for again
each second, thirty times, instead of being dropped from the tile.

| Key | On foot | In a car or motorcycle | In an aircraft |
|---|---|---|---|
| ZQSD / WASD | walk | drive | see [Flying](#flying) |
| Maj | run | | push down / descend |
| Space | jump | handbrake in cars / rear brake on motorcycles | pull up / climb |
| F | take the nearest car, motorcycle, boat or aircraft | get out at any speed | get out, at any moment |
| Mouse | orbit the camera | turn the head | |
| M / Échap | the map; **Reprendre** returns (M toggles water in debug) | | |
| T | type a cheat code (T toggles trees in debug) | | |
| 2 / right mouse / left mouse | pistol / aim / shoot | same on motorcycles; left hand stays on the bar | |
| E | reload | reload the motorcycle pistol | |
| B | | radio on/off | |
| N / Maj+N | | next / previous local station | |
| Page Up / Page Down | | radio volume | |

### VR

```powershell
powershell -File game\Play.ps1 --vr --spawn 2.3522 48.8566
```

`--vr` opens an OpenXR session (Quest Link or another runtime must be active)
and skips the map: the game travels straight to `--spawn` (Paris by default).
The view is first person from where the player stands; turning the head is the
only control. The rig (SaidaEngine's `XROrigin`) follows the player's feet and
heading, and the player's body is hidden. Tracked hands (Quest hand tracking,
controllers put down) are drawn as SaidaEngine's `XRHand` joints in a skin albedo. The desktop window stays hidden; close
the session from the headset to quit.

### Cheat codes

As in GTA, **T** opens a line above the controls; type a code, **Enter** runs
it, **Escape** closes it. Case and surrounding spaces do not matter. While the
line is open its canvas has the keyboard (Saida's
`UIInteractionSystem::focusWebCanvas`) and the game reads none of its own keys,
so typing `comet` neither walks nor toggles the debug panel.

| Code | Effect |
|---|---|
| `buzzoff` | a helicopter beside you |
| `plane` | a business jet beside you |
| `comet` | the fleet's sports car, in its reddest paint, parked beside you |
| `moto` | a plain city motorcycle parked beside you |
| `rocket` | a motocross motorcycle parked beside you |
| `painkiller` | invincible for 5 minutes; the HUD counts down under the health bar |

A vehicle stands parallel to the player, ahead of him for an aircraft and to
his right for the car, on loaded dry ground clear of walls and bodies, within
40 m; a jet wider than the street may overhang a pavement when nowhere nearer
has room for its span, never with its fuselage. It is left standing like one he
stepped out of: **F** takes it, and the usual limits clear the oldest. A code
that cannot be done says why on screen and in the log (`[World cheat]`): in
flight, or no room. Invincible, nothing takes a hit point and a fall no longer
knocks him down; a car still throws him, unhurt. The rules are
`native/cheats.hpp`; `Cheats.*` and `Combat.an_invincible_body_*` hold them and
the smoke types every code as a player does.

On foot, the camera follows 3.8 m behind the player and moves closer when
obstacles or terrain block the view.

Press **R** in play to show FPS and enable performance debug. **V** toggles
road traffic (simulation, cars and their collisions), **Y** toggles volumetric
grass, **M** toggles WaterNodes (sea and inland water), and **T** toggles tree
models, LOD behaviours and trunk collisions. Each switch is independent and
also applies to newly streamed tiles and props. The player's vehicle remains
usable. In debug, **Escape** opens the map; M opens it during normal play.
Press R again to hide the counter and restore all four features.

The counter uses elapsed whole-frame wall time, including presentation waits,
and reports frames / elapsed seconds with average milliseconds per frame over
half-second windows. Changing a switch resets the window. It uses the existing
frame clock without enabling or copying the engine profiler; its separate
360 × 218 non-interactive canvas is created on first use and refreshed at most
twice a second, avoiding a redraw of the full map/HUD for each FPS update.

Validated on 8 October 2026 with an offline Paris debug smoke: 2,039 streamed
trees, seven grass fields and four WaterNodes; held keys, independent switches,
newly streamed nodes, removal of trunk physics, traffic pause/resume, FPS
averaging and HUD loading passed. The captured HUD was visually checked.
Run with `R1WORLD_DEBUG_SMOKE=1` and `--smoke --spawn 2.3522 48.8566`;
`R1WORLD_DEBUG_SHOT=<png>` additionally captures the restored debug HUD.

Tree foliage materials (9 October 2026): Saida now reads the urban tree's
authored specular color map and IOR instead of applying the default 4% dielectric
reflectance. R1World additionally weights dry leaf specular by 0.25, including
grazing angles, once when loading the shared Near/Mid prototypes. Bark retains
its authored material; geometry, albedo and UVs are unchanged.
Identically lit Near/Mid captures before, after engine correction and after
the dry-leaf adjustment were visually checked in
`generated/streaming-validation/mx450/tree-material-study/{before,after,matte}.png`.
Engine pixel checks distinguish sRGB specular color from linear alpha strength;
environment/HDR and double-sided material regressions pass.

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

`R1WORLD_MENU_SMOKE=1` runs only the menu zoom regression and exits: single
click, long hold, rapid clicks, zoom-out, bounds and reset. It needs no world
spawn; the same checks also run at the beginning of `--smoke`.

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
| `--dive` | the whole arrival instead of the instant spawn: the fade finds the nine tiles mounted and the globe on the world's camera, two seconds of the right key slide him east, the canopy opens at 20 s 50 m above the ground chosen (or earlier over higher ground), lands him unhurt; the log gives the frames per phase |
| `R1WORLD_DIVE_SHOT=<png>`, `R1WORLD_DIVE_SHOT_AT=<seconds after Go>`, `R1WORLD_DIVE_HOUR=<local hour>` | photograph that instant of `--dive` as it is, at that local hour (held for the run, never saved as the player's option) |
| `--sail` | board a boat and sail (e.g. Vieux-Port `5.3698 43.2951`) |
| `--fly` | take off in a jet, stop against a building, land; then a helicopter, hover, jump out (Roissy `2.5700 49.0060`, Villacoublay `2.1900 48.7725`) |
| `--screenshot <png>` | capture the frame after the spawn; with `--camera-pos`, `--camera-look`, `--after-frames` |
| `--temperature <C>` | inspection temperature, with `--at` and `--screenshot`; finite range −100..65 °C |
| `R1WORLD_RETAIL_SMOKE=1` | enter a shop, walk an aisle, leave, close, evict, regenerate |
| `R1WORLD_WADE_SMOKE="<lon> <lat>"` | walk from the spawn to that point over water cells; fail the moment he swims (Penvins groyne: spawn `-2.681180 47.493875`, target `-2.681700 47.493785`) |
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
| `saint_armel` | the reported Lasné regression point: ordinary homes, irregular roofs and small annexes |
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

Four assertions of `--spawn2` exist because of bugs that shipped:

- **The press and its matching click act once.** The menu acts on `mousedown`
  immediately and deduplicates its matching `click`, including long holds.
  The driver fires a press without a release, and separately requires a full
  press-and-release to act exactly once. Mouse consumption previously erased
  hardware history, repeating held presses and losing releases; this is now
  corrected in Saida's shared input sampler.
- **The Sun took the observer.** The spawn fails if `setObserver` was refused;
  otherwise the scene quietly lights the wrong hemisphere.
- **The second Go** must reach a complete spawn, not a stalled counter.
- **Vehicle view becomes walking direction.** The driver looks 90° sideways
  in a car before teleporting. Arrival folds that view into the walking heading
  and clears the vehicle look offset, as a normal vehicle exit does; otherwise
  Z/W moves sideways relative to the camera after the environment loads.
  Verified on 8 October 2026 with an offline Paris → Tunis smoke run: car
  entry, side view, teleport heading assertion, walking/jumping and resume all
  passed (`generated/streaming-validation/mx450/movement-vehicle-teleport.log`).

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
generator version is `kVersion` (40). Within 18 km of a pole the
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
shared models. `kTileVertexBudget` (120 000, `native/gen/cook.hpp`) is a soft
generation target. The [density policy](../docs/PLAN.md) keeps dense tiles
visitable. A neighbourhood past either resident arena
limit is **refused, not truncated**:
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

### Memory

Measured on 16 October 2026, offline Paris smoke (nine tiles resident). The
engine starts at about 660 MB of private memory, with 83 MB of it heap. A
Paris tile, once cooked and prepared, held about 140 MB on the CPU: 60 MB of the
generator's double-precision meshes, 8 MB of their weld keys, 60 MB of prepared
engine vertices (100 bytes each), 12 MB of collision and 1 MB of JSON. The
service keeps the tiles wanted plus the most recent, up to 36.

The generator's own copy is now released as soon as the tile is prepared
(`releaseCookedGeometry`): parts keep their names and materials, buildings
their ids and bounds, and everything drawn or collided with is the prepared
copy. The smoke's peak went from 4.36–4.72 GB to 3.71–3.96 GB; the heap at
rest from 2.9 GB to 1.7 GB. What remains is the prepared copies, about 1.2 GB
outside the heap (driver and Vulkan; 570 MB of it at start-up) and about
650 MB the heap keeps committed after the cooks' peaks.

Two settings were measured and left as they are. Three cooking threads instead
of six lower the peak by about 220 MB but bring the ninth tile 3 s later (12.4
instead of 9.2 s). Keeping 16 tiles instead of 36 changes nothing in a smoke,
which never cooks more than 16; it would only matter on a long drive.

A cook that runs out of memory is said, never a crash later: the service logs
`OUT-OF-MEMORY <tile>`, halves the number of tiles cooked at once and tries the
tile again two seconds later. The HUD says "Mémoire saturée" and the smoke
fails on that reason (`Service.a_cook_out_of_memory_*`). The mount line of each
tile carries `private_mb`.

## Ground and terrain

### Elevation

The terrain mesh starts with a 41 × 41 grid over a tile (`kTerrainMeshSize`),
and every source is read at that grid (`native/gen/sources.cpp`). Its north and
south boundary strips also contain the regular longitude knots of both
adjacent rows. Those rows have different tile widths; resampling a shared
curve separately onto their grids previously left cracks on steep terrain.
Generator v37 triangulates both sides at the shared knots. Ground queries,
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

Validation on 10 October 2026: nine ground inference tests and 116 Python tests
pass. In the cached Vannes centre tile at -2.7608, 47.6579, 2,652 of the
2,711 unclassified terrain triangles now use inferred made ground. The mapped
356 grass triangles remain unchanged and the measured fraction remains 17.35%.
The grass cover after construction masks falls from 39.53% to 11.07% of the
tile; baseline/resident geometry gains only 10 vertices (127,734 / 59,731).
The 640 neighbour-height estimates are unchanged. Snapshots:
`generated/{building-heights-vannes-after,ground-density-vannes-after}.jsonl`.
The optimized game builds; offline Paris/Vannes captures pass and were visually
inspected (`generated/gallery/20261010-011137-c7f3fae`). An earlier simultaneous
native-suite/gallery run exhausted memory in Paris; the serial gallery passed.
The offline Paris Rivoli smoke passes running, jumping, driving, walking and
resume (`cache/sessions/7190bb24f6bd466686c889f1af6db26e`).

The following earlier observations predate this density-and-height rule:

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

### The Atlas decides what the survey did not say

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

Generator v43 corrects cadastral village morphology. Attached French homes
receive the regional home profile when an observed village or hamlet lies
within 700 m and no town or city lies within 1.5 km. These rows do not invent
gardens. Back-row village homes without a road within 45 m keep their home
architecture without an inferred garden or driveway. Untyped footprints below
45 m² beside a recognized home become low
annexes, unless their use or mapped dimensions says otherwise. Untyped
`wall=no` light constructions stay low instead of becoming towers; this
cadastre tag does not prove a fully open roof (see the
[OSM tag documentation](https://wiki.openstreetmap.org/wiki/Tag%3Awall%3Dno)). The Breton fallback favors
two wall levels (80%) rather than one (formerly 90%); this is an adjustable
architectural prior, not a measurement. Mapped heights and levels still win.
Pitched home roofs are partitioned into rectangular wings when possible;
otherwise their planes are clipped to the actual irregular footprint. Near,
Mid and collision geometry retain the same dimensional plan, also used for
ground inference. Residential classifications apply to adjacent-tile height
references as well. The v43 key regenerates previously cooked geometry using
the cached observations.

Validated on 10 October 2026 at the reported Saint-Armel point
(-2.716508, 47.563261), using cached observations only: houses 171538265 and
171538393 now have two wall levels and pitched roofs; the 24 m² footprint
171539754 has one 2.3 m wall level and a 1.5 m inferred roof. Back-row house
171538471 also receives the home profile. Shape-flattened roofs on the tile
fall from 65 to 5; resident geometry remains 30,673 vertices (baseline geometry
51,327 → 51,107). Seven new regression tests and all 116 Python tests passed at that date. Mapped floor counts in the Vannes centre tile remain 134; no new
building dataset is used. Offline captures of Saint-Armel, Lasné, Vannes and
Paris were inspected in `generated/gallery/20261010-014443-c7f3fae`; the final
height-reference adjustment also passes all ten `BuildingHeights` tests.

`heightMeasured` now counts explicit heights only. `heightFromLevels`,
`heightFromNeighbours` and `heightFromAtlas` split `heightInferred`;
`neighbourHeights` records each estimate, its reference IDs, distances, heights
and whether each reference came from a height or floor count. Each manifest
says which tier answered and how much was inferred. For example, the cached
Vannes centre tile at -2.7608, 47.6579 on 10 October 2026 contains:

```json
"inference": {"count": 783, "heightMeasured": 0, "heightInferred": 783,
              "heightFromLevels": 134, "heightFromNeighbours": 640,
              "heightFromAtlas": 9}
```

All 640 neighbour estimates in this tile use three references. Its cache has
no explicit heights: those references derive from mapped floor counts, so these
results remain estimates. The v41 cache key regenerates the old geometry.
The optimized game and cook tools build; 116 Python tests passed at that date. Tall-height tests cover mapped totals through 828 m,
163 floors, all visual levels and physical shells.
Offline Paris and Vannes gallery captures pass and were visually inspected
(`generated/gallery/20261010-004341-c7f3fae`). The Vannes port spawn/swim smoke
passes. A first smoke from the centre sample point loaded the buildings but
its drive hit a building collider. The Paris Rivoli smoke passes running,
jumping, driving, walking and resume checks
(`cache/sessions/431d9ded91d6434595b202b30ccede05`).

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

Verified on 9 October 2026 in `generated/building-lod-validation`: 11 new
generator tests pass.
The engine passes 92 native CTest cases and 226 GPU streaming checks. Offline
1920×1080 captures pass in Paris and Grenoble; the close Grenoble view shows
25 reduced Near buildings and no detailed Near. Its 382 closed buildings use
9,278 Mid triangles and 4,584 Far triangles (12 each), against 18,658 triangles
for their reduced Near envelopes. The Tunis home entry/exit, collision,
eviction and regeneration smoke and the Paris-to-Tunis walk/drive/teleport
smoke pass. These checks ran on the current RTX 4070 desktop; they verify the
LOD implementation and do not measure the reference laptop's frame rate.

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

Benches align their long edge with the nearest road segment and face the
carriageway, on either side and independently of the road's mapped direction.

### Trees

Generator v58 places non-cherry trees 30 cm below the sampled ground to bury
their roots. Sakura trees retain their existing ground-level placement.

Generator v52 also uses the user-selected Innerscene flowering cherry
(`nature_selected/sakura*.glb`). Observed `Prunus serrulata`, `Prunus [x/×]
yedoensis`, `Prunus subhirtella` and `Cerasus` select it worldwide, including
rural areas and the southern hemisphere. For trees without botanical identity,
deterministic urban planting selects it with probability 15% in Japan or 5%
elsewhere north of the equator; the rates do not stack. Since v55, both rates
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
cycle. See `docs/JAPAN_UPDATE.md` for the selection policy and asset provenance.

Generator v53 adds an adult, tall pine for all observed conifers worldwide:
`leaf_type=needleleaved`, legacy `wood=coniferous`, and conifer genera in
`genus`, `species` or `taxon` (Pinus, Abies, Picea, Cedrus, Larix, etc.).
The source is TerrainTale's **Pine Tree Tall**, published under CC0 1.0,
with ambientCG materials and ez-tree credited. The existing photoscanned
saplings remain on disk; the adult pine widens the supported tree forms.
`tools/r1/pine.py` pins the source SHA-256, normalizes its planting pivot and
height, preserves its PBR materials, and derives all LODs from that same tree:
Near 6481 vertices / 5128 triangles, Mid 5735 / 3969, Far 4 / 2. In v54, Mid
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

Generator v54 fills missing botanical identities using sourced Atlas
`treeAreaObservations`, limited to existing OSM woodland contours. Brural's
park has no botanical tags, and its two woods (ways 1085310430 and 1085310431)
only say `natural=wood`. The user's 2026-10-16 pine observation supplies
`genus=Pinus` for those two areas; it neither extends across the whole park nor
overrides an explicit botanical identity on a woodland.

The importer records the CC0 source, authors, file hashes and derivations in
`assets/THIRD_PARTY_ASSETS.json`, `nature_selected/SOURCES.json`, and
`assets/licenses/pine_tall-CC0.txt`. The original download is
`../data/source-assets/pine_tree_tall-terraintale.glb`:
`https://terraintale.com/library/realistic-surfaces/pine_tree_tall.glb`.
Source SHA-256: `8380454b66bd5b3fad4eebed694de207b87056937ecd259a5ce5220369f47bde`.
Import with NumPy, Pillow, ModernGL and the engine AutoLOD executable, from `game`:
`PYTHONPATH=tools python -m r1.pine ../data/source-assets/pine_tree_tall-terraintale.glb`.

v54 validation: 18 native checks pass (Nature 12, Canopy 4, Determinism 2),
as do four Python asset/impostor checks. The native game builds successfully.
Brural's generated placement audit finds all 98 retained trees inside the two
woods are pines (56 + 42; 40 measured canopy cells and 58 area plantings).
The rebuilt game's hidden Brural capture completed with exit code 0, all nine
tiles mounted and no errors; the PNG was visually inspected:
`generated/pine-update/brural-pines-v54.png` (command and executable SHA-256
recorded in the adjacent JSON).
`tools/r1/tree_quality.py` compares identical orthographic views against LOD0,
including angles between atlas views: 64 azimuths × two elevations, at 64,
128 and 256 px. At 128 px, mean silhouette IoU rises from 0.951 to 0.9999 for
Mid and from 0.819 to 0.925 for Far; the worst Far rises from 0.667 to 0.859.
These isolate shape/albedo, without runtime lighting or performance claims.
Reports and inspected silhouettes are in
`generated/pine-update/lod-quality/final-quality.json` and
`generated/pine-update/lod-quality/128/selected-angles.png`.

Other trees include the project's photoscanned Poly Haven species (`CLAUDE.md` §1; a
test refuses any kit tree). A full scan does not fit (`island_tree_01` is
1 303 928 vertices; at a streamable 12 000 only 0.43% of the canopy survives),
and the scene loader parses a referenced file once per node. So trees are
planted as impostors: `tools/bake_nature.py` bakes each species into layered
cards (`assets/models/external/nature_cards/`) keeping its scanned albedo and
alpha, and a city tree carries a near model (`nature_selected/urban_tree.glb`)
switched by `LODGroupBehaviour`. Urban trees now keep Near within 15 m (10%
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

The 12 October Far lighting correction matches Mid's ambient fill and removes
static hemisphere occlusion from direct sunlight, with the 0.5 normal/volume
blend above. An identical-light Near/Mid/Far comparison confirms the
brighter canopy without changing any model, atlas, albedo or LOD threshold.
The engine's 23 native billboard pixel cases pass, including mesh/impostor
ambient parity, separation of direct/sky occlusion and diffuse sky color/crossfade; native and Web
shaders compile, and the translated pipeline validates in Chrome WebGPU.
An offline Paris capture also completes (`paris-final.png`); the intermediate
ambient-only change remained too dark and is not the final visual result.
The current native CTest suite passes 100/100, including Git build identity.
The temporary
shader comparison scripts/captures were removed after recording these results;
whole-world/headset performance is not
qualified by that synthetic scene.
The final 861-tree fixture (20 Mid, 841 Far, 1280×720, same executable and
camera/light) measures median GPU frame 4.492 → 4.801 ms over samples 100–219.
This single pair is a small increase, not a performance gain; the Far still
uses four atlas reads and the existing volume blend. The exported fixture
mirrors the game's shared-material weight in its shader because it does not
run `World::prototype`; no authored model or texture is rewritten.

Regenerate only this Far with `python -m r1.tree_impostor` from `tools`
(NumPy, Pillow, ModernGL and an OpenGL 3.3 context required). Running
`python -m r1.tree_lod` also regenerates it after Mid. Source hashes and the
original CC-BY attribution are in `urban_tree_far.source.json` and `SOURCES.json`.

The 11 October contrast correction is verified by an identical offline Paris
camera (`generated/tree-contrast-validation/paris-tree-contrast.png`) and a
Near/Mid/Far comparison under shared light (`near-mid-far.png` in that directory).
The two bake/asset checks, 15 engine billboard pixel cases, dielectric-material
regression, 95/95 native CTest and native runtime contract pass. Mono and XR
shaders compile, and the translated scene pipeline validates in Chrome WebGPU.
This verifies rendering compatibility; headset performance remains unmeasured.
The Release Web player also builds successfully. In a local MX450 comparison
with 20 Mid + 841 Far trees at 1280×720, median GPU frame time over samples
100–219 is 4.747 ms for coarse lighting and 4.748 ms for relit lighting. These
two runs show no measurable total-frame regression in this scene; they do not
qualify whole-world or headset performance. Traces and counts are in
`generated/tree-contrast-validation/forest`, summarized by `measurement.json`.

Measured on 9 October 2026, NVIDIA MX450, 1280×720, with 861 shared tree
instances and identical camera/light: 476 Mid + 385 old Far became 20 Mid +
841 billboard Far. Median GPU frame time over frames 100–219 fell from
22.98 ms to 5.52 ms (scene HDR 15.43 → 4.63 ms; shadows 7.19 → 0.30 ms).
This is a synthetic forest comparison, not a whole-world FPS guarantee.
Captures/traces: `generated/streaming-validation/mx450/tree-far-study`.
Billboard lighting/orientation/atlas pixel tests, WebGPU pipeline validation,
92/92 native engine tests, 187 GPU streaming checks and offline Paris
performance-debug smoke pass. The broader generator run was not green:
294/299 passed on the broad run. Subsequent Nature (3), Canopy (4) and
Appearance (2) checks pass after updating the canopy Far expectation and
running the country-map test from its required game directory. Failures remain
in the resident tile vertex budget, far-relief material maps and paired bridge
decks; those checks were not changed as part of the billboard work.

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
trees. On the Theix periurban tile the frame
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
pass. Shop walls use clean, warm pine cladding from the CC0 WoodSiding009 scan:
native 1024 × 512 colour, tangent-space normals and roughness, with a 2 × 1 m
repeat (eight horizontal boards). A stable choice from the shop identity alternates
light pine and dark brown finishes. Shops share the same three maps and batch
by finish. `python -m r1.retail_materials` rebuilds that finish, the plaster used
by interior floors and the fascia letter coverage; it uses local sources only.

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

Verified 2026-10-09 without launching R1World: optimized engine and game builds,
94/94 engine CTest cases, six forecast/wetness CPU cases, 53 particle GPU state
checks and 99 weather rendering checks with 18 hidden engine captures.
The images verify animated impacts, wet shading, untouched opt-out/wall/unlit
surfaces, storm structure, return to clear sky and dark nighttime clouds.
Logs and captures are in `generated/rain-validation/`; the real rendered path
was mono Vulkan on the current desktop, while multiview and Web shaders were
compiled. Open-Meteo's [current interval documentation](https://open-meteo.com/en/docs)
defines the precipitation-period conversion used above.

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

Handling verification on 11 October 2026: `tools/verify_vehicle_runtime.ps1`
uses each fleet manifest's dimensions at the game's 0.68 scale and absorbs the
solver pose into the geographic owner every frame. All 42 combinations of seven
models, flat/22-degree ground and three damage levels pass acceleration and
restart checks. The compact exceeds 80 km/h within three seconds at 10, 15, 30,
60, 90 and 120 Hz; after five seconds it reaches 99 km/h on flat ground and
95 km/h on a 22-degree slope. The actual Paris smoke run, using the real clock,
applies 65 kJ to the retained car mesh and rebuilt collider, drives 27.6 m at a
peak of 66 km/h with the maximum 35% power penalty, brakes and takes over a
traffic car successfully. Evidence: `generated/vehicle-validation/handling-runtime.log`
and `handling-paris-damaged.log`. The 95 native engine tests, native/Web runtime
contracts, Web player build and Chrome gameplay/restart checks pass. Frame-rate
equivalence tests do not qualify a physical VR headset or a complete Web port
of the geographic game.

Street furniture continues importing at any speed. Vegetation keeps the same
visibility radii for the selected graphics mode on foot and in vehicles.
Vehicle motion still prioritizes loading the road ahead.

### Motorcycles and local radio — 17–18 October 2026

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

Reproduce the checks from the repository root with
`python game/tools/verify_motorcycle_pose.py`,
`game/tools/verify_vehicle_runtime.ps1 -MotorcyclesOnly`,
`python game/tools/verify_motorcycles.py --skip-build`, and
`python game/tools/verify_radio.py --skip-build` after `game/Play.ps1 -BuildOnly`.
Pose validation covers 70 hand contacts across the actual rigs; physics covers
both models and both turn directions at 30/60/120 Hz. Generated logs and captures
go to `generated/motorcycle-validation/`. The portable media harness remains
`engine/tests/portable_media`, with `engine/tools/verify_portable_media.py` and
`engine/tools/verify_portable_media_web.mjs` as its maintained entry points.

### The fleet

`r1/vehicle_imports.py` converts seven licensed source vehicles to GLB — compact,
estate, SUV, off-roader, sports car, lorry, bus — and preserves two sourced
motorcycles in the shared fleet manifest. The city bike and motocross bike are
CC0 models from Innerscene, stored with pinned SHA-256 hashes in
`data/source-assets/vehicles/motorcycles/sources.json`. Both have independent
wheel pivots and Near/Far LODs. The near car models retain the source
geometry and normals **without decimation**; only distance models are reduced.
The source ZIPs and SHA-256 hashes are pinned in `data/source-assets/vehicles`,
so rebuilding the car models needs Blender 4.2, not a live asset server. The
source motorcycle GLBs and their download links are pinned alongside them.

Ruff's compact and the Scopia / Space Mushrooms vehicles are CC BY 3.0, credited
in the menu and `assets/licenses/Road-vehicles-CC-BY-3.0.txt`. Logos and hidden
interiors are removed. Paint, dark glazing, alloy wheels, mirrors and rubber
have distinct opaque metallic/roughness materials; detail textures ship inside
the GLBs. The motorcycles keep their source models' parts and colors, with
reduced geometry and texture sizes for traffic. `assets/models/vehicles/fleet.json`
lists dimensions and both LODs for every vehicle and counts the exported vertices.
The engine switches to the distance mesh below about one fifth of the screen
height (coverage threshold 0.16),
with hysteresis; a nearby car keeps its full mesh, and distant traffic does
not keep drawing tens of thousands of triangles per compact.
The engine now binds the selected HDR sky to the PBR shader; previously a
descriptor rebuild replaced it with white, flattening every reflection. Diffuse
sky light remains supplied by the solar ambient, avoiding a second diffuse wash.
People and road
vehicles are drawn at **0.68** of their size (`kVehicleScale`), reduced by
15% from 0.8 on 9 October 2026 at the player's request. Dimensions are scaled
with them so doors, cameras and gaps agree.
Validated with an optimized game build, 22 human/player/fleet asset tests and
one offline Paris smoke run: walking, jump, driving, exit and traffic
car takeover passed.

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

Verification on 13–14 October 2026: the standalone traffic suite, eleven lane-graph
cases and nineteen street cases pass. The offline 60-second Paris traffic run
reports 50 live cars at peak, 36 moving at peak and a mean 15.86 moving across
the 30–60 s samples, against 11.29 before the changes (about 40% more). Its final
sample has no junction/blocked-exit stops and no dead-end trips; one physical
incident is recorded. The final build also passes a daytime 60-second check:
51 live cars and 35 moving at peak, 11.71 moving on average after 30 seconds,
zero incidents throughout and no junction/blocked-exit stops in its final sample.
Menu clicks pass their regression and the stopped character has zero horizontal
drift after walking. These are local Paris checks,
not a qualification of every city or the MX450 frame budget.

The reported queue at Paris 48.857268, 2.352571 exposed a region-boundary
regression missed by the neighbourhood motion average: early handoff repeatedly
returned the body to its old region and reset its route. Waiting exceeded
156 simulated seconds. Fixing ownership alone still caused seam collisions
because the upstream region lost sight of the transferred body. Forward-only
handoff plus neighbouring vehicle footprints passes the final main executable's
120-second offline daytime run on 14 October 2026: no physical vehicle within
60 m waits over 60 seconds (largest sampled local wait 15.9 seconds), 35 moving
cars at peak and 11.16 moving on average after warmup. The standalone suite
passes the short-entry and two-region queue/restart regressions. One disabling
incident remains about 120 m west of the observer, after the next junction;
this check establishes the reported seam queue fix, not collision-free traffic
throughout the neighbourhood. Logs: `generated/traffic-deadlock-before.log`,
`generated/traffic-deadlock-after.log` (ownership-only intermediate failure),
`generated/traffic-deadlock-final-validation.log` (final PASS).


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
player stands 1.24 m at 0.68 runtime scale (the authored scale 0.8 multiplied
by `kPeopleVehicleResize=0.85`). Run and sprint retain 2.8 and 7 m/s with
faster playback through engine clip views, so the feet stay planted. Crowd
paces, seated offsets and all human capsules follow the smaller size.
On supported ground the player requests no downward velocity: the engine's
floor sticking keeps contact with slopes, steps and furniture. The former
constant -2 m/s downward request projected into downhill movement after
releasing the walking keys. Unsupported feet still receive that downward
request, and jumping keeps its existing vertical movement.

`R1WORLD_IDLE_SMOKE=1` with `--smoke --spawn 2.3522 48.8566` walks for one
second, releases movement for thirteen seconds, then checks maximum horizontal
drift below 5 mm, grounded feet and the idle clip. Run it offline through
`tools/play_world.py` with the closed proxies described above; it uses the
actual streamed Paris terrain and the normal game update.
The offline check on 13 October 2026 reports zero horizontal drift throughout
the stopped interval, with grounded feet and the idle clip; the diagnostic
before the fix drifted about 2 m downhill over that interval. The targeted
engine character-physics, character-controller and input-consumption tests pass.
The normal offline Paris smoke also passes running, jumping, landing, follow,
ground/seam checks, driving and moving exit. It then reproduces the already
recorded traffic-takeover failure (`took over a different car than the one reached`).

Each tile weights the light, medium and dark scans by
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

## The fight (Fun update, 13 October 2026)

Fists and a pistol, in the spirit of GTA III. **Clic** punches or fires,
**clic droit** aims the pistol over the right shoulder, **1 / 2** switch fists
and pistol, **E** reloads (an empty magazine reloads by itself).

- **Rules** (`native/combat.hpp`, `r1test Combat`): 100 health; punches of
  14, the third of a combo a hook (x1.8) that knocks down; a 17-round pistol,
  34 a round, x3.2 to the head; a car takes the legs from about 9 km/h and
  kills past 50 km/h; a fall from about 2 m hurts, from about 9 m kills.
- **Bodies**: every passer-by and the player carry Saida's `RagdollBehaviour`
  (eleven capsules, `native/combat_kit.hpp` fits it to the Rocketbox biped's
  A-pose). A knocked-down body tumbles in Jolt, freezes once still, and the
  living get up after 2.2 s; the dead stay until out of sight (45 s). At most
  eight tumble at once (`kLiveRagdolls`); the oldest is frozen, never refused.
  A refusal by a full physics world is logged and fails the smoke.
- **The crowd** (`gen/crowd.*`): a shot sends everyone within 55 m running, a
  fight everyone within 14 m; about one passer-by in five (a hash of tile and
  slot) fights back, closing in at a run and punching every second or so.
- **Cars**: whoever stands in the path of the player's car is thrown over the
  bonnet by the solver; a car touching the player on foot knocks him down.
- **Player**: hurt, knocked down, he gets up where he lies; dead, he wakes
  outside the nearest mapped hospital, clinic or doctor's (OSM, the interiors'
  `clinic` recipe), or where he fell when none is resident.
- **Arms and pistol**: `TwoBoneIKModifier` reaches the punch to the target's
  head and holds the pistol on the crosshair; the pistol is drawn by the
  project (two black finishes, albedo 0.04-0.05) and held by
  `BoneAttachmentBehaviour` on the right hand, its grip measured on the rig.
  The player's scan now has one Animator instead of three.
- **Effects and sounds**: pooled particle bursts (flash, smoke, dust, sparks,
  blood) and a muzzle light; eight sounds synthesised by
  `python -m r1.combat_sounds` (standard library, seeded, project-made) and
  preloaded so a shot never reads the disk.
- **Cost**: nothing is simulated or posed until a fight starts; the HUD (health,
  weapon, ammo) changes only when a value does, on the existing half-second tick.

Verified offline: the combat cases in `r1test`, Saida's `saida_ragdoll_tests`, and
the smoke at Paris (night, no crowd: fight skipped) and at Vannes
(`R1WORLD_SMOKE_HOUR=13:00 python tools\play_world.py --smoke --spawn -2.76022 47.6573`:
a passer-by knocked down came to rest in 1.7 s, a pistol round fired, then the
drive and the walk passed). Not measured yet: frame time during a fight on the
MX450, the look of falls and of the pistol in the hand.

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

## Performance

R1World's native build now consumes SaidaEngine's source/build-tree CMake SDK.
`Play.ps1` updates the optimized engine, configures this project's
`CMakeLists.txt` with `SaidaEngine_DIR=../engine/build-rel/sdk`, and builds its
`R1World` target. Ninja tracks actual C++ dependencies; the launcher no longer
extracts flags or library lists from the engine's generated Ninja files.
`R1_BUILD_JOBS` and `Play.cmd` keep their existing interfaces. The vehicle,
motorcycle-pose and radio integration checks also use named CMake targets.
Geography, generation and the optional traffic integration remain game-owned.

### Matched motion measurements, 18 October 2026

The current optimized player was measured offline on the Ryzen 7 5700U / MX450,
with noon/clear conditions, live traffic, real time and unchanged FIFO VSync.
Each preset repeats the same scripted movement three times after the nine local
tiles and their props are ready. Normal uses 1600 x 900; toaster uses 1280 x 720.
These are short local sequences: about four seconds walking and two seconds
accelerating a car over 20 m, not sustained cross-city streaming. Two boundary
frames are excluded per end to remove smoke assertions/scene inspection.

| Location / motion | Normal mean FPS | Toaster mean FPS | Gain |
|---|---:|---:|---:|
| Marais (2.352571, 48.857268), walking | 42.2 | 51.1 | +8.9 (+21.0%) |
| Same departure, driving | 36.0 | 39.5 | +3.5 (+9.7%) |
| Near Saint-Jacques (2.3486, 48.8580), walking | 32.8 | 49.4 | +16.5 (+50.4%) |

Marais driving varies substantially between repeats: normal 30.1–41.5 FPS,
toaster 37.8–40.6 FPS. The pooled gain is not a constant benefit at every moment
or a sustained 40 FPS qualification. Saint-Jacques driving is excluded in both
modes because the manoeuvre encounters an obstacle; the completed walking phases
remain valid. The first two Marais trials, before/during closing other heavy
applications, are also excluded. Game code and saved options are unchanged.
The measured results and limits are retained here; one-off comparison scripts,
captures, executable copies and duplicate Markdown reports were cleaned up.

A separate same-resolution control uses the identical executable, with both
presets verified at 1280 x 720 at the start of every movement phase. Only the
test child window is resized after its options check; no game code changes.
Three repeats per mode in the Marais give:

| Motion, both at 720p | Normal mean FPS | Toaster mean FPS | Gain |
|---|---:|---:|---:|
| Walking | 48.6 | 50.7 | +2.1 (+4.4%) |
| Driving | 39.4 | 43.4 | +4.0 (+10.0%) |

Walking repeat ranges overlap (normal 45.8–50.0, toaster 47.6–52.4 FPS), so its
small average difference is not a constant/clearly separated benefit. Driving
ranges are 38.5–40.6 versus 43.2–43.6 FPS; pooled p95 frame times improve from
32.5 to 29.0 ms. The same short-route limitations apply. Separate-series values
vary; do not subtract their means to attribute an exact cost to resolution.
Saved options remained unchanged. The temporary same-resolution controller and
duplicate report were removed after recording these results.

### Toaster and standard optimization, 18 October 2026

Both presets now share incremental engine transform propagation and conservative
light-volume shadow rejection. Transform comparison still visits hidden nodes
and detects retained mutable references; only changed world matrices propagate.
Hierarchy edits reuse unchanged traversal branches. Skinned shadow casters bypass
the new bounds test. Building LOD scheduling sorts only pending uploads. No model,
texture, density, LOD distance or shadow quality setting was reduced by this work.
The options smoke restores the requested quality/time/weather before capture;
`R1WORLD_SMOKE_QUALITY=standard|toaster` provides an explicit test-only override.

Optimized offline Vulkan comparisons on the Ryzen 7 5700U / MX450, fixed noon,
clear weather and resident Paris tiles, use real time (`--fixed-step 0`). Each
stationary sample selects the last 300 frames before screenshot readback. Before
the final traversal branch-reuse refinement, the same central Paris view gave:

| Preset | Original mean frame / FPS | Optimized mean frame / FPS | Optimized p95 |
|---|---:|---:|---:|
| Toaster, 1280 × 720, run 1 | 46.77 ms / 21.4 | 20.90 ms / 47.9 | 25.69 ms |
| Toaster, repeat 2 | — | 20.96 ms / 47.7 | 23.75 ms |
| Toaster, repeat 3 | — | 23.80 ms / 42.0 | 32.93 ms |
| Standard, 1600 × 900 | 44.70 ms / 22.4 | 26.00 ms / 38.5 | 29.61 ms |

Two other stationary Paris samples averaged 45.5 and 42.9 FPS. Shadow submission
was about 900 draws for 9,000 candidates. These are stationary measurements, not
a sustained 40 FPS qualification: an initial warmed walking run averaged 25 FPS
with hierarchy churn. The final branch-reuse change targets that rebuild cost;
movement and tile-arrival qualification on the compiled version remain open.
Test-only `R1WORLD_PERF_SMOKE=walk|drive` warms the resident scene and labels motion
frames with `World/PerfPhase`, allowing motion to be measured separately.

World generation now defaults to at most two concurrent cooks. Its service cache
retains at most twelve prepared tiles (the visible neighbourhood and three forecast
tiles), rather than thirty-six; requested speculative tiles cannot defeat this
limit. Cold tiles are released under memory pressure, and new cooks wait until
host headroom covers 512 MiB for the frame plus 256 MiB per admitted cook. This
changes preparation/cache timing, not geometry, textures or visible quality.
The runtime supplies the engine's host RAM/commit measurement to the otherwise
engine-independent generator. A paused loader says so in the HUD and log.
`R1WORLD_QUALITY_SWITCH_SMOKE=1` with `--smoke` and
`R1WORLD_PERF_SMOKE=walk` exercises twenty-four standard/toaster switches while
walking, logging private memory after each switch without saving test options.
On 2026-10-18 the offline Paris run at 2.352571, 48.857268 completed all 24
switches and the walking checks with exit 0, no allocation/raster errors and
3033–3205 MiB of private memory at the switch samples. The three deterministic
service memory/cache/admission tests also pass. This is a short regression run,
not a multi-hour stability qualification. Log: `generated/memory-quality-smoke.log`.

The final native streaming GPU suite passes 21,531 checks, all 99 native CTest
cases pass, runtime reflection passes (31 nodes, 23 behaviours, 275 properties),
and the Web player compiles. Authoring WASM smoke passed on the preceding candidate;
the final refinement changes runtime traversal only. Temporary baseline binaries
and audit artifacts were removed after recording these results. The matched
motion measurements above supersede the older performance reports.

## Vehicle and traffic verification — 11 October 2026

The shipped `VehicleRuntime` passes hill starts, braking holds and restarts on
8°, 15° and 22° slopes at 30, 60 and 120 Hz, plus a flat-to-15° ramp transition.
Run `powershell -File tools/verify_vehicle_runtime.ps1` from the game directory.
The same fixture verifies signal triangle winding and shared mesh budgets.
Roadside placement passes four geometry cases and two street integration cases:
wide roads, both driving sides, junction mouths, diagonal clearance, median
islands, missing edges, agreement with rendered pavement and tile-boundary
continuity. The full Streets group passes all 17 cases. The final v46
1920×1080 [signal capture](generated/vehicle-validation/paris-signal-roadside.png)
shows the post outside the carriageway with its foundation on the ground.
The sustained offline Paris run holds the observer stationary for 60 simulated
seconds: peak 42 live vehicles, 41 at completion, peak 22 moving and an average
13 moving over the sampled second half. The acceptance window spans changing
signal phases, instead of requiring movement during a particular final phase.

The follow-up offline gameplay run passes with nine resident tiles: 107.594 m
walked, 20.4142 m driven, peak 14.9092 m/s, braking to 1.41842 m/s, takeover of
a traffic car, braking to 1.12586 m/s, exit and resumed walking. Both runs use
1280×720. They verify behavior, not sustained frame timing.

The optimized native engine builds and all **95 CTest cases pass**. The standalone
traffic suite passes with `g++ -std=c++20 -O2 -Wall -Wextra`. Native runtime
reflection verifies 31 node types, 23 behaviours and 263 properties.
The dedicated vehicle-impact test passes 5,424 checks, including 30/60/120 Hz
physics runs, bounded cage gradients, restoration, continuous collision and
absence of accumulated damage at resting contacts.

The Release Web player and authoring WASM build successfully. The authoring
Node smoke test passes. On local Windows Chrome with its native GPU adapter,
the Web reflection contract passes (18 node types, 15 behaviours, 246
properties), as do the Witness gameplay and saved-state restart scenarios.
This is an engine runtime proof; the Witness scene does not exercise a detailed
vehicle dent in a constrained browser.

The GPU streaming/deformation check passes **295 checks** on the reference MX450,
including private mesh instances, internal LODs, GPU vertex readback, collision
shape replacement, saved dent restoration and an automatic solver-driven crash.
An additional eight-car test loads the actual `city.glb`, applies simultaneous
impacts and checks that every eligible surface publishes private geometry:
0.447 ms to prepare the eight impacts, deformation CPU p95 0.352 ms / maximum
1.177 ms, uploads p95 0.112 ms / maximum 0.397 ms across 600 simulated frames.
Those measurements isolate deformation work; they do not measure a complete
rendered frame or guarantee that a whole fleet's detailed dents appear at once.
The CPU budget is 6,144 vertices per frame globally, with a default 2,048 per
component update at 15 Hz. Geometry upload budgets are also bounded.

The initial 10 October offline Paris gameplay run passes with nine resident tiles: 78.1961 m
walked, 20.4339 m driven with a peak 10.5453 m/s, braking below the exit limit,
then takeover of a moving traffic car and a second complete stop before exit.
The resumed walking phase also passes. The run and its CPU profile use a
1920×1080 window, but include streaming and accelerated smoke-test steps;
they are not proof of sustained 30 fps or headset frame timing.
The matching 1920×1080 [intact](generated/vehicle-validation/car-intact.png) and
[dented](generated/vehicle-validation/car-dented.png) captures use the shipped
vehicle and renderer. The second applies a manual frontal impact through the
normal deformation API; automatic contact-to-dent integration is verified
separately by the GPU crash fixture. Both capture runs complete successfully.

Logs and local captures are under `generated/vehicle-validation/`. A retained
source mesh must opt into `GLTFLoadOptions::retainDeformationSource`; the game
does so for vehicle prototypes. The generic system retains topology and uses a
plastic control lattice with bounded gradients, rather than a complete shell
or fracture simulation. Wheels and glazing are excluded from the body cage.
Separate mechanical zones, detachable parts and dedicated collision effects
remain extensions. PC/OpenXR and Web implementations share the mesh state;
real headset performance and constrained-device Web deformation remain to be
qualified, and these engine changes do not port R1World's geography to Web.

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

Generator v35 reads inland-water multipolygons (OSM query v14), joins reversed shoreline members, preserves inner land rings and rejects incomplete rings. Kawaguchi relation 2313174 was refreshed from the official OSM API; the targeted check and offline capture passed. Water geometry is clipped to each resident tile, and inferred vegetation is excluded from the lake. Distant water beyond resident tiles still uses the coarse relief.


### Coastal beaches (generator v49)

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

### Road grading (generator v50)

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
