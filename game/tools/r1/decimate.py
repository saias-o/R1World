"""Decimation for photoreal vegetation — plan §4, and the arena's price list.

The project ships photoscanned CC0 trees from Poly Haven. They are the best asset in the repository and nothing may
replace them (see `CLAUDE.md`, rule 1). They are also, measured:

    fir_sapling          515 299 vertices    49% of the whole geometry arena
    pine_sapling_small   406 356 vertices    39%
    quiver_tree_01        88 548 vertices     8%

Saida's arena holds 1 048 576 vertices for the entire scene, and a dense
neighbourhood of world tiles already occupies 841 035 of them. One species of
tree at native density therefore cannot be resident at the same time as a city.

This module is the answer §4 already named: the model enters the game
decimated, not exchanged for a lesser model.

**How, and why not the usual way.** A fir sapling is not one surface. It is a
woody stem of about a thousand vertices and half a million vertices of needle
clusters, and the two want opposite treatments:

  - The stem is small already and carries the silhouette. It is kept whole.
  - The needles are thousands of small, separate, closed clusters. Collapsing
    vertices across them — what a quadric-error or vertex-clustering decimator
    does — averages texture coordinates between clusters that sample different
    parts of the atlas, and the result is a tree smeared with the wrong pixels.
    So no vertex is ever moved or merged here. **Whole clusters are kept or
    dropped**, and every survivor keeps its geometry and its UVs exactly as
    scanned.

The consequence is honest and worth stating: a decimated tree is a *thinner*
tree, not a blurrier one. At 3% it is a sapling that lost most of its needles;
what it did not lose is its shape, its bark, and the fact that it is a
photograph of a real plant rather than a drawing of one.

**Which clusters survive.** Not the biggest — keeping the biggest strips the
canopy and leaves a bald crown over a dense skirt. Clusters are ordered by
position and kept at a fixed stride, so the survivors are spread through the
volume the tree occupied. The order is deterministic and depends only on the
mesh, so a decimated model is byte-identical on every machine (§3 I3).
"""

from __future__ import annotations

import base64
import json
import struct
from pathlib import Path

# glTF component types, and how to unpack them.
_COMPONENT = {
    5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2),
    5123: ("H", 2), 5125: ("I", 4), 5126: ("f", 4),
}
_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def _buffer_bytes(document: dict, folder: Path) -> list[bytes]:
    out = []
    for buffer in document.get("buffers", []):
        uri = buffer.get("uri")
        if uri is None:
            raise RuntimeError("GLB input is not supported here; pass the .gltf")
        if uri.startswith("data:"):
            out.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            out.append((folder / uri).read_bytes())
    return out


def _read_accessor(document: dict, buffers: list[bytes], index: int) -> list[tuple]:
    accessor = document["accessors"][index]
    view = document["bufferViews"][accessor["bufferView"]]
    fmt, size = _COMPONENT[accessor["componentType"]]
    width = _COUNT[accessor["type"]]
    stride = view.get("byteStride") or size * width
    base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    blob = buffers[view.get("buffer", 0)]
    unpack = struct.Struct("<" + fmt * width).unpack_from
    return [unpack(blob, base + i * stride) for i in range(accessor["count"])]


_PNG = bytes((0x89, 0x50, 0x4E, 0x47))
_JPEG = bytes((0xFF, 0xD8))


def _image_kind(blob: bytes) -> str | None:
    """The glTF mimeType of a payload, by its magic number, or None."""
    if blob[:4] == _PNG:
        return "image/png"
    if blob[:2] == _JPEG:
        return "image/jpeg"
    return None


class _Components:
    """Union-find over vertex indices: one set per connected cluster."""

    __slots__ = ("parent",)

    def __init__(self, count: int) -> None:
        self.parent = list(range(count))

    def find(self, item: int) -> int:
        root = item
        while self.parent[root] != root:
            root = self.parent[root]
        while self.parent[item] != root:
            self.parent[item], item = root, self.parent[item]
        return root

    def union(self, a: int, b: int) -> None:
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.parent[max(ra, rb)] = min(ra, rb)


def _cluster_triangles(indices: list[int], vertex_count: int) -> list[list[int]]:
    """Group triangle offsets by the connected cluster they belong to."""
    sets = _Components(vertex_count)
    for base in range(0, len(indices), 3):
        a, b, c = indices[base], indices[base + 1], indices[base + 2]
        sets.union(a, b)
        sets.union(a, c)
    groups: dict[int, list[int]] = {}
    for base in range(0, len(indices), 3):
        groups.setdefault(sets.find(indices[base]), []).append(base)
    return list(groups.values())


SLABS = 14


def _keep(clusters, positions, ratio: float) -> list[list[int]]:
    """Keep the canopy's outer shell, slab by slab.

    The first version of this took an even stride through the whole canopy, and
    at 3% it produced bare sticks: two thirds of a tree's needles are *inside*
    the crown where nothing can see them, so spending a third of a small budget
    on them buys nothing and starves the outline that does the work.

    So the tree is cut into horizontal slabs, and within each slab the clusters
    furthest from the trunk axis are kept. Every height therefore keeps its own
    silhouette — a fir stays a cone and a broadleaf stays a ball — and what is
    thrown away first is the part that was hidden anyway. Slabs are what stops
    it becoming a hollow ring at one height; distance is what stops it becoming
    a stick.
    """
    if ratio >= 1.0 or not clusters:
        return clusters

    centres = []
    for cluster in clusters:
        xs = ys = zs = 0.0
        for base in cluster:
            point = positions[base]
            xs += point[0]
            ys += point[1]
            zs += point[2]
        n = float(len(cluster))
        centres.append((xs / n, ys / n, zs / n))

    axis_x = sum(c[0] for c in centres) / len(centres)
    axis_z = sum(c[2] for c in centres) / len(centres)
    low = min(c[1] for c in centres)
    high = max(c[1] for c in centres)
    span = max(1e-6, high - low)

    slabs: dict[int, list[int]] = {}
    for index, centre in enumerate(centres):
        slab = min(SLABS - 1, int((centre[1] - low) / span * SLABS))
        slabs.setdefault(slab, []).append(index)

    kept: list[list[int]] = []
    for slab in sorted(slabs):
        members = slabs[slab]
        # Furthest from the axis first, then by position so the order cannot
        # depend on how the file happened to be written (§3 I3).
        members.sort(key=lambda i: (
            -((centres[i][0] - axis_x) ** 2 + (centres[i][2] - axis_z) ** 2),
            centres[i][1], centres[i][0], centres[i][2]))
        wanted = max(1, int(round(len(members) * ratio)))
        kept.extend(clusters[i] for i in members[:wanted])
    return kept


def _pad(data: bytes, byte: bytes = b"\0") -> bytes:
    return data + byte * ((-len(data)) % 4)


def _shrink_texture(blob: bytes, size: int) -> bytes:
    """Re-encode an image at `size` on its long edge.

    **File size is paid per instance, not per kind.** Saida deduplicates the
    *mesh* an instance points at — sixty-four trees share one upload — but the
    scene loader opens and parses the referenced file once per node. A 7 MB
    tree therefore costs 7 MB of parsing and nine JPEG decodes for every tree on
    the street: measured, 476 loads of one `broadleaf.glb` on a single
    neighbourhood, and tiles that had taken a second each took thirty-six.

    So the decimated model carries small textures, and only the base colour.
    A normal or an occlusion map on a six-thousand-vertex tree seen from twenty
    metres changes nothing anybody can see, and each one is another decode per
    instance.
    """
    from PIL import Image
    import io

    with Image.open(io.BytesIO(blob)) as image:
        image = image.convert("RGB")
        if max(image.size) > size:
            scale = size / max(image.size)
            image = image.resize(
                (max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                Image.LANCZOS)
        out = io.BytesIO()
        image.save(out, format="JPEG", quality=82, optimize=True)
        return out.getvalue()


def _strip_to_base_colour(document: dict) -> None:
    """Drop every texture a material has except its base colour, then the
    images nobody points at any more."""
    for material in document.get("materials", []):
        for key in ("normalTexture", "occlusionTexture", "emissiveTexture"):
            material.pop(key, None)
        material.pop("emissiveFactor", None)
        pbr = material.get("pbrMetallicRoughness", {})
        pbr.pop("metallicRoughnessTexture", None)
        # With the map gone the factors have to say something sensible: bark
        # and leaves are dielectric and rough.
        pbr["metallicFactor"] = 0.0
        pbr.setdefault("roughnessFactor", 0.9)

    used = {texture.get("source")
            for material in document.get("materials", [])
            for texture in [document["textures"][
                material.get("pbrMetallicRoughness", {})
                .get("baseColorTexture", {}).get("index")]]
            if texture is not None}
    keep = sorted(i for i in used if i is not None)
    remap = {old: new for new, old in enumerate(keep)}
    document["images"] = [document["images"][i] for i in keep]
    textures = []
    texture_remap = {}
    for index, texture in enumerate(document.get("textures", [])):
        if texture.get("source") in remap:
            texture_remap[index] = len(textures)
            textures.append({**texture, "source": remap[texture["source"]]})
    document["textures"] = textures
    for material in document.get("materials", []):
        base = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if base is not None:
            base["index"] = texture_remap[base["index"]]


def decimate_gltf(source: Path, target: Path, budget: int,
                  keep_whole_below: int = 4000, texture_size: int = 256) -> dict:
    """Rewrite `source` as a self-contained GLB of at most about `budget` vertices.

    `keep_whole_below` is the size under which a primitive is copied untouched:
    a thousand-vertex stem is not what is expensive, and thinning it would cost
    the silhouette for nothing.

    Textures are embedded, so the decimated model is one file that cannot be
    half-installed — the same reason the props embed theirs.
    """
    folder = source.parent
    document = json.loads(source.read_text(encoding="utf-8"))
    buffers = _buffer_bytes(document, folder)

    primitives = [(mesh, primitive)
                  for mesh in document.get("meshes", [])
                  for primitive in mesh.get("primitives", [])]
    # **Every primitive gets its own ratio, and that is not a detail.** One
    # global ratio spends the budget in proportion to how big a part already
    # was, so a trunk of 19 000 vertices beside a canopy of 927 000 is cut to
    # 0.46% along with it — eighty-nine vertices of trunk, and the tree renders
    # as a spider. That is what the first version did.
    #
    # Water-filling instead: every part is offered an equal share, a part that
    # needs less than its share takes only what it needs, and the surplus is
    # offered round again. A trunk is small and gets kept nearly whole; the
    # canopy, which is where the vertices actually are, absorbs the rest.
    counts = {id(p): _count_of(document, p) for _m, p in primitives}
    allowance: dict[int, int] = {}
    pending = sorted(counts, key=lambda key: counts[key])
    remaining = budget
    while pending:
        share = remaining // len(pending)
        key = pending[0]
        if counts[key] <= share or counts[key] < keep_whole_below:
            take = min(counts[key], max(share, min(counts[key], keep_whole_below)))
        else:
            take = share
        allowance[key] = take
        remaining = max(0, remaining - take)
        pending.pop(0)
    ratios = {key: (1.0 if counts[key] == 0 else min(1.0, allowance[key] / counts[key]))
              for key in counts}

    binary = bytearray()
    views: list[dict] = []
    accessors: list[dict] = []

    def store(payload: bytes, target_kind: int | None = None) -> int:
        nonlocal binary
        binary += b"\0" * ((-len(binary)) % 4)
        view = {"buffer": 0, "byteOffset": len(binary), "byteLength": len(payload)}
        if target_kind is not None:
            view["target"] = target_kind
        views.append(view)
        binary += payload
        return len(views) - 1

    def add_accessor(values, kind: str, component: int) -> int:
        fmt, _size = _COMPONENT[component]
        width = _COUNT[kind]
        packer = struct.Struct("<" + fmt * width)
        payload = b"".join(packer.pack(*value) for value in values)
        view = store(payload, 34962 if component == 5126 else 34963)
        accessor = {"bufferView": view, "componentType": component,
                    "count": len(values), "type": kind}
        if kind == "VEC3" and component == 5126:
            accessor["min"] = [min(v[i] for v in values) for i in range(3)]
            accessor["max"] = [max(v[i] for v in values) for i in range(3)]
        accessors.append(accessor)
        return len(accessors) - 1

    kept_total = 0
    for _mesh, primitive in primitives:
        attributes = primitive["attributes"]
        positions = _read_accessor(document, buffers, attributes["POSITION"])
        indices = [i[0] for i in _read_accessor(document, buffers, primitive["indices"])]
        others = {name: _read_accessor(document, buffers, index)
                  for name, index in attributes.items() if name != "POSITION"}

        ratio = ratios[id(primitive)]
        if ratio >= 1.0:
            chosen = list(range(0, len(indices), 3))
        else:
            clusters = _cluster_triangles(indices, len(positions))
            centres = {base: positions[indices[base]] for base in range(0, len(indices), 3)}
            chosen = [base for cluster in _keep(clusters, centres, ratio) for base in cluster]
            chosen.sort()

        remap: dict[int, int] = {}
        new_indices = []
        for base in chosen:
            for offset in range(3):
                old = indices[base + offset]
                new = remap.get(old)
                if new is None:
                    new = remap[old] = len(remap)
                new_indices.append(new)
        order = sorted(remap, key=remap.get)
        primitive["attributes"] = {
            "POSITION": add_accessor([positions[i] for i in order], "VEC3", 5126)
        }
        for name, values in others.items():
            kind = document["accessors"][attributes[name]]["type"]
            primitive["attributes"][name] = add_accessor(
                [values[i] for i in order], kind, 5126)
        primitive["indices"] = add_accessor(
            [(i,) for i in new_indices], "SCALAR", 5125)
        kept_total += len(order)

    _strip_to_base_colour(document)
    for image in document.get("images", []):
        uri = image.pop("uri", None)
        if uri is None:
            continue
        blob = (folder / uri).read_bytes()
        # A texture that is not an image must not enter, and this is not
        # hypothetical: nine of these files were 94-byte JSON 404 bodies saved
        # under a `.jpg` name by a download whose URL had been guessed rather
        # than read from the index. The engine's answer was `Failed to load
        # memory texture` five thousand seven hundred times in one run, and
        # nothing on screen said which asset. Two bytes of check here, once.
        kind = _image_kind(blob)
        if kind is None:
            raise RuntimeError(
                f"{source.name} references {uri}, which is {len(blob)} bytes and "
                f"is not a PNG or a JPEG. It is very likely an error page saved "
                f"under an image's name (plan §4: un asset non conforme "
                f"n'entre pas)."
            )
        blob = _shrink_texture(blob, texture_size)
        image["bufferView"] = store(blob)
        image["mimeType"] = "image/jpeg"

    document["bufferViews"] = views
    document["accessors"] = accessors
    document["buffers"] = [{"byteLength": len(_pad(bytes(binary)))}]
    encoded = _pad(json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8"), b" ")
    payload = _pad(bytes(binary))
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(
        struct.pack("<III", 0x46546C67, 2, 12 + 8 + len(encoded) + 8 + len(payload))
        + struct.pack("<II", len(encoded), 0x4E4F534A) + encoded
        + struct.pack("<II", len(payload), 0x004E4942) + payload
    )
    return {"source": source.name, "before": sum(counts.values()), "after": kept_total,
            "ratios": {counts[k]: round(ratios[k], 4) for k in counts},
            "bytes": target.stat().st_size}


def _count_of(document: dict, primitive: dict) -> int:
    return document["accessors"][primitive["attributes"]["POSITION"]]["count"]
