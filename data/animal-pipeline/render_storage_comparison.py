"""Headless before/after visual smoke at the same authored pose."""
import sys,json,bpy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,render,quiet
DEST=ROOT/'data/animal-pipeline/cleanup-work/visual';DEST.mkdir(exist_ok=True)
NAMES=['horse','camel','wolf','rabbit','shrike','rat_street','turkey','frog_red_eye']
records=[]
for name in NAMES:
 for state,folder in [('before',ROOT/'data/animal-pipeline/cleanup-work/before'),('after',ROOT/'game/assets/animals')]:
  bpy.ops.wm.read_factory_settings(use_empty=True)
  with quiet():bpy.ops.import_scene.gltf(filepath=str(folder/(name+'.glb')))
  meshes=[o for o in bpy.data.objects if o.type=='MESH' and o.find_armature() and not o.hide_render]
  rigs=[o for o in bpy.data.objects if o.type=='ARMATURE'];rig=max(rigs,key=lambda o:len(o.data.bones))
  actions=list(bpy.data.actions);action=next((a for a in actions if 'walk' in a.name.lower()),actions[0] if actions else None)
  rig.animation_data_create()
  for t in rig.animation_data.nla_tracks:t.mute=True
  rig.animation_data.action=action
  if action:
   start,end=action.frame_range;f=start+(end-start)*.37
   bpy.context.scene.frame_set(int(f),subframe=f-int(f))
  render(meshes,DEST/(name+'-'+state+'.png'))
  records.append({'id':name,'state':state,'action':action.name if action else None})
 print('VISUAL_COMPARISON',name,flush=True)
(DEST/'report.json').write_text(json.dumps(records,indent=2),encoding='utf-8')
