"""Walking and waiting impostors of Rocketbox people, baked from their own scans.

    python -m r1.human_impostor            the player's (assets/models/humans/impostors/)
    python -m r1.human_impostor --sample   and three passers-by, for the sheet only (generated/)

(from game/tools; numpy, Pillow, moderngl.) Not drawn by the game yet: the
user validates them on the sheets first (docs/PLAN.md; CLAUDE.md rule 1).

Each clip is an animated atlas of the engine's billboard material
(SAIDA_billboard: `columns` azimuths, one row, `frames` stacked): the walk
in WALK_FRAMES poses over its loop, timed by the ground covered (`stride`),
the idle in IDLE_FRAMES poses over its seconds (`loopSeconds`). As the tree
impostors (tree_impostor.py): scanned albedo and alpha, normals in the
model's frame, cosine-weighted hemisphere visibility in the normal's alpha,
no sun baked -- the game lights them. Every cell is posed by the clip itself
(human_views.Character), the skin as the game skins it.

Sheets for the validation: generated/human-impostors/ -- every cell relit by
a sun, and the impostor beside the mesh it replaces at 23 and 12 pixels tall.
"""
from __future__ import annotations

import hashlib
import io
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from .human_views import GAME, HUMANS, RUNTIME_RESIZE, Character, Renderer, encode_color, shaded, view_projection
from .tree_impostor import ambient_visibility, dilate, sphere_directions
from .tree_lod import Builder, write_glb

OUT = HUMANS / "impostors"
SHEETS = GAME / "generated/human-impostors"
SAMPLE_OUT = SHEETS / "models"
COLUMNS = 8            # azimuths, every 45 degrees
WALK_FRAMES, IDLE_FRAMES = 12, 8
RESOLUTION, SUPERSAMPLE = 128, 2
OCCLUSION_DIRECTIONS, OCCLUSION_RESOLUTION, OCCLUSION_BIAS_TEXELS = 32, 512, 1.0
ALPHA_CUTOFF = .4
SAMPLE = ["Female_Adult_01", "Male_Adult_12", "Business_Female_01"]


def png(array: np.ndarray) -> bytes:
    stream = io.BytesIO()
    Image.fromarray(np.uint8(np.clip(array, 0, 1) * 255 + .5)).save(stream, format="PNG", optimize=True)
    return stream.getvalue()


def bake_clip(character: Character, mesh: str, clip: str, frames: int):
    """The atlas of one clip: albedo+alpha and normal+visibility, rows of frames."""
    seconds = character.seconds(clip)
    instants = [seconds * f / frames for f in range(frames)]
    poses = [character.posed(mesh, clip, t) for t in instants]
    points = np.concatenate([p[0] for pose in poses for p in pose])
    low, high = points.min(axis=0), points.max(axis=0)
    # One frame for every cell: the feet stay where they are from frame to frame.
    center = np.array([(low[0] + high[0]) / 2, (low[1] + high[1]) / 2, (low[2] + high[2]) / 2])
    extent = float(2 * np.max(np.linalg.norm(points - center, axis=1)) * 1.04)
    size = RESOLUTION * SUPERSAMPLE
    renderer = Renderer(character, size)
    ctx = renderer.ctx
    shadow_depth = ctx.depth_texture((OCCLUSION_RESOLUTION, OCCLUSION_RESOLUTION))
    shadow_fbo = ctx.framebuffer(depth_attachment=shadow_depth)
    albedo = np.zeros((frames * RESOLUTION, COLUMNS * RESOLUTION, 4), dtype=np.float32)
    normal = np.zeros_like(albedo)
    for f, pose in enumerate(poses):
        shadows = []
        for direction in sphere_directions(OCCLUSION_DIRECTIONS):
            right = np.array([direction[2], 0, -direction[0]], dtype=np.float64)
            right = right / max(np.linalg.norm(right), 1e-9) if np.linalg.norm(right) > 1e-6 else np.array([1., 0, 0])
            up = np.cross(direction, right)
            frame = np.stack((right, up, direction), axis=1)
            projection = np.eye(4)
            projection[:3, :3] = frame.T * 2 / extent
            projection[2, :3] *= -1
            projection[:3, 3] = -projection[:3, :3] @ center
            shadow_fbo.use()
            shadow_fbo.clear(depth=1)
            renderer.draw(pose, projection, target=shadow_fbo)
            depth = np.frombuffer(shadow_depth.read(), dtype=np.float32).reshape(OCCLUSION_RESOLUTION, OCCLUSION_RESOLUTION)
            shadows.append((direction.astype(np.float32), projection.astype(np.float32), depth))
        for column in range(COLUMNS):
            # Column c is seen from azimuth c * 45 degrees, as billboard.glsl
            # picks it: atan(facing.x, facing.z), the eye's direction in the
            # model's frame.
            renderer.draw(pose, view_projection(center, extent, column * 2 * math.pi / COLUMNS))
            samples = renderer.read(0)
            pixels = samples.reshape(RESOLUTION, SUPERSAMPLE, RESOLUTION, SUPERSAMPLE, 4).mean(axis=(1, 3))
            alpha = pixels[:, :, 3]
            rgb = dilate(encode_color(pixels[:, :, :3] / np.maximum(alpha[:, :, None], 1e-8)), alpha)

            def covered(index):
                s = renderer.read(index).reshape(RESOLUTION, SUPERSAMPLE, RESOLUTION, SUPERSAMPLE, 4).mean(axis=(1, 3))
                return s[:, :, :3] / np.maximum(alpha[:, :, None], 1e-8)
            mapped = covered(1)
            mapped /= np.maximum(np.linalg.norm(mapped, axis=2, keepdims=True), 1e-8)
            visibility = ambient_visibility(covered(2).astype(np.float32), mapped.astype(np.float32), shadows)
            visibility = dilate(visibility[:, :, None], alpha)[:, :, 0]
            y, x = f * RESOLUTION, column * RESOLUTION
            albedo[y:y + RESOLUTION, x:x + RESOLUTION] = np.concatenate((rgb, alpha[:, :, None]), axis=2)
            normal[y:y + RESOLUTION, x:x + RESOLUTION] = np.concatenate((dilate(mapped * .5 + .5, alpha), visibility[:, :, None]), axis=2)
    ctx.release()
    return albedo, normal, center, extent, instants


def write_impostor(target: Path, name: str, albedo, normal, center, extent, frames: int, timing: dict) -> list[bytes]:
    builder = Builder()
    builder.doc["asset"]["generator"] = "R1World human_impostor.py"
    blobs = []
    for atlas in (albedo, normal):
        blob = png(atlas)
        blobs.append(blob)
        builder.doc["images"].append({"bufferView": builder.store(blob), "mimeType": "image/png"})
        builder.doc["textures"].append({"source": len(builder.doc["images"]) - 1})
    builder.doc["materials"].append({
        "name": f"{name} impostor", "doubleSided": True, "alphaMode": "MASK", "alphaCutoff": ALPHA_CUTOFF,
        "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0, "roughnessFactor": .8},
        "normalTexture": {"index": 1},
        "extras": {"SAIDA_billboard": {"columns": COLUMNS, "rows": 1, "frames": frames, **timing}}})
    h = extent / 2
    builder.mesh("Impostor", {
        "POSITION": builder.accessor([(-h, -h, 0), (h, -h, 0), (h, h, 0), (-h, h, 0)], "VEC3", 5126),
        "NORMAL": builder.accessor([(0, 0, 1)] * 4, "VEC3", 5126),
        "TANGENT": builder.accessor([(1, 0, 0, 1)] * 4, "VEC4", 5126),
        "TEXCOORD_0": builder.accessor([(0, 1), (1, 1), (1, 0), (0, 0)], "VEC2", 5126)},
        builder.accessor([(i,) for i in (0, 1, 2, 0, 2, 3)], "SCALAR", 5123), 0)
    builder.doc["nodes"][0]["translation"] = [float(v) for v in center]
    write_glb(target, builder.doc, bytes(builder.binary))
    return blobs


def bake_person(entry: dict, mesh: str, out: Path) -> list[dict]:
    model = GAME / entry["model"]
    character = Character(model)
    reports = []
    for clip, frames in (("walk", WALK_FRAMES), ("idle", IDLE_FRAMES)):
        albedo, normal, center, extent, instants = bake_clip(character, mesh, clip, frames)
        info = entry["clips"][clip]
        # The walk follows the ground covered: one loop is this many metres
        # in the model's units (humans.json speed x seconds; the instance's
        # scale applies on top, as to the mesh). The idle follows the clock.
        timing = {"stride": round(info["speed"] * info["seconds"], 4)} if clip == "walk" else {"loopSeconds": info["seconds"]}
        if clip == "walk" and not 0 < timing["stride"] <= 20:
            raise SystemExit(f"{entry['name']}: walk stride {timing['stride']} m is outside the material's 0..20 m")
        target = out / f"{entry['name'].lower()}_{clip}.glb"
        write_impostor(target, entry["name"], albedo, normal, center, extent, frames, timing)
        Image.fromarray(np.uint8(np.clip(albedo, 0, 1) * 255 + .5)).save(SHEETS / f"{target.stem}_albedo.png")
        Image.fromarray(np.uint8(np.clip(normal, 0, 1) * 255 + .5)).save(SHEETS / f"{target.stem}_normal.png")
        reports.append({"output": target.relative_to(GAME).as_posix(), "person": entry["name"], "clip": clip,
                        "derivedFrom": entry["model"], "mesh": mesh, "sourceSha256": hashlib.sha256(model.read_bytes()).hexdigest(),
                        "columns": COLUMNS, "rows": 1, "frames": frames, "resolution": RESOLUTION, "supersample": SUPERSAMPLE,
                        "extent": round(extent, 4), "pivot": [round(float(v), 4) for v in center],
                        "instants": [round(t, 4) for t in instants], **timing, "bytes": target.stat().st_size,
                        "method": "posed by the clip's own channels, skinned as the game skins; scanned albedo/alpha, "
                                  "model-frame normals and cosine-weighted hemisphere visibility in normal alpha; no sun baked"})
        relit_sheet(albedo, normal, SHEETS / f"{target.stem}_relit.png")
        compare_sheet(character, mesh, clip, instants, albedo, normal, center, extent, SHEETS / f"{target.stem}_beside_mesh.png")
    return reports


LIGHT = (.4, .8, .45)  # human_views.shaded's sun, so the sheets compare like with like


def relit(albedo: np.ndarray, normal: np.ndarray, light=LIGHT) -> np.ndarray:
    light = np.asarray(light) / np.linalg.norm(light)
    alpha = albedo[:, :, 3:4]
    rgb = albedo[:, :, :3] ** 2.2
    n = normal[:, :, :3] * 2 - 1
    visibility = normal[:, :, 3:4]
    lit = rgb * (.35 * visibility + .9 * np.maximum((n * light).sum(axis=2, keepdims=True), 0))
    # human_views.shaded's ground behind, in the same light units.
    return encode_color(lit * alpha + .45 * (1 - alpha))


def relit_sheet(albedo, normal, path: Path) -> None:
    Image.fromarray(np.uint8(np.clip(relit(albedo, normal), 0, 1) * 255 + .5)).save(path)


def compare_sheet(character: Character, mesh: str, clip: str, instants, albedo, normal, center, extent, path: Path) -> None:
    """The impostor's cell, relit, beside the mesh it replaces lit the same way, at 23 and 12 px tall."""
    renderer = Renderer(character, 512)
    rows = []
    height_fraction = .93  # a person fills about this much of the cell's height
    for pixels in (23, 12):
        side = max(8, int(round(pixels / height_fraction)))
        row = []
        for column in (0, 1, 3):
            renderer.draw(character.posed(mesh, clip, instants[0]), view_projection(center, extent, column * 2 * math.pi / COLUMNS))
            mesh_view = Image.fromarray(np.uint8(np.clip(shaded(renderer.read(0), renderer.read(1)), 0, 1) * 255 + .5))
            cells = (slice(0, RESOLUTION), slice(column * RESOLUTION, (column + 1) * RESOLUTION))
            cell_view = Image.fromarray(np.uint8(np.clip(relit(albedo[cells], normal[cells]), 0, 1) * 255 + .5))
            for view in (mesh_view, cell_view):
                small = view.resize((side, side), Image.LANCZOS).resize((side * 8, side * 8), Image.NEAREST)
                row.append(np.asarray(small))
        rows.append(np.concatenate(row, axis=1))
    width = max(r.shape[1] for r in rows)
    sheet = np.concatenate([np.pad(r, ((4, 4), (0, width - r.shape[1]), (0, 0)), constant_values=255) for r in rows], axis=0)
    Image.fromarray(sheet).save(path)
    renderer.ctx.release()


def register(reports: list[dict]) -> None:
    path = GAME / "assets/THIRD_PARTY_ASSETS.json"
    doc = json.loads(path.read_text(encoding="utf-8"))
    name = "Microsoft Rocketbox Avatar Library - player impostors"
    doc["assets"] = [a for a in doc["assets"] if a.get("name") != name]
    doc["assets"].append({
        "name": name, "author": "Microsoft (Rocketbox Studios)",
        "source": "https://github.com/microsoft/Microsoft-Rocketbox/tree/0943055db6ec570bcef9f2c8b41c9e5467c808f9",
        "license": "MIT", "licenseFile": "assets/licenses/Microsoft-Rocketbox-MIT.txt",
        "generatedBy": "game/tools/r1/human_impostor.py",
        "modifications": "multi-view animated billboard atlases baked from the player's scan posed by its walk and idle "
                         "clips: albedo, alpha, model-frame normals and ambient visibility; awaiting visual validation, not drawn yet",
        "files": sorted([r["output"] for r in reports] + [(OUT / "SOURCES.json").relative_to(GAME).as_posix()]),
    })
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    manifest = json.loads((HUMANS / "humans.json").read_text(encoding="utf-8"))
    OUT.mkdir(parents=True, exist_ok=True)
    SHEETS.mkdir(parents=True, exist_ok=True)
    player = manifest["player"]
    reports = bake_person(player, player["lods"][0]["name"], OUT)
    (OUT / "SOURCES.json").write_text(json.dumps(reports, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    register(reports)
    if "--sample" in sys.argv:
        SAMPLE_OUT.mkdir(parents=True, exist_ok=True)
        for entry in manifest["crowd"]:
            if entry["name"] in SAMPLE:
                bake_person(entry, "Near", SAMPLE_OUT)
    for r in reports:
        print(f"{r['output']}: {r['frames']} frames x {r['columns']} azimuths, {r['bytes'] / 1e6:.2f} MB")
    print(f"sheets in {SHEETS}; runtime size x{RUNTIME_RESIZE} applies on top of humans.json's scale")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
