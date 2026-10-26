"""Restore invalid animation of unused Collada IK controls to rest values.

Never repairs weighted joints or their ancestors. Such errors must fail loudly.
"""
import json
from pathlib import Path
import numpy as np
from glb_tools import read,write,accessor,COMPONENT,WIDTH
ROOT=Path(__file__).resolve().parents[2]
reports=[]
for path in sorted((ROOT/'game/assets/animals').glob('*.glb')):
    doc,binary=read(path)
    used=set()
    for node in doc['nodes']:
        if 'mesh' not in node or 'skin' not in node:
            continue
        skin=doc['skins'][node['skin']]
        for p in doc['meshes'][node['mesh']]['primitives']:
            attrs=p['attributes']
            js=accessor(doc,binary,attrs['JOINTS_0']).astype(int)
            ws=accessor(doc,binary,attrs['WEIGHTS_0'])
            used.update(skin['joints'][i] for i in np.unique(js[ws>0.000001]))
    parents={c:i for i,n in enumerate(doc['nodes']) for c in n.get('children',[])}
    for node in list(used):
        while node in parents:
            node=parents[node]
            used.add(node)
    corrected=[]
    mutable=bytearray(binary)
    for clip in doc.get('animations',[]):
        for channel in clip['channels']:
            index=clip['samplers'][channel['sampler']]['output']
            a=doc['accessors'][index]
            view=doc['bufferViews'][a['bufferView']]
            dtype=np.dtype(COMPONENT[a['componentType']])
            width=WIDTH[a['type']]
            values=np.ndarray((a['count'],width),dtype=dtype,buffer=mutable,offset=view.get('byteOffset',0)+a.get('byteOffset',0),strides=(view.get('byteStride',width*dtype.itemsize),dtype.itemsize))
            if np.isfinite(values).all():
                continue
            target=channel['target']
            assert target['node'] not in used, f"Invalid animated weighted joint: {path.name}, {target}"
            defaults={'translation':[0,0,0],'rotation':[0,0,0,1],'scale':[1,1,1]}
            assert target['path'] in defaults
            rest=doc['nodes'][target['node']].get(target['path'],defaults[target['path']])
            values[:]=rest
            corrected.append({'clip':clip['name'],'node':doc['nodes'][target['node']]['name'],'path':target['path'],'reason':'nonfinite Collada IK helper, no vertex influence and no weighted descendant; restored rest transform'})
    if corrected:
        write(path,doc,bytes(mutable))
        reports.append({'file':path.name,'repairs':corrected})
report_file=ROOT/'data/animal-pipeline/review/control-track-repairs.json'
history=json.loads(report_file.read_text()) if report_file.exists() else []
merged={r['file']:r for r in history}
merged.update({r['file']:r for r in reports})
report_file.write_text(json.dumps(list(merged.values()),indent=2),encoding='utf-8')
print('Repaired unused control tracks in',len(reports),'files')
