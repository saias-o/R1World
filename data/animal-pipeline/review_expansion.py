"""Visual QA of reimported candidate GLBs and actual exported animation clips."""
import json,sys
from pathlib import Path
import bpy
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA,render,quiet
from rig_expansion import CONFIG,STAGE
dest=QA/'expansion/final-renders';dest.mkdir(exist_ok=True)
args=sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else []
delivered='--delivered' in args
selected=[a for a in args if not a.startswith('--')]
source=ROOT/'game/assets/animals' if delivered else STAGE
report=[]
for config in CONFIG.values():
 name=config['id']
 if selected and name not in selected:continue
 for suffix in ['', '-lod1']:
  bpy.ops.wm.read_factory_settings(use_empty=True)
  with quiet():bpy.ops.import_scene.gltf(filepath=str(source/(name+suffix+'.glb')))
  meshes=[o for o in bpy.data.objects if o.type=='MESH' and o.find_armature() and not o.hide_render]
  rigs=[o for o in bpy.data.objects if o.type=='ARMATURE']
  for rig in rigs:
   rig.animation_data_create()
   for t in rig.animation_data.nla_tracks:t.mute=True
   rig.animation_data.action=None
  bpy.context.scene.frame_set(0)
  render(meshes,dest/(name+suffix+'.png'))
  if suffix:continue
  actions=list(bpy.data.actions)
  for i,action in enumerate(actions):
   rigs[0].animation_data.action=action
   start,end=action.frame_range
   bpy.context.scene.frame_set(round(start+(end-start)*.5))
   render(meshes,dest/(name+'-pose'+str(i)+'.png'))
   report.append({'id':name,'clip':action.name,'sample_frame':bpy.context.scene.frame_current})
 print('REIMPORT_REVIEW',name,flush=True)
(dest/'animations-reviewed.json').write_text(json.dumps(report,indent=2))
