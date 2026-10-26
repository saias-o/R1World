"""Download only 0 A.D. fauna dependencies; no game/runtime changes."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import time
import xml.etree.ElementTree as ET

import requests

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'data/source-assets/animals/0ad'
API = 'https://api.github.com/repos/0ad/0ad'
existing_manifest=OUT/'manifest.json'
REV = json.loads(existing_manifest.read_text())['revision'] if existing_manifest.exists() else requests.get(API + '/commits/master', timeout=40).json()['sha']
BASE = f'https://raw.githubusercontent.com/0ad/0ad/{REV}/binaries/data/mods/public/art/'
OUT.mkdir(parents=True, exist_ok=True)


def fetch(path):
    dest = OUT / path
    if dest.exists():
        return dest
    for attempt in range(4):
        r = requests.get(BASE + path, timeout=90)
        if r.ok:
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(r.content)
            return dest
        time.sleep(attempt + 1)
    raise RuntimeError(f'{r.status_code}: {path}')


def expand(node, seen=()):
    out = []
    if node.tag == 'variant' and node.get('file'):
        path = 'variants/' + node.get('file')
        if path not in seen:
            out.extend(expand(ET.parse(fetch(path)).getroot(), seen + (path,)))
    out.append(node)
    for child in node:
        out.extend(expand(child, seen))
    return out


def actor(entry):
    name = entry['name'][:-4]
    # Fantasy, burning animals and schools/orbit props are not standalone fauna.
    if name in {'dragon', 'pig_flaming', 'fish', 'tuna_shoal', 'crow_circle', 'seagull_circle', 'elephant'}:
        return None
    path = 'actors/fauna/' + entry['name']
    nodes = expand(ET.parse(fetch(path)).getroot())
    meshes = list(dict.fromkeys(n.text.strip() for n in nodes if n.tag == 'mesh' and n.text))
    textures = list(dict.fromkeys(n.get('file') for n in nodes if n.tag == 'texture' and n.get('name') == 'baseTex'))
    normals = list(dict.fromkeys(n.get('file') for n in nodes if n.tag == 'texture' and n.get('name') == 'normTex' and n.get('file') != 'default_norm.png'))
    anims = []
    for n in nodes:
        if n.tag == 'animation' and n.get('file') and n.get('file') not in [a['file'] for a in anims]:
            anims.append(dict(n.attrib))
    if not meshes or not textures:
        return {'id': name, 'skip': 'missing mesh or diffuse texture', 'actor': path}
    dependencies = ['meshes/' + m for m in meshes] + ['textures/skins/' + t for t in textures + normals] + ['animation/' + a['file'] for a in anims]
    for dep in dependencies:
        fetch(dep)
    return {'id': name, 'actor': path, 'meshes': meshes, 'textures': textures, 'normals': normals, 'animations': anims}


if __name__ == '__main__':
    fetch('LICENSE.txt')
    entries = requests.get(API + f'/contents/binaries/data/mods/public/art/actors/fauna?ref={REV}', timeout=40).json()
    records = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for result in pool.map(actor, entries):
            if result:
                records.append(result)
                print(result['id'], result.get('skip', f"{len(result['animations'])} animations"), flush=True)
    manifest = {'repository': 'https://github.com/0ad/0ad', 'revision': REV, 'license': 'CC-BY-SA-3.0', 'author': 'Wildfire Games', 'assets': records}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    checksums = {p.relative_to(OUT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(OUT.rglob('*')) if p.is_file() and p.name != 'SHA256SUMS.json'}
    (OUT / 'SHA256SUMS.json').write_text(json.dumps(checksums, indent=2), encoding='utf-8')
    print(f'Downloaded {len(records)} actor records, {len(checksums)} source files; revision {REV}', flush=True)
