"""Sea-ice surfaces, derived from the snow already on disk.

The pack ice at the North Pole is snow on ice, and the project already carries
a photographed snow (ambientCG Snow014, CC0; CLAUDE.md rule 1: reuse before
adding). Two things in it are wrong for the sea, though, and both are fixed
here rather than by downloading a lesser asset:

* **snow_clean** -- Snow014 was shot on a meadow, and grass tips show through
  it. On a mountain they read as rock and heather; on sea ice, 4 000 km from
  the nearest blade of grass, they are a mistake. They are found (dark or
  coloured against the local snow) and filled from the snow around them.
* **sea_ice** -- bare ice: the blue flanks of a pressure ridge, the blocks
  in it, a lead frozen over, a melt pond frozen in autumn. It keeps the snow's
  large-scale structure at a third of its contrast, a softened relief and a
  glazed roughness: ice is smooth where snow is granular.

Neither texture carries albedo (rule 2): each is normalised like every other
surface (`surfaces._normalise`) and the palette's measured albedo gives the
colour (`atlas.json` → ground → seaIce).

`python -m r1.sea_ice_textures` writes the maps into
`assets/textures/surfaces/` and adds both families to `surfaces.json`;
`python -m r1.surfaces` calls `derive` too, so a full rebake keeps them.
"""

from __future__ import annotations

import json

from . import surfaces

SOURCE = "snow"


def _load(family: dict):
    import numpy as np
    from PIL import Image
    read = lambda rel, mode: np.asarray(Image.open(surfaces.GAME_ROOT / rel).convert(mode))
    albedo = read(family["albedo"], "RGB").astype(np.float64) / 255.0
    return albedo, read(family["normal"], "RGB"), read(family["mr"], "RGB")


def _blur(array, radius: int):
    """A wrap-around box blur: the maps tile, so their edges must too."""
    import numpy as np
    out = array.astype(np.float64)
    for axis in (0, 1):
        acc = np.zeros_like(out)
        for shift in range(-radius, radius + 1):
            acc += np.roll(out, shift, axis=axis)
        out = acc / (2 * radius + 1)
    return out


def _specks(linear):
    """Grass and soil in the snow: darker than the snow around them, or
    coloured where snow is grey. Grown by three pixels to take their halo."""
    import numpy as np
    luma = linear @ np.array([0.2126, 0.7152, 0.0722])
    local = _blur(luma, 12)
    chroma = linear.max(axis=-1) - linear.min(axis=-1)
    mask = (luma < 0.82 * local) | (chroma > 0.08)
    grown = mask.astype(np.float64)
    for _ in range(3):
        grown = np.maximum(grown, _blur(grown, 1) > 0.0)
    return grown > 0.0


def _fill(array, mask):
    """Each masked pixel takes the mean of the unmasked pixels around it,
    looking further out where a speck is too wide for the nearest ring."""
    import numpy as np
    out = array.astype(np.float64).copy()
    todo = mask.copy()
    for radius in (6, 12, 24, 48, 96):
        if not todo.any():
            break
        keep = (~todo).astype(np.float64)
        weights = _blur(keep, radius)
        reached = todo & (weights > 0.05)
        if out.ndim == 3:
            filled = _blur(out * keep[..., None], radius) / np.maximum(weights, 1e-6)[..., None]
            out = np.where(reached[..., None], filled, out)
        else:
            filled = _blur(out * keep, radius) / np.maximum(weights, 1e-6)
            out = np.where(reached, filled, out)
        todo &= ~reached
    return out


def _flatten(normal, keep: float):
    """A tangent-space normal map with its relief scaled by `keep`."""
    import numpy as np
    n = normal.astype(np.float64) / 127.5 - 1.0
    n[..., 0] *= keep
    n[..., 1] *= keep
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return ((n + 1.0) * 127.5 + 0.5).clip(0, 255).astype(np.uint8)


def _bake(name: str, linear, normal, rough, level: float) -> dict:
    import numpy as np
    out, used, clipped = surfaces._normalise(linear, level)
    return {
        "level": used,
        "albedo": surfaces._save(surfaces.SURFACE_ROOT / f"{name}_albedo.jpg",
                                 (surfaces._to_srgb(out) * 255 + 0.5).astype(np.uint8), "RGB"),
        "normal": surfaces._save(surfaces.SURFACE_ROOT / f"{name}_normal.jpg", normal, "RGB"),
        "mr": surfaces._save(surfaces.SURFACE_ROOT / f"{name}_mr.jpg", surfaces._mr(rough), "RGB"),
        "clipped": round(clipped, 4),
    }


def derive(families: dict) -> dict:
    """The two sea-ice families, baked from `families["snow"]`."""
    import numpy as np
    source = families[SOURCE]
    albedo, normal, mr = _load(source)
    linear = surfaces._to_linear(albedo)
    mask = _specks(linear)
    snow = _fill(linear, mask)
    # The relief maps are half the colour map's size (`surfaces.PIXELS`).
    k = mask.shape[0] // normal.shape[0]
    small = mask.reshape(normal.shape[0], k, normal.shape[1], k).any(axis=(1, 3))
    snow_normal = _fill(normal.astype(np.float64), small).clip(0, 255).astype(np.uint8)
    snow_rough = _fill(mr[..., 1].astype(np.float64), small)
    # Snow is rough: Snow014's map averages 0.5, which under a clear sky
    # mirrors the blue of the zenith at a grazing angle and reads as water.
    # Its variation is kept, around 0.85.
    snow_rough = (217 + (snow_rough - snow_rough.mean()) * 0.6).clip(190, 245).astype(np.uint8)

    luma = snow @ np.array([0.2126, 0.7152, 0.0722])
    broad = _blur(luma, 3)
    grey = broad.mean() + (broad - broad.mean()) * 0.35
    ice = np.repeat(grey[..., None], 3, axis=-1).clip(1e-4, 1.0)
    ice_normal = _flatten(_blur(snow_normal, 1).clip(0, 255).astype(np.uint8), 0.45)
    # Glazed: a roughness of 0.2 to 0.3 where the snow is 0.6 to 0.9.
    ice_rough = (52 + (snow_rough.astype(np.float64) - snow_rough.mean()) * 0.25).clip(30, 90).astype(np.uint8)

    common = {"kind": "ground", "source": source.get("source"), "asset": source.get("asset"),
              "derivedFrom": SOURCE, "sizeMeasured": source.get("sizeMeasured", False)}
    out = {}
    baked = _bake("snow_clean", snow, snow_normal, snow_rough, surfaces.SNOW_LEVEL)
    out["snow_clean"] = {**common, "size": source["size"], "uvSize": source["uvSize"], **baked,
                         "specksFilled": round(float(mask.mean()), 4)}
    baked = _bake("sea_ice", ice, ice_normal, ice_rough, surfaces.LEVEL)
    # Ice is drawn larger than the snow it came from: its structure is the
    # snow's broad drift, not its grain.
    out["sea_ice"] = {**common, "size": source["size"] * 2.0, "uvSize": source["uvSize"] * 2.0, **baked}
    for name, entry in out.items():
        print(f"{name:12s} level {entry['level']:.3f}  clipped {entry['clipped']:.4f}")
    return out


def main() -> int:
    table = json.loads(surfaces.TABLE_PATH.read_text(encoding="utf-8"))
    table["families"].update(derive(table["families"]))
    surfaces.TABLE_PATH.write_text(json.dumps(table, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
