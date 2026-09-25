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
SHEET_PIXELS = 512
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
    }


def _bake_facade(name: str, albedo, normal, rough, size_m: float) -> dict:
    """One bay by one storey: the material tiled a whole number of times, with
    the window of the old generic sheet on top of it."""
    import numpy as np
    from PIL import Image
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

    base = tiled((albedo * 255).astype(np.uint8), "RGB").astype(np.float64) / 255.0
    nrm = tiled(normal, "RGB").copy()
    rgh = tiled(rough, "L").copy()

    # The opening, in the old sheet's proportions (128 px): frame 36-92 across
    # and 20-106 down, glazing inset by 4, a mullion and a transom, a sill.
    s = n / 128.0
    box = lambda x0, x1, y0, y1: (slice(int(y0 * s), int(y1 * s)), slice(int(x0 * s), int(x1 * s)))
    wall = np.ones((n, n), dtype=bool)
    wall[box(34, 94, 20, 109)] = False
    wall[box(0, 128, 0, 3)] = False
    linear, level, clipped = _normalise(_to_linear(base), WALL_LEVEL, wall)
    flat = np.array([128, 128, 255], dtype=np.uint8)
    paint = [
        (box(0, 128, 0, 3), (0.55, 0.53, 0.50), 170),      # floor line
        (box(36, 92, 20, 106), (0.40, 0.39, 0.36), 150),   # reveal
        (box(40, 88, 24, 102), (0.025, 0.035, 0.045), 30),  # glazing
        (box(43, 62, 27, 62), (0.05, 0.07, 0.08), 25),     # reflection
        (box(62, 66, 24, 102), (0.72, 0.70, 0.66), 140),   # mullion
        (box(40, 88, 63, 67), (0.72, 0.70, 0.66), 140),    # transom
        (box(34, 94, 103, 109), (0.80, 0.78, 0.72), 150),  # sill
    ]
    for region, colour, r in paint:
        linear[region] = colour
        nrm[region] = flat
        rgh[region] = r
    return {
        "level": level,
        "albedo": _save(FACADE_ROOT / f"{name}_albedo.jpg",
                        (_to_srgb(linear) * 255 + 0.5).astype(np.uint8), "RGB"),
        "normal": _save(FACADE_ROOT / f"{name}_normal.jpg", nrm, "RGB"),
        "mr": _save(FACADE_ROOT / f"{name}_mr.jpg", _mr(rgh), "RGB"),
        "clipped": round(clipped, 4),
        "repeats": [across, up],
    }


def build() -> dict:
    families = {}
    for name, spec in _SOURCES_SPEC.items():
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
    families.update(derive(families))
    return {"schema": 1, "level": LEVEL, "snowLevel": SNOW_LEVEL, "wallLevel": WALL_LEVEL,
            "bay": [BAY_WIDTH, STOREY_HEIGHT], "families": families}


def main() -> int:
    table = build()
    TABLE_PATH.parent.mkdir(parents=True, exist_ok=True)
    TABLE_PATH.write_text(json.dumps(table, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
