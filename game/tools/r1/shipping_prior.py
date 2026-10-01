"""Build the sea-traffic prior: where ships are, on average, by kind.

Source: *Global Shipping Traffic Density* (World Bank / IMF, CC BY 4.0) --
every hourly AIS position reported between January 2015 and February 2021,
counted on a 0.005-degree grid, one raster per kind of ship. Real GPS, six
years of it; what the game needs from it is small.

The prior is deliberately coarse (the game asks "about how many ships of each
kind, and along which axis", not "where exactly"):

- a 0.1-degree cell (~11 km), 20 x 20 source pixels summed;
- the mean number of ships present in it, per kind, on one log byte
  (12 steps per octave, ~6 %);
- the axis of its lane, one byte (the principal axis of where the positions
  fell inside the cell), or `NO_AXIS` where traffic has none;
- cut in 10-degree blocks, zlib-compressed, empty blocks left out, so the
  game reads one ~50 KB block around the player and nothing else.

This is a build step, run once by a developer, not by the player:

    python -m r1.shipping_prior        # needs `tifffile` (developer Python)

The ~10 GB rasters are unpacked one at a time, reduced, then deleted.
"""

from __future__ import annotations

import hashlib
import json
import math
import struct
import sys
import urllib.request
import zipfile
import zlib
from pathlib import Path

import numpy as np

GAME = Path(__file__).resolve().parents[2]
SOURCE = GAME / "cache" / "shipping-source"
PRIOR = GAME / "assets" / "world" / "shipping" / "prior.bin"

BASE = "https://datacatalogfiles.worldbank.org/ddh-published/0037580/5/"
# kind, archive, member, sha256 of the archive
LAYERS = (
    ("commercial", "DR0045405/shipdensity_commercial_.zip", "ShipDensity_Commercial1.tif",
     "0cfdce41d9934982bbc8f16bd92ce7545cc088839327cbaa9b0ed1469dda31e8"),
    ("fishing", "DR0045403/ShipDensity_Fishing.zip", "ShipDensity_Fishing1.tif",
     "c45f341f496eaf0d0954326015f68d208941c56bbe53d59f1ee6166636330dc3"),
    ("passenger", "DR0045404/ShipDensity_Passenger.zip", "ShipDensity_Passenger1.tif",
     "a2bea6398afee17bcb90bc34514abe6719e92cb764cc74e66def01e6fa8040a2"),
    ("leisure", "DR0045401/ShipDensity_Leisure.zip", "ShipDensity_Leisure1.tif",
     "2714f25d8c6f041bfaa342e2b29fcacc55faa49584c9d28ff18da1f7b4968a73"),
)
KINDS = tuple(layer[0] for layer in LAYERS)

# The rasters' own grid (their .tfw): 0.005 degree, top-left corner.
PIXEL = 0.005
LON0, LAT0 = -180.0128112750, 85.0001479370
NODATA = 2147483647
# The rasters count every AIS message received, and how many are received
# depends on the receivers (dense along European coasts, sparse by satellite
# mid-ocean), so a count is not a number of ships. Each layer is scaled to a
# mean number of ships present in a 0.1-degree cell on one reference, and the
# rest of the world follows that layer's pattern. Approximate on purpose:
# commercial   -- Dover Strait (1.45 E, 51.0 N), ~8 ships in the cell;
# fishing      -- the same cell, ~2 boats;
# passenger    -- Strait of Gibraltar (5.6 W, 35.95 N), ~3 ferries;
# leisure      -- off Marseille (5.2 E, 43.2 N), ~5 boats on a yearly mean.
SCALE = {"commercial": 8.0 / 1.202e10, "fishing": 2.0 / 8.004e6,
         "passenger": 3.0 / 7.684e3, "leisure": 5.0 / 1.692e8}

CELL_PIXELS = 20                 # 0.1 degree
CELL = PIXEL * CELL_PIXELS
BLOCK = 100                      # cells per block side: 10 degrees
FLOOR = 1e-4                     # ships per cell below which a cell is empty
PER_OCTAVE = 12
NO_AXIS = 255
WIDE = 4                         # cells: the wide-lane scale is 9 x 9 cells
MAGIC = b"R1SEA\x00\x01\x00"


def quantize(ships: np.ndarray) -> np.ndarray:
    q = np.zeros(ships.shape, np.uint8)
    live = ships >= FLOOR
    q[live] = np.clip(np.round(1 + PER_OCTAVE * np.log2(ships[live] / FLOOR)), 1, 255)
    return q


def _download(url: str, path: Path, digest: str) -> None:
    if path.exists() and _sha256(path) == digest:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    print("download", url, flush=True)
    with urllib.request.urlopen(url, timeout=120) as response, open(path, "wb") as out:
        while chunk := response.read(1 << 20):
            out.write(chunk)
    if _sha256(path) != digest:
        raise RuntimeError(f"{path.name}: checksum mismatch")


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
    return h.hexdigest()


def reduce_layer(tif: Path):
    """Per 0.1-degree cell: the position count and its second moments."""
    import tifffile  # developer dependency, only here

    with tifffile.TiffFile(tif) as t:
        page = t.pages[0]
        if page.compression != 1 or not page.is_tiled:
            raise RuntimeError("expected an uncompressed tiled raster")
        height, width = page.shape
        th, tw = page.tilelength, page.tilewidth
        offsets, counts = list(page.dataoffsets), list(page.databytecounts)
    tiles_x = -(-width // tw)
    rows, cols = -(-height // CELL_PIXELS), -(-width // CELL_PIXELS)
    band = th * CELL_PIXELS // math.gcd(th, CELL_PIXELS)        # 640 rows
    out = {name: np.zeros((rows, cols), np.float64) for name in ("n", "x", "y", "xx", "xy", "yy")}
    # Pixel offsets from the cell centre, in pixels (x east, y south).
    u = (np.arange(CELL_PIXELS, dtype=np.float32) - (CELL_PIXELS - 1) / 2)
    with open(tif, "rb") as f:
        for top in range(0, height, band):
            arr = np.zeros((band, tiles_x * tw), np.int32)
            for tr in range(top // th, min(-(-height // th), (top + band) // th)):
                for tx in range(tiles_x):
                    i = tr * tiles_x + tx
                    f.seek(offsets[i])
                    tile = np.frombuffer(f.read(counts[i]), "<i4").reshape(th, tw)
                    arr[(tr * th - top):(tr * th - top) + th, tx * tw:(tx + 1) * tw] = tile
            arr[(arr == NODATA) | (arr < 0)] = 0
            cells = band // CELL_PIXELS
            r0 = top // CELL_PIXELS
            r1 = min(rows, r0 + cells)
            # Column chunks keep the float copies small.
            for c0 in range(0, cols, 400):
                c1 = min(cols, c0 + 400)
                w = arr[:(r1 - r0) * CELL_PIXELS, c0 * CELL_PIXELS:c1 * CELL_PIXELS].astype(np.float32)
                w = w.reshape(r1 - r0, CELL_PIXELS, c1 - c0, CELL_PIXELS)
                ux, uy = u[None, None, None, :], u[None, :, None, None]
                out["n"][r0:r1, c0:c1] += w.sum((1, 3))
                out["x"][r0:r1, c0:c1] += (w * ux).sum((1, 3))
                out["y"][r0:r1, c0:c1] += (w * uy).sum((1, 3))
                out["xx"][r0:r1, c0:c1] += (w * ux * ux).sum((1, 3))
                out["xy"][r0:r1, c0:c1] += (w * ux * uy).sum((1, 3))
                out["yy"][r0:r1, c0:c1] += (w * uy * uy).sum((1, 3))
            print(f"  rows {top}/{height}", end="\r", flush=True)
    print()
    return {k: v.astype(np.float32) for k, v in out.items()}


def lane_axis(m, ships) -> np.ndarray:
    """The compass bearing of each cell's lane axis, 0-180 degrees on a byte.

    The principal axis of where the positions fell inside the cell; where a
    lane is wider than a cell (the Channel is 30 km), the axis of the density
    field around it -- across its gradient, over 3 x 3 cells. Where neither
    has a direction (open sea, a port basin) the cell has NO_AXIS.
    """
    n = np.maximum(m["n"], 1e-9)
    mx, my = m["x"] / n, m["y"] / n
    cxx = m["xx"] / n - mx * mx
    cyy = m["yy"] / n - my * my
    cxy = m["xy"] / n - mx * my
    theta = 0.5 * np.arctan2(2 * cxy, cxx - cyy)        # from +x (east), y south
    spread = np.sqrt((cxx - cyy) ** 2 + 4 * cxy ** 2) / np.maximum(cxx + cyy, 1e-9)
    # Axis vector (cos t east, sin t south) -> bearing atan2(east, north).
    bearing = np.degrees(np.arctan2(np.cos(theta), -np.sin(theta))) % 180.0
    # The wide lanes: structure tensor of the log density at ~100 km (a
    # busy strait is saturated and flat over a few cells, so the scale has
    # to be a strait's), x east, y south, an east step shortened by latitude.
    rows = ships.shape[0]
    lat = LAT0 - (np.arange(rows) + 0.5) * CELL
    shrink = np.maximum(np.cos(np.radians(lat)), 0.05)[:, None]

    def box(a, r=WIDE):
        # Separable box mean over (2r + 1)^2 cells, wrapping in longitude.
        c = np.cumsum(np.pad(a, ((0, 0), (r + 1, r)), mode="wrap"), axis=1)
        a = (c[:, 2 * r + 1:] - c[:, :-2 * r - 1]) / (2 * r + 1)
        c = np.cumsum(np.pad(a, ((r + 1, r), (0, 0)), mode="edge"), axis=0)
        return (c[2 * r + 1:] - c[:-2 * r - 1]) / (2 * r + 1)

    field = box(np.log1p(ships / 1e-3))
    gx = (np.roll(field, -1, 1) - np.roll(field, 1, 1)) / (2 * shrink)
    gy = np.zeros_like(field)
    gy[1:-1] = (field[2:] - field[:-2]) / 2
    jxx, jxy, jyy = box(gx * gx), box(gx * gy), box(gy * gy)
    coherence = np.sqrt((jxx - jyy) ** 2 + 4 * jxy ** 2) / np.maximum(jxx + jyy, 1e-9)
    phi = 0.5 * np.arctan2(2 * jxy, jxx - jyy)          # the gradient's axis
    # The lane runs across the gradient: (-sin phi east, cos phi south).
    wide = np.degrees(np.arctan2(-np.sin(phi), -np.cos(phi))) % 180.0
    narrow = spread >= 0.35
    bearing = np.where(narrow, bearing, wide)
    axis = np.round(bearing * 255.0 / 180.0).astype(np.int32) % 255
    axis = axis.astype(np.uint8)
    axis[(~narrow & (coherence < 0.25)) | (ships <= 0)] = NO_AXIS
    return axis


def write_prior(planes: dict[str, np.ndarray], axis: np.ndarray, path: Path = PRIOR) -> None:
    rows, cols = axis.shape
    by, bx = -(-rows // BLOCK), -(-cols // BLOCK)
    header = json.dumps({
        "lon0": LON0, "lat0": LAT0, "cell": CELL, "rows": rows, "cols": cols, "block": BLOCK,
        "kinds": list(KINDS), "floor": FLOOR, "perOctave": PER_OCTAVE, "noAxis": NO_AXIS,
        "source": "Global Shipping Traffic Density, World Bank / IMF, AIS Jan 2015 - Feb 2021, CC BY 4.0",
    }).encode()
    blobs, index = [], []
    for j in range(by):
        for i in range(bx):
            sl = (slice(j * BLOCK, (j + 1) * BLOCK), slice(i * BLOCK, (i + 1) * BLOCK))
            stack = np.zeros((len(KINDS) + 1, BLOCK, BLOCK), np.uint8)
            for k, kind in enumerate(KINDS):
                part = planes[kind][sl]
                stack[k, :part.shape[0], :part.shape[1]] = part
            part = axis[sl]
            stack[-1, :, :] = NO_AXIS
            stack[-1, :part.shape[0], :part.shape[1]] = part
            if not stack[:-1].any():
                index.append((0, 0))
                continue
            blob = zlib.compress(stack.tobytes(), 9)
            index.append((sum(len(b) for b in blobs), len(blob)))
            blobs.append(blob)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as out:
        out.write(MAGIC + struct.pack("<I", len(header)) + header)
        out.write(b"".join(struct.pack("<II", o, n) for o, n in index))
        out.write(b"".join(blobs))


def main() -> None:
    SOURCE.mkdir(parents=True, exist_ok=True)
    moments = None
    total = None
    planes = {}
    for kind, archive, member, digest in LAYERS:
        reduced = SOURCE / f"{kind}.npz"
        if not reduced.exists():
            zpath = SOURCE / Path(archive).name
            _download(BASE + archive, zpath, digest)
            tif = SOURCE / member
            if not tif.exists():
                print("unpack", member, flush=True)
                with zipfile.ZipFile(zpath) as z:
                    z.extract(member, SOURCE)
            print("reduce", kind, flush=True)
            m = reduce_layer(tif)
            np.savez_compressed(reduced, **m)
            tif.unlink()
        m = dict(np.load(reduced))
        ships = m["n"] * SCALE[kind]
        total = ships if kind == KINDS[0] else total + ships
        planes[kind] = quantize(ships)
        if moments is None:
            moments = m
        else:
            for k in moments:
                moments[k] = moments[k] + m[k]
        print(kind, "cells with ships:", int((planes[kind] > 0).sum()), flush=True)
    write_prior(planes, lane_axis(moments, total))
    print("wrote", PRIOR, PRIOR.stat().st_size, "bytes")


if __name__ == "__main__":
    sys.exit(main())
