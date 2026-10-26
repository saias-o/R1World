"""Download selected freely licensed files from the public Objaverse distribution."""
import concurrent.futures,json,hashlib,sys
from pathlib import Path
import requests
sys.path.insert(0,str(Path(__file__).resolve().parent))
from glb_tools import read
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'data/source-assets/animals'
records=json.loads((SRC/'research-expansion/objaverse/common-candidates.json').read_text())
selected={33:'frog_green',48:'frog_amazon',126:'gecko_house',135:'frog_red_eye',170:'pigeon_white',175:'pigeon_gray',180:'pigeon_rock',211:'mouse_house',213:'rat_white',216:'mouse_brown'}
def download(pair):
 i,name=pair;r=records[i];folder=SRC/'expansion'/('obj_'+name);folder.mkdir(exist_ok=True)
 url='https://huggingface.co/datasets/allenai/objaverse/resolve/main/'+r['objaverse_path']
 path=folder/(r['uid']+'.glb')
 if not path.exists():
  response=requests.get(url,timeout=180);response.raise_for_status();path.write_bytes(response.content)
 doc,binary=read(path)
 copyright=doc.get('asset',{}).get('copyright','')
 # Require actual embedded original author/license evidence, not dataset license.
 (folder/'archive-metadata.json').write_text(json.dumps(r,indent=2,ensure_ascii=False),encoding='utf-8')
 proof={'id':'obj_'+name,'source_file':str(path.relative_to(ROOT)),'source_url':'https://sketchfab.com/3d-models/'+r['uid'],'download_url':url,'author':r['user'].get('displayName') or r['user']['username'],'author_profile':r['user']['profileUrl'],'license_slug':r['license'],'embedded_copyright':copyright,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'archive_metadata_group':r['archive_metadata_group']}
 if 'creativecommons.org/licenses/by/4.0' in copyright:proof['license']='CC-BY-4.0'
 elif 'creativecommons.org/licenses/by/3.0' in copyright:proof['license']='CC-BY-3.0'
 elif r['license']=='cc0':proof['license']='CC0-1.0'
 else:proof['license']='NEEDS_VERSION_CHECK'
 (folder/'provenance.json').write_text(json.dumps(proof,indent=2,ensure_ascii=False),encoding='utf-8')
 print(name,path.stat().st_size,proof['license'],copyright.encode('ascii','replace').decode(),flush=True)
 return proof
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:results=list(pool.map(download,selected.items()))
p=SRC/'expansion/manifest.json';existing=json.loads(p.read_text());ids={r['id'] for r in results};existing=[r for r in existing if r['id'] not in ids];p.write_text(json.dumps(existing+results,indent=2,ensure_ascii=False),encoding='utf-8')
