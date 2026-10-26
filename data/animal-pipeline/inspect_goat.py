import bpy
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]/'data/source-assets/animals/0ad'
for p in ['meshes/skeletal/goat.dae','animation/quadraped/goat_walk.dae']:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.collada_import(filepath=str(ROOT/p),import_units=True)
    for o in bpy.data.objects:
        if o.type=='ARMATURE':
            print('GOAT_RIG',p,o.name,'scale',list(o.scale),'mat',list(o.matrix_world),'bones',len(o.data.bones))
            print('GOAT_BONES',[(b.name,list(b.head_local),list(b.matrix_local.to_scale())) for b in list(o.data.bones)[:5]])
            if o.animation_data and o.animation_data.action:
                a=o.animation_data.action
                print('GOAT_ACTION',[(f.data_path,f.array_index,min(k.co.y for k in f.keyframe_points),max(k.co.y for k in f.keyframe_points)) for f in a.fcurves][:35])
