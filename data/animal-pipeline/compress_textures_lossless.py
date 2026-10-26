"""Repack PNGs without reducing dimensions or changing shader-used channels."""
import hashlib
import io
import json
from pathlib import Path
from PIL import Image, ImageFile
from glb_tools import read, write

ImageFile.MAXBLOCK = max(ImageFile.MAXBLOCK, 4 * 1024 * 1024)
ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'game/assets/animals'
CACHE = {}


def compress(path):
    doc, binary = read(path)
    alpha = set()
    for material in doc.get('materials', []):
        color = material.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        if color and material.get('alphaMode', 'OPAQUE') != 'OPAQUE':
            alpha.add(doc['textures'][color['index']]['source'])
    by_view = {}
    for i, record in enumerate(doc.get('images', [])):
        if record['mimeType'] == 'image/png':
            by_view.setdefault(record['bufferView'], []).append(i)
    replacements, changes = {}, []
    for index, image_ids in by_view.items():
        view = doc['bufferViews'][index]
        raw = binary[view.get('byteOffset', 0):view.get('byteOffset', 0) + view['byteLength']]
        keep_alpha = any(i in alpha for i in image_ids)
        key = (hashlib.sha256(raw).digest(), keep_alpha)
        if key not in CACHE:
            image = Image.open(io.BytesIO(raw)).convert('RGBA')
            mode = 'RGBA' if keep_alpha and image.getchannel('A').getextrema() != (255, 255) else 'RGB'
            packed = image.convert(mode)
            buf = io.BytesIO()
            packed.save(buf, format='PNG', optimize=True, compress_level=9)
            result = buf.getvalue()
            # Compare all shader-used bytes; alpha is irrelevant on normal maps,
            # packed data maps and opaque materials.
            decoded = Image.open(io.BytesIO(result)).convert('RGBA' if keep_alpha else 'RGB')
            assert decoded.size == image.size
            assert decoded.tobytes() == image.convert(decoded.mode).tobytes()
            CACHE[key] = (result if len(result) < len(raw) else raw, image.size, mode)
        result, size, mode = CACHE[key]
        if len(result) < len(raw):
            replacements[index] = result
            changes.append(dict(images=image_ids, before=len(raw), after=len(result),
                                dimensions=list(size), mode=mode, used_pixels_identical=True))
    if sum(c['before']-c['after'] for c in changes) < 256:
        return None
    before = path.stat().st_size
    mutable = bytearray()
    for i, view in enumerate(doc['bufferViews']):
        raw = replacements.get(i, binary[view.get('byteOffset', 0):view.get('byteOffset', 0) + view['byteLength']])
        mutable.extend(b'\0' * ((-len(mutable)) % 4))
        view['byteOffset'] = len(mutable)
        view['byteLength'] = len(raw)
        mutable.extend(raw)
    doc['asset'].setdefault('extras', {})['r1_lossless_png_revision'] = 1
    write(path, doc, bytes(mutable))
    assert path.stat().st_size < before
    return dict(file=path.name, before=before, after=path.stat().st_size, images=changes)


if __name__ == '__main__':
    reports = []
    for path in sorted(OUT.glob('*.glb')):
        result = compress(path)
        if result:
            reports.append(result)
    (ROOT / 'data/animal-pipeline/review/lossless-textures.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
    print(json.dumps(dict(files=len(reports), saved_MiB=sum(r['before']-r['after'] for r in reports)/1048576), indent=2))
