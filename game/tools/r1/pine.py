"""Import the CC0 adult pine and derive its Mid/Far from the same model.

Run from game with PYTHONPATH=tools: python -m r1.pine <source.glb>.
Near retains the source geometry and PBR materials. Mid keeps every needle
card and simplifies only bark with UV-aware QEM. Far bakes LOD0 directly into
the shared relightable billboard. No pre-existing tree species is overwritten.
"""
import hashlib
import json
import sys
import subprocess
from pathlib import Path

import numpy as np

from .tree_impostor import transformed_parts, bake
from .tree_lod import (Builder, GAME_ROOT, NATURE, read_glb, write_glb,
                       copy_primitive, part_glb, autolod_executable, tool_env)

SOURCE_URL = "https://terraintale.com/library/realistic-surfaces/pine_tree_tall.glb"
SOURCE_SHA256 = "8380454b66bd5b3fad4eebed694de207b87056937ecd259a5ce5220369f47bde"
ATTRIBUTION = {
    "title": "Pine Tree Tall", "author": "TerrainTale; ambientCG; ez-tree by Daniel Greenheck",
    "license": "CC0-1.0",
    "source": "https://terraintale.com/library/realistic-surfaces/pine-tree-tall",
}


def tangents(vertices, indices):
    """Give the source bark normal map a tangent frame without changing UVs."""
    p, n, uv = (vertices[:, a:b].astype(np.float64) for a, b in ((0, 3), (3, 6), (6, 8)))
    n /= np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-12)
    tri = indices.reshape(-1, 3)
    e1, e2 = p[tri[:, 1]] - p[tri[:, 0]], p[tri[:, 2]] - p[tri[:, 0]]
    u1, u2 = uv[tri[:, 1]] - uv[tri[:, 0]], uv[tri[:, 2]] - uv[tri[:, 0]]
    det = u1[:, 0] * u2[:, 1] - u1[:, 1] * u2[:, 0]
    inverse = np.zeros_like(det)
    np.divide(1, det, out=inverse, where=np.abs(det) > 1e-10)
    t = (e1 * u2[:, 1, None] - e2 * u1[:, 1, None]) * inverse[:, None]
    b = (e2 * u1[:, 0, None] - e1 * u2[:, 0, None]) * inverse[:, None]
    ts, bs = np.zeros_like(p), np.zeros_like(p)
    for column in range(3):
        np.add.at(ts, tri[:, column], t)
        np.add.at(bs, tri[:, column], b)
    ts -= n * np.sum(n * ts, axis=1)[:, None]
    lengths = np.linalg.norm(ts, axis=1)
    degenerate = lengths < 1e-8
    ts[degenerate] = np.cross(n[degenerate], np.eye(3)[np.argmin(np.abs(n[degenerate]), axis=1)])
    ts /= np.maximum(np.linalg.norm(ts, axis=1)[:, None], 1e-8)
    handedness = np.where(np.sum(np.cross(n, ts) * bs, axis=1) < 0, -1, 1)
    return np.column_stack((ts, handedness))


def build(source):
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError("Adult pine source does not match its pinned CC0 download")
    doc, binary = read_glb(source)
    parts = transformed_parts(doc, binary)
    points = np.concatenate([p[0][:, :3] for p in parts])
    low, high = points.min(0), points.max(0)
    height = float(high[1] - low[1])
    near = Builder()
    near.doc["asset"]["generator"] = "R1World pine.py"
    for vertices, indices, material, _ in parts:
        vertices = vertices.copy()
        vertices[:, :3] = (vertices[:, :3] - [0, low[1], 0]) / height
        attributes = {"POSITION": near.accessor(vertices[:, :3].tolist(), "VEC3", 5126),
                      "NORMAL": near.accessor(vertices[:, 3:6].tolist(), "VEC3", 5126),
                      "TEXCOORD_0": near.accessor(vertices[:, 6:8].tolist(), "VEC2", 5126),
                      "TANGENT": near.accessor(tangents(vertices, indices).tolist(), "VEC4", 5126)}
        # Decode the source WebP to PNG for the engine; retain its PBR factors.
        mat = near.material(doc, binary, material, 1024)
        near.mesh(doc["materials"][material]["name"], attributes,
                  near.accessor([(int(i),) for i in indices], "SCALAR", 5125), mat)
    write_glb(NATURE / "pine_tall.glb", near.doc, bytes(near.binary))
    mid = Builder()
    scratch = GAME_ROOT / "generated/pine-update/lod-quality"
    scratch.mkdir(parents=True, exist_ok=True)
    for mesh in near.doc["meshes"]:
        primitive, = mesh["primitives"]
        if near.doc["materials"][primitive["material"]].get("alphaMode") == "MASK":
            # Keep the exact LOD0 crown, including texture resolution and alpha.
            copy_primitive(mid, near.doc, bytes(near.binary), primitive, None, mesh["name"])
        else:
            part_glb(near.doc, bytes(near.binary), primitive, scratch / "bark.glb")
            command = [str(autolod_executable()), str(scratch / "bark.glb"), str(scratch / "bark-lods.glb"),
                       "--ratios", ".63", "--errors", ".01", "--uv-weight", ".05", "--split"]
            result = subprocess.run(command, check=True, env=tool_env(), capture_output=True, text=True)
            (scratch / "bark-simplification.log").write_text(result.stdout, encoding="utf-8")
            reduced, reduced_binary = read_glb(scratch / "bark-lods_LOD1.glb")
            for vertices, indices, material, _ in transformed_parts(reduced, reduced_binary):
                attributes = {"POSITION": mid.accessor(vertices[:, :3].tolist(), "VEC3", 5126),
                              "NORMAL": mid.accessor(vertices[:, 3:6].tolist(), "VEC3", 5126),
                              "TEXCOORD_0": mid.accessor(vertices[:, 6:8].tolist(), "VEC2", 5126),
                              "TANGENT": mid.accessor(tangents(vertices, indices).tolist(), "VEC4", 5126)}
                mat = mid.material(reduced, reduced_binary, material, None)
                mid.mesh(mesh["name"], attributes,
                         mid.accessor([(int(i),) for i in indices], "SCALAR", 5125), mat)
    write_glb(NATURE / "pine_tall_mid.glb", mid.doc, bytes(mid.binary))
    reports = []
    for name, builder in (("pine_tall.glb", near), ("pine_tall_mid.glb", mid)):
        reports.append({"output": name, "sourceSha256": digest, "download": SOURCE_URL,
            "attribution": ATTRIBUTION, "normalizedHeight": 1, "sourceHeight": height,
            "vertices": sum(a["count"] for m in builder.doc["meshes"] for p in m["primitives"]
                            for a in [builder.doc["accessors"][p["attributes"]["POSITION"]]]),
            "triangles": sum(builder.doc["accessors"][p["indices"]]["count"] // 3
                             for m in builder.doc["meshes"] for p in m["primitives"]),
            "derivedFrom": "pine_tall.glb" if builder is mid else source.name,
            "method": "Exact LOD0 needle geometry/materials; UV-aware QEM bark at 63% triangles, no proxy or sloppy fallback" if builder is mid
                      else "Authored geometry/PBR retained; WebP decoded to PNG; pivot normalized to one metre; tangents generated"})
    manifest_path = NATURE / "SOURCES.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    outputs = {r["output"] for r in reports}
    manifest = [r for r in manifest if r.get("output") not in outputs] + reports
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    reports.append(bake(NATURE / "pine_tall.glb", NATURE / "pine_tall_far.glb",
                        reference_level="LOD0", columns=32, alpha_cutoff=.5, resolution=192))
    provenance_path = GAME_ROOT / "assets/THIRD_PARTY_ASSETS.json"
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    entry = {"name": "Pine Tree Tall — adult conifer", **ATTRIBUTION, "download": SOURCE_URL,
             "sourceSha256": digest,
             "modified": "Normalized LOD0; exact needle crown in Mid with UV-aware bark reduction; 160-view camera-facing Far baked directly from LOD0 with 50% coverage cutoff, 192px per view",
             "files": ["assets/models/external/nature_selected/" + name for name in (
                 "pine_tall.glb", "pine_tall_mid.glb", "pine_tall_far.glb", "pine_tall_far.source.json",
                 "pine_tall_far_albedo.png", "pine_tall_far_normal.png")] + ["assets/licenses/pine_tall-CC0.txt"]}
    provenance["assets"] = [e for e in provenance["assets"] if e.get("name") != entry["name"]] + [entry]
    provenance_path.write_text(json.dumps(provenance, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (GAME_ROOT / "assets/licenses/pine_tall-CC0.txt").write_text(
        "Pine Tree Tall, published by TerrainTale under CC0 1.0 Universal.\n"
        "Credits: TerrainTale; ambientCG; trees grown with ez-tree by Daniel Greenheck (MIT).\n"
        f"Model and license declaration: {ATTRIBUTION['source']}\n"
        "License: https://creativecommons.org/publicdomain/zero/1.0/\n"
        f"Download: {SOURCE_URL}\nSHA-256: {digest}\n"
        "Modified for R1World: normalized height/pivot, tangents and derived LODs.\n", encoding="utf-8")
    print(json.dumps(reports, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    build(Path(sys.argv[1]))
