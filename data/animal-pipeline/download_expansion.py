"""Archive candidate sources for common-animal expansion; public CC0 files."""
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import uuid
from urllib.parse import urljoin
import requests

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'data/source-assets/animals/expansion'
OUT.mkdir(parents=True,exist_ok=True)
RESEARCH=ROOT/'data/source-assets/animals/research-expansion'

def get(url,dest):
    dest.parent.mkdir(parents=True,exist_ok=True)
    if not dest.exists():
        r=requests.get(url,timeout=120)
        r.raise_for_status()
        dest.write_bytes(r.content)
    return dest

def grab(slug):
    folder=OUT/('grab_'+slug.replace('-','_'))
    url='https://grab3d.com/models/animals/'+slug+'/'
    page=get(url,folder/'source-page.html').read_text(encoding='utf-8')
    assert 'CC0' in page
    # Lightweight download is explicitly published by the author, not a viewer rip.
    links=list(dict.fromkeys(urljoin(url,u) for u in re.findall(r'href="([^"]+)"',page) if u.endswith('.glb') and '/dl/' in u))
    if not links:
        raise ValueError('No downloadable GLB: '+slug)
    link=next((u for u in links if u.endswith('-light.glb')),links[0])
    dest=get(link,folder/Path(link).name)
    get('https://grab3d.com/license/',OUT/'licenses/grab3d-license.html')
    record={'id':'grab_'+slug.replace('-','_'),'source_url':url,'download_url':link,'source_file':str(dest.relative_to(ROOT)),'author':'Grab3D','license':'CC0-1.0','sha256':hashlib.sha256(dest.read_bytes()).hexdigest(),'rig_origin':'to inspect'}
    (folder/'provenance.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(record['id'],dest.stat().st_size,flush=True)
    return record

def polyhaven():
    name='street_rat'
    folder=OUT/'polyhaven_street_rat'
    data=requests.get('https://api.polyhaven.com/files/'+name,timeout=40).json()
    info=requests.get('https://api.polyhaven.com/info/'+name,timeout=40).json()
    folder.mkdir(exist_ok=True)
    (folder/'files.json').write_text(json.dumps(data,indent=2),encoding='utf-8')
    (folder/'info.json').write_text(json.dumps(info,indent=2),encoding='utf-8')
    gltf=data['gltf']['1k']['gltf']
    dest=get(gltf['url'],folder/'street_rat_1k.gltf')
    for relative,asset in gltf['include'].items():
        get(asset['url'],folder/relative)
    get('https://polyhaven.com/license',OUT/'licenses/polyhaven-license.html')
    record={'id':'street_rat','source_url':'https://polyhaven.com/a/street_rat','source_file':str(dest.relative_to(ROOT)),'author':'Sean Buckley / Poly Haven','license':'CC0-1.0','rig_origin':'unrigged source; new rig required'}
    (folder/'provenance.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(record['id'],'downloaded',flush=True)
    return record

def blenderkit(asset):
    assert asset['license']=='cc_zero' and asset['isFree'] and not asset.get('isPrivate')
    ident='bk_'+re.sub('[^a-z0-9]+','_',asset['name'].lower()).strip('_')
    folder=OUT/ident
    folder.mkdir(exist_ok=True)
    (folder/'metadata.json').write_text(json.dumps(asset,indent=2),encoding='utf-8')
    file=next((f for f in asset['files'] if f['fileType']=='resolution_1K'),next(f for f in asset['files'] if f['fileType']=='blend'))
    response=requests.get(file['downloadUrl'],params={'scene_uuid':str(uuid.uuid4())},timeout=40)
    response.raise_for_status()
    signed=response.json()['filePath']
    dest=get(signed,folder/'source.blend')
    record={'id':ident,'source_url':'https://www.blenderkit.com/asset-gallery-detail/'+asset['assetBaseId']+'/','source_file':str(dest.relative_to(ROOT)),'author':asset['author'].get('fullName'),'license':'CC0-1.0','rig_origin':'to inspect','blenderkit_asset_id':asset['id'],'sha256':hashlib.sha256(dest.read_bytes()).hexdigest()}
    (folder/'provenance.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(ident,dest.stat().st_size,flush=True)
    return record

if __name__=='__main__':
    slugs=['cat','black-cat','bicolor-sphynx-cat','frog','bird','snake','sea-turtle','dolphin','gorilla','owl','parrot','eagle','octopus']
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures=[pool.submit(grab,slug) for slug in slugs]+[pool.submit(polyhaven)]
        for f in concurrent.futures.as_completed(futures):
            try: f.result()
            except Exception as e: print('DOWNLOAD_ERROR',repr(e),flush=True)
    searches=json.loads((RESEARCH/'blenderkit-search.json').read_text())
    assets=[a for key in ['bee','spider','lizard'] for a in searches[key].get('results',[]) if a.get('license')=='cc_zero' and any(s in a['name'].lower() for s in ['wasp','spider','lizard'])]
    for asset in assets:
        try: blenderkit(asset)
        except Exception as e: print('DOWNLOAD_ERROR',repr(e),flush=True)
    records=[json.loads(p.read_text()) for p in sorted(OUT.glob('*/provenance.json'))]
    (OUT/'manifest.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
