"""Render final optimized GLBs only; no game window or GPU job."""
import json
from pathlib import Path
import sys
import bpy
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA,render

dest=QA/'final-renders'
dest.mkdir(exist_ok=True)
names=['bear_brown','bear_white','wolf','fox_red','deer','stag','elephant_african_bush','elephant_asian','giraffe_adult','lion','tiger','horse','cow','goat','boar','pig1','rhino','hippopotamus','zebra','wildebeest','wolfhound','rabbit','chicken_kelgar','rooster','peacock','turkey','crow','seagull','hawk','crocodile','shark','tuna','whale_fin','fish_kelgar','pike','butterfly','ant','bat','horseshoe_crab']
for name in names:
    for suffix in ['', '-lod1']:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=str(ROOT/'game/assets/animals'/(name+suffix+'.glb')))
        # Keep the glTF importer's bind-pose compensation. Switching an imported
        # skin to Blender REST can discard that compensation on legacy rigs.
        # glTF import creates an unrendered Icosphere bone-display widget.
        # It must not frame centimetre-sized animals as a two-metre object.
        meshes=[o for o in bpy.data.objects if o.type=='MESH' and o.find_armature() and not o.hide_render]
        render(meshes,dest/(name+suffix+'.png'))
    print('FINAL_REVIEW',name,flush=True)
(dest/'rendered.json').write_text(json.dumps(names,indent=2))
