import bpy
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'data/animal-pipeline'
records = []
for folder in sorted((ROOT / 'data/source-assets/animals/opengameart').iterdir()):
    if not folder.is_dir():
        continue
    files = list(folder.rglob('*.blend'))
    if folder.name == 'wasp':
        files = [p for p in files if 'Hornet6' in p.name]
    for source in files:
        bpy.ops.wm.open_mainfile(filepath=str(source), load_ui=False, use_scripts=False)
        meshes = [{'name': o.name, 'triangles': sum(len(p.vertices)-2 for p in o.data.polygons), 'rig': o.find_armature().name if o.find_armature() else None, 'materials': [s.material.name if s.material else None for s in o.material_slots], 'visible': not o.hide_render} for o in bpy.data.objects if o.type == 'MESH']
        record = {'id': folder.name, 'file': str(source.relative_to(ROOT)), 'meshes': meshes, 'rigs': [{'name': o.name, 'bones': len(o.data.bones)} for o in bpy.data.objects if o.type == 'ARMATURE'], 'images': [{'name': i.name, 'path': i.filepath, 'packed': bool(i.packed_file), 'size': list(i.size)} for i in bpy.data.images if i.type == 'IMAGE'], 'actions': [{'name': a.name, 'range': list(a.frame_range), 'curves': len(a.fcurves)} for a in bpy.data.actions]}
        records.append(record)
(OUT / 'oga-inspection.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
print('INSPECTED', len(records), flush=True)
