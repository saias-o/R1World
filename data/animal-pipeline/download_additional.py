"""Extra fauna found outside 0 A.D.'s main fauna actor directory."""
import json
from pathlib import Path
import requests
import xml.etree.ElementTree as ET
ROOT=Path(__file__).resolve().parents[2]
SRC=ROOT/'data/source-assets/animals/0ad'
manifest=json.loads((SRC/'manifest.json').read_text())
items=[('wolfhound','0ad/0ad','binaries/data/mods/public/art','actors/units/britons/wolfhound.xml','Wildfire Games'),('turkey','0ADMods/terra_magna','art','actors/fauna/turkey.xml','The Council of Modders, Fallen Empire Studio, Scion Development')]
for name,repo,prefix,actor_path,author in items:
    existing=next((x for x in manifest['assets'] if x['id']==name and x.get('revision')),None)
    revision=existing['revision'] if existing else requests.get(f'https://api.github.com/repos/{repo}/commits/master',timeout=40).json()['sha']
    base=f'https://raw.githubusercontent.com/{repo}/{revision}/{prefix}/'
    def fetch(path):
        dest=SRC/path
        if not dest.exists():
            r=requests.get(base+path,timeout=60)
            r.raise_for_status()
            dest.parent.mkdir(parents=True,exist_ok=True)
            dest.write_bytes(r.content)
        return dest
    node=ET.parse(fetch(actor_path)).getroot()
    meshes=list(dict.fromkeys(n.text.strip() for n in node.iter('mesh')))
    textures=list(dict.fromkeys(n.get('file') for n in node.iter('texture') if n.get('name')=='baseTex'))
    animations=[]
    for n in node.iter('animation'):
        if n.get('file') not in [a['file'] for a in animations]:
            animations.append(dict(n.attrib))
    for path in ['meshes/'+m for m in meshes]+['textures/skins/'+t for t in textures]+['animation/'+a['file'] for a in animations]:
        fetch(path)
    record={'id':name,'actor':actor_path,'meshes':meshes,'textures':textures,'normals':[],'animations':animations,'repository':'https://github.com/'+repo,'revision':revision,'author':author,'license':'CC-BY-SA-3.0'}
    if name=='turkey':
        # Distinct license evidence for mod artwork, separate from engine GPL.
        dest=SRC/'licenses/terra-magna-art.txt'
        dest.parent.mkdir(exist_ok=True)
        dest.write_bytes(requests.get(base+'LICENSE.txt',timeout=40).content)
    manifest['assets']=[x for x in manifest['assets'] if x['id']!=name]+[record]
    print(name,len(animations),'animations',flush=True)
(SRC/'manifest.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
