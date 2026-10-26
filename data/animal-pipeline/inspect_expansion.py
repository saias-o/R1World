"""Background candidate renders and measurements before any selection."""
import json
from pathlib import Path
import sys
import traceback
import bpy
from mathutils import Vector
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA,bounds,render,quiet

DEST=QA/'expansion'
DEST.mkdir(exist_ok=True)
records=json.loads((ROOT/'data/source-assets/animals/expansion/manifest.json').read_text())
for candidate in ['shrike','brown_bird','snail']:
    folder=ROOT/'data/source-assets/animals/opengameart'/candidate
    p=folder/'provenance.json'
    if p.exists():
        r=json.loads(p.read_text())
        files=list(folder.rglob('*.blend'))
        if files:
            r.update({'source_url':r['source'],'source_file':str(files[0].relative_to(ROOT))})
            records.append(r)
out=[]
for record in records:
    if '--' in sys.argv and record['id'] not in sys.argv[sys.argv.index('--')+1:]:
        continue
    try:
        path=ROOT/record['source_file']
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if path.suffix.lower()=='.blend':
            bpy.ops.wm.open_mainfile(filepath=str(path),load_ui=False,use_scripts=False)
            if bpy.context.object and bpy.context.object.mode!='OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')
            for obj in bpy.data.objects:
                if obj.name not in bpy.context.scene.objects:
                    bpy.context.scene.collection.objects.link(obj)
        else:
            with quiet(): bpy.ops.import_scene.gltf(filepath=str(path))
        meshes=[o for o in bpy.context.scene.objects if o.type=='MESH' and len(o.data.polygons)>0 and not any(x in o.name.lower() for x in ['icosphere','floor','ground','plane','preview','widget'])]
        if not meshes:
            raise ValueError('No candidate mesh')
        for o in bpy.data.objects:
            o.hide_render=o.type!='MESH' or o not in meshes
        for c in bpy.data.collections:
            c.hide_render=False
        if bpy.context.scene.world is None:
            bpy.context.scene.world=bpy.data.worlds.new('world')
        # Disable high-cost particles and subdivision in candidate review.
        for o in meshes:
            o.hide_set(False)
            o.hide_render=False
            for mod in o.modifiers:
                if mod.type in {'SUBSURF','PARTICLE_SYSTEM','NODES'}:
                    mod.show_render=False
                    mod.show_viewport=False
        lo,hi=bounds(meshes)
        record['source_dimensions']=list(hi-lo)
        record['meshes']=[{'name':o.name,'triangles':sum(len(p.vertices)-2 for p in o.data.polygons),'rig':o.find_armature().name if o.find_armature() else None,'materials':[s.material.name if s.material else None for s in o.material_slots]} for o in meshes]
        record['rigs']=[{'name':o.name,'bones':len(o.data.bones)} for o in bpy.data.objects if o.type=='ARMATURE']
        record['actions']=[{'name':a.name,'curves':len(a.fcurves),'frames':list(a.frame_range)} for a in bpy.data.actions]
        record['images']=[{'name':i.name,'file':i.filepath,'size':list(i.size),'packed':bool(i.packed_file)} for i in bpy.data.images if i.type=='IMAGE']
        record['status']='inspected'
        # Show geometry at manageable complexity, preserve all source textures.
        for o in meshes:
            count=sum(len(p.vertices)-2 for p in o.data.polygons)
            if count>14000 and not o.find_armature():
                mod=o.modifiers.new('candidate_preview_only','DECIMATE')
                mod.ratio=14000/count
        render(meshes,DEST/(record['id']+'.png'))
    except Exception as error:
        record.update({'status':'error','reason':str(error),'traceback':traceback.format_exc()})
    (DEST/(record['id']+'.json')).write_text(json.dumps(record,indent=2),encoding='utf-8')
    out.append(record)
    print('INSPECT',record['id'],record['status'],record.get('reason',''),flush=True)
(DEST/'inspection.json').write_text(json.dumps(out,indent=2),encoding='utf-8')
