"""Apply the recorded human/model visual review; do not substitute assets."""
import json
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[2]
QA=ROOT/'data/animal-pipeline/review'
OUT=ROOT/'game/assets/animals'
ACCEPT={'ant','butterfly','bat','fish_kelgar','chicken_kelgar','rooster','doe','stag','rabbit','pike','horseshoe_crab'}
REASONS={
    'duck':'Toy-like smooth body and proportions; incompatible with textured realistic fauna.',
    'duckling':'Toy-like proportions and large head.',
    'lemur':'Anthropomorphic upright character, oversized eyes and painted face.',
    'rat':'Fantasy monster teeth, aggressive exaggerated face and proportions.',
    'spider':'Exaggerated red eyes, abstract painted body; incompatible style.',
    'wasp':'Unfinished green surface, not a finished realistic hornet texture.',
    'scorpion':'Cartoon giant eye and exaggerated claws.',
    'snake':'Extremely angular 206-triangle mesh, stick-like silhouette in the reviewed pose.',
    'penguin':'Cartoon painted eyes and toy proportions.',
    'gorilla':'No usable rig in downloaded file.',
    'bestiary':'Placeholder textures and inconsistent quality; several meshes are unrigged.',
    'cobra':'No downloadable 3D source exposed on the checked source page.',
    'turtle':'Source page access unavailable; other located source explicitly described as cartoon.',
}

decisions=[]
for report in sorted(QA.glob('oga_*.json')):
    r=json.loads(report.read_text())
    if r['id'] in ACCEPT and r['status'] in {'candidate','prepared'}:
        for name in r['files']:
            shutil.copy2(QA/'candidates'/name,OUT/name)
        r['status']='prepared'
        r['visual_review']='Accepted from headless textured render and LOD comparison; natural silhouette and compatible materials.'
        report.write_text(json.dumps(r,indent=2),encoding='utf-8')
        decisions.append({'id':r['id'],'decision':'accepted','reason':r['visual_review']})
    else:
        decisions.append({'id':r['id'],'decision':'rejected','reason':REASONS.get(r['id'],r.get('reason','Not selected'))})
for ident in ['gorilla','bestiary','cobra','turtle']:
    decisions.append({'id':ident,'decision':'rejected','reason':REASONS[ident]})

# The newer CC0 hen/rooster is visibly better than the old rectangular-tail
# chicken. Keep old downloads as sources, but not as game-ready selections.
rejected_dir=QA/'rejected-0ad'
rejected_dir.mkdir(exist_ok=True)
old_chickens=['chicken','chicken_coat2']
blood_skins=[]
for report in QA.glob('*.json'):
    r=json.loads(report.read_text())
    if isinstance(r,dict) and r.get('source_texture') and any(t in r['source_texture'].lower() for t in ['_dead','_death','_blood']):
        blood_skins.append(r['id'])
lower_grade=[]
for report in QA.glob('*.json'):
    r=json.loads(report.read_text())
    if isinstance(r,dict) and (r.get('id','').startswith('rabbit1') or r.get('id')=='zebra_2'):
        lower_grade.append(r['id'])
for ident in old_chickens+blood_skins+lower_grade:
    reason='Opaque rectangular tail cards; better hen and rooster selected.' if ident in old_chickens else ('Angular mesh or incompatible painted appearance; better animal retained.' if ident in lower_grade else 'Death-state bloody texture; excluded from the living animal catalogue.')
    for suffix in ['.glb','-lod1.glb']:
        p=OUT/(ident+suffix)
        if p.exists():
            shutil.move(str(p),str(rejected_dir/p.name))
    report=QA/(ident+'.json')
    if report.exists():
        r=json.loads(report.read_text())
        r['status']='rejected'
        r['reason']=reason
        report.write_text(json.dumps(r,indent=2),encoding='utf-8')
    decisions.append({'id':ident,'decision':'rejected','reason':reason})
(QA/'visual-decisions.json').write_text(json.dumps(decisions,indent=2),encoding='utf-8')
print('Accepted OGA candidates:',len(ACCEPT))
