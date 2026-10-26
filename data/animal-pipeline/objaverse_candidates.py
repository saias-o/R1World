"""Use public archived annotations, respecting the API's rate-limit response."""
import concurrent.futures
import gzip
import json
from pathlib import Path
import requests
from PIL import Image,ImageDraw
import io

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'data/source-assets/animals/research-expansion/objaverse'
classes=json.loads((OUT/'classes.json').read_text())
paths=json.load(gzip.open(OUT/'object-paths.json.gz'))
categories=['pigeon','duck','squirrel','frog','goose','lizard','rat']
groups=sorted({paths[u].split('/')[1] for k in categories for u in (classes[k] if k in {'pigeon','duck','squirrel','goose'} else classes[k][:3])})
def metadata(group):
    path=OUT/(group+'.json.gz')
    if not path.exists():
        r=requests.get('https://huggingface.co/datasets/allenai/objaverse/resolve/main/metadata/'+path.name,timeout=120)
        r.raise_for_status()
        path.write_bytes(r.content)
    with gzip.open(path) as f: data=json.load(f)
    selected=[]
    for k in categories:
        for uid in classes[k]:
            if uid in data and data[uid].get('license') in {'by','by-sa','cc0'}:
                record=data[uid]
                record.update({'lvis_class':k,'objaverse_path':paths[uid],'archive_metadata_group':group})
                selected.append(record)
    return selected
records=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    for result in pool.map(metadata,groups): records.extend(result)
records=list({r['uid']:r for r in records}.values())
records.sort(key=lambda r:(r['lvis_class'],-r.get('likeCount',0)))
(OUT/'common-candidates.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
for i,r in enumerate(records):
    print(i,r['lvis_class'],r['uid'],r['name'].encode('ascii','replace').decode(),r['faceCount'],r['license'],flush=True)
def thumb(pair):
    i,r=pair
    file=OUT/'thumbs'/(r['uid']+'.jpg')
    file.parent.mkdir(exist_ok=True)
    images=r.get('thumbnails',{}).get('images',[])
    if not images: return
    image=min(images,key=lambda im:abs(im.get('width',0)-720))
    if not file.exists():
        try:
            data=requests.get(image['url'],timeout=40)
            if data.ok:
                im=Image.open(io.BytesIO(data.content)).convert('RGB')
                im.save(file)
        except Exception: pass
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool: list(pool.map(thumb,enumerate(records)))
for start in range(0,len(records),24):
    subset=records[start:start+24]
    sheet=Image.new('RGB',(1440,220*((len(subset)+3)//4)),'#282828')
    draw=ImageDraw.Draw(sheet)
    for j,r in enumerate(subset):
        x,y=(j%4)*360,(j//4)*220
        file=OUT/'thumbs'/(r['uid']+'.jpg')
        if file.exists():
            im=Image.open(file); im.thumbnail((360,195))
            sheet.paste(im,(x+(360-im.width)//2,y))
        draw.text((x+4,y+196),f"{start+j} {r['lvis_class']}: "+r['name'].encode('ascii','replace').decode()[:30],fill='white')
    sheet.save(OUT/f'candidates-{start//24+1:02}.jpg')
