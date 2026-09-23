"""Draw every landmark model to one contact sheet, without the engine.

    python tools/landmark_preview.py [out.png] [slug ...]

R1_PREVIEW_LOD=1 or 2 draws a far level instead; R1_PREVIEW_CELL sets the size.

A test can say a model fits the arena and encloses a volume; only a picture
says it looks like the Eiffel Tower (CLAUDE.md rule 1: the player is the one
who sees a regression, so look before they do). This is a z-buffered
rasteriser in numpy, lit by one sun, seen from a three-quarter view at the
height a visitor would photograph it from.
"""
from __future__ import annotations

import math
import os
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from r1 import landmarks  # noqa: E402

CELL = int(os.environ.get("R1_PREVIEW_CELL", "420"))
# 0 up close, 1 and 2 the far models (`sculpt.detail`).
LOD = int(os.environ.get("R1_PREVIEW_LOD", "0"))


def _png(path: Path, rgb: np.ndarray) -> None:
    h, w, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].tobytes() for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def render(parts, azimuth: float, elevation: float, size: int = CELL) -> np.ndarray:
    pos = np.concatenate([np.asarray(p.mesh.positions, float) for p in parts])
    lo, hi = pos.min(0), pos.max(0)
    centre = (lo + hi) / 2
    radius = np.linalg.norm(hi - lo) / 2
    az, el = math.radians(azimuth), math.radians(elevation)
    # View direction from the camera towards the model; engine x east, z south.
    forward = np.array([-math.sin(az) * math.cos(el), -math.sin(el), math.cos(az) * math.cos(el)])
    right = np.cross(forward, [0, 1, 0]); right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    sun = np.array([0.45, 0.8, 0.35]); sun /= np.linalg.norm(sun)
    image = np.zeros((size, size, 3)); image[:] = (0.55, 0.66, 0.78)
    depth = np.full((size, size), np.inf)
    scale = size * 0.47 / radius
    for part in parts:
        p = np.asarray(part.mesh.positions, float) - centre
        n = np.asarray(part.mesh.normals, float)
        idx = np.asarray(part.mesh.indices, int).reshape(-1, 3)
        sx = p @ right * scale + size / 2
        sy = size / 2 - p @ up * scale
        sz = p @ forward
        colour = np.asarray(part.material.color[:3])
        if part.material.base_color_texture:
            colour = colour * 0.5   # the texture's own level, see surfaces.material
        light = np.clip(n @ sun, 0, 1) * 0.8 + 0.25 + 0.1 * n[:, 1]
        for a, b, c in idx:
            xs, ys = sx[[a, b, c]], sy[[a, b, c]]
            x0, x1 = max(0, int(xs.min())), min(size - 1, int(xs.max()) + 1)
            y0, y1 = max(0, int(ys.min())), min(size - 1, int(ys.max()) + 1)
            if x0 > x1 or y0 > y1:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
            d = (ys[1] - ys[2]) * (xs[0] - xs[2]) + (xs[2] - xs[1]) * (ys[0] - ys[2])
            if abs(d) < 1e-12:
                continue
            w0 = ((ys[1] - ys[2]) * (gx - xs[2]) + (xs[2] - xs[1]) * (gy - ys[2])) / d
            w1 = ((ys[2] - ys[0]) * (gx - xs[2]) + (xs[0] - xs[2]) * (gy - ys[2])) / d
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            z = w0 * sz[a] + w1 * sz[b] + w2 * sz[c]
            shade = w0 * light[a] + w1 * light[b] + w2 * light[c]
            # Back faces drawn darker: a hole in a model shows as a dark patch.
            facing = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (ys[1] - ys[0]) * (xs[2] - xs[0])
            if facing > 0:
                shade = shade * 0 + 0.05
            region = depth[y0:y1 + 1, x0:x1 + 1]
            win = inside & (z < region)
            region[win] = z[win]
            image[y0:y1 + 1, x0:x1 + 1][win] = colour * shade[win][:, None] * 2.2
    return (np.clip(image, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("landmarks.png")
    wanted = set(sys.argv[2:])
    chosen = [l for l in landmarks.LANDMARKS if not wanted or l.slug in wanted]
    cols = min(4, len(chosen))
    rows = math.ceil(len(chosen) / cols)
    sheet = np.zeros((rows * CELL, cols * CELL, 3), np.uint8)
    for i, landmark in enumerate(chosen):
        parts = landmarks.model_parts(landmark, LOD)
        cell = render(parts, landmark.view, 14)
        r, c = divmod(i, cols)
        sheet[r * CELL:(r + 1) * CELL, c * CELL:(c + 1) * CELL] = cell
        verts = sum(len(p.mesh.positions) for p in parts)
        print(f"{landmark.slug:22s} {verts:7d} vertices")
    _png(out, sheet)
    print(out)


if __name__ == "__main__":
    main()
