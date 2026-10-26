import json
from pathlib import Path
import re
import requests
from urllib.parse import unquote, urljoin
import tarfile
ROOT = Path(__file__).resolve().parents[2] / 'data/source-assets/animals/opengameart'
for ident in ['spider', 'cobra']:
    folder = ROOT / ident
    page = (folder / 'source-page.html').read_text(encoding='utf-8')
    urls = list(dict.fromkeys(urljoin('https://opengameart.org', u) for u in re.findall('href="([^"]+)"', page) if '/sites/default/files/' in u and re.search(r'\.(png|jpg|tar\.gz)$',u,re.I)))
    for url in urls:
        r = requests.get(url, timeout=60)
        if r.ok:
            (folder / unquote(url.rsplit('/',1)[-1])).write_bytes(r.content)
    print(ident, urls, flush=True)
for ident,slug in [('snake','snake-low-poly-animated-3d-model'),('penguin','penguin-low-poly-animated-3d-model')]:
    folder = ROOT / ident
    folder.mkdir(exist_ok=True)
    url = 'https://opengameart.org/content/' + slug
    r = requests.get(url,timeout=60)
    (folder/'source-page.html').write_text(r.text,encoding='utf-8')
    if 'creativecommons.org/publicdomain/zero' not in r.text:
        continue
    links = list(dict.fromkeys(urljoin(url,u) for u in re.findall('href="([^"]+)"', r.text) if '/sites/default/files/' in u and re.search(r'\.(tar\.gz|blend|zip)$',u,re.I)))
    for link in links:
        archive = folder / unquote(link.rsplit('/',1)[-1])
        archive.write_bytes(requests.get(link,timeout=60).content)
        if archive.name.endswith('.tar.gz'):
            with tarfile.open(archive) as tar:
                tar.extractall(folder / 'extracted', filter='data')
    (folder / 'provenance.json').write_text(json.dumps({'id':ident,'source':url,'author':'methodical pixel','license':'CC0-1.0','downloads':links},indent=2))
    print(ident, links, flush=True)
