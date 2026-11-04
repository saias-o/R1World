"""People seen from outside the game: a Rocketbox model posed by its own clips, drawn offscreen.

    python -m r1.human_views lods   (from game/tools; numpy, Pillow, moderngl)

The player's levels of detail (r1.humans --lods) side by side, posed in his
walk, each pair at the pixel height it has where the finer one hands over
(a 1080-line screen at the game's 62 degree field of view), with how many
of its pixels change: generated/human-lods/. The walking and waiting
impostors (r1.human_impostor) are drawn by the same posing and renderer.

The pose is the glTF one: every node's rest transform, overridden by the
clip's channels at that instant (LINEAR, STEP), the skin's joints times its
inverse binds, each vertex the weighted sum of its four joints.
"""
from __future__ import annotations

import io
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from .tree_lod import read_glb

GAME = Path(__file__).resolve().parents[2]
HUMANS = GAME / "assets/models/humans"
OUT = GAME / "generated/human-lods"

# The game's camera, and the people's runtime resize on top of humans.json's
# scale (native/world: sceneFov; limits.hpp kPeopleVehicleResize).
FOV_DEGREES, SCREEN_LINES = 62.0, 1080
RUNTIME_RESIZE = 0.85

_COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def accessor(doc: dict, binary: bytes, index: int) -> np.ndarray:
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    dtype = np.dtype(_COMPONENT[a["componentType"]])
    width = _WIDTH[a["type"]]
    stride = view.get("byteStride") or dtype.itemsize * width
    base = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    raw = np.frombuffer(binary, dtype=np.uint8, count=stride * (a["count"] - 1) + dtype.itemsize * width, offset=base)
    rows = np.lib.stride_tricks.as_strided(raw, shape=(a["count"], dtype.itemsize * width), strides=(stride, 1))
    values = np.ascontiguousarray(rows).view(dtype).reshape(a["count"], width)
    if a.get("normalized") and dtype != np.float32:
        values = values.astype(np.float32) / np.iinfo(dtype).max
    return values


def trs_matrix(t, r, s) -> np.ndarray:
    x, y, z, w = r
    m = np.eye(4)
    m[:3, :3] = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                          [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                          [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]) * np.asarray(s)
    m[:3, 3] = t
    return m


class Character:
    """One Rocketbox GLB: its skeleton, clips, meshes and textures."""

    def __init__(self, path: Path):
        self.path = path
        self.doc, self.binary = read_glb(path)
        doc = self.doc
        self.parent = [-1] * len(doc["nodes"])
        for i, node in enumerate(doc["nodes"]):
            for child in node.get("children", []):
                self.parent[child] = i
        self.rest = [(n.get("translation", [0, 0, 0]), n.get("rotation", [0, 0, 0, 1]), n.get("scale", [1, 1, 1]))
                     for n in doc["nodes"]]
        if any("matrix" in n for n in doc["nodes"]):
            raise SystemExit(f"{path.name}: a node is given by a matrix; the clips animate TRS nodes")
        skin = doc["skins"][0]
        self.joints = skin["joints"]
        self.inverse_binds = accessor(doc, self.binary, skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
        self.clips = {}
        for clip in doc["animations"]:
            channels = []
            for channel in clip["channels"]:
                sampler = clip["samplers"][channel["sampler"]]
                times = accessor(doc, self.binary, sampler["input"])[:, 0].astype(np.float64)
                values = accessor(doc, self.binary, sampler["output"]).astype(np.float64)
                channels.append((channel["target"]["node"], channel["target"]["path"], times, values,
                                 sampler.get("interpolation", "LINEAR")))
            self.clips[clip["name"]] = (channels, max(float(c[2][-1]) for c in channels))
        # Every mesh by name: the levels of detail are meshes of their own.
        self.meshes = {}
        for node in doc["nodes"]:
            if "mesh" in node:
                mesh = doc["meshes"][node["mesh"]]
                prims = []
                for p in mesh["primitives"]:
                    at = p["attributes"]
                    prims.append({
                        "position": accessor(doc, self.binary, at["POSITION"]).astype(np.float64),
                        "normal": accessor(doc, self.binary, at["NORMAL"]).astype(np.float64),
                        "uv": accessor(doc, self.binary, at["TEXCOORD_0"]).astype(np.float32),
                        "joints": accessor(doc, self.binary, at["JOINTS_0"]).astype(np.int64),
                        "weights": accessor(doc, self.binary, at["WEIGHTS_0"]).astype(np.float64),
                        "indices": accessor(doc, self.binary, p["indices"]).astype(np.uint32).ravel(),
                        "material": p.get("material", 0)})
                self.meshes[mesh.get("name")] = prims

    def seconds(self, clip: str) -> float:
        return self.clips[clip][1]

    def image(self, texture_index: int) -> Image.Image:
        image = self.doc["images"][self.doc["textures"][texture_index]["source"]]
        view = self.doc["bufferViews"][image["bufferView"]]
        offset = view.get("byteOffset", 0)
        return Image.open(io.BytesIO(self.binary[offset:offset + view["byteLength"]])).convert("RGBA")

    def joint_matrices(self, clip: str, t: float) -> np.ndarray:
        """The skin's joint matrices at `t` seconds into `clip` (looped)."""
        channels, length = self.clips[clip]
        t = t % length if length > 0 else 0.0
        local = [list(r) for r in self.rest]
        for node, path, times, values, interpolation in channels:
            k = int(np.searchsorted(times, t, side="right"))
            if k <= 0:
                value = values[0]
            elif k >= len(times):
                value = values[-1]
            elif interpolation == "STEP":
                value = values[k - 1]
            else:
                f = (t - times[k - 1]) / max(times[k] - times[k - 1], 1e-9)
                a, b = values[k - 1], values[k]
                if path == "rotation":
                    if np.dot(a, b) < 0:
                        b = -b
                    value = a + (b - a) * f
                    value = value / np.linalg.norm(value)
                else:
                    value = a + (b - a) * f
            local[node][{"translation": 0, "rotation": 1, "scale": 2}[path]] = value
        world = [None] * len(local)

        def resolve(i):
            if world[i] is None:
                m = trs_matrix(*local[i])
                world[i] = m if self.parent[i] < 0 else resolve(self.parent[i]) @ m
            return world[i]
        return np.stack([resolve(j) for j in self.joints]) @ self.inverse_binds

    def posed(self, mesh: str, clip: str, t: float):
        """The mesh's primitives skinned at that instant: positions, normals."""
        joints = self.joint_matrices(clip, t)
        out = []
        for p in self.meshes[mesh]:
            m = np.einsum("vk,vkij->vij", p["weights"], joints[p["joints"]])
            position = np.einsum("vij,vj->vi", m[:, :3, :3], p["position"]) + m[:, :3, 3]
            normal = np.einsum("vij,vj->vi", m[:, :3, :3], p["normal"])
            normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-9)
            out.append((position, normal, p))
        return out


VERTEX = """#version 330
in vec3 position; in vec3 normal; in vec2 uv;
uniform mat4 projection;
out vec2 texcoord; out vec3 surfaceNormal; out vec3 surfacePosition;
void main() { gl_Position=projection*vec4(position,1); texcoord=uv; surfaceNormal=normal; surfacePosition=position; }
"""
FRAGMENT = """#version 330
in vec2 texcoord; in vec3 surfaceNormal; in vec3 surfacePosition;
uniform sampler2D albedo; uniform vec4 color; uniform float cutoff;
layout(location=0) out vec4 bakedColor;
layout(location=1) out vec4 bakedNormal;
layout(location=2) out vec4 bakedPosition;
vec3 linearColor(vec3 c) {return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),greaterThan(c,vec3(.04045)));}
void main() {
 vec4 s=texture(albedo,texcoord); if(s.a*color.a<cutoff) discard;
 bakedColor=vec4(linearColor(s.rgb)*color.rgb,1.);
 vec3 n=normalize(surfaceNormal); if(!gl_FrontFacing) n=-n;
 bakedNormal=vec4(n,1.); bakedPosition=vec4(surfacePosition,1.);
}
"""


class Renderer:
    """Orthographic views of posed meshes into colour, normal and position."""

    def __init__(self, character: Character, size: int):
        import moderngl  # only drawing needs an OpenGL context
        self.mgl = moderngl
        self.character = character
        self.size = size
        self.ctx = moderngl.create_standalone_context(require=330)
        self.program = self.ctx.program(vertex_shader=VERTEX, fragment_shader=FRAGMENT)
        self.program["albedo"].value = 0
        self.textures = {}
        self.white = self.ctx.texture((1, 1), 4, bytes([255] * 4))
        self.colors = self.ctx.texture((size, size), 4, dtype="f4")
        self.normals = self.ctx.texture((size, size), 4, dtype="f4")
        self.positions = self.ctx.texture((size, size), 4, dtype="f4")
        self.fbo = self.ctx.framebuffer([self.colors, self.normals, self.positions], self.ctx.depth_renderbuffer((size, size)))

    def texture(self, material: dict):
        ref = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if not ref:
            return self.white
        index = ref["index"]
        if index not in self.textures:
            im = self.character.image(index)
            tex = self.ctx.texture(im.size, 4, im.tobytes())
            tex.build_mipmaps()
            tex.filter = (self.mgl.LINEAR_MIPMAP_LINEAR, self.mgl.LINEAR)
            self.textures[index] = tex
        return self.textures[index]

    def draw(self, posed, projection: np.ndarray, target=None) -> None:
        ctx = self.ctx
        (target or self.fbo).use()
        if target is None:
            self.fbo.clear(0, 0, 0, 0, depth=1)
        ctx.enable(self.mgl.DEPTH_TEST)
        ctx.disable(self.mgl.CULL_FACE)  # cloth and hair cards are seen from both sides
        self.program["projection"].write(projection.T.astype("f4").tobytes())
        for position, normal, prim in posed:
            material = self.character.doc["materials"][prim["material"]]
            vertices = np.concatenate((position, normal, prim["uv"]), axis=1).astype("f4")
            vao = self.ctx.vertex_array(self.program, [(self.ctx.buffer(vertices.tobytes()), "3f 3f 2f", "position", "normal", "uv")],
                                        self.ctx.buffer(prim["indices"].tobytes()))
            self.texture(material).use(0)
            self.program["color"].value = tuple(material.get("pbrMetallicRoughness", {}).get("baseColorFactor", [1, 1, 1, 1]))
            # Hair and lashes are blended in the game; a still picture keeps what covers half a texel.
            mode = material.get("alphaMode", "OPAQUE")
            self.program["cutoff"].value = material.get("alphaCutoff", .5) if mode == "MASK" else .5 if mode == "BLEND" else 0.
            vao.render()
            vao.release()

    def read(self, attachment: int) -> np.ndarray:
        data = np.frombuffer(self.fbo.read(components=4, attachment=attachment, dtype="f4"), dtype=np.float32)
        return data.reshape(self.size, self.size, 4)[::-1]


def view_projection(center: np.ndarray, extent: float, azimuth: float, elevation: float = 0.0):
    """Orthographic, looking at `center` from `azimuth` (radians, 0: from +Z, the
    model's front) and `elevation`, `extent` metres across."""
    direction = np.array([math.sin(azimuth) * math.cos(elevation), math.sin(elevation), math.cos(azimuth) * math.cos(elevation)])
    right = np.array([math.cos(azimuth), 0.0, -math.sin(azimuth)])
    up = np.cross(direction, right)
    frame = np.stack((right, up, direction), axis=1)
    projection = np.eye(4)
    projection[:3, :3] = frame.T * 2 / extent
    projection[2, :3] *= -1
    projection[:3, 3] = -projection[:3, :3] @ center
    return projection


def encode_color(linear):
    return np.where(linear <= .0031308, linear * 12.92, 1.055 * np.maximum(linear, 0) ** (1 / 2.4) - .055)


def shaded(color: np.ndarray, normal: np.ndarray, light=(.4, .8, .45)) -> np.ndarray:
    """A plain sunlit picture of a view, for looking at, over a grey ground."""
    light = np.asarray(light) / np.linalg.norm(light)
    alpha = color[:, :, 3:4]
    rgb = color[:, :, :3] / np.maximum(alpha, 1e-8)
    n = normal[:, :, :3] / np.maximum(np.linalg.norm(normal[:, :, :3], axis=2, keepdims=True), 1e-8)
    lit = rgb * (.35 + .9 * np.maximum((n * light).sum(axis=2, keepdims=True), 0))
    background = np.full_like(lit, .45)
    return encode_color(lit * alpha + background * (1 - alpha))


def lods() -> int:
    """The player's levels of detail at the size each takes over."""
    import json
    manifest = json.loads((HUMANS / "humans.json").read_text(encoding="utf-8"))
    entry = manifest["player"]
    levels = [lod["name"] for lod in entry["lods"]] + [lod["name"] for lod in entry.get("autoLods", [])]
    if len(levels) < 2:
        raise SystemExit("the player has one level of detail: python -m r1.humans --lods first")
    character = Character(GAME / entry["model"])
    node = next(n for n in character.doc["nodes"] if "MSFT_lod" in n.get("extensions", {}))
    coverage = node["extras"]["MSFT_screencoverage"][1:]
    scale = float(manifest["scale"]) * RUNTIME_RESIZE
    clip, instant = "walk", .3
    posed = {name: character.posed(name, clip, instant) for name in levels}
    points = np.concatenate([p[0] for p in posed[levels[0]]])
    low, high = points.min(axis=0), points.max(axis=0)
    center = (low + high) / 2
    extent = float(max(high - low) * 1.08)
    # The bounding sphere MeshLod.cpp measures (rest pose, at the game's scale).
    rest = np.concatenate([p["position"] for p in character.meshes[levels[0]]])
    radius = float(np.linalg.norm(rest.max(axis=0) - rest.min(axis=0)) / 2 * scale)
    tan = math.tan(math.radians(FOV_DEGREES / 2))
    height = float(high[1] - low[1]) * scale
    renderer = Renderer(character, 1024)
    OUT.mkdir(parents=True, exist_ok=True)
    tiles, report = [], []
    for k, c in enumerate(coverage):
        metres = 2 * radius / tan / math.sqrt(c)
        pixels = height / (2 * metres * tan) * SCREEN_LINES  # his height on the screen there
        side = max(8, int(round(pixels * extent / (height / scale))))
        pair = []
        for name in (levels[k], levels[k + 1]):
            renderer.draw(posed[name], view_projection(center, extent, math.radians(30)))
            picture = shaded(renderer.read(0), renderer.read(1))
            small = Image.fromarray(np.uint8(np.clip(picture, 0, 1) * 255 + .5)).resize((side, side), Image.LANCZOS)
            pair.append(np.asarray(small, dtype=np.float32) / 255)
        changed = float((np.abs(pair[0] - pair[1]).max(axis=2) > 8 / 255).mean())
        report.append({"from": levels[k], "to": levels[k + 1], "coverage": c, "metres": round(metres, 1),
                       "pixelsTall": round(pixels, 1), "pixelsChanged": round(changed * 100, 2)})
        zoom = max(1, 256 // side)
        tiles.append(np.concatenate([np.kron(p, np.ones((zoom, zoom, 1))) for p in pair], axis=1))
    width = max(t.shape[1] for t in tiles)
    sheet = np.concatenate([np.pad(t, ((4, 4), (0, width - t.shape[1]), (0, 0)), constant_values=1) for t in tiles], axis=0)
    Image.fromarray(np.uint8(np.clip(sheet, 0, 1) * 255 + .5)).save(OUT / "player-lod-switches.png")
    # And every level at full size, side by side, for its shape.
    full = []
    for name in levels:
        renderer.draw(posed[name], view_projection(center, extent, math.radians(30)))
        full.append(np.asarray(Image.fromarray(np.uint8(np.clip(shaded(renderer.read(0), renderer.read(1)), 0, 1) * 255 + .5)).resize((384, 384), Image.LANCZOS)))
    Image.fromarray(np.concatenate(full, axis=1)).save(OUT / "player-lods.png")
    (OUT / "player-lods.json").write_text(json.dumps(report, indent=1) + "\n", encoding="utf-8")
    for r in report:
        print(f"{r['from']} -> {r['to']} at {r['metres']} m ({r['pixelsTall']} px tall): {r['pixelsChanged']} % of pixels change")
    print(f"pictures in {OUT}")
    return 0


if __name__ == "__main__":
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command != "lods":
        raise SystemExit("usage: python -m r1.human_views lods")
    raise SystemExit(lods())
