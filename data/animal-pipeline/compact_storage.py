"""Core-glTF storage compaction, without changing rendered geometry or motion.

Retain the complete skeleton. Remove animation channels only on nodes which
cannot influence any weighted joint, and deduplicate identical storage records.
No resampling, quantization, mesh simplification or texture change occurs here.
"""
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
from glb_tools import read, write, accessor

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'game/assets/animals'
QA = ROOT / 'data/animal-pipeline/review'


def visible_nodes(doc, binary):
    used = set()
    parents = {c: i for i, n in enumerate(doc['nodes']) for c in n.get('children', [])}
    for node in doc['nodes']:
        if 'mesh' not in node:
            continue
        if 'skin' not in node:
            used.add(doc['nodes'].index(node))
            continue
        skin = doc['skins'][node['skin']]
        for primitive in doc['meshes'][node['mesh']]['primitives']:
            attrs = primitive['attributes']
            assert 'JOINTS_1' not in attrs and 'WEIGHTS_1' not in attrs
            joints = accessor(doc, binary, attrs['JOINTS_0']).astype(int)
            weights = accessor(doc, binary, attrs['WEIGHTS_0'])
            used.update(skin['joints'][int(j)] for j in np.unique(joints[weights > 0]))
    for node in list(used):
        while node in parents:
            node = parents[node]
            used.add(node)
    return used


def duration(doc, binary, clip):
    return max(float(accessor(doc, binary, s['input']).max()) for s in clip['samplers'])


def signature(doc, binary, visible):
    """Exact attribute/channel equality, including topology, weights and UVs."""
    cache = {}
    def value(index):
        if index not in cache:
            a = accessor(doc, binary, index)
            cache[index] = (a.shape, str(a.dtype), hashlib.sha256(a.tobytes()).hexdigest())
        return cache[index]
    meshes = []
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            meshes.append((p.get('mode', 4), p.get('material'),
                           sorted((k, value(v)) for k, v in p['attributes'].items()),
                           value(p['indices']),
                           [sorted((k, value(v)) for k, v in t.items()) for t in p.get('targets', [])]))
    clips = []
    for clip in doc.get('animations', []):
        channels = []
        for c in clip['channels']:
            target = c['target']
            if target['node'] not in visible and target['path'] != 'weights':
                continue
            s = clip['samplers'][c['sampler']]
            channels.append((target['node'], target['path'], s.get('interpolation', 'LINEAR'),
                             value(s['input']), value(s['output'])))
        clips.append((clip.get('name'), duration(doc, binary, clip), sorted(channels)))
    return (copy.deepcopy(doc['nodes']), meshes,
            [(s['joints'], value(s['inverseBindMatrices'])) for s in doc['skins']], clips)


def remap_accessors(doc, mapping):
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            p['attributes'] = {k: mapping[v] for k, v in p['attributes'].items()}
            p['indices'] = mapping[p['indices']]
            for target in p.get('targets', []):
                for k in target:
                    target[k] = mapping[target[k]]
    for skin in doc['skins']:
        skin['inverseBindMatrices'] = mapping[skin['inverseBindMatrices']]
    for clip in doc.get('animations', []):
        for sampler in clip['samplers']:
            for key in ['input', 'output']:
                sampler[key] = mapping[sampler[key]]


def compact(path):
    doc, binary = read(path)
    if doc['asset'].get('extras', {}).get('r1_storage_revision') == 1:
        return None
    before = path.stat().st_size
    visible = visible_nodes(doc, binary)
    expected = signature(doc, binary, visible)
    dropped = 0
    for clip in doc.get('animations', []):
        original = clip['channels']
        channels = [c for c in original if c['target']['node'] in visible or c['target']['path'] == 'weights']
        # Preserve names and duration even for a static helper-only pose clip.
        last = max(original, key=lambda c: float(accessor(doc, binary, clip['samplers'][c['sampler']]['input']).max()))
        end = float(accessor(doc, binary, clip['samplers'][last['sampler']]['input']).max())
        if not channels or max(float(accessor(doc, binary, clip['samplers'][c['sampler']]['input']).max()) for c in channels) < end:
            if last not in channels:
                channels.append(last)
        dropped += len(original) - len(channels)
        indices = sorted({c['sampler'] for c in channels})
        mapping = {old: new for new, old in enumerate(indices)}
        clip['samplers'] = [clip['samplers'][i] for i in indices]
        for c in channels:
            c['sampler'] = mapping[c['sampler']]
        clip['channels'] = channels

    used = set()
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            used.update(p['attributes'].values())
            used.add(p['indices'])
            for target in p.get('targets', []):
                used.update(target.values())
    used.update(s['inverseBindMatrices'] for s in doc['skins'])
    used.update(s[k] for a in doc.get('animations', []) for s in a['samplers'] for k in ['input', 'output'])
    indices = sorted(used)
    remap_accessors(doc, {old: new for new, old in enumerate(indices)})
    doc['accessors'] = [doc['accessors'][i] for i in indices]

    used_views = sorted({a['bufferView'] for a in doc['accessors']} | {im['bufferView'] for im in doc.get('images', [])})
    views, mutable, mapping, seen = [], bytearray(), {}, {}
    duplicate_views = 0
    for old in used_views:
        view = doc['bufferViews'][old]
        raw = binary[view.get('byteOffset', 0):view.get('byteOffset', 0) + view['byteLength']]
        meta = {k: v for k, v in view.items() if k not in {'byteOffset', 'name'}}
        key = (json.dumps(meta, sort_keys=True), raw)
        if key in seen:
            mapping[old] = seen[key]
            duplicate_views += 1
            continue
        mapping[old] = seen[key] = len(views)
        new = view.copy()
        mutable.extend(b'\0' * ((-len(mutable)) % 4))
        new['byteOffset'] = len(mutable)
        mutable.extend(raw)
        views.append(new)
    doc['bufferViews'] = views
    for a in doc['accessors']:
        a['bufferView'] = mapping[a['bufferView']]
    for im in doc.get('images', []):
        im['bufferView'] = mapping[im['bufferView']]

    accessors, mapping, seen = [], {}, {}
    for old, a in enumerate(doc['accessors']):
        key = json.dumps({k: v for k, v in a.items() if k != 'name'}, sort_keys=True)
        if key not in seen:
            seen[key] = len(accessors)
            accessors.append(a)
        mapping[old] = seen[key]
    duplicate_accessors = len(doc['accessors']) - len(accessors)
    remap_accessors(doc, mapping)
    doc['accessors'] = accessors
    duplicate_samplers = 0
    for clip in doc.get('animations', []):
        samplers, mapping, seen = [], {}, {}
        for old, sampler in enumerate(clip['samplers']):
            key = json.dumps(sampler, sort_keys=True)
            if key not in seen:
                seen[key] = len(samplers)
                samplers.append(sampler)
            mapping[old] = seen[key]
        duplicate_samplers += len(clip['samplers']) - len(samplers)
        clip['samplers'] = samplers
        for c in clip['channels']:
            c['sampler'] = mapping[c['sampler']]

    assert signature(doc, bytes(mutable), visible) == expected, path.name
    doc['asset'].setdefault('extras', {})['r1_storage_revision'] = 1
    write(path, doc, bytes(mutable))
    assert path.stat().st_size <= before + 64, path.name
    return dict(file=path.name, before=before, after=path.stat().st_size,
                removed_nonvisual_channels=dropped, duplicate_views=duplicate_views,
                duplicate_accessors=duplicate_accessors, duplicate_samplers=duplicate_samplers,
                exact_visible_motion_and_geometry=True)


if __name__ == '__main__':
    reports = []
    for path in sorted(OUT.glob('*.glb')):
        result = compact(path)
        if result:
            reports.append(result)
    (QA / 'storage-compaction.json').write_text(json.dumps(reports, indent=2), encoding='utf-8')
    print(json.dumps({'files': len(reports), 'before': sum(r['before'] for r in reports),
                      'after': sum(r['after'] for r in reports),
                      'saved_MiB': sum(r['before'] - r['after'] for r in reports) / 1048576}, indent=2))
