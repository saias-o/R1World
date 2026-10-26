import bpy
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2] / 'data/source-assets/animals/0ad'
bpy.ops.wm.read_factory_settings(use_empty=True)
for p in [root / 'meshes/skeletal/wolf.dae', root / 'animation/quadraped/wolf_walk.dae']:
    if not p.exists():
        continue
    before = set(bpy.data.objects)
    bpy.ops.wm.collada_import(filepath=str(p))
    for o in set(bpy.data.objects) - before:
        print('INSPECT', p.name, o.name, o.type, list(o.scale), list(o.dimensions))
        if o.type == 'ARMATURE':
            print('BONES', [(b.name, list(b.head_local)) for b in o.data.bones][:35])
            print('ACTION', o.animation_data.action.name if o.animation_data and o.animation_data.action else None)
    print('ACTIONS', [(a.name, len(a.fcurves), list(a.frame_range)) for a in bpy.data.actions])
