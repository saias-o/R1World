"""Remove redundant rest channels and unreferenced accessors/buffer views.

Only removes TRS channels that stay at the node's default at every keyframe.
Every geometry attribute, skin and nonconstant animation is preserved verbatim.
"""
import json
from pathlib import Path
import numpy as np
from glb_tools import read,write,accessor
ROOT=Path(__file__).resolve().parents[2]
reports=[]
for path in sorted((ROOT/'game/assets/animals').glob('*.glb')):
    doc,binary=read(path)
    before=path.stat().st_size
    removed=0
    defaults={'translation':[0,0,0],'rotation':[0,0,0,1],'scale':[1,1,1]}
    clips=[]
    for clip in doc.get('animations',[]):
        channels=[]
        for channel in clip['channels']:
            target=channel['target']
            sampler=clip['samplers'][channel['sampler']]
            if target['path'] in defaults and sampler.get('interpolation','LINEAR') in {'LINEAR','STEP'}:
                values=accessor(doc,binary,sampler['output'])
                expected=doc['nodes'][target['node']].get(target['path'],defaults[target['path']])
                if np.all(np.abs(values-np.array(expected))<1e-7):
                    removed+=1
                    continue
            channels.append(channel)
        if not channels:
            # Preserve a valid named static pose clip as one constant channel.
            channels=[clip['channels'][0]]
            removed-=1
        indices=sorted({c['sampler'] for c in channels})
        remap={old:new for new,old in enumerate(indices)}
        clip['samplers']=[clip['samplers'][i] for i in indices]
        for c in channels:
            c['sampler']=remap[c['sampler']]
        clip['channels']=channels
        clips.append(clip)
    doc['animations']=clips
    used=set()
    for m in doc['meshes']:
        for p in m['primitives']:
            used.update(p['attributes'].values())
            if 'indices' in p: used.add(p['indices'])
            for target in p.get('targets',[]): used.update(target.values())
    used.update(s['inverseBindMatrices'] for s in doc.get('skins',[]) if 'inverseBindMatrices' in s)
    used.update(s[k] for a in clips for s in a['samplers'] for k in ['input','output'])
    indices=sorted(used)
    remap={old:new for new,old in enumerate(indices)}
    doc['accessors']=[doc['accessors'][i] for i in indices]
    for m in doc['meshes']:
        for p in m['primitives']:
            p['attributes']={k:remap[v] for k,v in p['attributes'].items()}
            if 'indices' in p: p['indices']=remap[p['indices']]
            for target in p.get('targets',[]):
                for key in target: target[key]=remap[target[key]]
    for skin in doc.get('skins',[]):
        if 'inverseBindMatrices' in skin: skin['inverseBindMatrices']=remap[skin['inverseBindMatrices']]
    for a in clips:
        for s in a['samplers']:
            for key in ['input','output']: s[key]=remap[s[key]]
    assert not any('sparse' in a for a in doc['accessors'])
    used_views=sorted({a['bufferView'] for a in doc['accessors']}|{i['bufferView'] for i in doc.get('images',[])})
    remap={old:new for new,old in enumerate(used_views)}
    new_binary=bytearray()
    views=[]
    for old in used_views:
        v=doc['bufferViews'][old].copy()
        data=binary[v.get('byteOffset',0):v.get('byteOffset',0)+v['byteLength']]
        new_binary.extend(b'\0'*((-len(new_binary))%4))
        v['byteOffset']=len(new_binary)
        new_binary.extend(data)
        views.append(v)
    doc['bufferViews']=views
    for a in doc['accessors']: a['bufferView']=remap[a['bufferView']]
    for i in doc.get('images',[]): i['bufferView']=remap[i['bufferView']]
    write(path,doc,bytes(new_binary))
    reports.append({'file':path.name,'removed_rest_channels':removed,'before':before,'after':path.stat().st_size})
(ROOT/'data/animal-pipeline/review/animation-compaction.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
print('Animation compaction:',sum(r['before'] for r in reports),'->',sum(r['after'] for r in reports),'bytes')
