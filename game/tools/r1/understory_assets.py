"""Reproducible CC0 photographed understory -> depth-sliced, relightable meshes.

Run from game/tools: python -m r1.understory_assets --download --bake.
Only the authoring bake needs ModernGL; runtime assets are ordinary shared glTF.
Near uses 176 triangles, Mid 44: three radial sets of three slabs plus two
horizontal slabs, with independently photographed front/back surfaces. Source
leaf normals and hemisphere visibility survive in the normal atlas. No sun is
baked into the albedo, and no full source mesh is shipped.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
from pathlib import Path
import urllib.request

import numpy as np
from PIL import Image

from .tree_lod import Builder, read_glb, write_glb
from .tree_impostor import (VERTEX, FRAGMENT, transformed_parts, dilate,
                            encode_color, png, sphere_directions)

ROOT = Path(__file__).resolve().parents[3]
OUTPUT = ROOT / 'game/assets/models/external/understory'
CACHE = ROOT / 'game/generated/source-assets/understory'
LOCK = Path(__file__).with_name('understory_sources.json')
SPECIES = ('shrub_01', 'shrub_02', 'shrub_03', 'shrub_04',
           'nettle_plant', 'fern_02', 'grass_medium_02')
RESOLUTION, GUTTER, COLS, ROWS = 192, 8, 6, 4


def fetch(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'R1World asset authoring/1.0 (CC0 plants)'})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read()


def download():
    """Existing lock pins every byte; only first acquisition consults the API."""
    lock = json.loads(LOCK.read_text('utf8')) if LOCK.exists() else {}
    for name in SPECIES:
        directory = CACHE / name
        directory.mkdir(parents=True, exist_ok=True)
        if name not in lock:
            files = json.loads(fetch(f'https://api.polyhaven.com/files/{name}'))['gltf']['1k']['gltf']
            info = json.loads(fetch(f'https://api.polyhaven.com/info/{name}'))
            entries = {f'{name}.gltf': {k: v for k, v in files.items() if k != 'include'}}
            entries.update(files['include'])
            lock[name] = {'source': f'https://polyhaven.com/a/{name}',
                          'license': 'CC0-1.0', 'licenseUrl': 'https://polyhaven.com/license',
                          'authors': info.get('authors', {}), 'files': entries}
        # Poly Haven's glTF JPEG bundle omits alpha despite MASK materials.
        # Fetch the separate original mask rather than shipping opaque cards.
        if 'textures/alpha.png' not in lock[name]['files']:
            files = json.loads(fetch(f'https://api.polyhaven.com/files/{name}'))
            lock[name]['files']['textures/alpha.png'] = files['Alpha']['1k']['png']
        for path, entry in lock[name]['files'].items():
            target = directory / path
            payload = target.read_bytes() if target.exists() else fetch(entry['url'])
            if hashlib.md5(payload).hexdigest() != entry['md5']:
                raise ValueError(f'Source digest mismatch: {name}/{path}')
            digest = hashlib.sha256(payload).hexdigest()
            if entry.get('sha256', digest) != digest:
                raise ValueError(f'Pinned source changed: {name}/{path}')
            entry['sha256'] = digest
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
        LOCK.write_text(json.dumps(lock, indent=2, ensure_ascii=False) + '\n', encoding='utf8')
        print(f'Downloaded and verified {name}', flush=True)


def source(name):
    directory = CACHE / name
    doc = json.loads((directory / f'{name}.gltf').read_text('utf8'))
    binary = bytearray()
    offsets = []
    for buffer in doc['buffers']:
        offsets.append(len(binary))
        binary.extend((directory / buffer['uri']).read_bytes())
        binary.extend(b'\0' * (-len(binary) % 4))
    for view in doc['bufferViews']:
        view['byteOffset'] = view.get('byteOffset', 0) + offsets[view.get('buffer', 0)]
        view['buffer'] = 0
    for image in doc.get('images', []):
        if 'uri' in image:
            data = (directory / image.pop('uri')).read_bytes()
            image['bufferView'] = len(doc['bufferViews'])
            doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(data)})
            binary.extend(data)
            binary.extend(b'\0' * (-len(binary) % 4))
    return doc, bytes(binary)


def slab_frames():
    for axis in range(3):
        yaw = axis * math.pi / 3
        direction = np.array([math.sin(yaw), 0, math.cos(yaw)], dtype='f4')
        right = np.array([math.cos(yaw), 0, -math.sin(yaw)], dtype='f4')
        for slab in range(3):
            for face in (1, -1):
                yield direction * face, right * face, (slab + .5) / 3, 1 / 3, direction
    for slab in range(2):
        for face in (1, -1):
            yield np.array([0, face, 0], dtype='f4'), np.array([1, 0, 0], dtype='f4'), (slab + .5) / 2, .5, np.array([0, 1, 0], dtype='f4')


def prepared_source(name):
    doc, binary = source(name)
    # Most downloads present several variants in a row. Select an individual
    # plant; the two welded presentation rows also need a whole-triangle crop.
    doc['scenes'][doc.get('scene', 0)]['nodes'] = [doc['scenes'][doc.get('scene', 0)]['nodes'][0]]
    parts = transformed_parts(doc, binary)
    if name in ('shrub_01', 'shrub_04'):
        threshold = doc['nodes'][0].get('translation', [0, 0, 0])[0] - (.15 if name == 'shrub_01' else .10)
        cropped = []
        for vertices, indices, material, tangents in parts:
            triangles = indices.reshape(-1, 3)
            triangles = triangles[np.all(vertices[triangles, 0] > threshold, axis=1)]
            used, inverse = np.unique(triangles, return_inverse=True)
            cropped.append((vertices[used], inverse.astype('u4'), material, tangents))
        parts = cropped
    points = np.concatenate([part[0][:, :3] for part in parts])
    low, high = points.min(axis=0), points.max(axis=0)
    height = float(high[1] - low[1])
    center = (low + high) / 2
    # All exported plants are one metre high, with their measured proportions.
    for vertices, _, _, _ in parts:
        vertices[:, :3] = (vertices[:, :3] - [center[0], low[1], center[2]]) / height
    # Build compact asymmetric clumps from photographed plants, not from
    # repeated billboard copies. The bake keeps their intersecting leaf depth.
    count = 1 if name == 'fern_02' else 3 if name in ('shrub_02', 'shrub_03') else 5
    originals, parts = parts, []
    for copy in range(count):
        angle = copy * 2.399963229728653
        c, s = math.cos(angle), math.sin(angle)
        rotation = np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype='f4')
        scale = 1.0 if copy == 0 else .73 + .18 * ((copy * .6180339) % 1)
        offset = np.array([math.cos(angle) * .24, 0, math.sin(angle) * .24]) if copy else np.zeros(3)
        for vertices, indices, material, tangents in originals:
            v = vertices.copy()
            v[:, :3] = vertices[:, :3] @ rotation.T * scale + offset
            v[:, 3:6] = vertices[:, 3:6] @ rotation.T
            v[:, 8:11] = vertices[:, 8:11] @ rotation.T
            parts.append((v, indices, material, tangents))
    points = np.concatenate([p[0][:, :3] for p in parts])
    low, high = points.min(axis=0), points.max(axis=0)
    center = (low + high) / 2
    # glTF often omits tangents; reconstruct them before baking leaf normal maps.
    tangent_parts = []
    for v, indices, material, authored in parts:
        if not authored:
            tri = indices.reshape(-1, 3)
            p = v[tri, :3]
            uv = v[tri, 6:8]
            du, dv = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
            determinant = du[:, 0] * dv[:, 1] - du[:, 1] * dv[:, 0]
            reciprocal = np.where(abs(determinant) > 1e-10, 1 / np.where(abs(determinant) > 1e-10, determinant, 1), 0)
            tangent = ((p[:, 1] - p[:, 0]) * dv[:, 1, None] - (p[:, 2] - p[:, 0]) * du[:, 1, None]) * reciprocal[:, None]
            bitangent = ((p[:, 2] - p[:, 0]) * du[:, 0, None] - (p[:, 1] - p[:, 0]) * dv[:, 0, None]) * reciprocal[:, None]
            t, b = np.zeros_like(v[:, :3]), np.zeros_like(v[:, :3])
            for corner in range(3):
                np.add.at(t, tri[:, corner], tangent)
                np.add.at(b, tri[:, corner], bitangent)
            t -= v[:, 3:6] * np.sum(t * v[:, 3:6], axis=1)[:, None]
            t /= np.maximum(np.linalg.norm(t, axis=1, keepdims=True), 1e-8)
            v[:, 8:11] = t
            v[:, 11] = np.where(np.sum(np.cross(v[:, 3:6], t) * b, axis=1) < 0, -1, 1)
        tangent_parts.append((v, indices, material, True))
    return doc, binary, tangent_parts, height, low, high, center, count


def bake(name):
    import moderngl
    doc, binary, parts, height, low, high, center, count = prepared_source(name)
    ctx = moderngl.create_standalone_context(require=330)
    # Clip depth slabs in source space, before any colour/normal bake.
    fragment = FRAGMENT.replace('void main() {',
        'uniform vec3 slabAxis; uniform vec2 slabRange;\nvoid main() {\n'
        ' float d=dot(surfacePosition,slabAxis); if(d<slabRange.x||d>slabRange.y) discard;')
    program = ctx.program(vertex_shader=VERTEX, fragment_shader=fragment)
    program['albedo'].value = 0
    program['normalMap'].value = 1
    textures = {}
    white = ctx.texture((1, 1), 4, bytes([255] * 4))
    neutral = ctx.texture((1, 1), 4, bytes([128, 128, 255, 255]))

    def texture(ref, default):
        if not ref:
            return default
        index = ref['index']
        if index not in textures:
            image = doc['images'][doc['textures'][index]['source']]
            view = doc['bufferViews'][image['bufferView']]
            offset = view.get('byteOffset', 0)
            im = Image.open(io.BytesIO(binary[offset:offset + view['byteLength']])).convert('RGBA')
            if 'diff' in image.get('name', ''):
                mask = Image.open(CACHE / name / 'textures/alpha.png').convert('L').resize(im.size)
                im.putalpha(mask)
            textures[index] = ctx.texture(im.size, 4, im.tobytes())
            textures[index].filter = (moderngl.LINEAR, moderngl.LINEAR)
        return textures[index]

    draws = []
    for vertices, indices, index, tangents in parts:
        mat = doc['materials'][index]
        pbr = mat.get('pbrMetallicRoughness', {})
        vao = ctx.vertex_array(program, [(ctx.buffer(vertices.astype('f4').tobytes()),
            '3f 3f 2f 4f', 'position', 'normal', 'uv', 'tangent')], ctx.buffer(indices.tobytes()))
        draws.append((vao, texture(pbr.get('baseColorTexture'), white), texture(mat.get('normalTexture'), neutral),
                      pbr.get('baseColorFactor', [1] * 4), mat.get('alphaCutoff', .5) if mat.get('alphaMode') == 'MASK' else 0,
                      tangents and 'normalTexture' in mat, mat.get('normalTexture', {}).get('scale', 1)))
    size = RESOLUTION * 2
    attachments = [ctx.texture((size, size), 4, dtype='f4') for _ in range(3)]
    fbo = ctx.framebuffer(attachments, ctx.depth_texture((size, size)))
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.disable(moderngl.CULL_FACE)
    atlas = np.zeros((ROWS * RESOLUTION, COLS * RESOLUTION, 4), dtype='f4')
    normals = np.zeros_like(atlas)
    planes = []
    corners = np.array([[x, y, z] for x in (low[0], high[0]) for y in (low[1], high[1]) for z in (low[2], high[2])])
    program['slabRange'].value = (-1000, 1000)

    def draw(projection):
        program['projection'].write(projection.T.astype('f4').tobytes())
        fbo.use()
        fbo.clear(0, 0, 0, 0, depth=1)
        for vao, color, normal, factor, cutoff, mapped, scale in draws:
            color.use(0)
            normal.use(1)
            program['color'].value = tuple(factor)
            program['cutoff'].value = cutoff
            program['mappedNormal'].value = mapped
            program['normalScale'].value = scale
            vao.render()

    # Static hemispherical visibility of the full source, independent of sun.
    shadows = []
    radius = float(np.linalg.norm(corners - center, axis=1).max()) * 1.03
    for direction in sphere_directions(16):
        right = np.array([direction[2], 0, -direction[0]], dtype='f4')
        right /= np.linalg.norm(right)
        up = np.cross(direction, right)
        projection = np.eye(4, dtype='f4')
        projection[:3, :3] = np.stack((right, up, -direction)) / radius
        projection[:3, 3] = -projection[:3, :3] @ center
        program['slabAxis'].value = (0, 1, 0)
        draw(projection)
        depth = np.frombuffer(fbo.depth_attachment.read(), dtype='f4').reshape(size, size)
        shadows.append((direction, projection, depth))
    for cell, (direction, right, fraction, thickness, axis) in enumerate(slab_frames()):
        up = np.cross(direction, right)
        depth_min, depth_max = float((corners @ axis).min()), float((corners @ axis).max())
        depth = depth_min + fraction * (depth_max - depth_min)
        half = thickness * (depth_max - depth_min) / 2
        plane_center = center + axis * (depth - float(center @ axis))
        width = float(np.ptp(corners @ right)) * 1.04
        tall = float(np.ptp(corners @ up)) * 1.04
        # A gutter keeps adjacent slices isolated at useful mip levels.
        expansion = RESOLUTION / (RESOLUTION - 2 * GUTTER)
        width *= expansion
        tall *= expansion
        frame = np.stack((right, up, -direction))
        projection = np.eye(4, dtype='f4')
        projection[:3, :3] = frame * np.array([2 / width, 2 / tall, 1 / radius])[:, None]
        projection[:3, 3] = -projection[:3, :3] @ plane_center
        program['slabAxis'].value = tuple(float(v) for v in axis)
        program['slabRange'].value = (depth - half - 1e-5, depth + half + 1e-5)
        draw(projection)

        def reduced(attachment):
            data = np.frombuffer(fbo.read(components=4, attachment=attachment, dtype='f4'), dtype='f4')
            return data.reshape(size, size, 4)[::-1].reshape(RESOLUTION, 2, RESOLUTION, 2, 4).mean(axis=(1, 3))

        color = reduced(0)
        alpha = color[:, :, 3]
        rgb = color[:, :, :3] / np.maximum(alpha[:, :, None], 1e-8)
        normal = reduced(1)[:, :, :3]
        normal /= np.maximum(np.linalg.norm(normal, axis=2, keepdims=True), 1e-8)
        position = reduced(2)[:, :, :3] / np.maximum(alpha[:, :, None], 1e-8)
        visibility = np.zeros_like(alpha)
        total = np.zeros_like(alpha)
        for d, proj, zbuffer in shadows:
            weight = np.maximum(normal @ d, 0)
            q = position @ proj[:3, :3].T + proj[:3, 3]
            uv = np.clip(((q[:, :, :2] * .5 + .5) * size).astype('i4'), 0, size - 1)
            visible = q[:, :, 2] * .5 + .5 <= zbuffer[uv[:, :, 1], uv[:, :, 0]] + .004
            visibility += weight * visible
            total += weight
        visibility = visibility / np.maximum(total, 1e-8)
        y, x = divmod(cell, COLS)
        region = np.s_[y * RESOLUTION:(y + 1) * RESOLUTION, x * RESOLUTION:(x + 1) * RESOLUTION]
        atlas[region] = np.concatenate((dilate(encode_color(rgb), alpha), alpha[:, :, None]), axis=2)
        normals[region] = np.concatenate((dilate(normal * .5 + .5, alpha), dilate(visibility[:, :, None], alpha)), axis=2)
        planes.append((plane_center, right, up, width, tall, cell))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for suffix, data in (('albedo', atlas), ('normal', normals)):
        (OUTPUT / f'{name}_{suffix}.png').write_bytes(png(data))
    for level, divisions in (('near', 2), ('mid', 1)):
        builder = Builder()
        builder.doc['asset']['generator'] = 'R1World depth-sliced photographed understory'
        for suffix in ('albedo', 'normal'):
            builder.doc['images'].append({'uri': f'{name}_{suffix}.png'})
            builder.doc['textures'].append({'source': len(builder.doc['images']) - 1})
        builder.doc['materials'].append({'name': f'{name} relightable foliage', 'doubleSided': False,
            'alphaMode': 'MASK', 'alphaCutoff': .32, 'pbrMetallicRoughness': {
                'baseColorTexture': {'index': 0}, 'metallicFactor': 0, 'roughnessFactor': .9},
            'normalTexture': {'index': 1}, 'extras': {'SAIDA_foliage': {
                'wrap': .35, 'transmission': .16, 'wind': .035, 'normalSpace': 'object'}}})
        vertices, uv, indices, face_normals, tangents = [], [], [], [], []
        for origin, right, up, width, tall, cell in planes:
            base = len(vertices)
            cy, cx = divmod(cell, COLS)
            for j in range(divisions + 1):
                for i in range(divisions + 1):
                    u, v = i / divisions, j / divisions
                    vertices.append(tuple(float(c) for c in origin + right * (u - .5) * width + up * (v - .5) * tall))
                    uv.append(((cx + u) / COLS, (cy + 1 - v) / ROWS))
                    face_normals.append(tuple(float(c) for c in np.cross(right, up)))
                    tangents.append(tuple(float(c) for c in right) + (1.,))
            for j in range(divisions):
                for i in range(divisions):
                    a = base + j * (divisions + 1) + i
                    indices.extend((a, a + 1, a + divisions + 2, a, a + divisions + 2, a + divisions + 1))
        builder.mesh(name, {'POSITION': builder.accessor(vertices, 'VEC3', 5126),
            'NORMAL': builder.accessor(face_normals, 'VEC3', 5126),
            'TANGENT': builder.accessor(tangents, 'VEC4', 5126),
            'TEXCOORD_0': builder.accessor(uv, 'VEC2', 5126)},
            builder.accessor([(i,) for i in indices], 'SCALAR', 5123), 0)
        if len(indices) // 3 > 200:
            raise ValueError('Bush triangle budget exceeded')
        write_glb(OUTPUT / f'{name}_{level}.glb', builder.doc, bytes(builder.binary))
    lock = json.loads(LOCK.read_text('utf8'))[name]
    report = {'name': name, 'source': lock['source'], 'authors': lock['authors'], 'license': lock['license'],
              'sourceFiles': {path: entry['sha256'] for path, entry in lock['files'].items()},
              'sourceTriangles': sum(len(part[1]) // 3 for part in parts),
              'triangles': {'near': 176, 'mid': 44}, 'heightMetres': 1,
              'originalHeightMetres': height, 'extent': [float(x) for x in high - low],
              'sourcePlantsPerClump': count,
              'atlas': [COLS * RESOLUTION, ROWS * RESOLUTION], 'slabs': len(planes),
              'method': '22 directional depth slabs, object-space leaf normals, 16-direction static visibility; no baked sunlight'}
    (OUTPUT / f'{name}.source.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf8')
    ctx.release()
    print(json.dumps(report, ensure_ascii=False), flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--download', action='store_true')
    parser.add_argument('--bake', action='store_true')
    parser.add_argument('--species', choices=SPECIES, nargs='+', default=SPECIES)
    args = parser.parse_args()
    if args.download:
        download()
    if args.bake:
        for name in args.species:
            bake(name)
        if 'grass_medium_02' in args.species:
            dry_grass()


def dry_grass():
    """Senescent leaves from the same scan, preserving veins and coverage."""
    image = np.asarray(Image.open(OUTPUT / 'grass_medium_02_albedo.png').convert('RGBA'), dtype='f4') / 255
    rgb = image[:, :, :3]
    linear = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    luminance = linear @ np.array([.2126, .7152, .0722])
    straw = luminance[:, :, None] * np.array([1.38, .98, .52])
    image[:, :, :3] = encode_color(linear * .12 + straw * .88)
    (OUTPUT / 'grass_dry_albedo.png').write_bytes(png(image))
    for level in ('near', 'mid'):
        doc, binary = read_glb(OUTPUT / f'grass_medium_02_{level}.glb')
        doc['images'][0]['uri'] = 'grass_dry_albedo.png'
        doc['materials'][0]['name'] = 'senescent photographed verge grass'
        write_glb(OUTPUT / f'grass_dry_{level}.glb', doc, binary)
    report = json.loads((OUTPUT / 'grass_medium_02.source.json').read_text('utf8'))
    report.update(name='grass_dry', derivedFrom='grass_medium_02',
                  modification='luminance-preserving senescent leaf recolour; source veins, normal detail and alpha unchanged')
    (OUTPUT / 'grass_dry.source.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf8')


if __name__ == '__main__':
    main()
