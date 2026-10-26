"""File and numerical skinning smoke tests. Never launches the game or a UI."""
import json
from pathlib import Path
import numpy as np
from glb_tools import read, accessor, globals_for, animation_overrides, vertices_for

ROOT = Path(__file__).resolve().parents[2]
ASSETS = ROOT / 'game/assets/animals'


def inspect(path):
    doc,binary = read(path)
    assert all('uri' not in b for b in doc.get('buffers',[])), 'external buffer'
    assert all('bufferView' in i and 'uri' not in i for i in doc.get('images',[])), 'external image'
    assert doc.get('skins'), 'missing skin'
    assert not doc.get('extensionsRequired'), 'requires non-core glTF extensions'
    triangles = vertices = 0
    for m in doc['meshes']:
        for p in m['primitives']:
            assert p.get('mode',4)==4
            attrs = p['attributes']
            assert {'JOINTS_0','WEIGHTS_0','POSITION','NORMAL'} <= set(attrs)
            vertices += doc['accessors'][attrs['POSITION']]['count']
            triangles += doc['accessors'][p['indices']]['count']//3
    rest = vertices_for(doc,binary,globals_for(doc,{}))
    extent = np.ptp(rest,axis=0)
    assert np.isfinite(rest).all() and extent.max()>1e-6
    clips = []
    for clip in doc.get('animations',[]):
        duration = max(accessor(doc,binary,s['input']).max() for s in clip['samplers'])
        samples = []
        max_extent = 0.0
        for fraction in [0.0,0.25,0.5,0.75,1.0]:
            cloud = vertices_for(doc,binary,globals_for(doc,animation_overrides(doc,binary,clip,duration*fraction)))
            assert np.isfinite(cloud).all()
            max_extent=max(max_extent,float(np.ptp(cloud,axis=0).max()))
            samples.append(cloud)
        movement = max(float(np.linalg.norm(s-samples[0],axis=1).max()) for s in samples)
        ratio = max_extent/extent.max()
        assert ratio < 5.0, f"exploding animation {clip['name']}: {ratio}"
        clips.append({'name':clip.get('name'),'duration':float(duration),'max_displacement_m':movement,'bounds_ratio':float(ratio)})
    return {'file':path.name,'bytes':path.stat().st_size,'triangles':triangles,'vertices':vertices,'bones':sum(len(s['joints']) for s in doc['skins']),'materials':len(doc.get('materials',[])),'textures':len(doc.get('images',[])),'dimensions_m':extent.tolist(),'clips':clips}


if __name__ == '__main__':
    results,errors = [],[]
    for path in sorted(ASSETS.glob('*.glb')):
        try:
            results.append(inspect(path))
        except Exception as error:
            errors.append({'file':path.name,'reason':str(error)})
    by_file={r['file']:r for r in results}
    for near in [r for r in results if '-lod1.glb' not in r['file']]:
        lod=by_file.get(near['file'].replace('.glb','-lod1.glb'))
        if lod is None:
            errors.append({'file':near['file'],'reason':'missing LOD1'})
            continue
        for condition,message in [(lod['triangles']<near['triangles'],'LOD not reduced'),(lod['bones']==near['bones'],'LOD skeleton changed'),([c['name'] for c in lod['clips']]==[c['name'] for c in near['clips']],'LOD clips differ')]:
            if not condition:
                errors.append({'file':near['file'],'reason':message})
    report={'files':results,'errors':errors}
    (ROOT/'data/animal-pipeline/review/smoke.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({'files':len(results),'errors':errors},indent=2))
    raise SystemExit(bool(errors))
