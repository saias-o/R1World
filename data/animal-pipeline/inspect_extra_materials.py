import bpy
import json
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
inspections = json.loads((ROOT / 'data/animal-pipeline/oga-inspection.json').read_text())
out = {}
for entry in inspections:
    if entry['id'] in {'gorilla', 'bestiary'}:
        continue
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / entry['file']), load_ui=False, use_scripts=False)
    out[entry['id']] = {'materials': [], 'rigs': []}
    for m in bpy.data.materials:
        out[entry['id']]['materials'].append({'name': m.name, 'diffuse': list(m.diffuse_color), 'nodes': [(n.name, n.type, n.image.name if n.type == 'TEX_IMAGE' and n.image else None) for n in m.node_tree.nodes] if m.use_nodes else [], 'slots': []})
    for o in bpy.data.objects:
        if o.type == 'ARMATURE':
            out[entry['id']]['rigs'].append({'name':o.name,'action':o.animation_data.action.name if o.animation_data and o.animation_data.action else None,'tracks':[(t.name,[s.action.name if s.action else None for s in t.strips]) for t in o.animation_data.nla_tracks] if o.animation_data else []})
(ROOT / 'data/animal-pipeline/oga-materials.json').write_text(json.dumps(out,indent=2))
