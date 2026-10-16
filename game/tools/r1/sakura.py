"""Import the user-selected Innerscene cherry, retaining authored Near geometry.

Run from game: python -m r1.sakura <download.glb> (PYTHONPATH=tools).
Requires numpy, Pillow and moderngl, like tree_impostor. Mid keeps whole
flowers/leaves selected by the existing multi-view coverage method. Far is
the same relightable 40-view impostor used by the current urban tree.
"""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

from . import decimate as D
from .tree_lod import Builder, Envelope, NATURE, read_glb, write_glb, covering
from .tree_impostor import transformed_parts, bake

SOURCE_SHA256 = "2412baf8c41da00801512b56d1dbcba6025f983e2470cdd7a2c3f9703ba91b12"
ATTRIBUTION = {
    "title": "Flowering Cherry Tree", "author": "Innerscene", "license": "CC0-1.0",
    "source": "https://www.innerscene.com/tools/library/3d-parts/flowering-cherry-tree-6d80049e",
}


def build(source):
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError("Source differs from the cherry selected by the user")
    doc, binary = read_glb(source)
    parts = transformed_parts(doc, binary)
    points = np.concatenate([p[0][:, :3] for p in parts])
    low, high = points.min(0), points.max(0)
    height = float(high[1] - low[1])
    # Author's trunk is centred at x=z=0; retain that planting pivot.
    for vertices, _, _, _ in parts:
        vertices[:, :3] -= np.array([0, low[1], 0])
        vertices[:, :3] /= height
    reports = []
    for mid in (False, True):
        builder = Builder()
        builder.doc["asset"]["generator"] = "R1World sakura.py"
        triangles = vertices_count = 0
        for material_index, material in enumerate(doc["materials"]):
            chunks, indices, offset = [], [], 0
            for vertices, index, mat, _ in parts:
                if mat != material_index:
                    continue
                if mid and mat >= 2:
                    # Authored petals have split normals/UVs; connectivity for
                    # selection uses coincident positions, without welding shading.
                    positions, links = np.unique(vertices[:, :3].round(7), axis=0, return_inverse=True)
                    clusters = D._cluster_triangles(links[index].tolist(), len(positions))
                    kept = covering(clusters, index.tolist(), vertices[:, :3].tolist(), 3000 if mat < 5 else 800)
                    # Compensate for removed internal flowers, within the original
                    # canopy envelope, as for the existing urban-tree leaf cards.
                    envelope = Envelope(vertices[:, :3].tolist())
                    original = vertices[:, :3].copy()
                    vertices = vertices.copy()
                    for cluster in kept:
                        mine = sorted({int(index[base + k]) for base in cluster for k in range(3)})
                        centre = original[mine].mean(0)
                        lo, hi = 1., 1.8
                        for _ in range(7):
                            grow = (lo + hi) / 2
                            points = centre + (original[mine] - centre) * grow
                            if all(envelope.inside(p) for p in points):
                                lo = grow
                            else:
                                hi = grow
                        vertices[mine, :3] = centre + (original[mine] - centre) * lo
                    chosen = sorted(base for cluster in kept for base in cluster)
                    index = np.asarray([index[base + k] for base in chosen for k in range(3)], dtype=np.uint32)
                used, inverse = np.unique(index, return_inverse=True)
                chunks.append(vertices[used])
                indices.extend((inverse + offset).tolist())
                offset += len(used)
            if not chunks:
                continue
            v = np.concatenate(chunks)
            attributes = {"POSITION": builder.accessor(v[:, :3].tolist(), "VEC3", 5126),
                          "NORMAL": builder.accessor(v[:, 3:6].tolist(), "VEC3", 5126),
                          "TEXCOORD_0": builder.accessor(v[:, 6:8].tolist(), "VEC2", 5126)}
            mat = builder.material(doc, binary, material_index, 512 if mid else None)
            builder.mesh(material["name"], attributes,
                         builder.accessor([(i,) for i in indices], "SCALAR", 5125), mat)
            triangles += len(indices) // 3
            vertices_count += len(v)
        name = "sakura_mid.glb" if mid else "sakura.glb"
        write_glb(NATURE / name, builder.doc, bytes(builder.binary))
        reports.append(dict(output=name, sourceSha256=digest, attribution=ATTRIBUTION,
                            normalizedHeight=1, sourceHeight=height, vertices=vertices_count,
                            triangles=triangles, meshes=len(builder.doc["meshes"]),
                            method="Whole flowers/leaves selected by multi-view coverage, grown within source envelope; bark retained" if mid
                            else "Authored geometry/textures retained; transforms baked; six materials merged"))
    manifest_path = NATURE / "SOURCES.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf8"))
    outputs = {r["output"] for r in reports}
    manifest = [r for r in manifest if r.get("output") not in outputs] + reports
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf8")
    reports.append(bake(NATURE / "sakura_mid.glb", NATURE / "sakura_far.glb"))
    print(json.dumps(reports, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    build(Path(sys.argv[1]))
