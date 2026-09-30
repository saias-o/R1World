"""Bake shop finishes from the existing CC0 scan and our lettering atlas.

Run with python -m r1.retail_materials from game/tools. No download, no runtime
image processing. The mineral finish averages 0.5 in linear colour; materials
multiply it by twice their intended albedo. Keep the scan's native resolution.
"""
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]


def bake():
    target = ROOT / "game/assets/textures/interiors"
    target.mkdir(parents=True, exist_ok=True)
    source = ROOT / "data/source-assets/textures/painted_plaster_wall/painted_plaster_wall_albedo_1k.jpg"
    srgb = np.asarray(Image.open(source).convert("RGB"), dtype=np.float64) / 255
    linear = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    # Retain photographed mineral variation, subdued for a sealed finish.
    linear = np.clip(.5 + .22 * (linear / linear.mean(axis=(0, 1)) - 1), 0, 1)
    encoded = np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - .055)
    Image.fromarray(np.rint(encoded * 255).astype("uint8")).save(target / "mineral_albedo.jpg", quality=95)

    atlas = ROOT / "game/assets/models/signs/faces/blade_glyphs-r1.png"
    pixels = np.asarray(Image.open(atlas).convert("RGB"), dtype=np.float64)
    # The source font is antialiased between green (25,82,53) and white
    # (170,170,168). Recover coverage without changing the mesh UVs.
    coverage = np.clip((pixels[:, :, 0] - 25) / (170 - 25), 0, 1)
    rgba = np.full((*coverage.shape, 4), 255, dtype="uint8")
    rgba[:, :, 3] = np.rint(coverage * 255).astype("uint8")
    Image.fromarray(rgba).save(target / "fascia_letters.png")


if __name__ == "__main__":
    bake()
