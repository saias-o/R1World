"""Road signs, drawn by the project from the regulation, baked into the game.

The predictive model (`native/gen/predict.cpp`) decides *where* a sign stands
and *which* one it is; this file decides what each one looks like. Nothing
here is photographed or borrowed: every face is drawn from the shapes and
proportions of the Instruction interministérielle sur la signalisation
routière (IISR, 1re et 4e parties) -- a triangle, a disc, a diamond, a band of
red -- so no third-party artwork enters the game (PLAN §4).

**Colours are albedos** (CLAUDE.md rule 2). Retroreflective sheeting is
specified by its luminance factor (EN 12899-1, class RA1): white above 0.35,
yellow 0.27, red 0.03 to 0.15, black under 0.03. Those measured values are
what the faces carry, encoded as sRGB in the PNG like every other albedo map
of the project.

**Sizes and heights** are the IISR's. Two mounts, because the regulation has
two: in the open country the lower edge of the lowest plate stands 1 m above
the carriageway and the plates are of the normal range; in town it clears a
pedestrian at 2.30 m and the plates are of the small range.

Run: `python -m r1.signage` from `game/tools`. It writes the models, their
faces, and `assets/world/signs.json`, the catalogue the generator reads.
"""

from __future__ import annotations

import json
import math
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from .mesh import Material, Mesh, MeshPart, write_glb

GAME = Path(__file__).resolve().parents[2]
MODEL_DIR = GAME / "assets" / "models" / "signs"
FACE_DIR = MODEL_DIR / "faces"
CATALOGUE = GAME / "assets" / "world" / "signs.json"
REVISION = 1

# ── measured sheeting (linear albedo) ───────────────────────────────────────
WHITE = (0.40, 0.40, 0.39)
YELLOW = (0.42, 0.26, 0.015)
RED = (0.26, 0.018, 0.02)
BLACK = (0.025, 0.025, 0.025)
# ASTM D4956 type I green, luminance factor 0.03 to 0.09: the street-name blade.
GREEN = (0.010, 0.085, 0.035)
# The back of a plate and its post: galvanised steel, weathered.
STEEL = (0.22, 0.22, 0.21)

# The two mounts: height of the lowest plate's lower edge, and the range.
MOUNTS = {"rural": (1.00, "normale"), "urban": (2.30, "petite")}
# Each country's mounts. The United States (MUTCD 2A.18): 5 ft above the road
# in the open, 7 ft where people walk; the conventional-road sizes.
COUNTRY_MOUNTS = {
    "FR": MOUNTS,
    "US": {"rural": (1.52, "us"), "urban": (2.13, "us")},
}


def mounts_of(code: str) -> dict:
    return COUNTRY_MOUNTS[code.split(":")[0]]
# IISR 4e partie, art. 5: dimensions by range, in metres.
RANGE = {
    "normale": {"triangle": 1.00, "disc": 0.85, "diamond": 0.70, "zone": (0.85, 1.25), "octagon": 0.80},
    "petite": {"triangle": 0.70, "disc": 0.65, "diamond": 0.50, "zone": (0.65, 0.95), "octagon": 0.60},
    # MUTCD table 2B-1, conventional road: STOP 30 in, YIELD 36 in, DO NOT
    # ENTER 30 in, SPEED LIMIT 24 x 30 in, the ALL WAY plaque 18 x 6 in.
    "us": {"octagon": 0.762, "triangle": 0.914, "square": 0.762, "speed": (0.610, 0.762), "plaque": (0.457, 0.152)},
}
THICKNESS = 0.012
POST_RADIUS = 0.035
PIXELS = 512
SUPERSAMPLE = 4


def _srgb(c: float) -> int:
    c = max(0.0, min(1.0, c))
    s = c * 12.92 if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
    return int(round(s * 255))


def _rgb(albedo) -> tuple[int, int, int]:
    return tuple(_srgb(c) for c in albedo)


# ── outlines ────────────────────────────────────────────────────────────────
# Every plate is convex: a regular polygon with rounded corners, a disc, or a
# rounded rectangle. The same outline cuts the plate and draws its face, so a
# border painted on the face follows the plate's own edge.

def rounded_polygon(sides: int, inradius: float, rotation: float, corner: float,
                    cx: float = 0.0, cy: float = 0.0, arc: int = 5) -> list[tuple[float, float]]:
    """A regular polygon of `inradius` whose corners are arcs of `corner`,
    counter-clockwise (y up). Insetting it by d is the same call with
    `inradius - d` and `corner - d`."""
    corner = max(0.0, min(corner, inradius * 0.9))
    core = (inradius - corner) / math.cos(math.pi / sides)  # circumradius of the core
    points = []
    for k in range(sides):
        a = rotation + 2 * math.pi * k / sides
        vx, vy = cx + core * math.cos(a), cy + core * math.sin(a)
        if corner <= 0:
            points.append((vx, vy))
            continue
        half = math.pi / sides
        for j in range(arc + 1):
            t = a - half + 2 * half * j / arc
            points.append((vx + corner * math.cos(t), vy + corner * math.sin(t)))
    return points


def disc(radius: float, sides: int = 40) -> list[tuple[float, float]]:
    return [(radius * math.cos(2 * math.pi * k / sides), radius * math.sin(2 * math.pi * k / sides))
            for k in range(sides)]


def rounded_rect(w: float, h: float, corner: float, arc: int = 4) -> list[tuple[float, float]]:
    points = []
    for (cx, cy, start) in ((w / 2 - corner, -h / 2 + corner, -90), (w / 2 - corner, h / 2 - corner, 0),
                            (-w / 2 + corner, h / 2 - corner, 90), (-w / 2 + corner, -h / 2 + corner, 180)):
        for j in range(arc + 1):
            t = math.radians(start + 90 * j / arc)
            points.append((cx + corner * math.cos(t), cy + corner * math.sin(t)))
    return points


# ── a small stroke font ─────────────────────────────────────────────────────
# The regulation's typeface (L1/L4) is a rounded, even-stroke alphabet: drawn
# here as centre lines in a unit box (y down), stroked with round caps.

def _arc(cx, cy, rx, ry, a0, a1, n=18):
    return [(cx + rx * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
             cy + ry * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]


_LETTERS = {
    "A": lambda: [[(0, 1), (0.5, 0), (1, 1)], [(0.2, 0.64), (0.8, 0.64)]],
    "B": lambda: [[(0, 1), (0, 0), (0.55, 0)] + _arc(0.55, 0.24, 0.38, 0.24, 270, 450) + [(0, 0.48)],
                  [(0.58, 0.48)] + _arc(0.58, 0.74, 0.42, 0.26, 270, 450) + [(0, 1)]],
    "C": lambda: [_arc(0.55, 0.5, 0.52, 0.5, 318, 42)],
    "D": lambda: [[(0, 1), (0, 0), (0.42, 0)] + _arc(0.42, 0.5, 0.58, 0.5, 270, 450) + [(0, 1)]],
    "E": lambda: [[(1, 0), (0, 0), (0, 1), (1, 1)], [(0, 0.5), (0.8, 0.5)]],
    "F": lambda: [[(1, 0), (0, 0), (0, 1)], [(0, 0.5), (0.8, 0.5)]],
    "G": lambda: [_arc(0.52, 0.5, 0.5, 0.5, 318, 20) + [(1.0, 0.55), (0.58, 0.55)]],
    "H": lambda: [[(0, 0), (0, 1)], [(1, 0), (1, 1)], [(0, 0.5), (1, 0.5)]],
    "I": lambda: [[(0.5, 0), (0.5, 1)]],
    "J": lambda: [[(0.85, 0), (0.85, 0.68)] + _arc(0.45, 0.68, 0.4, 0.32, 0, 170)],
    "K": lambda: [[(0, 0), (0, 1)], [(1, 0), (0, 0.62)], [(0.32, 0.42), (1, 1)]],
    "L": lambda: [[(0, 0), (0, 1), (0.92, 1)]],
    "M": lambda: [[(0, 1), (0, 0), (0.5, 0.68), (1, 0), (1, 1)]],
    "N": lambda: [[(0, 1), (0, 0), (1, 1), (1, 0)]],
    "O": lambda: [_arc(0.5, 0.5, 0.5, 0.5, 0, 360, 36)],
    "P": lambda: [[(0, 1), (0, 0), (0.55, 0)] + _arc(0.55, 0.27, 0.45, 0.27, 270, 450) + [(0, 0.54)]],
    "Q": lambda: [_arc(0.5, 0.5, 0.5, 0.5, 0, 360, 36), [(0.62, 0.72), (1.02, 1.06)]],
    "R": lambda: [[(0, 1), (0, 0), (0.55, 0)] + _arc(0.55, 0.27, 0.45, 0.27, 270, 450) + [(0, 0.54)],
                  [(0.48, 0.54), (1, 1)]],
    "S": lambda: [_arc(0.5, 0.26, 0.46, 0.26, 335, 90) + _arc(0.5, 0.74, 0.5, 0.26, 270, 515)],
    "T": lambda: [[(0, 0), (1, 0)], [(0.5, 0), (0.5, 1)]],
    "U": lambda: [[(0, 0), (0, 0.64)] + _arc(0.5, 0.64, 0.5, 0.36, 180, 0) + [(1, 0)]],
    "V": lambda: [[(0, 0), (0.5, 1), (1, 0)]],
    "W": lambda: [[(0, 0), (0.24, 1), (0.5, 0.3), (0.76, 1), (1, 0)]],
    "X": lambda: [[(0, 0), (1, 1)], [(1, 0), (0, 1)]],
    "Y": lambda: [[(0, 0), (0.5, 0.52), (1, 0)], [(0.5, 0.52), (0.5, 1)]],
    "Z": lambda: [[(0, 0), (1, 0), (0, 1), (1, 1)]],
    "-": lambda: [[(0.1, 0.56), (0.9, 0.56)]],
    "'": lambda: [[(0.55, 0), (0.4, 0.28)]],
    ".": lambda: [[(0.5, 0.97), (0.5, 1.0)]],
}
_ACCENTS = {
    "acute": [[(0.42, -0.1), (0.66, -0.3)]],
    "grave": [[(0.34, -0.3), (0.58, -0.1)]],
    "circumflex": [[(0.26, -0.1), (0.5, -0.3), (0.74, -0.1)]],
    "diaeresis": [[(0.28, -0.2), (0.28, -0.17)], [(0.72, -0.2), (0.72, -0.17)]],
    "cedilla": [[(0.52, 1.0), (0.52, 1.1), (0.66, 1.18), (0.4, 1.3)]],
}
# The capitals a French place name is written in, and the letter and mark
# each accented one is drawn from.
ACCENTED = {"À": ("A", "grave"), "Â": ("A", "circumflex"), "Ä": ("A", "diaeresis"), "Ç": ("C", "cedilla"),
            "É": ("E", "acute"), "È": ("E", "grave"), "Ê": ("E", "circumflex"), "Ë": ("E", "diaeresis"),
            "Î": ("I", "circumflex"), "Ï": ("I", "diaeresis"), "Ô": ("O", "circumflex"), "Ö": ("O", "diaeresis"),
            "Ù": ("U", "grave"), "Û": ("U", "circumflex"), "Ü": ("U", "diaeresis"), "Ÿ": ("Y", "diaeresis")}


def _glyph(ch: str) -> list[list[tuple[float, float]]]:
    if ch in _LETTERS:
        return _LETTERS[ch]()
    if ch in ACCENTED:
        base, mark = ACCENTED[ch]
        return _glyph(base) + _ACCENTS[mark]
    if ch == "0":
        return [_arc(0.5, 0.3, 0.5, 0.3, 180, 360) + [(1, 0.7)] + _arc(0.5, 0.7, 0.5, 0.3, 0, 180) + [(0, 0.3)]]
    if ch == "1":
        return [[(0.15, 0.22), (0.62, 0.0), (0.62, 1.0)]]
    if ch == "2":
        return [_arc(0.5, 0.28, 0.5, 0.28, 195, 390) + [(0.0, 1.0), (1.0, 1.0)]]
    if ch == "3":
        return [_arc(0.48, 0.25, 0.45, 0.25, 205, 450) + _arc(0.5, 0.74, 0.5, 0.26, 270, 520)]
    if ch == "4":
        return [[(0.78, 1.0), (0.78, 0.0), (0.0, 0.7), (1.0, 0.7)]]
    if ch == "5":
        return [[(0.92, 0.0), (0.14, 0.0), (0.09, 0.47)] + _arc(0.5, 0.68, 0.48, 0.32, 215, 500)]
    if ch == "6":
        return [_arc(0.5, 0.68, 0.5, 0.68, 305, 180), _arc(0.5, 0.68, 0.5, 0.32, 180, 540)]
    if ch == "7":
        return [[(0.0, 0.0), (1.0, 0.0), (0.32, 1.0)]]
    if ch == "8":
        return [_arc(0.5, 0.25, 0.42, 0.25, 0, 360), _arc(0.5, 0.73, 0.5, 0.27, 0, 360)]
    if ch == "9":
        return [[(1 - x, 1 - y) for x, y in line] for line in _glyph("6")]
    raise ValueError(f"no glyph for {ch!r}")


def _text(draw, text: str, cx: float, cy: float, height: float, width: float, gap: float,
          stroke: float, fill) -> None:
    """Centre `text` at (cx, cy) in pixels; `width` is one glyph's centre-line
    box, `stroke` the pen."""
    total = len(text) * width + (len(text) - 1) * gap
    x0 = cx - total / 2
    for i, ch in enumerate(text):
        gx = x0 + i * (width + gap)
        if ch == " ":
            continue
        for line in _glyph(ch):
            pts = [(gx + x * width, cy - height / 2 + y * height) for x, y in line]
            draw.line(pts, fill=fill, width=int(round(stroke)), joint="curve")
            r = stroke / 2
            for px, py in (pts[0], pts[-1]):
                draw.ellipse((px - r, py - r, px + r, py + r), fill=fill)


# ── plates ──────────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Plate:
    code: str        # the regulation's name, as OSM's `traffic_sign` writes it
    title: str       # what it says, for the catalogue
    shape: str       # triangle-up, triangle-down, disc, diamond, zone
    paint: Callable  # (draw, outline_px, size_px, inset) -> None


def _poly_px(points, extent, size):
    """Model outline (metres, y up, centred) to pixels (y down)."""
    k = size / extent
    return [(size / 2 + x * k, size / 2 - y * k) for x, y in points]


def outline(shape: str, rng: str, inset: float = 0.0):
    """The plate's outline in metres, centred on its middle, and its extent
    (the side of the square the face is drawn in)."""
    dims = RANGE[rng]
    if shape in ("triangle-up", "triangle-down"):
        side = dims["triangle"]
        inr = side / (2 * math.sqrt(3))
        rot = math.pi / 2 if shape == "triangle-up" else -math.pi / 2
        # Centred on the triangle's incentre, then moved so its box is centred.
        pts = rounded_polygon(3, inr - inset, rot, side * 0.06 - inset)
        shift = (inr - (side * math.sqrt(3) / 2 - inr)) / 2 * (1 if shape == "triangle-up" else -1)
        return [(x, y + shift) for x, y in pts], side
    if shape == "disc":
        d = dims["disc"]
        return disc(d / 2 - inset), d
    if shape == "diamond":
        side = dims["diamond"]
        return rounded_polygon(4, side / 2 - inset, 0.0, side * 0.05 - inset), side * math.sqrt(2)
    if shape == "zone":
        w, h = dims["zone"]
        return rounded_rect(w - 2 * inset, h - 2 * inset, max(0.0, 0.04 - inset)), h
    if shape == "octagon":
        side = dims["octagon"]  # across the flats
        return rounded_polygon(8, side / 2 - inset, math.pi / 8, max(0.0, side * 0.02 - inset)), side
    if shape == "square":
        side = dims["square"]
        return rounded_rect(side - 2 * inset, side - 2 * inset, max(0.0, 0.03 - inset)), side
    if shape in ("speed", "plaque"):
        w, h = dims[shape]
        return rounded_rect(w - 2 * inset, h - 2 * inset, max(0.0, 0.025 - inset)), max(w, h)
    raise ValueError(shape)


def _bordered(fill_border, fill_inside, band: float, rim: float = 0.012):
    """White rim, then a coloured band, then the field: the triangles and
    discs of the regulation."""
    def paint(draw, shape, rng, size):
        from PIL import ImageDraw  # noqa: F401  (Pillow is an authoring dependency)
        ext = outline(shape, rng)[1]
        draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(WHITE))
        draw.polygon(_poly_px(outline(shape, rng, rim * ext)[0], ext, size), fill=_rgb(fill_border))
        draw.polygon(_poly_px(outline(shape, rng, (rim + band) * ext)[0], ext, size), fill=_rgb(fill_inside))
        return ext
    return paint


def _paint_ab3a(draw, shape, rng, size):
    _bordered(RED, WHITE, 0.11)(draw, shape, rng, size)


def _paint_a2b(draw, shape, rng, size):
    ext = _bordered(RED, WHITE, 0.11)(draw, shape, rng, size)
    # The pictogram: a hump on the road line.
    k = size / ext
    base = size / 2 + 0.16 * ext * k
    w, h = 0.40 * ext * k, 0.13 * ext * k
    draw.rectangle((size / 2 - w * 0.75, base, size / 2 + w * 0.75, base + 0.03 * ext * k), fill=_rgb(BLACK))
    draw.chord((size / 2 - w / 2, base - h, size / 2 + w / 2, base + h), 180, 360, fill=_rgb(BLACK))


def _paint_ab6(draw, shape, rng, size):
    ext = outline(shape, rng)[1]
    draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(BLACK))
    draw.polygon(_poly_px(outline(shape, rng, 0.010 * ext)[0], ext, size), fill=_rgb(WHITE))
    draw.polygon(_poly_px(outline(shape, rng, 0.14 * ext)[0], ext, size), fill=_rgb(BLACK))
    draw.polygon(_poly_px(outline(shape, rng, 0.15 * ext)[0], ext, size), fill=_rgb(YELLOW))


def _paint_limit(value: int):
    def paint(draw, shape, rng, size):
        ext = _bordered(RED, WHITE, 0.10)(draw, shape, rng, size)
        text = str(value)
        if len(text) == 3:
            _text(draw, text, size / 2, size / 2, 0.36 * size, 0.15 * size, 0.07 * size, 0.07 * size, _rgb(BLACK))
        else:
            _text(draw, text, size / 2, size / 2, 0.40 * size, 0.20 * size, 0.09 * size, 0.08 * size, _rgb(BLACK))
    return paint


def _paint_zone30(draw, shape, rng, size):
    ext = outline(shape, rng)[1]
    draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(BLACK))
    draw.polygon(_poly_px(outline(shape, rng, 0.012 * ext)[0], ext, size), fill=_rgb(WHITE))
    _text(draw, "ZONE", size / 2, size * 0.19, 0.10 * size, 0.085 * size, 0.035 * size, 0.030 * size, _rgb(BLACK))
    # The B14 30 at the heart of the panel, drawn to its own proportions.
    r = 0.26 * size
    cx, cy = size / 2, size * 0.56
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=_rgb(RED))
    ri = r * 0.80
    draw.ellipse((cx - ri, cy - ri, cx + ri, cy + ri), fill=_rgb(WHITE))
    _text(draw, "30", cx, cy, 0.20 * size, 0.10 * size, 0.045 * size, 0.040 * size, _rgb(BLACK))


def _paint_stop(draw, shape, rng, size):
    ext = outline(shape, rng)[1]
    draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(WHITE))
    draw.polygon(_poly_px(outline(shape, rng, 0.025 * ext)[0], ext, size), fill=_rgb(RED))
    _text(draw, "STOP", size / 2, size / 2, 0.30 * size, 0.15 * size, 0.055 * size, 0.062 * size, _rgb(WHITE))


def _paint_yield(draw, shape, rng, size):
    _bordered(RED, WHITE, 0.12)(draw, shape, rng, size)
    _text(draw, "YIELD", size / 2, size * 0.33, 0.075 * size, 0.052 * size, 0.02 * size, 0.02 * size, _rgb(RED))


def _paint_all_way(draw, shape, rng, size):
    ext = outline(shape, rng)[1]
    draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(RED))
    draw.polygon(_poly_px(outline(shape, rng, 0.012 * ext)[0], ext, size), fill=_rgb(WHITE))
    _text(draw, "ALL WAY", size / 2, size / 2, 0.16 * size, 0.085 * size, 0.035 * size, 0.035 * size, _rgb(RED))


def _paint_speed(value: int):
    def paint(draw, shape, rng, size):
        ext = outline(shape, rng)[1]
        draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(WHITE))
        draw.polygon(_poly_px(outline(shape, rng, 0.02 * ext)[0], ext, size), fill=_rgb(BLACK))
        draw.polygon(_poly_px(outline(shape, rng, 0.03 * ext)[0], ext, size), fill=_rgb(WHITE))
        _text(draw, "SPEED", size / 2, size * 0.20, 0.10 * size, 0.07 * size, 0.03 * size, 0.024 * size, _rgb(BLACK))
        _text(draw, "LIMIT", size / 2, size * 0.36, 0.10 * size, 0.07 * size, 0.03 * size, 0.024 * size, _rgb(BLACK))
        _text(draw, str(value), size / 2, size * 0.66, 0.30 * size, 0.15 * size, 0.06 * size, 0.06 * size, _rgb(BLACK))
    return paint


def _paint_dne(draw, shape, rng, size):
    ext = outline(shape, rng)[1]
    draw.polygon(_poly_px(outline(shape, rng)[0], ext, size), fill=_rgb(BLACK))
    draw.polygon(_poly_px(outline(shape, rng, 0.008 * ext)[0], ext, size), fill=_rgb(WHITE))
    r = 0.46 * size
    draw.ellipse((size / 2 - r, size / 2 - r, size / 2 + r, size / 2 + r), fill=_rgb(RED))
    draw.rectangle((size * 0.2, size * 0.45, size * 0.8, size * 0.55), fill=_rgb(WHITE))
    _text(draw, "DO NOT", size / 2, size * 0.33, 0.09 * size, 0.06 * size, 0.022 * size, 0.022 * size, _rgb(WHITE))
    _text(draw, "ENTER", size / 2, size * 0.67, 0.09 * size, 0.06 * size, 0.022 * size, 0.022 * size, _rgb(WHITE))


LIMITS = (20, 30, 40, 50, 60, 70, 80, 90, 110, 130)
US_LIMITS = tuple(range(15, 80, 5))

PLATES = {
    "FR:AB3a": Plate("FR:AB3a", "Cédez le passage", "triangle-down", _paint_ab3a),
    "FR:AB6": Plate("FR:AB6", "Route prioritaire", "diamond", _paint_ab6),
    "FR:AB4": Plate("FR:AB4", "Arrêt à l'intersection", "octagon", _paint_stop),
    "FR:A2b": Plate("FR:A2b", "Ralentisseur de type dos-d'âne", "triangle-up", _paint_a2b),
    "FR:B30": Plate("FR:B30", "Entrée d'une zone 30", "zone", _paint_zone30),
    **{f"FR:B14[{v}]": Plate(f"FR:B14[{v}]", f"Limitation de vitesse à {v} km/h", "disc", _paint_limit(v))
       for v in LIMITS},
    # The United States, from the MUTCD.
    "US:R1-1": Plate("US:R1-1", "Stop", "octagon", _paint_stop),
    "US:R1-3P": Plate("US:R1-3P", "All way", "plaque", _paint_all_way),
    "US:R1-2": Plate("US:R1-2", "Yield", "triangle-down", _paint_yield),
    "US:R5-1": Plate("US:R5-1", "Do not enter", "square", _paint_dne),
    **{f"US:R2-1[{v}]": Plate(f"US:R2-1[{v}]", f"Speed limit {v} mph", "speed", _paint_speed(v)) for v in US_LIMITS},
}

# A sign is plates on one post, top first, as the regulation stacks them.
# A plaque never stands alone: it is hung under the sign it qualifies.
PLAQUES = {"US:R1-3P"}
SIGNS = {
    **{code: (code,) for code in PLATES if code not in PLAQUES},
    "FR:A2b,FR:B14[30]": ("FR:A2b", "FR:B14[30]"),
    "US:R1-1,US:R1-3P": ("US:R1-1", "US:R1-3P"),
}


def slug(code: str) -> str:
    return code.lower().replace(":", "_").replace("[", "_").replace("]", "").replace(",", "+")


# ── faces ───────────────────────────────────────────────────────────────────

def face_path(code: str, rng: str) -> Path:
    return FACE_DIR / f"{slug(code)}-{rng}-r{REVISION}.png"


def draw_face(plate: Plate, rng: str) -> Path:
    from PIL import Image, ImageDraw
    path = face_path(plate.code, rng)
    big = PIXELS * SUPERSAMPLE
    # The margin outside the plate takes the edge colour, so a mip level never
    # bleeds a foreign colour into the rim.
    image = Image.new("RGB", (big, big), _rgb(WHITE))
    plate.paint(ImageDraw.Draw(image), plate.shape, rng, big)
    image = image.resize((PIXELS, PIXELS), Image.LANCZOS)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(f".{os.getpid()}.tmp.png")
    image.save(temporary, optimize=True)
    os.replace(temporary, path)
    return path


# ── models ──────────────────────────────────────────────────────────────────

def _plate_mesh(points, extent: float, lift: float, face: Mesh, back: Mesh) -> None:
    """A plate facing +Z, its outline centred at (0, lift); the face UVs map
    the drawn square of side `extent`."""
    z0, z1 = -THICKNESS / 2, THICKNESS / 2
    uv = lambda x, y: (0.5 + x / extent, 0.5 - y / extent)  # noqa: E731
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    n = len(points)
    for i in range(n):
        (ax, ay), (bx, by) = points[i], points[(i + 1) % n]
        face.add_triangle((cx, cy + lift, z1), (ax, ay + lift, z1), (bx, by + lift, z1),
                          (uv(cx, cy), uv(ax, ay), uv(bx, by)))
        back.add_triangle((cx, cy + lift, z0), (bx, by + lift, z0), (ax, ay + lift, z0))
        back.add_quad((ax, ay + lift, z0), (bx, by + lift, z0), (bx, by + lift, z1), (ax, ay + lift, z1))


def _post(mesh: Mesh, top: float, sides: int = 8) -> None:
    zc = -THICKNESS / 2 - POST_RADIUS
    ring = [(POST_RADIUS * math.cos(2 * math.pi * k / sides), zc + POST_RADIUS * math.sin(2 * math.pi * k / sides))
            for k in range(sides)]
    for k in range(sides):
        (ax, az), (bx, bz) = ring[k], ring[(k + 1) % sides]
        mesh.add_quad((ax, 0.0, az), (ax, top, az), (bx, top, bz), (bx, 0.0, bz))
        mesh.add_triangle((0.0, top, zc), (bx, top, bz), (ax, top, az))


def model_path(sign: str, mount: str) -> Path:
    return MODEL_DIR / f"{slug(sign)}-{mount}-r{REVISION}.glb"


def bake(sign: str, mount: str) -> tuple[Path, int, float]:
    """One sign on one mount: its path, vertex count and height."""
    low, rng = mounts_of(sign)[mount]
    parts: list[MeshPart] = []
    back, post = Mesh(), Mesh()
    gap = 0.05
    y = low
    placed = []
    # Stacked from the bottom: the last plate listed is the lowest.
    for code in reversed(SIGNS[sign]):
        plate = PLATES[code]
        pts, extent = outline(plate.shape, rng)
        ys = [p[1] for p in pts]
        lift = y - min(ys)
        placed.append((plate, pts, extent, lift))
        y = lift + max(ys) + gap
    top = y - gap
    for plate, pts, extent, lift in placed:
        face = Mesh()
        _plate_mesh(pts, extent, lift, face, back)
        uri = os.path.relpath(face_path(plate.code, rng), MODEL_DIR).replace(os.sep, "/")
        parts.append(MeshPart(f"Face {plate.code}", face,
                              Material(f"Sign face {plate.code}", (1.0, 1.0, 1.0, 1.0), 0.45, base_color_texture=uri)))
    _post(post, top - 0.02)
    parts.append(MeshPart("Back", back, Material("Sign back", (*STEEL, 1.0), 0.6)))
    parts.append(MeshPart("Post", post, Material("Sign post", (*STEEL, 1.0), 0.5)))
    path = model_path(sign, mount)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(f".{os.getpid()}.tmp")
    write_glb(temporary, parts)
    os.replace(temporary, path)
    return path, sum(len(p.mesh.positions) for p in parts), top


# ── town signs (EB10, EB20) ─────────────────────────────────────────────────
# A place's name cannot be a model per commune: the sign is assembled in the
# game from shared parts (CLAUDE.md rule 5) -- two posts, the plate's two ends
# and a middle stretched to the name, one small model per letter drawn from a
# single atlas, and for the way out, the red bar across it.

TOWN_CAP = 0.14          # capital height, metres
TOWN_LINE = 0.23         # from one line's baseline to the next
TOWN_PAD = 0.13          # between the border and the name, above and below it
TOWN_END = 0.10          # width of each end part of the plate
TOWN_BAND = 0.045        # the red border
TOWN_LOWER_EDGE = 1.0
GLYPH_STROKE = 0.16      # in capital heights
GLYPH_SPACING = 0.12
GLYPH_PIXELS = 96        # atlas pixels per capital height
GLYPH_ABOVE, GLYPH_BELOW = 0.36, 0.36  # room for accents and the cedilla
# Each glyph's centre-line box width, in capital heights.
GLYPH_WIDTH = {"I": 0.02, "M": 0.74, "W": 0.9, "-": 0.36, "'": 0.14, ".": 0.02, "J": 0.5, "L": 0.5, "F": 0.5,
               "E": 0.52, "T": 0.6, "O": 0.66, "Q": 0.66, "C": 0.56, "G": 0.6, "D": 0.6}
TOWN_GLYPHS = [chr(c) for c in range(ord("A"), ord("Z") + 1)] + list(ACCENTED) + list("0123456789-'.")


def town_plate_height(lines: int) -> float:
    return TOWN_CAP + 2 * TOWN_PAD + (lines - 1) * TOWN_LINE


def _flat_part(width: float, height: float, x0: float, texture, color=(1.0, 1.0, 1.0),
               front: float = THICKNESS / 2, both: bool = False) -> list:
    """A rectangle facing +Z from x0 to x0 + width, from 0 to height, mapped
    whole to its texture; its back and edges in steel, or with `both` its
    back faced too (a street-name blade is read from either side)."""
    face, rear = Mesh(), Mesh()
    x1 = x0 + width
    face.add_quad((x0, 0.0, front), (x1, 0.0, front), (x1, height, front), (x0, height, front),
                  ((0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)))
    z0 = -THICKNESS / 2
    if both:
        face.add_quad((x1, 0.0, z0), (x0, 0.0, z0), (x0, height, z0), (x1, height, z0),
                      ((1.0, 1.0), (0.0, 1.0), (0.0, 0.0), (1.0, 0.0)))
    else:
        rear.add_quad((x1, 0.0, z0), (x0, 0.0, z0), (x0, height, z0), (x1, height, z0))
    for (a, b) in (((x0, 0.0), (x1, 0.0)), ((x1, height), (x0, height)), ((x1, 0.0), (x1, height)),
                   ((x0, height), (x0, 0.0))):
        rear.add_quad((a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], front), (a[0], a[1], front))
    return [MeshPart("Face", face, Material("Sign face town", (*color, 1.0), 0.45, base_color_texture=texture)),
            MeshPart("Back", rear, Material("Sign back", (*STEEL, 1.0), 0.6))]


def _town_textures(lines: int) -> dict:
    """The plate's two ends and its middle, for a plate of `lines` lines."""
    from PIL import Image, ImageDraw
    height = town_plate_height(lines)
    per_metre = 512
    out = {}
    h = int(round(height * per_metre))
    rim, band = int(0.012 * per_metre), int(TOWN_BAND * per_metre)
    for part, width in (("left", TOWN_END), ("middle", 0.25), ("right", TOWN_END)):
        w = int(round(width * per_metre))
        image = Image.new("RGB", (w, h), _rgb(WHITE))
        draw = ImageDraw.Draw(image)
        # The red band top and bottom everywhere; at the ends, down the side too.
        draw.rectangle((0, rim, w, rim + band), fill=_rgb(RED))
        draw.rectangle((0, h - rim - band, w, h - rim), fill=_rgb(RED))
        if part == "left":
            draw.rectangle((rim, rim, rim + band, h - rim), fill=_rgb(RED))
        if part == "right":
            draw.rectangle((w - rim - band, rim, w - rim, h - rim), fill=_rgb(RED))
        path = FACE_DIR / f"town_{part}_{lines}-r{REVISION}.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path, optimize=True)
        out[part] = path
    return out


def _glyph_box(ch: str) -> float:
    base = ACCENTED[ch][0] if ch in ACCENTED else ch
    return GLYPH_WIDTH.get(base, 0.56)


ACCENT_WIDTH = 0.5  # an accent keeps its width over a narrow letter


def glyph_advance(ch: str) -> float:
    """In capital heights."""
    box = max(_glyph_box(ch), ACCENT_WIDTH if ch in ACCENTED else 0.0)
    return box + GLYPH_STROKE + GLYPH_SPACING


def _glyph_atlas(ink=BLACK, paper=WHITE, name="town_glyphs"):
    from PIL import Image, ImageDraw
    k = GLYPH_PIXELS * SUPERSAMPLE
    cell_h = int(round((1 + GLYPH_ABOVE + GLYPH_BELOW) * GLYPH_PIXELS))
    widths = {ch: int(round(glyph_advance(ch) * GLYPH_PIXELS)) for ch in TOWN_GLYPHS}
    atlas_w = 2048
    rows, x, y, placed = 1, 0, 0, {}
    for ch in TOWN_GLYPHS:
        if x + widths[ch] > atlas_w:
            x, y, rows = 0, y + cell_h, rows + 1
        placed[ch] = (x, y)
        x += widths[ch]
    atlas_h = rows * cell_h
    big = Image.new("RGB", (atlas_w * SUPERSAMPLE, atlas_h * SUPERSAMPLE), _rgb(paper))
    draw = ImageDraw.Draw(big)
    for ch in TOWN_GLYPHS:
        px, py = placed[ch]
        centre = (px + glyph_advance(ch) / 2 * GLYPH_PIXELS) * SUPERSAMPLE
        top = (py + GLYPH_ABOVE * GLYPH_PIXELS) * SUPERSAMPLE
        box = _glyph_box(ch)
        stroke = GLYPH_STROKE * k
        letter = _glyph(ACCENTED[ch][0]) if ch in ACCENTED else _glyph(ch)
        marks = _ACCENTS[ACCENTED[ch][1]] if ch in ACCENTED else []
        for line, width in [(l, box) for l in letter] + [(m, max(box, ACCENT_WIDTH)) for m in marks]:
            pts = [(centre + (gx - 0.5) * width * k, top + gy * k) for gx, gy in line]
            draw.line(pts, fill=_rgb(ink), width=int(round(stroke)), joint="curve")
            for qx, qy in (pts[0], pts[-1]):
                draw.ellipse((qx - stroke / 2, qy - stroke / 2, qx + stroke / 2, qy + stroke / 2), fill=_rgb(ink))
    image = big.resize((atlas_w, atlas_h), Image.LANCZOS)
    path = FACE_DIR / f"{name}-r{REVISION}.png"
    image.save(path, optimize=True)
    uv = {ch: (placed[ch][0] / atlas_w, placed[ch][1] / atlas_h, (placed[ch][0] + widths[ch]) / atlas_w,
               (placed[ch][1] + cell_h) / atlas_h) for ch in TOWN_GLYPHS}
    return path, uv


def _write(parts: list, name: str, folder: str = "town") -> str:
    path = MODEL_DIR / folder / f"{name}-r{REVISION}.glb"
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(f".{os.getpid()}.tmp")
    write_glb(temporary, parts)
    os.replace(temporary, path)
    return path.relative_to(GAME).as_posix()


def _uri(texture: Path, folder: str = "town") -> str:
    return os.path.relpath(texture, MODEL_DIR / folder).replace(os.sep, "/")


def bake_town_kit() -> dict:
    kit = {"cap": TOWN_CAP, "line": TOWN_LINE, "pad": TOWN_PAD, "end": TOWN_END, "lowerEdge": TOWN_LOWER_EDGE,
           "plates": {}, "glyphs": {}}
    for lines in (1, 2):
        textures = _town_textures(lines)
        height = town_plate_height(lines)
        kit["plates"][str(lines)] = {
            "height": round(height, 4),
            # Each end from its outer edge inward; the middle one metre wide, from 0.
            "left": _write(_flat_part(TOWN_END, height, 0.0, _uri(textures["left"])), f"plate_left_{lines}"),
            "middle": _write(_flat_part(1.0, height, 0.0, _uri(textures["middle"])), f"plate_middle_{lines}"),
            "right": _write(_flat_part(TOWN_END, height, -TOWN_END, _uri(textures["right"])), f"plate_right_{lines}"),
        }
    post = Mesh()
    _post(post, 1.0)
    kit["post"] = _write([MeshPart("Post", post, Material("Sign post", (*STEEL, 1.0), 0.5))], "post")
    # The way out: a red bar, one metre square, turned and stretched across.
    bar = Mesh()
    z = THICKNESS / 2 + 0.004
    bar.add_quad((-0.5, -0.5, z), (0.5, -0.5, z), (0.5, 0.5, z), (-0.5, 0.5, z))
    kit["bar"] = _write([MeshPart("Bar", bar, Material("Sign bar", (*RED, 1.0), 0.45))], "bar")
    atlas, uv = _glyph_atlas()
    for ch in TOWN_GLYPHS:
        u0, v0, u1, v1 = uv[ch]
        width = glyph_advance(ch) * TOWN_CAP
        z = THICKNESS / 2 + 0.002
        y0, y1 = -GLYPH_BELOW * TOWN_CAP, (1 + GLYPH_ABOVE) * TOWN_CAP
        m = Mesh()
        m.add_quad((0.0, y0, z), (width, y0, z), (width, y1, z), (0.0, y1, z), ((u0, v1), (u1, v1), (u1, v0), (u0, v0)))
        name = "glyph_" + "_".join(f"{ord(c):04x}" for c in ch)
        material = Material("Sign letters", (1.0, 1.0, 1.0, 1.0), 0.45, base_color_texture=_uri(atlas))
        kit["glyphs"][ch] = {"model": _write([MeshPart("Glyph", m, material)], name), "advance": round(width, 5)}
    return kit


# ── street-name blades (US D3-1) ────────────────────────────────────────────
# White capitals on green, read from both sides, two blades crossed on one
# post at each corner. Assembled in the game like a town's sign.

BLADE_CAP = 0.10          # 4 in capitals, MUTCD 2D.43 on a residential street
BLADE_PAD = 0.05
BLADE_END = 0.04
BLADE_LOWER_EDGE = 2.45   # the lower blade, clear of a truck's mirror
BLADE_STACK = 0.22        # the upper blade above it


def _blade_textures() -> dict:
    from PIL import Image, ImageDraw
    height = BLADE_CAP + 2 * BLADE_PAD
    per_metre = 512
    h = int(round(height * per_metre))
    rim, line = int(0.006 * per_metre), max(1, int(0.008 * per_metre))
    out = {}
    for part, width in (("left", BLADE_END), ("middle", 0.25), ("right", BLADE_END)):
        w = int(round(width * per_metre))
        image = Image.new("RGB", (w, h), _rgb(GREEN))
        draw = ImageDraw.Draw(image)
        draw.rectangle((0, rim, w, rim + line), fill=_rgb(WHITE))
        draw.rectangle((0, h - rim - line, w, h - rim), fill=_rgb(WHITE))
        if part == "left":
            draw.rectangle((rim, rim, rim + line, h - rim), fill=_rgb(WHITE))
        if part == "right":
            draw.rectangle((w - rim - line, rim, w - rim, h - rim), fill=_rgb(WHITE))
        path = FACE_DIR / f"blade_{part}-r{REVISION}.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path, optimize=True)
        out[part] = path
    return out


def bake_blade_kit() -> dict:
    height = BLADE_CAP + 2 * BLADE_PAD
    kit = {"cap": BLADE_CAP, "line": BLADE_STACK, "pad": BLADE_PAD, "end": BLADE_END, "lowerEdge": BLADE_LOWER_EDGE,
           "plates": {}, "glyphs": {}}
    textures = _blade_textures()
    kit["plates"]["1"] = {
        "height": round(height, 4),
        "left": _write(_flat_part(BLADE_END, height, 0.0, _uri(textures["left"], "street"), both=True), "blade_left", "street"),
        "middle": _write(_flat_part(1.0, height, 0.0, _uri(textures["middle"], "street"), both=True), "blade_middle", "street"),
        "right": _write(_flat_part(BLADE_END, height, -BLADE_END, _uri(textures["right"], "street"), both=True), "blade_right", "street"),
    }
    post = Mesh()
    _post(post, 1.0)
    kit["post"] = _write([MeshPart("Post", post, Material("Sign post", (*STEEL, 1.0), 0.5))], "post", "street")
    kit["bar"] = ""
    atlas, uv = _glyph_atlas(WHITE, GREEN, "blade_glyphs")
    for ch in TOWN_GLYPHS:
        u0, v0, u1, v1 = uv[ch]
        width = glyph_advance(ch) * BLADE_CAP
        z = THICKNESS / 2 + 0.002
        y0, y1 = -GLYPH_BELOW * BLADE_CAP, (1 + GLYPH_ABOVE) * BLADE_CAP
        m = Mesh()
        m.add_quad((0.0, y0, z), (width, y0, z), (width, y1, z), (0.0, y1, z), ((u0, v1), (u1, v1), (u1, v0), (u0, v0)))
        name = "glyph_" + "_".join(f"{ord(c):04x}" for c in ch)
        material = Material("Blade letters", (1.0, 1.0, 1.0, 1.0), 0.45, base_color_texture=_uri(atlas, "street"))
        kit["glyphs"][ch] = {"model": _write([MeshPart("Glyph", m, material)], name, "street"), "advance": round(width, 5)}
    return kit


def ship() -> Path:
    for plate in PLATES.values():
        for rng in {r for _, r in mounts_of(plate.code).values()}:
            draw_face(plate, rng)
    signs = {}
    for sign, plates in SIGNS.items():
        entry = {"plates": [{"code": c, "title": PLATES[c].title} for c in plates], "mounts": {}}
        for mount in mounts_of(sign):
            path, vertices, top = bake(sign, mount)
            entry["mounts"][mount] = {"model": path.relative_to(GAME).as_posix(), "vertices": vertices,
                                      "height": round(top, 3)}
        signs[sign] = entry
    doc = {
        "schema": 1,
        "revision": REVISION,
        "note": "Road signs drawn by the project from the IISR and the MUTCD (tools/r1/signage.py). "
                "Keys are OSM traffic_sign codes; the generator predicts which stands where (native/gen/predict.cpp).",
        "mounts": {m: {"lowerEdge": low, "range": rng} for m, (low, rng) in MOUNTS.items()},
        "signs": signs,
        # Assembled in the game around a place's name (native/gen/predict.cpp).
        "towns": {"FR": bake_town_kit()},
        # Street names at the corners, assembled the same way.
        "streets": {"US": bake_blade_kit()},
    }
    CATALOGUE.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    return CATALOGUE


if __name__ == "__main__":
    print(ship())
