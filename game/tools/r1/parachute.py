"""The arrival's parachute: a Creative Commons canopy, normalised (plan §4).

    python -m r1.parachute            (from game/tools; standard library only)

The project draws no parachute and has none on disk, and nothing better than a
downloaded one covers it (CLAUDE.md rule 1): the canopy is "Parachute" by
Poly by Google, CC BY 3.0, chosen by the project owner on 20 October 2026
from poly.pizza. It is a ram-air canopy -- the rectangular, cell-built wing a
skydiver flies, not a round one -- with its suspension lines.

It does not enter as downloaded:

- **Its colours are paint** (rule 2). The download carries one texture, with
  light and shadow painted into it, at up to 0.95 where dyed nylon is a third
  of that. Each triangle is classified by the colour its texture shows at its
  centre, and drawn in one flat albedo per class: five materials, no texture.
  A colour no class names is refused, as `normalize.repaint_kit_model`
  refuses an unnamed material: a canopy that enters in its own paint is the
  thing §4 exists to prevent.
- **Its lines end on a kite's bar**, 2.2 m wide. A skydiver's risers meet at
  his shoulders: the bar is removed and the lines' lower ends brought onto two
  points a shoulder-width apart, the canopy's origin between them.
- **Its front is +Z**, glTF's forward. This low-poly wing is symmetric fore
  and aft (its profile is as thick at either edge, measured here), so the
  front is a convention; a wing that ever comes with a real airfoil is turned
  until its thick leading edge faces +Z. The game turns it like his body.

The source stays byte-identical in `data/source-assets/`, pinned by digest;
`assets/models/external/parachute/parachute.glb` is the output, and the
provenance record in `assets/THIRD_PARTY_ASSETS.json` is rewritten with it.
"""
from __future__ import annotations

import colorsys
import hashlib
import json
import math
import struct
import urllib.request
import zlib
from pathlib import Path

from .normalize import _read_glb, _write_glb

ROOT = Path(__file__).resolve().parents[3]
GAME = ROOT / "game"
SOURCE = ROOT / "data" / "source-assets" / "poly_google_parachute_cd4e55aa.glb"
SOURCE_URL = "https://static.poly.pizza/cd4e55aa-0c83-4bc4-b2ee-a0c4a9506e31.glb"
SOURCE_PAGE = "https://poly.pizza/m/3Z7vJ96JIEB"
SOURCE_SHA256 = "a8e25997fa97b434fd8516ffceb3f9248318f21441ea03e29d8b656a4834a28f"
OUT = GAME / "assets" / "models" / "external" / "parachute" / "parachute.glb"
LICENSE = GAME / "assets" / "licenses" / "Poly-by-Google-Parachute-CC-BY-3.0.txt"
REGISTRY = GAME / "assets" / "THIRD_PARTY_ASSETS.json"
RECORD_NAME = "Parachute — Poly by Google"

# Albedos, not paint chips (CLAUDE.md rule 2): dyed ripstop nylon and white
# lines kept under the 0.35 this world's light saturates at, as the traffic
# paints are (white car paint 0.34). Ripstop has a little sheen.
CLASSES = {
    "canopy-red": ((0.30, 0.040, 0.035), 0.60),
    "canopy-yellow": ((0.33, 0.27, 0.040), 0.60),
    "canopy-blue": ((0.030, 0.11, 0.24), 0.60),
    "canopy-black": ((0.040, 0.040, 0.045), 0.62),
    "lines": ((0.30, 0.30, 0.28), 0.70),
}
# The risers meet at the shoulders, this far apart (metres, at full scale).
SHOULDERS = 0.42
# What the kite's bar is: everything wholly below this height. Below the
# second, a vertex is a line's lower end, wherever the bar's corners put it.
BAR_TOP = 0.4
LINE_FOOT = 1.2


def fetch() -> bytes:
    """The pinned source, downloaded once into data/source-assets."""
    if not SOURCE.exists():
        SOURCE.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(SOURCE_URL, timeout=60) as response:
            payload = response.read()
        part = SOURCE.with_suffix(".part")
        part.write_bytes(payload)
        part.replace(SOURCE)
    payload = SOURCE.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    if digest != SOURCE_SHA256:
        raise RuntimeError(f"{SOURCE.name} has SHA-256 {digest}, pinned {SOURCE_SHA256}: not the canopy chosen")
    return payload


def decode_png(png: bytes) -> tuple[int, int, int, list[bytes]]:
    """Width, height, bytes per pixel and rows of an 8-bit RGB(A) PNG."""
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("the canopy's texture is not a PNG")
    cursor, idat, header = 8, b"", None
    while cursor < len(png):
        length, kind = struct.unpack_from(">I4s", png, cursor)
        body = png[cursor + 8:cursor + 8 + length]
        cursor += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace:
        raise RuntimeError(f"unsupported PNG (depth {depth}, colour type {colour}, interlace {interlace})")
    bpp = 3 if colour == 2 else 4
    raw, stride = zlib.decompress(idat), width * bpp
    rows, previous, at = [], bytearray(stride), 0
    for _ in range(height):
        kind = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = previous[x]
            c = previous[x - bpp] if x >= bpp else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        previous = line
    return width, height, bpp, rows


def classify(rgb: tuple[int, int, int]) -> str:
    """The fabric a painted colour stands for, or a refusal."""
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in rgb))
    hue = h * 360
    if v > .62 and s < .18:
        return "lines"
    if s < .25:
        return "canopy-black"
    if hue < 20 or hue > 340:
        return "canopy-red"
    if 40 <= hue <= 75:
        return "canopy-yellow"
    if 170 <= hue <= 215:
        return "canopy-blue"
    raise RuntimeError(f"painted colour {rgb} (hue {hue:.0f}, saturation {s:.2f}) names no fabric: "
                       "the canopy would enter in its own paint (plan §4)")


def _accessor(document: dict, binary: bytes, index: int) -> list:
    accessor = document["accessors"][index]
    view = document["bufferViews"][accessor["bufferView"]]
    offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    count = accessor["count"]
    if accessor["componentType"] == 5126:
        width = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[accessor["type"]]
        stride = view.get("byteStride", 0) or 4 * width
        return [struct.unpack_from(f"<{width}f", binary, offset + k * stride) for k in range(count)]
    code = {5121: "B", 5123: "H", 5125: "I"}[accessor["componentType"]]
    return list(struct.unpack_from(f"<{count}{code}", binary, offset))


def normalise(payload: bytes) -> tuple[bytes, dict]:
    document, binary = _read_glb(payload)
    if len(document["meshes"]) != 1 or len(document["meshes"][0]["primitives"]) != 1:
        raise RuntimeError("the canopy was one mesh of one primitive; the download has changed")
    primitive = document["meshes"][0]["primitives"][0]
    positions = _accessor(document, binary, primitive["attributes"]["POSITION"])
    normals = _accessor(document, binary, primitive["attributes"]["NORMAL"])
    uvs = _accessor(document, binary, primitive["attributes"]["TEXCOORD_0"])
    indices = _accessor(document, binary, primitive["indices"])
    image = document["images"][0]
    view = document["bufferViews"][image["bufferView"]]
    width, height, bpp, rows = decode_png(binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]])

    def painted(u: float, v: float) -> tuple[int, int, int]:
        x = min(width - 1, max(0, int(u * width)))
        y = min(height - 1, max(0, int(v * height)))
        return tuple(rows[y][x * bpp:x * bpp + 3])

    # The kite's bar goes; the lines' lower ends come onto the shoulders.
    positions = [list(p) for p in positions]
    triangles, bar = [], 0
    for t in range(0, len(indices), 3):
        corner = indices[t:t + 3]
        if max(positions[k][1] for k in corner) < BAR_TOP:
            bar += 1
            continue
        triangles.append(corner)
    lowered = set()
    for corner in triangles:
        for k in corner:
            if positions[k][1] < LINE_FOOT and k not in lowered:
                lowered.add(k)
                positions[k][0] = math.copysign(SHOULDERS / 2, positions[k][0] if positions[k][0] else 1.0)
                positions[k][1] = 0.0
                positions[k][2] = 0.0
    if bar == 0 or not lowered:
        raise RuntimeError("no kite bar found under the canopy; the download has changed")

    # Each triangle drawn in the fabric its paint stood for.
    by_class: dict[str, list[tuple[int, int, int]]] = {name: [] for name in CLASSES}
    for corner in triangles:
        u = sum(uvs[k][0] for k in corner) / 3
        v = sum(uvs[k][1] for k in corner) / 3
        name = classify(painted(u, v))
        if name != "lines" and min(positions[k][1] for k in corner) < 3.5:
            name = "lines"  # a line's thin end can sample the paint beside it
        by_class[name].append(corner)

    # The front: the airfoil is thickest near its leading edge. Measured in
    # the centre cells only, where the arch adds nothing to the thickness.
    canopy = [positions[k] for name, tris in by_class.items() if name != "lines" for c in tris for k in c]
    centre = [p for p in canopy if abs(p[0]) < 1.0]
    front = [p[1] for p in centre if p[2] > 1.2]
    back = [p[1] for p in centre if p[2] < -1.2]
    thickness = {"+Z": max(front) - min(front), "-Z": max(back) - min(back)}
    leading = max(thickness, key=thickness.get)
    if abs(thickness["+Z"] - thickness["-Z"]) < .05 * max(thickness.values()):
        leading = "symmetric"
    if leading == "-Z":
        for p in positions:
            p[0], p[2] = -p[0], -p[2]
        normals = [(-n[0], n[1], -n[2]) for n in normals]

    # One compact vertex list per class, flat albedo materials, no texture.
    out_binary = bytearray()
    accessors, views, materials, primitives = [], [], [], []

    def add_view(data: bytes, target: int) -> int:
        while len(out_binary) % 4:
            out_binary.append(0)
        views.append({"buffer": 0, "byteOffset": len(out_binary), "byteLength": len(data), "target": target})
        out_binary.extend(data)
        return len(views) - 1

    counts = {}
    for name, tris in by_class.items():
        if not tris:
            raise RuntimeError(f"the canopy has no {name}: the download has changed")
        remap: dict[int, int] = {}
        order: list[int] = []
        local: list[int] = []
        for corner in tris:
            for k in corner:
                if k not in remap:
                    remap[k] = len(order)
                    order.append(k)
                local.append(remap[k])
        pos = [positions[k] for k in order]
        nrm = [normals[k] for k in order]
        pos_view = add_view(b"".join(struct.pack("<3f", *p) for p in pos), 34962)
        nrm_view = add_view(b"".join(struct.pack("<3f", *n) for n in nrm), 34962)
        idx_view = add_view(struct.pack(f"<{len(local)}H", *local), 34963)
        accessors.append({"bufferView": pos_view, "componentType": 5126, "count": len(pos), "type": "VEC3",
                          "min": [min(p[i] for p in pos) for i in range(3)],
                          "max": [max(p[i] for p in pos) for i in range(3)]})
        accessors.append({"bufferView": nrm_view, "componentType": 5126, "count": len(nrm), "type": "VEC3"})
        accessors.append({"bufferView": idx_view, "componentType": 5123, "count": len(local), "type": "SCALAR"})
        colour, roughness = CLASSES[name]
        materials.append({"name": name, "doubleSided": True,
                          "pbrMetallicRoughness": {"baseColorFactor": [*colour, 1.0],
                                                   "metallicFactor": 0.0, "roughnessFactor": roughness}})
        primitives.append({"attributes": {"POSITION": len(accessors) - 3, "NORMAL": len(accessors) - 2},
                           "indices": len(accessors) - 1, "material": len(materials) - 1})
        counts[name] = len(tris)
    out = {
        "asset": {"version": "2.0", "generator": "game/tools/r1/parachute.py",
                  "copyright": "\"Parachute\" by Poly by Google (" + SOURCE_PAGE + "), CC BY 3.0; modified by R1World"},
        "scene": 0, "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "parachute", "mesh": 0}],
        "meshes": [{"name": "parachute", "primitives": primitives}],
        "materials": materials, "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(out_binary)}],
    }
    every = [p for p in positions]
    report = {"triangles": counts, "barTriangles": bar, "linesLowered": len(lowered), "leadingEdge": leading,
              "thickness": thickness,
              "span": max(p[0] for p in every) - min(p[0] for p in every),
              "height": max(p[1] for p in every), "chord": max(p[2] for p in every) - min(p[2] for p in every)}
    return _write_glb(out, bytes(out_binary)), report


def record(report: dict) -> dict:
    return {
        "name": RECORD_NAME,
        "author": "Poly by Google",
        "source": SOURCE_PAGE,
        "license": "CC BY 3.0",
        "license_url": "https://creativecommons.org/licenses/by/3.0/",
        "licenseFile": LICENSE.relative_to(GAME).as_posix(),
        "generatedBy": "game/tools/r1/parachute.py",
        "sourceSha256": SOURCE_SHA256,
        "modifications": "Kite bar removed and line ends brought onto the shoulders; painted texture replaced by "
                         "five flat measured-albedo materials (red, yellow, blue and black ripstop, white lines); "
                         "front at +Z (the wing is symmetric fore and aft). Geometry otherwise as downloaded.",
        "files": [OUT.relative_to(GAME).as_posix()],
    }


def write_registry(entry: dict) -> None:
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    assets = [a for a in registry["assets"] if a.get("name") != RECORD_NAME]
    assets.append(entry)
    registry["assets"] = assets
    with REGISTRY.open("w", encoding="utf-8", newline="\r\n") as f:
        f.write(json.dumps(registry, indent=2, ensure_ascii=False) + "\n")


def main() -> None:
    glb, report = normalise(fetch())
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(glb)
    LICENSE.write_text(
        "Parachute model -- Creative Commons Attribution 3.0 Unported\n"
        "License: https://creativecommons.org/licenses/by/3.0/\n\n"
        "\"Parachute\" by Poly by Google.\n"
        f"Original: {SOURCE_PAGE} (file {SOURCE_URL})\n\n"
        "R1World modifications: removed the kite bar under the lines and brought the\n"
        "lines' lower ends onto the wearer's shoulders; replaced the painted texture\n"
        "with flat measured-albedo materials. Source SHA-256 " + SOURCE_SHA256 + ",\n"
        "retained in data/source-assets/" + SOURCE.name + ".\n"
        "No endorsement by the author is implied.\n",
        encoding="utf-8")
    write_registry(record(report))
    print(json.dumps({"file": OUT.relative_to(GAME).as_posix(), "bytes": len(glb), **report}, indent=1))


if __name__ == "__main__":
    main()
