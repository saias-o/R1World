import json
import sys
from pathlib import Path
import bpy
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA,render

dest=QA/'animation-samples'
dest.mkdir(exist_ok=True)
args=sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else ['goat','wolf','bear_brown','elephant_african_bush','horse','crocodile','wolfhound']
report=[]
for name in args:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(ROOT/'game/assets/animals'/(name+'.glb')))
    rigs=[o for o in bpy.data.objects if o.type=='ARMATURE']
    rig=max(rigs,key=lambda o:len(o.data.bones))
    actions=list(bpy.data.actions)
    action=next((a for a in actions if 'walk' in a.name.lower()),actions[0] if actions else None)
    if not action:
        continue
    rig.animation_data_create()
    for t in rig.animation_data.nla_tracks:
        t.mute=True
    rig.animation_data.action=action
    meshes=[o for o in bpy.data.objects if o.type=='MESH']
    for i,fraction in enumerate([0,.25,.5,.75]):
        bpy.context.scene.frame_set(round(action.frame_range[0]+(action.frame_range[1]-action.frame_range[0])*fraction))
        render(meshes,dest/f'{name}-{i}.png')
    report.append({'id':name,'action':action.name,'frame_range':list(action.frame_range)})
(dest/'report.json').write_text(json.dumps(report,indent=2))
