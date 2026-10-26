"""Archive OpenGameArt source pages, licenses and downloadable candidates."""
import hashlib
import json
from pathlib import Path
import re
import zipfile
import sys
from urllib.parse import unquote, urljoin, urlparse
import requests

OUT = Path(__file__).resolve().parents[2] / 'data/source-assets/animals/opengameart'
CANDIDATES = [
    ('shrike', 'bird-yellow-billed-shrike', 'pistachio / Al Stachio', 'CC0-1.0'),
    ('brown_bird', 'little-brown-bird', None, None),
    ('snail', 'snail-3d-model', None, None),
    ('pike', 'esox-animated-fish', 'CDmir', 'CC0-1.0'),
    ('horseshoe_crab', 'swamp-limule', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('rabbit', 'rabbit-0', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('doe', 'deer-female', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('stag', 'old-deer-male', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('chicken_kelgar', 'chicken-animated', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('rooster', 'rooster-animated', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('scorpion', 'scorpion-3d', 'Guillaume "GuieA_7" Englert', 'CC-BY-SA-4.0'),
    ('butterfly', 'butterfly-animated', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('ant', 'ant-3d-model-rigging-animated-low-poly-ish', 'mujtaba-io', 'CC0-1.0'),
    ('wasp', '3d-hornetwasp', 'MikeMoon', 'CC0-1.0'),
    ('bat', 'vampire-bat-animated', 'rubberduck', 'CC0-1.0'),
    ('fish_kelgar', 'fish-animated', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('rat', 'evil-giant-rat', 'CDmir / TinyWorlds', 'CC0-1.0'),
    ('cobra', 'cobra', None, None),
    ('lemur', 'lemur', None, None),
    ('turtle', 'turtle', None, None),
    ('spider', 'spider-animated', None, None),
    ('frog', 'low-poly-frog-blender', None, None),
    ('duck', 'rigged-duck', None, None),
    ('duckling', 'rigged-duckling', None, None),
    ('gorilla', 'gorilla-male', None, None),
    ('bestiary', 'animal-bestiary', 'rudy85', 'CC0-1.0'),
]


def run():
    records = []
    for ident, slug, author, license_name in CANDIDATES:
        if len(sys.argv)>1 and ident not in sys.argv[1:]:
            continue
        url = 'https://opengameart.org/content/' + slug
        r = requests.get(url, timeout=60)
        if not r.ok:
            print(ident, r.status_code, flush=True)
            continue
        folder = OUT / ident
        folder.mkdir(parents=True, exist_ok=True)
        (folder / 'source-page.html').write_text(r.text, encoding='utf-8')
        if license_name is None:
            if 'creativecommons.org/publicdomain/zero' in r.text:
                license_name = 'CC0-1.0'
            elif 'creativecommons.org/licenses/by-sa/3.0' in r.text:
                license_name = 'CC-BY-SA-3.0'
            elif 'creativecommons.org/licenses/by/4.0' in r.text:
                license_name = 'CC-BY-4.0'
            elif 'creativecommons.org/licenses/by/3.0' in r.text:
                license_name = 'CC-BY-3.0'
        if not license_name:
            print(ident, 'license requires review', flush=True)
            continue
        if author is None:
            match = re.search(r'field-name-field-art-authors.*?href="/users/([^"]+)', r.text, re.S)
            author = match.group(1) if match else 'See archived source page'
        links = list(dict.fromkeys(urljoin(url, h.replace('&amp;', '&')) for h in re.findall(r'href="([^"]+)"', r.text) if re.search(r'/sites/default/files/.*\.(zip|blend|fbx|glb|gltf|7z)(?:\?|$)', h, re.I)))
        files = []
        for link in links:
            filename = unquote(urlparse(link).path.split('/')[-1])
            dest = folder / filename
            if not dest.exists():
                data = requests.get(link, timeout=120)
                data.raise_for_status()
                dest.write_bytes(data.content)
            files.append({'file': filename, 'url': link, 'sha256': hashlib.sha256(dest.read_bytes()).hexdigest()})
            if dest.suffix.lower() == '.zip':
                with zipfile.ZipFile(dest) as z:
                    for member in z.infolist():
                        target = (folder / 'extracted' / member.filename).resolve()
                        if not target.is_relative_to((folder / 'extracted').resolve()):
                            raise ValueError('unsafe archive path')
                    z.extractall(folder / 'extracted')
        record = {'id': ident, 'source': url, 'author': author, 'license': license_name, 'downloads': files}
        (folder / 'provenance.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
        records.append(record)
        print(ident, license_name, [f['file'] for f in files], flush=True)
    records=[json.loads(p.read_text(encoding='utf-8')) for p in sorted(OUT.glob('*/provenance.json'))]
    (OUT / 'manifest.json').write_text(json.dumps(records, indent=2), encoding='utf-8')


if __name__ == '__main__':
    run()
