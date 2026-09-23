"""Small deterministic mesh writer used by the R1World offline generators.

The runtime should receive compact geometry, not thousands of authoring nodes.
This module intentionally implements only the GLB subset produced by R1World:
indexed triangles with positions, normals, optional UVs and one material per
mesh. Textures remain external files next to the generated assets so they can
be cached and packaged normally by Saida.
"""

from __future__ import annotations

import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence


Vec3 = tuple[float, float, float]
Vec2 = tuple[float, float]


def _sub(a: Vec3, b: Vec3) -> Vec3:
    return a[0] - b[0], a[1] - b[1], a[2] - b[2]


def _cross(a: Vec3, b: Vec3) -> Vec3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def _normal(a: Vec3, b: Vec3, c: Vec3) -> Vec3:
    n = _cross(_sub(b, a), _sub(c, a))
    length = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
    if length < 1e-12:
        return 0.0, 1.0, 0.0
    return n[0] / length, n[1] / length, n[2] / length


@dataclass
class Mesh:
    """Flat-shaded indexed triangle mesh.

    Vertices are welded on `(position, normal, uv)`. Flat shading gives every
    face its own normal, so this merges only what genuinely coincides — the two
    triangles of a quad share an edge and become four vertices instead of six —
    and never smooths a crease that should stay sharp.

    It is not a size optimisation. The engine's `GeometryRegistry` holds a fixed
    million vertices for the whole scene, and a generator that emits three per
    triangle spends that budget three times over: one alpine village's buildings
    alone reached 527 000 vertices for 176 000 triangles and the scene stopped
    loading. §4 I4 says the budget is a contract; welding is what makes the
    generator honour it rather than negotiate with it.

    The weld is deterministic: the first occurrence of a vertex wins and the
    ordering follows insertion, so the same input still produces byte-identical
    output (§4 I3).
    """

    positions: list[Vec3] = field(default_factory=list)
    normals: list[Vec3] = field(default_factory=list)
    texcoords: list[Vec2] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)
    _lookup: dict[tuple, int] = field(default_factory=dict, repr=False, compare=False)

    def _vertex(self, position: Vec3, normal: Vec3, uv: Vec2) -> int:
        # Coplanar triangles can differ by floating-point roundoff after local
        # frame projection. A raw float key then stores six vertices for a quad
        # instead of four. Quantise only the weld key (sub-microradian); retain
        # the original position/normal in the exported mesh and real creases.
        key = (position, tuple(round(n, 6) for n in normal), uv)
        existing = self._lookup.get(key)
        if existing is not None:
            return existing
        index = len(self.positions)
        self.positions.append(position)
        self.normals.append(normal)
        self.texcoords.append(uv)
        self._lookup[key] = index
        return index

    def add_triangle(
        self,
        a: Vec3,
        b: Vec3,
        c: Vec3,
        uvs: tuple[Vec2, Vec2, Vec2] | None = None,
    ) -> None:
        n = _normal(a, b, c)
        uv = uvs or ((0.0, 0.0), (0.0, 0.0), (0.0, 0.0))
        self.indices.extend((
            self._vertex(a, n, uv[0]),
            self._vertex(b, n, uv[1]),
            self._vertex(c, n, uv[2]),
        ))

    def add_up_triangle(
        self,
        a: Vec3,
        b: Vec3,
        c: Vec3,
        uvs: tuple[Vec2, Vec2, Vec2] | None = None,
    ) -> None:
        if _normal(a, b, c)[1] < 0.0:
            b, c = c, b
            if uvs is not None:
                uvs = uvs[0], uvs[2], uvs[1]
        self.add_triangle(a, b, c, uvs)

    def add_quad(
        self,
        a: Vec3,
        b: Vec3,
        c: Vec3,
        d: Vec3,
        uvs: tuple[Vec2, Vec2, Vec2, Vec2] | None = None,
    ) -> None:
        if uvs is None:
            self.add_triangle(a, b, c)
            self.add_triangle(a, c, d)
        else:
            self.add_triangle(a, b, c, (uvs[0], uvs[1], uvs[2]))
            self.add_triangle(a, c, d, (uvs[0], uvs[2], uvs[3]))

    def add_up_quad(
        self,
        a: Vec3,
        b: Vec3,
        c: Vec3,
        d: Vec3,
        uvs: tuple[Vec2, Vec2, Vec2, Vec2] | None = None,
    ) -> None:
        if _normal(a, b, c)[1] < 0.0:
            b, d = d, b
            if uvs is not None:
                uvs = uvs[0], uvs[3], uvs[2], uvs[1]
        self.add_quad(a, b, c, d, uvs)

    def add_box(
        self,
        center: Vec3,
        size: Vec3,
        yaw_radians: float = 0.0,
    ) -> None:
        hx, hy, hz = size[0] * 0.5, size[1] * 0.5, size[2] * 0.5
        cosine, sine = math.cos(yaw_radians), math.sin(yaw_radians)

        def point(x: float, y: float, z: float) -> Vec3:
            return (
                center[0] + x * cosine + z * sine,
                center[1] + y,
                center[2] - x * sine + z * cosine,
            )

        p = (
            point(-hx, -hy, -hz), point(hx, -hy, -hz),
            point(hx, -hy, hz), point(-hx, -hy, hz),
            point(-hx, hy, -hz), point(hx, hy, -hz),
            point(hx, hy, hz), point(-hx, hy, hz),
        )
        self.add_quad(p[0], p[3], p[2], p[1])
        self.add_quad(p[4], p[5], p[6], p[7])
        self.add_quad(p[0], p[1], p[5], p[4])
        self.add_quad(p[1], p[2], p[6], p[5])
        self.add_quad(p[2], p[3], p[7], p[6])
        self.add_quad(p[3], p[0], p[4], p[7])

    def add_cylinder(
        self,
        center: Vec3,
        radius: float,
        height: float,
        sides: int = 6,
    ) -> None:
        bottom = center[1] - height * 0.5
        top = center[1] + height * 0.5
        ring = [
            (
                center[0] + math.cos(2.0 * math.pi * i / sides) * radius,
                bottom,
                center[2] + math.sin(2.0 * math.pi * i / sides) * radius,
            )
            for i in range(sides)
        ]
        upper = [(x, top, z) for x, _, z in ring]
        for i in range(sides):
            j = (i + 1) % sides
            self.add_quad(ring[i], ring[j], upper[j], upper[i])
            self.add_up_triangle((center[0], top, center[2]), upper[i], upper[j])

    def add_cone(
        self,
        center: Vec3,
        radius: float,
        height: float,
        sides: int = 7,
    ) -> None:
        bottom = center[1] - height * 0.5
        apex = center[0], center[1] + height * 0.5, center[2]
        ring = [
            (
                center[0] + math.cos(2.0 * math.pi * i / sides) * radius,
                bottom,
                center[2] + math.sin(2.0 * math.pi * i / sides) * radius,
            )
            for i in range(sides)
        ]
        for i in range(sides):
            self.add_triangle(ring[i], ring[(i + 1) % sides], apex)

    def add_cross(self, center: Vec3, width: float, height: float) -> None:
        x, y, z = center
        half = width * 0.5
        self.add_quad(
            (x - half, y, z), (x + half, y, z),
            (x + half, y + height, z), (x - half, y + height, z),
        )
        self.add_quad(
            (x, y, z - half), (x, y, z + half),
            (x, y + height, z + half), (x, y + height, z - half),
        )


def model_height(path: Path) -> float:
    """The height in metres of a glTF/GLB model, from its position bounds.

    A generator that scales an imported model by a bare factor is holding a
    magic number: change the model and every instance silently becomes the wrong
    size, with nothing to notice it. Reading the model's own extent turns the
    factor into a target height in metres, which is a number a reader can check
    against a real tree.

    glTF requires `min`/`max` on a POSITION accessor, so this needs no vertex
    data and no buffer at all — the JSON chunk is enough.
    """
    payload = path.read_bytes()
    if payload[:4] == b"glTF":
        json_length, = struct.unpack_from("<I", payload, 12)
        document = json.loads(payload[20:20 + json_length].decode("utf-8"))
    else:
        document = json.loads(payload.decode("utf-8"))

    low = float("inf")
    high = float("-inf")
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            index = primitive.get("attributes", {}).get("POSITION")
            if index is None:
                continue
            accessor = document["accessors"][index]
            if "min" not in accessor or "max" not in accessor:
                continue
            low = min(low, float(accessor["min"][1]))
            high = max(high, float(accessor["max"][1]))
    if high <= low:
        raise RuntimeError(f"{path.name} declares no usable vertical extent")
    return high - low


@dataclass(frozen=True)
class Material:
    name: str
    color: tuple[float, float, float, float]
    roughness: float = 0.9
    metallic: float = 0.0
    double_sided: bool = False
    base_color_texture: str | None = None


@dataclass(frozen=True)
class MeshPart:
    name: str
    mesh: Mesh
    material: Material


def _pad4(data: bytes, byte: bytes = b"\0") -> bytes:
    return data + byte * ((-len(data)) % 4)


def write_glb(path: Path, parts: Sequence[MeshPart]) -> None:
    """Write a deterministic GLB 2.0 containing all non-empty mesh parts."""
    kept = [part for part in parts if part.mesh.indices]
    if not kept:
        raise ValueError("a GLB must contain at least one triangle")

    binary = bytearray()
    buffer_views = []
    accessors = []
    meshes = []
    nodes = []
    materials = []
    images = []
    textures = []
    texture_indices: dict[str, int] = {}

    def texture_index(uri: str) -> int:
        existing = texture_indices.get(uri)
        if existing is not None:
            return existing
        index = len(textures)
        images.append({"uri": uri})
        textures.append({"source": len(images) - 1})
        texture_indices[uri] = index
        return index

    def append_view(payload: bytes, target: int) -> int:
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        binary.extend(payload)
        index = len(buffer_views)
        buffer_views.append({
            "buffer": 0,
            "byteOffset": offset,
            "byteLength": len(payload),
            "target": target,
        })
        return index

    def append_accessor(
        view: int,
        component_type: int,
        count: int,
        kind: str,
        minimum: Iterable[float] | None = None,
        maximum: Iterable[float] | None = None,
    ) -> int:
        accessor = {
            "bufferView": view,
            "componentType": component_type,
            "count": count,
            "type": kind,
        }
        if minimum is not None:
            accessor["min"] = list(minimum)
        if maximum is not None:
            accessor["max"] = list(maximum)
        index = len(accessors)
        accessors.append(accessor)
        return index

    for mesh_index, part in enumerate(kept):
        mesh = part.mesh
        pos_payload = b"".join(struct.pack("<3f", *p) for p in mesh.positions)
        normal_payload = b"".join(struct.pack("<3f", *n) for n in mesh.normals)
        uv_payload = b"".join(struct.pack("<2f", *uv) for uv in mesh.texcoords)
        index_payload = b"".join(struct.pack("<I", i) for i in mesh.indices)
        pos_view = append_view(pos_payload, 34962)
        normal_view = append_view(normal_payload, 34962)
        uv_view = append_view(uv_payload, 34962)
        index_view = append_view(index_payload, 34963)

        minimum = [min(p[axis] for p in mesh.positions) for axis in range(3)]
        maximum = [max(p[axis] for p in mesh.positions) for axis in range(3)]
        pos_accessor = append_accessor(
            pos_view, 5126, len(mesh.positions), "VEC3", minimum, maximum
        )
        normal_accessor = append_accessor(
            normal_view, 5126, len(mesh.normals), "VEC3"
        )
        uv_accessor = append_accessor(
            uv_view, 5126, len(mesh.texcoords), "VEC2"
        )
        index_accessor = append_accessor(
            index_view, 5125, len(mesh.indices), "SCALAR"
        )

        material_index = len(materials)
        pbr = {
            "baseColorFactor": list(part.material.color),
            "metallicFactor": part.material.metallic,
            "roughnessFactor": part.material.roughness,
        }
        if part.material.base_color_texture:
            pbr["baseColorTexture"] = {
                "index": texture_index(part.material.base_color_texture)
            }
        materials.append({
            "name": part.material.name,
            "doubleSided": part.material.double_sided,
            "pbrMetallicRoughness": pbr,
        })
        meshes.append({
            "name": part.name,
            "primitives": [{
                "attributes": {
                    "POSITION": pos_accessor,
                    "NORMAL": normal_accessor,
                    "TEXCOORD_0": uv_accessor,
                },
                "indices": index_accessor,
                "material": material_index,
                "mode": 4,
            }],
        })
        nodes.append({"name": part.name, "mesh": mesh_index})

    document = {
        "asset": {"version": "2.0", "generator": "R1World generator v1"},
        "scene": 0,
        "scenes": [{"nodes": list(range(len(nodes)))}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(binary)}],
    }
    if textures:
        document["images"] = images
        document["textures"] = textures
    json_chunk = _pad4(
        json.dumps(document, separators=(",", ":"), sort_keys=True).encode("utf-8"),
        b" ",
    )
    bin_chunk = _pad4(bytes(binary))
    total = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    glb = (
        struct.pack("<III", 0x46546C67, 2, total)
        + struct.pack("<II", len(json_chunk), 0x4E4F534A)
        + json_chunk
        + struct.pack("<II", len(bin_chunk), 0x004E4942)
        + bin_chunk
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(glb)
