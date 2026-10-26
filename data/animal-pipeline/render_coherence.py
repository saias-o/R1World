import sys,bpy
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA,render,quiet
dest=QA/'expansion/references';dest.mkdir(exist_ok=True)
for name in ['wolf','fox_red','rabbit','chicken_kelgar','crow','hawk','crocodile','ant','pike']:
 bpy.ops.wm.read_factory_settings(use_empty=True)
 with quiet():bpy.ops.import_scene.gltf(filepath=str(ROOT/'game/assets/animals'/(name+'.glb')))
 meshes=[o for o in bpy.data.objects if o.type=='MESH' and o.find_armature() and not o.hide_render]
 render(meshes,dest/(name+'.png'))
