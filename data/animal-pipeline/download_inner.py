import json
import re
import hashlib
from pathlib import Path
import requests

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'data/source-assets/animals/expansion'
PARTS={'pigeon':'urban-pigeon-3977bbba','sparrow':'house-sparrow-506ebec6','duck':'farm-duck-4256e6b1','squirrel':'tree-squirrel-6499352e'}
for name,part in PARTS.items():
    folder=OUT/('inner_'+name)
    folder.mkdir(exist_ok=True)
    url='https://www.innerscene.com/tools/library/3d-parts/'+part
    response=requests.get(url,timeout=50)
    response.raise_for_status()
    (folder/'source-page.html').write_text(response.text,encoding='utf-8')
    data=json.loads(re.search(r'<script id="__NEXT_DATA__" type="application/json">(.+?)</script>',response.text,re.S).group(1))['props']['pageProps']['part']
    assert data['license']=='CC0-1.0' and data['sourceName']=='Innerscene - original model'
    (folder/'metadata.json').write_text(json.dumps(data,indent=2),encoding='utf-8')
    download='https://www.innerscene.com/api/library/'+part+'/download'
    dest=folder/data['fileName']
    if not dest.exists():
        r=requests.get(download,timeout=90)
        r.raise_for_status()
        assert r.content[:4]==b'glTF'
        dest.write_bytes(r.content)
    record={'id':'inner_'+name,'source_url':url,'source_file':str(dest.relative_to(ROOT)),'download_url':download,'author':'Innerscene','license':'CC0-1.0','rig_origin':'to inspect','sha256':hashlib.sha256(dest.read_bytes()).hexdigest()}
    (folder/'provenance.json').write_text(json.dumps(record,indent=2),encoding='utf-8')
    print(record['id'],dest.stat().st_size,flush=True)
manifest=[json.loads(p.read_text()) for p in sorted(OUT.glob('*/provenance.json'))]
(OUT/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
