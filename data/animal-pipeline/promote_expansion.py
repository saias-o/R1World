"""Promote only visually accepted sources; never overwrite the initial delivery."""
import json,hashlib,shutil,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from glb_tools import read,write
ROOT=Path(__file__).resolve().parents[2]
QA=ROOT/'data/animal-pipeline/review';P=QA/'expansion';OUT=ROOT/'game/assets/animals'
ACCEPTED=['cat_sphynx','cat_tabby','dolphin','eagle_bald','frog_amazon','frog_blue','frog_red_eye','gorilla','lizard','mouse_brown','owl_eagle','parrot_macaw','pigeon_rock','rat_street','robin','shrike','spider_garden','turtle_pond','turtle_sea','wasp_realistic']
original=json.loads((P/'original-delivery-sha.json').read_text())
assert all(hashlib.sha256((OUT/k).read_bytes()).hexdigest()==v for k,v in original.items())
for ident in ACCEPTED:
 r=json.loads((P/('prepared_'+ident+'.json')).read_text());assert r['status']=='candidate' and r['license'] in {'CC0-1.0','CC-BY-4.0'}
 if ident=='shrike':r['animation_status']='rig_validation_only; original skeleton; control clips created here'
 if r['rig_origin']=='created_for_R1World':
  r['limitations']+=['Basic deformation rig with topology-derived weights; no authored locomotion. Animation-specific refinement may be required.']
 if ident in {'shrike','spider_garden','wasp_realistic'}:r['limitations']+=['Portable PBR uses original albedo; particle fur and Cycles-only micro-bump are omitted.']
 if ident=='lizard':r['limitations']+=['Source has a simple brown material, without a detailed albedo texture.']
 if ident=='turtle_pond':r['limitations']+=['Original PoseLib clip is static. Eye meshes attached to head skin; Rig_Check clips added. Color textures capped at 512px; normal/occlusion maps at 256px (half for LOD1).']
 r['visual_review']='Accepted after comparison with current fox, rabbit, wolf, chicken, crow, hawk, crocodile, pike and ant under identical headless lighting. Natural proportions; no cartoon/figurine styling. Reimported LOD0/LOD1 and sampled exported poses reviewed.'
 for name in r['files']:
  assert name not in original
  source=P/'candidates'/name
  doc,binary=read(source)
  doc['asset']['copyright']=r['author']+'; '+r['license']+'; source '+r['source_url']+'; adapted for R1World: GLB, scale, LOD, textures'+(', deformation rig and control clips' if r['rig_origin']=='created_for_R1World' else '')
  doc['asset'].setdefault('extras',{}).update(rig_origin=r['rig_origin'],animation_status=r['animation_status'])
  write(OUT/name,doc,binary)
 r['status']='prepared'
 (QA/(ident+'.json')).write_text(json.dumps(r,indent=2,ensure_ascii=False),encoding='utf-8')
 for suffix in ['', '-lod1']:
  shutil.copyfile(P/'final-renders'/(ident+suffix+'.png'),QA/'final-renders'/(ident+suffix+'.png'))
path=QA/'final-renders/rendered.json';names=json.loads(path.read_text());names=list(dict.fromkeys(names+ACCEPTED));path.write_text(json.dumps(names,indent=2))
assert all(hashlib.sha256((OUT/k).read_bytes()).hexdigest()==v for k,v in original.items())
print('Promoted',len(ACCEPTED),'models; original',len(original),'GLBs unchanged')
