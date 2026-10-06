"""Surfaces — what the ground, the streets, the walls and the roofs are made of.

Until this module every surface of the world was a flat colour: a measured
albedo from the palette, right on average and wrong everywhere up
close, because no real ground is one colour. This gives each of them a
photographed, seamless, physically sized material from Poly Haven or ambientCG
— both CC0, the same grade as the skies and the trees (CLAUDE.md, rule 1).

**A texture brings structure, never albedo** (rule 2). Every downloaded colour
map is normalised so each channel averages to a fixed level (`level`), and the
glTF base colour factor is the palette's measured albedo divided by that level.
The rendered mean is therefore exactly the albedo the palette already carried —
the region's hue, the published value — and the photograph only decides how it
varies around it: grass blades, pebbles, mortar joints, tiles.

**A texture is shown at its real size.** Poly Haven publishes the physical
dimensions of each scan; the UVs are in metres and each material divides them
by that size, so a brick is a brick's size and a paving slab a slab's. Where a
source does not publish a size (three ambientCG sets), the size is estimated
from the image and `sizeMeasured` says so in the table.

**Which ground, where.** The climate is a property of the region
(`RegionProfile.climate`), refined by latitude, and every ground class is
answered per climate: a park is lawn in Paris, withered grass in Rome, lush in
Bangkok and frosted in Murmansk; a forest floor is leaf litter in the Vosges,
moss and needles in the taiga and dark humus in the rainforest. Above the
snowline the ground is snow, and just below it frost, at every latitude: the
snowline falls from 5 500 m in the tropics to sea level near 72°.

**Walls are baked sheets.** A wall's UVs span one bay and one storey, so each
wall family is baked into a sheet: its material tiled a whole number of times
across a nominal bay, with the window opening of the old generic sheet on top.
A sheet is therefore a facade and never a plain material: a monument, whose
UVs are metres, takes a windowless masonry family (`ashlar`, `brick_bond`...).

`python -m r1.surfaces` downloads (pinned), normalises and bakes everything
into `assets/textures/surfaces/` and `assets/textures/facades/`, and writes
`assets/textures/surfaces.json`, which is all the tile cook reads: it needs no
image library at play time.
"""

from __future__ import annotations

import hashlib
import io
import json
import urllib.request
import zipfile
from pathlib import Path

from .mesh import Material

ROOT = Path(__file__).resolve().parents[3]
GAME_ROOT = ROOT / "game"
SOURCE_ROOT = ROOT / "data" / "source-assets" / "textures"
SURFACE_ROOT = GAME_ROOT / "assets" / "textures" / "surfaces"
FACADE_ROOT = GAME_ROOT / "assets" / "textures" / "facades"
TABLE_PATH = GAME_ROOT / "assets" / "textures" / "surfaces.json"
USER_AGENT = "R1World-generator/1.0"

# A tile's GLB lives at cache/world/<key>/world.glb; textures are referenced
# from there, like the facade sheet always was.
URI_PREFIX = "../../../"

# Every colour map is normalised to average this per channel (lower where the
# scan is too contrasted to reach it unclipped, see `_normalise`), and the base
# colour factor is the palette's albedo divided by the level actually used.
LEVEL = 0.5
SNOW_LEVEL = 0.85

# The nominal bay a facade sheet spans: a wall's UVs cover one bay across and
# one storey up, whatever their exact size in a given region.
BAY_WIDTH = 3.1
STOREY_HEIGHT = 2.9
SHEET_PIXELS = 1024  # the colour, where a window's bars are read
MAP_PIXELS = 512     # normal, roughness and height
# Wall area of a sheet averages to this per channel; the factor is albedo / it.
WALL_LEVEL = 0.75


# name: (kind, origin, asset, side of the square texture in metres[, measured]).
# Downloads are pinned in data/source-assets/textures/pins.json on first fetch.
_SOURCES_SPEC = {
    # ── ground ───────────────────────────────────────────────────────────────
    "grass":            ("ground", "ambientcg", "Grass004", 1.4, True),
    "grass_lush":       ("ground", "ambientcg", "Grass001", 1.4, True),
    "grass_dry":        ("ground", "polyhaven", "withered_grass", 2.0),
    "forest_temperate": ("ground", "polyhaven", "forrest_ground_01", 2.0),
    "forest_boreal":    ("ground", "polyhaven", "forest_leaves_02", 3.001),
    "forest_tropical":  ("ground", "polyhaven", "mud_forest", 2.35),
    "tropical_ground":  ("ground", "polyhaven", "brown_mud_leaves_01", 1.3),
    "farmland":         ("ground", "polyhaven", "farm_soil", 2.0),
    "soil_arid":        ("ground", "polyhaven", "dry_ground_rocks", 4.0),
    "cracked_earth":    ("ground", "polyhaven", "mud_cracked_dry_03", 1.5),
    "sand_desert":      ("ground", "polyhaven", "sand_01", 1.5),
    "sand_red":         ("ground", "polyhaven", "red_sand", 3.0),
    "sand_beach":       ("ground", "polyhaven", "coast_sand_01", 15.0),
    "savanna":          ("ground", "polyhaven", "red_laterite_soil_stones", 2.0),
    "rock":             ("ground", "polyhaven", "rocks_ground_02", 2.0),
    "cliff":            ("ground", "polyhaven", "rock_face_03", 27.0),
    "bare":             ("ground", "polyhaven", "rock_ground", 1.5),
    "mud":              ("ground", "polyhaven", "brown_mud_02", 1.3),
    # Snow014 and Snow015 publish no size: estimated from the drift features
    # and the grass tufts in the scans.
    "snow":             ("ground", "ambientcg", "Snow014", 2.0, False),
    "frost":            ("ground", "ambientcg", "Snow015", 1.5, False),
    "made_ground":      ("ground", "polyhaven", "gravel_concrete", 2.12),
    # ── streets ──────────────────────────────────────────────────────────────
    "asphalt":          ("street", "polyhaven", "asphalt_01", 2.08),
    "pavement":         ("street", "polyhaven", "concrete_pavement", 1.8),
    "cobbles":          ("street", "polyhaven", "patterned_cobblestone", 2.5),
    # ── the sea's works (harbours.py) ────────────────────────────────────────
    "deck":             ("street", "polyhaven", "weathered_planks", 2.0),
    "riprap":           ("street", "polyhaven", "rock_boulder_dry", 1.8),
    "quay":             ("street", "polyhaven", "concrete", 4.0),
    # ── entrance steps (gen/interiors.cpp): worn stone, without joints ─────
    # that would cross a tread or a riser where no mason would cut one.
    "step_stone":       ("street", "polyhaven", "marble_rock_02", 2.0),
    # ── monuments (landmarks.py): masonry at its real size, never a sheet ──
    # A landmark's UVs are metres, so a wall family's sheet drew a window in
    # every square metre of the Eiffel Tower's footings. These are the same
    # grade of scan without a window: sawn ashlar, rock-faced or weathered
    # ashlar, the metre-high courses of a pyramid's core, and the scan that
    # the `brick_red` sheet is baked from, kept at a brick's size.
    "ashlar":           ("street", "polyhaven", "sandstone_blocks_08", 3.0),
    "ashlar_rough":     ("street", "polyhaven", "sandstone_blocks_05", 3.0),
    "ashlar_large":     ("street", "polyhaven", "large_sandstone_blocks_01", 3.0),
    "brick_bond":       ("street", "polyhaven", "brick_wall_02", 2.0),
    # ── roofs ────────────────────────────────────────────────────────────────
    "tile_canal":       ("roof", "polyhaven", "clay_roof_tiles_02", 2.5),
    "tile_flat":        ("roof", "polyhaven", "roof_09", 4.0),
    "slate":            ("roof", "polyhaven", "roof_slates_03", 3.0),
    "tile_grey":        ("roof", "polyhaven", "grey_roof_tiles", 3.0),
    "metal_seam":       ("roof", "polyhaven", "corrugated_iron_02", 2.7),
    "corrugated":       ("roof", "polyhaven", "corrugated_iron", 1.12),
    "shingle":          ("roof", "polyhaven", "grey_roof_01", 8.0),
    "membrane":         ("roof", "polyhaven", "bitumen", 20.0),
    "terrace":          ("roof", "polyhaven", "gravel_concrete", 2.12),
    "concrete_tile":    ("roof", "polyhaven", "roof_07", 2.0),
    # ── walls (baked into facade sheets) ─────────────────────────────────────
    "brick_red":        ("wall", "polyhaven", "brick_wall_02", 2.0),
    "brick_dark":       ("wall", "polyhaven", "red_brick_03", 1.0),
    "brick_buff":       ("wall", "polyhaven", "yellow_bricks", 2.0),
    "stone":            ("wall", "polyhaven", "white_sandstone_blocks_02", 2.0),
    "render":           ("wall", "polyhaven", "painted_plaster_wall", 2.0),
    "render_rough":     ("wall", "polyhaven", "white_plaster_02", 1.0),
    "earth":            ("wall", "polyhaven", "clay_plaster", 2.0),
    "concrete":         ("wall", "polyhaven", "concrete", 4.0),
    "concrete_panel":   ("wall", "polyhaven", "concrete_wall_004", 2.0),
    # WoodSiding009 publishes no size: eight boards of a ~15 cm reveal.
    "siding":           ("wall", "ambientcg", "WoodSiding009", 1.2, False),
    "timber":           ("wall", "polyhaven", "weathered_plank_siding", 1.57),
}

KINDS = {name: spec[0] for name, spec in _SOURCES_SPEC.items()}

# The window each wall family is drawn with (`r1.windows.MODULES`): the tall
# window with its surround in plaster, stone and brick; the bare opening in
# timber, earth and siding; two casements in concrete.
WINDOW_OF = {
    "render": "small", "render_rough": "small", "stone": "small",
    "brick_red": "small", "brick_dark": "small", "brick_buff": "small",
    "earth": "plain", "siding": "plain", "timber": "plain",
    "concrete": "wide", "concrete_panel": "wide",
}


# ── the runtime half: the table and the materials ──────────────────────────

_table_cache: dict | None = None


def load_table(path: Path = TABLE_PATH) -> dict:
    global _table_cache
    if _table_cache is None or path != TABLE_PATH:
        table = json.loads(path.read_text(encoding="utf-8"))
        if path != TABLE_PATH:
            return table
        _table_cache = table
    return _table_cache


def material(name: str, color: tuple[float, float, float], roughness: float,
             family: str | None, *, double_sided: bool = False,
             table: dict | None = None) -> Material:
    """A material showing `family` at its real size, averaging to `color`.

    Without a family, or before the textures are built, it is the flat
    measured colour it always was.
    """
    if family is None:
        return Material(name, (*color, 1.0), roughness, double_sided=double_sided)
    try:
        entry = (table or load_table())["families"][family]
    except (FileNotFoundError, KeyError):
        return Material(name, (*color, 1.0), roughness, double_sided=double_sided)
    # Above 1 where a contrasted scan was normalised low: the engine does not
    # clamp the factor, and the product still averages to `color`.
    factor = tuple(c / entry["level"] for c in color)
    return Material(
        name, (*factor, 1.0), 1.0, double_sided=double_sided,
        base_color_texture=URI_PREFIX + entry["albedo"],
        normal_texture=URI_PREFIX + entry["normal"],
        metallic_roughness_texture=URI_PREFIX + entry["mr"],
        uv_scale=1.0 / entry["uvSize"],
    )


# ── building the textures (authoring only; needs Pillow and numpy) ─────────

def _get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=180) as response:
        return response.read()


def _download(family: str, spec: tuple) -> dict[str, bytes]:
    """The three maps of a family's source, fetched once and pinned."""
    origin, asset = spec[1], spec[2]
    folder = SOURCE_ROOT / asset
    folder.mkdir(parents=True, exist_ok=True)
    pins_path = SOURCE_ROOT / "pins.json"
    pins = json.loads(pins_path.read_text()) if pins_path.exists() else {}
    maps: dict[str, bytes] = {}
    if origin == "polyhaven":
        files = None
        for key, want in (("Diffuse", "albedo"), ("nor_gl", "normal"), ("Rough", "rough")):
            target = folder / f"{asset}_{want}_1k.jpg"
            pin = pins.get(f"{asset}/{want}")
            if target.exists() and pin and hashlib.md5(target.read_bytes()).hexdigest() == pin:
                maps[want] = target.read_bytes()
                continue
            if files is None:
                files = json.loads(_get(f"https://api.polyhaven.com/files/{asset}"))
            entry = files[key]["1k"]["jpg"]
            payload = _get(entry["url"])
            digest = hashlib.md5(payload).hexdigest()
            if digest != entry["md5"] or (pin and digest != pin):
                raise RuntimeError(f"{asset}/{want}: download does not match its md5")
            target.write_bytes(payload)
            pins[f"{asset}/{want}"] = digest
            maps[want] = payload
    else:
        target = folder / f"{asset}_1K-JPG.zip"
        pin = pins.get(f"{asset}/zip")
        if not (target.exists() and pin and hashlib.sha256(target.read_bytes()).hexdigest() == pin):
            payload = _get(f"https://ambientcg.com/get?file={asset}_1K-JPG.zip")
            digest = hashlib.sha256(payload).hexdigest()
            if pin and digest != pin:
                raise RuntimeError(f"{asset}: download does not match its pinned sha256")
            target.write_bytes(payload)
            pins[f"{asset}/zip"] = digest
        with zipfile.ZipFile(target) as archive:
            names = archive.namelist()
            pick = lambda suffix: archive.read(next(n for n in names if n.endswith(suffix)))
            maps = {"albedo": pick("_Color.jpg"), "normal": pick("_NormalGL.jpg"),
                    "rough": pick("_Roughness.jpg")}
    pins_path.write_text(json.dumps(pins, indent=1, sort_keys=True) + "\n")
    return maps


# Texture memory is a budget like the vertices (§3 I4): three 1024² maps per
# material, across the fifteen or so a city neighbourhood shows, overran the
# engine's 256 MB GPU budget on their own. The colour of what is walked on —
# ground and streets — keeps 1024²; everything else, and every normal and
# roughness map, is 512², where the difference is not visible from the street.
DETAIL_PIXELS = 1024
PIXELS = 512


def _images(maps: dict[str, bytes], albedo_pixels: int):
    import numpy as np
    from PIL import Image
    load = lambda data, mode, n: Image.open(io.BytesIO(data)).convert(mode).resize((n, n), Image.LANCZOS)
    albedo = np.asarray(load(maps["albedo"], "RGB", albedo_pixels), dtype=np.float64) / 255.0
    normal = np.asarray(load(maps["normal"], "RGB", PIXELS), dtype=np.uint8)
    rough = np.asarray(load(maps["rough"], "L", PIXELS), dtype=np.uint8)
    return albedo, normal, rough


def _to_linear(srgb):
    import numpy as np
    return np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)


def _to_srgb(linear):
    import numpy as np
    linear = np.clip(linear, 0.0, 1.0)
    return np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)


# At most this fraction of a texture's pixels may clip at normalisation.
CLIP_BUDGET = 0.01


def _normalise(linear, level: float, mask=None):
    """Scale each channel to average `level` over `mask` — or, where that
    would clip a contrasted scan (bright snow on dark grass, pale mortar
    between dark bricks), to the highest level that keeps clipping within
    `CLIP_BUDGET`. The material's colour factor makes up the difference, so
    the rendered mean is the palette's albedo either way. Returns the image,
    the level used and the fraction clipped."""
    import numpy as np
    region = linear if mask is None else linear[mask]
    mean = np.maximum(region.reshape(-1, 3).mean(axis=0), 1e-4)
    # Per pixel, the level at which its brightest channel reaches 1.
    ceiling = 1.0 / np.maximum((region.reshape(-1, 3) / mean).max(axis=1), 1e-6)
    used = min(level, float(np.quantile(ceiling, CLIP_BUDGET)))
    out = linear * (used / mean)
    clipped = float((out > 1.0).any(axis=-1).mean())
    return np.clip(out, 0.0, 1.0), round(used, 4), clipped


def _save(path: Path, array, mode: str) -> str:
    from PIL import Image
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(array, mode).save(path, quality=92, optimize=True)
    return path.relative_to(GAME_ROOT).as_posix()


def _mr(rough):
    """glTF packs roughness in green and metallic in blue."""
    import numpy as np
    out = np.zeros(rough.shape + (3,), dtype=np.uint8)
    out[..., 0] = 255
    out[..., 1] = rough
    return out


# ── relief (parallax occlusion mapping, engine MaterialDesc::heightId) ─────
# The height a surface is drawn with, 1 its outermost point, 0 `depth` metres
# behind it. Nothing new is downloaded: a scan's relief is integrated from its
# own normal map, and a sheet's window is cut at the measures it is painted at.
#
# Which laid surfaces get relief: those whose joints a pedestrian sees sunk.
RELIEF_STREETS = {"pavement": 0.012, "cobbles": 0.025, "step_stone": 0.008}
# Walls: the masonry's own relief, then the kit's window set into it.
WALL_DEPTH = 0.36      # metres from the outermost sill to the deepest glazing bar
WALL_FACE = 0.55       # the wall plane, as a height: the kit's window reaches 18 cm behind it
WALL_RELIEF = 0.05     # the masonry's relief either side of it: under 2 cm
GLASS_ROUGHNESS = 0.06 # float glass: the sky is seen in it, sharp
ROOM_ALBEDO = 0.03     # a room seen through a window by day: little comes back out
RELIEF_HIGHPASS = 1.0 / 16.0  # cycles per pixel below which relief fades


def _relief(normal):
    """Height, zero mean and unit peak, whose gradient is the normal map's:
    the least-squares integral (Frankot and Chellappa), exact on a texture
    that tiles, as every scan here does."""
    import numpy as np
    n = normal.astype(np.float64) / 127.5 - 1.0
    nz = np.maximum(n[..., 2], 0.2)
    # OpenGL normal maps: +y up the image, which is -row.
    p, q = -n[..., 0] / nz, n[..., 1] / nz
    rows, cols = p.shape
    wy = np.fft.fftfreq(rows)[:, None] * 2.0 * np.pi
    wx = np.fft.fftfreq(cols)[None, :] * 2.0 * np.pi
    denom = wx * wx + wy * wy
    denom[0, 0] = 1.0
    spectrum = (-1j * wx * np.fft.fft2(p) - 1j * wy * np.fft.fft2(q)) / denom
    # A scan's slow undulation is the photograph's lighting as much as its
    # relief, and it would drown the joints: long wavelengths fade out.
    cutoff = 2.0 * np.pi * RELIEF_HIGHPASS
    spectrum *= 1.0 - np.exp(-denom / (cutoff * cutoff))
    h = np.real(np.fft.ifft2(spectrum))
    h -= h.mean()
    return np.clip(h / max(np.quantile(np.abs(h), 0.99), 1e-6), -1.0, 1.0)


def _save_height(path: Path, height) -> str:
    import numpy as np
    return _save(path, (np.clip(height, 0.0, 1.0) * 255 + 0.5).astype(np.uint8), "L")


def _bake_tile(name: str, albedo, normal, rough, level: float) -> dict:
    import numpy as np
    linear, level, clipped = _normalise(_to_linear(albedo), level)
    return {
        "level": level,
        "albedo": _save(SURFACE_ROOT / f"{name}_albedo.jpg",
                        (_to_srgb(linear) * 255 + 0.5).astype(np.uint8), "RGB"),
        "normal": _save(SURFACE_ROOT / f"{name}_normal.jpg", normal, "RGB"),
        "mr": _save(SURFACE_ROOT / f"{name}_mr.jpg", _mr(rough), "RGB"),
        "clipped": round(clipped, 4),
        **({"height": _save_height(SURFACE_ROOT / f"{name}_height.jpg", 0.5 + 0.5 * _relief(normal)),
            "depth": RELIEF_STREETS[name]} if name in RELIEF_STREETS else {}),
    }


def _bake_facade(name: str, albedo, normal, rough, size_m: float) -> dict:
    """One bay by one storey: the material tiled a whole number of times, with
    a window of the facade kit (`r1.windows`) set into it. The file says so
    (`<family>_and_window_*`): a sheet is a facade, never a plain material."""
    import numpy as np
    from PIL import Image
    from . import windows
    n = SHEET_PIXELS
    # The same count both ways, so the scan keeps its aspect: a bay and a
    # storey are within 7% of square, whereas rounding each axis on its own
    # stretched a 2 m brick scan by 45% up the wall. The price is scale, which
    # moves by at most a factor of 1.5 towards the nearest whole count.
    across = up = max(1, round((BAY_WIDTH + STOREY_HEIGHT) / 2.0 / size_m))

    def tiled(array, mode):
        img = Image.fromarray(array, mode).resize((n // across, n // up), Image.LANCZOS)
        tile = np.asarray(img)
        reps = (up, across) + ((1,) if tile.ndim == 3 else ())
        out = np.tile(tile, reps)
        return np.asarray(Image.fromarray(out, mode).resize((n, n), Image.LANCZOS))

    def reduced(array):
        return np.asarray(Image.fromarray(array).resize((MAP_PIXELS, MAP_PIXELS), Image.LANCZOS))

    base = tiled((albedo * 255).astype(np.uint8), "RGB").astype(np.float64) / 255.0
    masonry_normal = tiled(normal, "RGB").astype(np.float64) / 127.5 - 1.0
    masonry_rough = tiled(rough, "L").astype(np.float64) / 255.0

    window = windows.layer(WINDOW_OF[name], n)
    a = window["coverage"]
    wall = a < 0.5
    linear, level, clipped = _normalise(_to_linear(base), WALL_LEVEL, wall)
    # The window keeps the kit's own colours: a scanned frame and its glass,
    # whose film of dirt lets the dark room behind it through.
    pane = window["glass"] > 0.5
    colour = window["color"].copy()
    seen = window["opacity"][..., None]
    colour[pane] = (colour * seen + ROOM_ALBEDO * (1.0 - seen))[pane]
    linear = linear * (1.0 - a[..., None]) + colour * a[..., None]
    nrm = masonry_normal * (1.0 - a[..., None]) + window["normal"] * a[..., None]
    nrm /= np.maximum(np.linalg.norm(nrm, axis=-1, keepdims=True), 1e-6)
    rgh = masonry_rough * (1.0 - a) + np.where(window["glass"] > 0.5, GLASS_ROUGHNESS, window["rough"]) * a
    metal = window["metal"] * a
    masonry_height = WALL_FACE + WALL_RELIEF * _relief(tiled(normal, "RGB"))
    height = masonry_height * (1.0 - a) + (WALL_FACE + window["depth"] / WALL_DEPTH) * a

    mr = np.zeros((n, n, 3), dtype=np.uint8)
    mr[..., 0] = 255
    mr[..., 1] = np.clip(rgh * 255 + 0.5, 0, 255).astype(np.uint8)
    mr[..., 2] = np.clip(metal * 255 + 0.5, 0, 255).astype(np.uint8)
    return {
        "level": level,
        "albedo": _save(FACADE_ROOT / f"{name}_and_window_albedo.jpg",
                        (_to_srgb(linear) * 255 + 0.5).astype(np.uint8), "RGB"),
        "normal": _save(FACADE_ROOT / f"{name}_and_window_normal.jpg",
                        reduced(np.clip(nrm * 127.5 + 128.0, 0, 255).astype(np.uint8)), "RGB"),
        "mr": _save(FACADE_ROOT / f"{name}_and_window_mr.jpg", reduced(mr), "RGB"),
        "height": _save_height(FACADE_ROOT / f"{name}_and_window_height.jpg",
                               np.asarray(Image.fromarray(np.clip(height, 0, 1).astype(np.float32))
                                          .resize((MAP_PIXELS, MAP_PIXELS), Image.LANCZOS))),
        "depth": WALL_DEPTH,
        "window": windows.MODULES[WINDOW_OF[name]],
        "clipped": round(clipped, 4),
        "repeats": [across, up],
    }


def build(names: list[str] | None = None) -> dict:
    families = {}
    for name, spec in _SOURCES_SPEC.items():
        if names is not None and name not in names:
            continue
        _kind, origin, asset, size_m = spec[:4]
        measured = spec[4] if len(spec) > 4 else True
        kind = KINDS[name]
        albedo, normal, rough = _images(_download(name, spec),
                                        DETAIL_PIXELS if kind in ("ground", "street") else PIXELS)
        if kind == "wall":
            baked = _bake_facade(name, albedo, normal, rough, size_m)
            uv_size = 1.0  # wall UVs are already bays and storeys
        else:
            baked = _bake_tile(name, albedo, normal, rough, SNOW_LEVEL if name == "snow" else LEVEL)
            uv_size = size_m
        families[name] = {"kind": kind, "source": origin, "asset": asset,
                          "size": size_m, "sizeMeasured": measured,
                          "uvSize": uv_size, **baked}
        print(f"{name:18s} {kind:6s} {asset:28s} {size_m:6.2f} m  "
              f"level {baked['level']:.3f}  clipped {baked['clipped']:.3f}")
    # The sea ice's two surfaces are made from the snow just baked.
    from .sea_ice_textures import derive
    if "snow" in families:
        families.update(derive(families))
    return {"schema": 1, "level": LEVEL, "snowLevel": SNOW_LEVEL, "wallLevel": WALL_LEVEL,
            "bay": [BAY_WIDTH, STOREY_HEIGHT], "families": families}


def main() -> int:
    import argparse
    parser = argparse.ArgumentParser(description="Build pinned, albedo-normalised surface textures.")
    parser.add_argument("families", nargs="*", metavar="FAMILY")
    args = parser.parse_args()
    unknown = set(args.families) - _SOURCES_SPEC.keys()
    if unknown:
        parser.error("unknown surface families: " + ", ".join(sorted(unknown)))
    table = build(args.families or None)
    if args.families and TABLE_PATH.exists():
        previous = json.loads(TABLE_PATH.read_text(encoding="utf-8"))
        previous["families"].update(table["families"])
        table = previous
    TABLE_PATH.parent.mkdir(parents=True, exist_ok=True)
    TABLE_PATH.write_text(json.dumps(table, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
