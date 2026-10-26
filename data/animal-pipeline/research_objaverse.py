"""Find distributable naturalistic common fauna in public Objaverse mirrors.

ODC-BY database license is NOT treated as an asset license. Every selected model
must independently carry a CC0 / CC BY / CC BY-SA model license.
"""
import concurrent.futures
import gzip
import json
from pathlib import Path
import requests
import sys
sys.stdout.reconfigure(errors='replace')

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'data/source-assets/animals/research-expansion/objaverse'
OUT.mkdir(exist_ok=True)
BASE='https://huggingface.co/datasets/allenai/objaverse/resolve/main/'
for filename in ['lvis-annotations.json.gz','object-paths.json.gz','README.md']:
    path=OUT/filename
    if not path.exists():
        r=requests.get(BASE+filename,timeout=120)
        r.raise_for_status()
        path.write_bytes(r.content)
with gzip.open(OUT/'lvis-annotations.json.gz') as f: classes=json.load(f)
with gzip.open(OUT/'object-paths.json.gz') as f: paths=json.load(f)
terms=['pigeon','duck','squirrel','mouse','rat','bee','snail','frog','hedgehog','goose','sparrow','lizard','turtle','crab','butterfly','dragonfly','raccoon','seal','otter','beaver']
chosen={k:v for k,v in classes.items() if any(t==k or k.startswith(t+' ') or k.startswith(t+'_') or k==t+'s' for t in terms)}
(OUT/'classes.json').write_text(json.dumps(chosen,indent=2))
print('CLASSES',[(k,len(v)) for k,v in chosen.items()],flush=True)
uids=list(dict.fromkeys(u for _,us in chosen.items() for u in us))
def fetch(uid):
    cache=OUT/'models'/ (uid+'.json')
    cache.parent.mkdir(exist_ok=True)
    if cache.exists(): return json.loads(cache.read_text())
    r=requests.get('https://api.sketchfab.com/v3/models/'+uid,timeout=45)
    if not r.ok: return {'uid':uid,'status':r.status_code}
    data=r.json()
    cache.write_text(json.dumps(data,indent=2),encoding='utf-8')
    return data
records=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    for data in pool.map(fetch,uids):
        license_name=data.get('license',{}).get('slug','')
        if license_name not in {'cc0','by','by-sa','cc-by','cc-by-sa'}: continue
        data['objaverse_path']=paths.get(data['uid'])
        data['lvis_classes']=[k for k,v in chosen.items() if data['uid'] in v]
        if data.get('faceCount',0)>2000000: continue
        records.append(data)
(OUT/'eligible.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
print('Eligible',len(records),flush=True)
for data in records:
    print(data['uid'],data.get('name'),data.get('license',{}).get('slug'),data.get('faceCount'),data.get('animationCount'),data['lvis_classes'],flush=True)
