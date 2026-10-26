"""Check every delivered file against its pre-cleanup copy."""
import io,json,sys
from pathlib import Path
from PIL import Image
from glb_tools import read
from compact_storage import signature,visible_nodes
ROOT=Path(__file__).resolve().parents[2]
BEFORE=ROOT/'data/animal-pipeline/cleanup-work/before'
OUT=ROOT/'game/assets/animals'
report=[]
for p in sorted(OUT.glob('*.glb')):
 old,ob=read(BEFORE/p.name);new,nb=read(p)
 visible=visible_nodes(old,ob)
 assert signature(old,ob,visible)==signature(new,nb,visible),p.name
 assert old['materials']==new['materials'] and old.get('textures')==new.get('textures'),p.name
 alpha=set()
 for mat in old['materials']:
  tex=mat.get('pbrMetallicRoughness',{}).get('baseColorTexture')
  if tex and mat.get('alphaMode','OPAQUE')!='OPAQUE':alpha.add(old['textures'][tex['index']]['source'])
 assert len(old.get('images',[]))==len(new.get('images',[]))
 changed=0
 for i,(a,b) in enumerate(zip(old.get('images',[]),new.get('images',[]))):
  def data(doc,raw,image):
   view=doc['bufferViews'][image['bufferView']];return raw[view.get('byteOffset',0):view.get('byteOffset',0)+view['byteLength']]
  x=data(old,ob,a);y=data(new,nb,b)
  if x==y:continue
  mode='RGBA' if i in alpha else 'RGB'
  x=Image.open(io.BytesIO(x)).convert(mode);y=Image.open(io.BytesIO(y)).convert(mode)
  assert x.size==y.size and x.tobytes()==y.tobytes(),(p.name,i)
  changed+=1
 report.append({'file':p.name,'geometry_weights_UVs_normals_identical':True,'visible_animation_keys_identical':True,'clip_names_and_durations_identical':True,'skeleton_identical':True,'texture_dimensions_and_used_pixels_identical':True,'repacked_images':changed})
(ROOT/'data/animal-pipeline/review/storage-equivalence.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('Exact equivalence:',len(report),'GLBs; geometry, skeletons, visible motion, texture dimensions and used pixels preserved.')
