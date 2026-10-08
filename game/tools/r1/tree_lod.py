"""The middle level of detail of a tree: the same tree, thinner.

A tree in the world has three levels (native/world.cpp, `plantNode`): the
photographed model up close, a 32-vertex card far away, and between the two
this one. It is made from the Near model and from nothing else -- no other
kit, no other species (CLAUDE.md rule 1) -- part by part, each with the tool
that suits it:

  - **The crown** is thousands of separate leaf cards under an alpha mask. A
    quadric decimator cannot simplify a card below its own two triangles, and
    merging across cards smears the atlas (see `decimate.py`). So whole cards
    are kept or dropped, and every survivor keeps its UVs exactly. Which ones
    is decided by what is seen: the cards that add most of the Near crown's
    picture, from a dozen views around and above it (`covering`). What is
    dropped is made up in size -- each kept card grows about its own centre,
    never beyond where the crown already reached (`Envelope`) -- so the
    outline stays the Near model's and the crown covers as much sky.
  - **The branches** are continuous bark. They go through Saida's AutoLOD
    (`engine/autolod`) in proxy mode: welded through their UV seams,
    simplified by meshoptimizer, re-unwrapped by xatlas, and their colour,
    normal, roughness and occlusion baked into a fresh atlas.
  - **The trunk** is rings up to its neck and the fork decimated above (see
    "the trunk" below), baked by AutoLOD onto that shape (`--proxy-shape`).

The silhouette of the middle level against the Near model's, from five views,
is 94% the same (measured on 8 October). A level replacing another is what
the eye compares, and the engine's cross-fade does the rest.

The result is one GLB beside the Near model. Its textures are small: it is
never seen closer than the Near model's switch distance.
"""

from __future__ import annotations

import io
import json
import math
import os
import struct
import subprocess
import tempfile
from pathlib import Path

from . import decimate as D

GAME_ROOT = Path(__file__).resolve().parents[2]
ENGINE_ROOT = GAME_ROOT.parent / "engine"
NATURE = GAME_ROOT / "assets" / "models" / "external" / "nature_selected"

# Triangles each part of a middle level keeps, as the user set them.
MID_TREES = {
    "urban_tree": {
        "source": "urban_tree.glb",
        "target": "urban_tree_mid.glb",
        "parts": {
            "crown merged_normal leaves_0": ("cards", 4000),
            "br_low_trunk_0": ("proxy", 2000),
            "trunk_low_trunk_0": ("trunk", 150),
        },
    },
}
# A kept card grows at most this much on each side: past it a leaf becomes a
# blob even at the middle distance.
CARD_GROWTH_CAP = 3.0
CARD_TEXTURE = 512
PROXY_ATLAS = {True: 512, False: 256}   # by whether the part is large


def tool_env() -> dict:
    """MSYS2's UCRT64 first on Windows, as the engine's build needs it
    (`engine/AGENTS.md`), for the build and for AutoLOD's runtime DLLs."""
    env = dict(os.environ)
    ucrt = Path("C:/msys64/ucrt64/bin")
    if os.name == "nt" and ucrt.exists():
        env["PATH"] = str(ucrt) + os.pathsep + env.get("PATH", "")
    return env


def autolod_executable() -> Path:
    """Saida's AutoLOD, built the way the engine says (`CMakeLists.txt`)."""
    build = ENGINE_ROOT / "build-rel" / "autolod"
    exe = build / ("autolod.exe" if os.name == "nt" else "autolod")
    if not exe.exists():
        subprocess.run(["cmake", "-S", str(ENGINE_ROOT / "autolod"), "-B", str(build),
                        "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"], check=True, env=tool_env())
        subprocess.run(["cmake", "--build", str(build)], check=True, env=tool_env())
    return exe


# ── GLB in and out ──────────────────────────────────────────────────────────

def read_glb(path: Path) -> tuple[dict, bytes]:
    blob = path.read_bytes()
    length = struct.unpack_from("<I", blob, 12)[0]
    document = json.loads(blob[20:20 + length])
    rest = blob[20 + length:]
    binary = rest[8:8 + struct.unpack_from("<I", rest, 0)[0]] if rest else b""
    return document, binary


def write_glb(path: Path, document: dict, binary: bytes) -> None:
    document["buffers"] = [{"byteLength": len(D._pad(binary))}]
    encoded = D._pad(json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8"), b" ")
    payload = D._pad(binary)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(encoded) + 8 + len(payload))
        + struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
        + struct.pack("<II", len(payload), 0x004E4942) + payload)


class Builder:
    """A new GLB, filled part by part from other documents."""

    def __init__(self) -> None:
        self.binary = bytearray()
        self.doc = {"asset": {"version": "2.0", "generator": "R1World tree_lod.py"},
                    "accessors": [], "bufferViews": [], "images": [], "samplers": [],
                    "textures": [], "materials": [], "meshes": [], "nodes": [],
                    "scenes": [{"nodes": []}], "scene": 0}

    def store(self, payload: bytes, target: int | None = None) -> int:
        self.binary += b"\0" * ((-len(self.binary)) % 4)
        view = {"buffer": 0, "byteOffset": len(self.binary), "byteLength": len(payload)}
        if target is not None:
            view["target"] = target
        self.doc["bufferViews"].append(view)
        self.binary += payload
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values: list[tuple], kind: str, component: int, normalized: bool = False) -> int:
        fmt, _ = D._COMPONENT[component]
        packer = struct.Struct("<" + fmt * D._COUNT[kind])
        view = self.store(b"".join(packer.pack(*v) for v in values),
                          34963 if kind == "SCALAR" and component in (5123, 5125) else 34962)
        out = {"bufferView": view, "componentType": component, "count": len(values), "type": kind}
        if normalized:
            out["normalized"] = True
        if kind == "VEC3" and component == 5126 and values:
            out["min"] = [min(v[i] for v in values) for i in range(3)]
            out["max"] = [max(v[i] for v in values) for i in range(3)]
        self.doc["accessors"].append(out)
        return len(self.doc["accessors"]) - 1

    def material(self, source: dict, binary: bytes, index: int, texture_size: int | None) -> int:
        """Copy material `index` of `source`, its textures resized to `texture_size`."""
        material = json.loads(json.dumps(source["materials"][index]))
        remap: dict[int, int] = {}

        def texture(ref: dict) -> None:
            old = ref["index"]
            if old not in remap:
                tex = source["textures"][old]
                image = source["images"][tex["source"]]
                view = source["bufferViews"][image["bufferView"]]
                blob = binary[view.get("byteOffset", 0):view.get("byteOffset", 0) + view["byteLength"]]
                if texture_size:
                    blob = shrink(blob, texture_size)
                self.doc["images"].append({"bufferView": self.store(blob), "mimeType": "image/png"})
                entry = {"source": len(self.doc["images"]) - 1}
                if "sampler" in tex:
                    self.doc["samplers"].append(source["samplers"][tex["sampler"]])
                    entry["sampler"] = len(self.doc["samplers"]) - 1
                self.doc["textures"].append(entry)
                remap[old] = len(self.doc["textures"]) - 1
            ref["index"] = remap[old]

        # Extensions come along with their textures, so the middle level is
        # shaded as the model it stands in for.
        for extension in material.get("extensions", {}).values():
            for value in extension.values():
                if isinstance(value, dict) and "index" in value:
                    texture(value)
        for key in ("normalTexture", "occlusionTexture", "emissiveTexture"):
            if key in material:
                texture(material[key])
        pbr = material.get("pbrMetallicRoughness", {})
        for key in ("baseColorTexture", "metallicRoughnessTexture"):
            if key in pbr:
                texture(pbr[key])
        self.doc["materials"].append(material)
        return len(self.doc["materials"]) - 1

    def mesh(self, name: str, attributes: dict[str, int], indices: int, material: int) -> None:
        self.doc["meshes"].append({"name": name, "primitives": [
            {"attributes": attributes, "indices": indices, "material": material, "mode": 4}]})
        self.doc["nodes"].append({"mesh": len(self.doc["meshes"]) - 1, "name": name})
        self.doc["scenes"][0]["nodes"].append(len(self.doc["nodes"]) - 1)


def shrink(blob: bytes, size: int) -> bytes:
    """Re-encode an image at `size` on its long edge, alpha kept (the leaves
    are cut out by it), as PNG."""
    from PIL import Image
    with Image.open(io.BytesIO(blob)) as image:
        image.load()
        if max(image.size) > size:
            scale = size / max(image.size)
            image = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                                 Image.LANCZOS)
        out = io.BytesIO()
        image.save(out, format="PNG", optimize=True)
        return out.getvalue()


def copy_primitive(builder: Builder, source: dict, binary: bytes, primitive: dict,
                   texture_size: int | None, name: str) -> int:
    """Copy a primitive as it is; returns its triangle count."""
    buffers = [binary]
    attributes = {}
    for key, index in primitive["attributes"].items():
        acc = source["accessors"][index]
        attributes[key] = builder.accessor(D._read_accessor(source, buffers, index), acc["type"],
                                           acc["componentType"], acc.get("normalized", False))
    indices = [i for i in D._read_accessor(source, buffers, primitive["indices"])]
    material = builder.material(source, binary, primitive["material"], texture_size)
    builder.mesh(name, attributes, builder.accessor(indices, "SCALAR", 5125), material)
    return len(indices) // 3


# ── the parts ───────────────────────────────────────────────────────────────

class Envelope:
    """How far the crown reaches in each direction from its centre.

    The middle level must keep the Near model's outline exactly: it is what
    the eye compares when one level replaces the other. A kept card grows only
    as far as the crown already reached in that direction, so no leaf ever
    stands where none stood.
    """
    AZIMUTH, ELEVATION = 64, 32

    def __init__(self, points) -> None:
        low = [min(p[i] for p in points) for i in range(3)]
        high = [max(p[i] for p in points) for i in range(3)]
        self.centre = [(low[i] + high[i]) / 2 for i in range(3)]
        self.reach = [0.0] * (self.AZIMUTH * self.ELEVATION)
        for p in points:
            b, r = self.bin(p)
            self.reach[b] = max(self.reach[b], r)

    def bin(self, p) -> tuple[int, float]:
        x, y, z = (p[i] - self.centre[i] for i in range(3))
        r = math.sqrt(x * x + y * y + z * z)
        az = int((math.atan2(z, x) / (2 * math.pi) + 0.5) * self.AZIMUTH) % self.AZIMUTH
        el = min(self.ELEVATION - 1, int((math.asin(max(-1.0, min(1.0, y / r))) / math.pi + 0.5)
                                       * self.ELEVATION)) if r > 0 else 0
        return el * self.AZIMUTH + az, r

    def inside(self, p) -> bool:
        b, r = self.bin(p)
        return r <= self.reach[b]


# The views a crown is judged from: around it at eye level and from above at
# 35 degrees, which is how a tree is seen from the street and from a hill.
VIEWS = [(math.radians(a), 0.0) for a in range(0, 360, 45)] +         [(math.radians(a), math.radians(35)) for a in range(0, 360, 90)]
VIEW_PIXELS = 128


def covering(clusters, indices, positions, triangles: int) -> list[list[int]]:
    """The cards that keep the crown looking the same from every side.

    Each card's footprint is projected into a dozen views; cards are then
    taken greedily by how much of the Near crown's picture they add that
    nothing taken yet covers, until the triangle budget is spent. A card on
    the outline is the only one to cover its pixels and is taken early; a card
    buried behind others adds nothing and is left. The result is deterministic:
    ties go to the earlier cluster in the file (plan §3 I3).
    """
    import heapq
    low = [min(p[i] for p in positions) for i in range(3)]
    high = [max(p[i] for p in positions) for i in range(3)]
    centre = [(low[i] + high[i]) / 2 for i in range(3)]
    radius = max(math.dist(p, centre) for p in positions)
    scale = (VIEW_PIXELS - 1) / (2 * radius)
    axes = []
    for azimuth, elevation in VIEWS:
        right = (math.cos(azimuth), 0.0, math.sin(azimuth))
        forward = (-math.sin(azimuth) * math.cos(elevation), math.sin(elevation),
                   math.cos(azimuth) * math.cos(elevation))
        up = (right[1] * forward[2] - right[2] * forward[1], right[2] * forward[0] - right[0] * forward[2],
              right[0] * forward[1] - right[1] * forward[0])
        axes.append((right, up))
    # Seven samples a triangle (corners, edge middles, centre): a card is a
    # pixel or two across in these views, so samples cover what it covers.
    weights = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (.5, .5, 0), (0, .5, .5), (.5, 0, .5), (1 / 3, 1 / 3, 1 / 3)]

    def footprint(cluster) -> set[int]:
        out = set()
        for base in cluster:
            a, b, c = (positions[indices[base + k]] for k in range(3))
            for wa, wb, wc in weights:
                q = [wa * a[i] + wb * b[i] + wc * c[i] - centre[i] for i in range(3)]
                for view, (right, up) in enumerate(axes):
                    x = int((sum(q[i] * right[i] for i in range(3)) + radius) * scale)
                    y = int((sum(q[i] * up[i] for i in range(3)) + radius) * scale)
                    out.add((view * VIEW_PIXELS + y) * VIEW_PIXELS + x)
        return out

    prints = [footprint(c) for c in clusters]
    covered: set[int] = set()
    heap = [(-len(p), i) for i, p in enumerate(prints)]
    heapq.heapify(heap)
    kept, spent = [], 0
    while heap and spent < triangles:
        gain, i = heapq.heappop(heap)
        fresh = len(prints[i] - covered)
        if heap and fresh < -heap[0][0]:
            heapq.heappush(heap, (-fresh, i))   # stale: its gain fell since
            continue
        kept.append(clusters[i])
        spent += len(clusters[i])
        covered |= prints[i]
    return kept


def thin_cards(builder: Builder, source: dict, binary: bytes, primitive: dict,
               triangles: int, name: str) -> int:
    """Keep about `triangles` of a crown's cards, those that add most to how
    it looks from every side, each grown inside the crown to cover what went."""
    buffers = [binary]
    positions = D._read_accessor(source, buffers, primitive["attributes"]["POSITION"])
    indices = [i[0] for i in D._read_accessor(source, buffers, primitive["indices"])]
    total = len(indices) // 3
    clusters = D._cluster_triangles(indices, len(positions))
    centres = {base: positions[indices[base]] for base in range(0, len(indices), 3)}
    envelope = Envelope(positions)

    def corners(cluster):
        return sorted({indices[base + k] for base in cluster for k in range(3)})

    kept = covering(clusters, indices, positions, triangles)
    kept_triangles = sum(len(c) for c in kept)
    wanted = min(CARD_GROWTH_CAP, 1.25 * math.sqrt(total / max(kept_triangles, 1)))

    moved = list(positions)
    chosen = []
    for cluster in kept:
        mine = corners(cluster)
        c = [sum(positions[v][i] for v in mine) / len(mine) for i in range(3)]

        def grown(g):
            return [tuple(c[i] + (positions[v][i] - c[i]) * g for i in range(3)) for v in mine]

        # The largest growth up to `wanted` that keeps the card inside the
        # crown's own reach. Growth only ever widens a card about its centre,
        # so the search is monotonic.
        lo, hi = 1.0, wanted
        if all(envelope.inside(p) for p in grown(hi)):
            lo = hi
        else:
            for _ in range(7):
                mid = (lo + hi) / 2
                if all(envelope.inside(p) for p in grown(mid)):
                    lo = mid
                else:
                    hi = mid
        for v, p in zip(mine, grown(lo)):
            moved[v] = p
        chosen.extend(cluster)
    chosen.sort()

    remap: dict[int, int] = {}
    new_indices = []
    for base in chosen:
        for k in range(3):
            old = indices[base + k]
            new_indices.append((remap.setdefault(old, len(remap)),))
    order = sorted(remap, key=remap.get)
    attributes = {"POSITION": builder.accessor([moved[i] for i in order], "VEC3", 5126)}
    for key, index in primitive["attributes"].items():
        if key == "POSITION":
            continue
        acc = source["accessors"][index]
        values = D._read_accessor(source, buffers, index)
        attributes[key] = builder.accessor([values[i] for i in order], acc["type"],
                                           acc["componentType"], acc.get("normalized", False))
    material = builder.material(source, binary, primitive["material"], CARD_TEXTURE)
    builder.mesh(name, attributes, builder.accessor(new_indices, "SCALAR", 5125), material)
    return len(new_indices) // 3


def part_glb(source: dict, binary: bytes, primitive: dict, path: Path, keep=None) -> int:
    """Write one primitive alone, full resolution, keeping the triangles
    `keep(a, b, c)` accepts; returns their count."""
    buffers = [binary]
    indices = [i[0] for i in D._read_accessor(source, buffers, primitive["indices"])]
    positions = D._read_accessor(source, buffers, primitive["attributes"]["POSITION"])
    chosen = [i for base in range(0, len(indices), 3)
              if keep is None or keep(*(positions[indices[base + k]] for k in range(3)))
              for i in indices[base:base + 3]]
    builder = Builder()
    attributes = {}
    for key, index in primitive["attributes"].items():
        acc = source["accessors"][index]
        attributes[key] = builder.accessor(D._read_accessor(source, buffers, index), acc["type"],
                                           acc["componentType"], acc.get("normalized", False))
    material = builder.material(source, binary, primitive["material"], None)
    builder.mesh("part", attributes, builder.accessor([(i,) for i in chosen], "SCALAR", 5125), material)
    write_glb(path, builder.doc, bytes(builder.binary))
    return len(chosen) // 3


def triangle_count(document: dict, binary: bytes) -> int:
    return sum(len(D._read_accessor(document, [binary], p["indices"])) // 3
               for m in document["meshes"] for p in m["primitives"])


def autolod(single: Path, scratch: Path, ratio: float, atlas: int, shape: Path | None = None):
    """One AutoLOD proxy level of `single`; returns (document, binary)."""
    command = [str(autolod_executable()), str(single), str(scratch / "out.glb"),
               "--ratios", f"{ratio:.5f}", "--errors", "1.0", "--proxy",
               "--proxy-below", "0.99", "--bake-res", str(atlas), "--split"]
    if shape is not None:
        command += ["--proxy-shape", str(shape)]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, env=tool_env())
    return read_glb(scratch / "out_LOD1.glb")


def decimated(single: Path, count: int, triangles: int, atlas: int, scratch: Path):
    """AutoLOD until it lands near `triangles`: meshoptimizer lands near, not
    on, the target once seams are welded, and a few proportional corrections
    bring it within a tenth."""
    ratio = min(0.95, triangles / count)
    best = None
    for _ in range(6):
        lod, lod_binary = autolod(single, scratch, ratio, atlas)
        got = triangle_count(lod, lod_binary)
        if best is None or abs(got - triangles) < abs(best[0] - triangles):
            best = (got, lod, lod_binary)
        if abs(got - triangles) <= max(2, triangles // 10):
            break
        ratio = min(0.95, ratio * triangles / max(got, 1))
    return best[1], best[2]


def proxy(builder: Builder, source: dict, binary: bytes, mesh_index: int,
          triangles: int, name: str, scratch: Path) -> int:
    """Run one mesh through AutoLOD's proxy mode until it lands near `triangles`."""
    (primitive,) = source["meshes"][mesh_index]["primitives"]
    single = scratch / "part.glb"
    count = part_glb(source, binary, primitive, single)
    lod, lod_binary = decimated(single, count, triangles, PROXY_ATLAS[triangles >= 500], scratch)
    return sum(copy_primitive(builder, lod, lod_binary, p, None, name) for p in lod["meshes"][0]["primitives"])


# ── the trunk ───────────────────────────────────────────────────────────────
#
# A trunk cannot be left to a decimator: meshoptimizer keeps the root flare
# and the fork and draws straight lines between them, and the trunk comes out
# fat in the middle with its foot cut off -- the one part of a tree under its
# crown that is seen whole. Up to its neck, where it starts to widen into its
# fork, a trunk is a tube, and a tube is rings: each ring follows the trunk's
# own section, each corner as far out as the bark reaches in that direction,
# and the rings stand where the profile bends most (the flare first). The
# fork and the limbs above take what is left of the budget, decimated. Both
# are one shape, and AutoLOD bakes the Near trunk's bark onto it
# (`--proxy-shape`).

TRUNK_SIDES = 8
TRUNK_TUBE_SHARE = 0.4      # of the trunk's triangles, for the tube below its neck
PROFILE_STEPS = 48
ANGLES = 72


def fork_height(points) -> float:
    """The lowest height at which the trunk's section splits in two."""
    low = min(p[1] for p in points)
    high = max(p[1] for p in points)
    band = (high - low) / PROFILE_STEPS
    foot = [p for p in points if p[1] < low + band]
    cx = sum(p[0] for p in foot) / len(foot)
    cz = sum(p[2] for p in foot) / len(foot)
    reach = sum(math.hypot(p[0] - cx, p[2] - cz) for p in foot) / len(foot)
    for step in range(PROFILE_STEPS // 8, PROFILE_STEPS):
        y = low + step * band
        section = [(p[0], p[2]) for p in points if y <= p[1] < y + band]
        groups: list[list[tuple]] = []
        for q in section:
            joined = [g for g in groups if any(math.dist(q, r) < reach * 0.6 for r in g)]
            merged = [q]
            for g in joined:
                merged += g
                groups.remove(g)
            groups.append(merged)
        if sum(len(g) >= 3 for g in groups) > 1:
            return y
    return high


def section(triangles, y: float) -> list[tuple]:
    """Where the plane at height `y` cuts the surface: exact, however sparse
    the mesh's vertices are at that height."""
    out = []
    for tri in triangles:
        for a, b in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
            if (a[1] - y) * (b[1] - y) <= 0 and a[1] != b[1]:
                t = (y - a[1]) / (b[1] - a[1])
                out.append((a[0] + (b[0] - a[0]) * t, y, a[2] + (b[2] - a[2]) * t))
    return out


def width(triangles, y: float) -> float:
    cut = section(triangles, y)
    return max(max(p[0] for p in cut) - min(p[0] for p in cut),
               max(p[2] for p in cut) - min(p[2] for p in cut)) if cut else 0.0


def neck(triangles, low: float, fork: float) -> float:
    """Where the trunk starts to widen again into its fork: the tube stops
    there, below the first limb's departure, which belongs to the fork."""
    steps = [low + (fork - low) * k / PROFILE_STEPS for k in range(PROFILE_STEPS // 6, PROFILE_STEPS)]
    widths = [width(triangles, y) for y in steps]
    narrowest = min(range(len(widths)), key=widths.__getitem__)
    for k in range(narrowest, len(widths)):
        if widths[k] > widths[narrowest] * 1.12:
            return steps[max(narrowest, k - 1)]
    return fork


def tube(triangles, bottom: float, top: float, sides: int, rings: int):
    """Rings along [bottom, top] following the trunk's section."""
    band = (top - bottom) / PROFILE_STEPS
    profile = []
    for step in range(PROFILE_STEPS + 1):
        # The foot is cut a hair above the ground it stands on.
        y = bottom + max(step * band, band * 0.05)
        cut = section(triangles, y)
        xs = [p[0] for p in cut]
        zs = [p[2] for p in cut]
        cx, cz = (min(xs) + max(xs)) / 2, (min(zs) + max(zs)) / 2
        reach = [max((p[0] - cx) * math.cos(a) + (p[2] - cz) * math.sin(a) for p in cut)
                 for a in (2 * math.pi * k / ANGLES for k in range(ANGLES))]
        profile.append((y, [cx, cz] + reach))
    # Rings where a straight line between the rings around it errs most.
    chosen = [0, PROFILE_STEPS]
    while len(chosen) < rings:
        worst, at = -1.0, None
        for a, b in zip(chosen, chosen[1:]):
            for i in range(a + 1, b):
                t = (i - a) / (b - a)
                err = max(abs(profile[i][1][k] - (profile[a][1][k] * (1 - t) + profile[b][1][k] * t))
                          for k in range(len(profile[i][1])))
                if err > worst:
                    worst, at = err, i
        if at is None:
            break
        chosen = sorted(chosen + [at])
    # A corner is where the bark reaches in its direction, pushed out half way
    # to the circumscribed polygon, so the width seen across corners and
    # across sides averages the trunk's own.
    widen = (1 + 1 / math.cos(math.pi / sides)) / 2
    positions = []
    for i in chosen:
        y, (cx, cz, *reach) = profile[i]
        for k in range(sides):
            a = 2 * math.pi * k / sides
            r = reach[round(k * ANGLES / sides) % ANGLES] * widen
            positions.append((cx + r * math.cos(a), y, cz + r * math.sin(a)))
    indices = []
    for r in range(len(chosen) - 1):
        for k in range(sides):
            a, b = r * sides + k, r * sides + (k + 1) % sides
            c, d = a + sides, b + sides
            indices += [a, c, b, b, c, d]
    # Outward winding: AutoLOD's bake casts along the shape's normals.
    p0, p1, p2 = (positions[i] for i in indices[:3])
    n = [(p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1]),
         (p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2]),
         (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])]
    _, (cx, cz, *_) = profile[chosen[0]]
    if n[0] * (p0[0] - cx) + n[2] * (p0[2] - cz) < 0:
        indices = [i for t in range(0, len(indices), 3) for i in (indices[t], indices[t + 2], indices[t + 1])]
    return positions, indices


def trunk(builder: Builder, source: dict, binary: bytes, mesh_index: int,
          triangles: int, name: str, scratch: Path) -> int:
    """A trunk of about `triangles`: rings up to its neck, its fork above."""
    (primitive,) = source["meshes"][mesh_index]["primitives"]
    corners = D._read_accessor(source, [binary], primitive["attributes"]["POSITION"])
    order = [i[0] for i in D._read_accessor(source, [binary], primitive["indices"])]
    surface = [tuple(corners[order[t + k]] for k in range(3)) for t in range(0, len(order), 3)]
    points = sorted(set(corners))
    low = min(p[1] for p in points)
    top = neck(surface, low, fork_height(points))
    band = (max(p[1] for p in points) - low) / PROFILE_STEPS
    sides = TRUNK_SIDES
    rings = max(2, int(triangles * TRUNK_TUBE_SHARE) // (2 * sides) + 1)
    shape_positions, shape_indices = tube([t for t in surface if min(p[1] for p in t) <= top + band],
                                          low, top, sides, rings)
    rest = triangles - len(shape_indices) // 3
    # The fork reaches a band down into the tube, so the two never part.
    above = scratch / "above.glb"
    count = part_glb(source, binary, primitive, above, lambda a, b, c: (a[1] + b[1] + c[1]) / 3 > top - band)
    if count and rest > 0:
        limbs, limbs_binary = decimated(above, count, rest, PROXY_ATLAS[False], scratch)
        for p in limbs["meshes"][0]["primitives"]:
            base = len(shape_positions)
            shape_positions += D._read_accessor(limbs, [limbs_binary], p["attributes"]["POSITION"])
            shape_indices += [base + i[0] for i in D._read_accessor(limbs, [limbs_binary], p["indices"])]
    shape = Builder()
    shape.doc["materials"].append({"name": "shape"})
    shape.mesh("shape", {"POSITION": shape.accessor(shape_positions, "VEC3", 5126)},
               shape.accessor([(i,) for i in shape_indices], "SCALAR", 5125), 0)
    write_glb(scratch / "shape.glb", shape.doc, bytes(shape.binary))
    whole = scratch / "trunk.glb"
    part_glb(source, binary, primitive, whole)
    lod, lod_binary = autolod(whole, scratch, 0.5, PROXY_ATLAS[False], scratch / "shape.glb")
    return sum(copy_primitive(builder, lod, lod_binary, p, None, name) for p in lod["meshes"][0]["primitives"])


def build_mid(name: str) -> dict:
    spec = MID_TREES[name]
    source, binary = read_glb(NATURE / spec["source"])
    builder = Builder()
    report = {}
    with tempfile.TemporaryDirectory() as folder:
        for mesh_index, mesh in enumerate(source["meshes"]):
            how, triangles = spec["parts"][mesh["name"]]
            if how == "cards":
                (primitive,) = mesh["primitives"]
                report[mesh["name"]] = thin_cards(builder, source, binary, primitive, triangles, mesh["name"])
            elif how == "trunk":
                report[mesh["name"]] = trunk(builder, source, binary, mesh_index, triangles, mesh["name"],
                                             Path(folder))
            else:
                report[mesh["name"]] = proxy(builder, source, binary, mesh_index, triangles,
                                             mesh["name"], Path(folder))
    target = NATURE / spec["target"]
    write_glb(target, builder.doc, bytes(builder.binary))
    vertices = sum(builder.doc["accessors"][m["primitives"][0]["attributes"]["POSITION"]]["count"]
                   for m in builder.doc["meshes"])
    return {"output": spec["target"], "derivedFrom": spec["source"], "triangles": report,
            "vertices": vertices, "bytes": target.stat().st_size}


def build_all() -> list[dict]:
    """Every middle level, and their record beside the models they come from."""
    report = [build_mid(name) for name in MID_TREES]
    sources = NATURE / "SOURCES.json"
    manifest = [e for e in json.loads(sources.read_text(encoding="utf-8"))
                if e.get("output") not in {r["output"] for r in report}]
    for entry in report:
        origin = next(e for e in manifest if e.get("output") == entry["derivedFrom"])
        manifest.append({**entry, "attribution": origin.get("attribution", {}),
                         "method": "crown: whole cards kept by what they add to a dozen views, grown inside its reach; "
                                   "branches: Saida AutoLOD proxy (meshoptimizer + xatlas bake); "
                                   "trunk: rings to its neck, fork decimated, baked by AutoLOD --proxy-shape"})
    sources.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return report


if __name__ == "__main__":
    for line in build_all():
        print(json.dumps(line))
