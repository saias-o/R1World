"""Compact embedded textures and flatten skinned mesh carriers, in place.

Original source assets remain untouched. Geometry/weights/animations are copied
byte-for-byte; LOD1 textures are capped at 512 px, LOD0 at 1024 px.
"""
import io
import json
import xml.etree.ElementTree as ET
from pathlib import Path
from PIL import Image,ImageFile
ImageFile.MAXBLOCK=max(ImageFile.MAXBLOCK,4*1024*1024)
from glb_tools import read,write

ROOT=Path(__file__).resolve().parents[2]
ASSETS=ROOT/'game/assets/animals'


def optimize(path):
    doc,binary=read(path)
    if doc.get('asset',{}).get('extras',{}).get('r1_animal_texture_revision')==3:
        return None
    old_size=path.stat().st_size
    normal_images=set()
    for mat in doc.get('materials',[]):
        for key in ['normalTexture','occlusionTexture']:
            if key in mat:
                normal_images.add(doc['textures'][mat[key]['index']]['source'])
    by_view={im['bufferView']:(i,im) for i,im in enumerate(doc.get('images',[]))}
    result=bytearray()
    image_info=[]
    lod='-lod1.glb' in path.name
    ident=path.stem.removesuffix('-lod1')
    report_path=ROOT/'data/animal-pipeline/review'/(ident+'.json')
    opaque_images=set()
    if report_path.exists():
        report=json.loads(report_path.read_text())
        if 'source_actor' in report:
            actor=ET.parse(ROOT/'data/source-assets/animals/0ad'/report['source_actor']).getroot()
            material=actor.findtext('material','')
            if 'no_trans' in material:
                for mat in doc.get('materials',[]):
                    mat['alphaMode']='OPAQUE'
                    mat.pop('alphaCutoff',None)
                    base=mat.get('pbrMetallicRoughness',{}).get('baseColorTexture')
                    if base:
                        opaque_images.add(doc['textures'][base['index']]['source'])
    for view_id,view in enumerate(doc['bufferViews']):
        data=binary[view.get('byteOffset',0):view.get('byteOffset',0)+view['byteLength']]
        if view_id in by_view:
            image_index,record=by_view[view_id]
            with Image.open(io.BytesIO(data)) as raw:
                image=raw.convert('RGBA')
            limit=512 if lod else 1024
            if path.name.startswith('turtle_pond'):
                limit=(128 if lod else 256) if image_index in normal_images else (256 if lod else 512)
            if max(image.size)>limit:
                image.thumbnail((limit,limit),Image.Resampling.LANCZOS)
            # Remove nonportable PNG ICC/chromaticity chunks on all textures.
            clean=Image.frombytes('RGBA',image.size,image.tobytes())
            buf=io.BytesIO()
            opaque=image_index in opaque_images or clean.getchannel('A').getextrema()==(255,255)
            if opaque and image_index not in normal_images:
                clean.convert('RGB').save(buf,format='JPEG',quality=86 if lod else 93,subsampling=0,optimize=True)
                record['mimeType']='image/jpeg'
            else:
                (clean.convert('RGB') if opaque else clean).save(buf,format='PNG',optimize=True)
                record['mimeType']='image/png'
            data=buf.getvalue()
            image_info.append({'index':image_index,'size':list(clean.size),'bytes':len(data),'mimeType':record['mimeType']})
        result.extend(b'\0'*((-len(result))%4))
        view['byteOffset']=len(result)
        view['byteLength']=len(data)
        result.extend(data)
    # Joint global transforms carry the skin. A skinned mesh transform is
    # ignored by glTF; root mesh nodes avoid ambiguous parent-transform warnings.
    skinned={i for i,n in enumerate(doc.get('nodes',[])) if 'mesh' in n and 'skin' in n}
    assert not any(c['target']['node'] in skinned for a in doc.get('animations',[]) for c in a['channels'])
    for node in doc.get('nodes',[]):
        if 'children' in node:
            node['children']=[i for i in node['children'] if i not in skinned]
    for scene in doc.get('scenes',[]):
        scene['nodes']=list(dict.fromkeys(scene.get('nodes',[])+sorted(skinned)))
    for i in skinned:
        for key in ['translation','rotation','scale','matrix']:
            doc['nodes'][i].pop(key,None)
    for mat in doc.get('materials',[]):
        if mat.get('alphaMode')=='BLEND':
            mat['alphaMode']='MASK'
            mat['alphaCutoff']=0.45
    doc['asset'].setdefault('extras',{})['r1_animal_texture_revision']=3
    write(path,doc,bytes(result))
    return {'file':path.name,'before':old_size,'after':path.stat().st_size,'images':image_info}


if __name__=='__main__':
    reports=[]
    for path in sorted(ASSETS.glob('*.glb')):
        r=optimize(path)
        if r:
            reports.append(r)
    (ROOT/'data/animal-pipeline/review/texture-optimization.json').write_text(json.dumps(reports,indent=2),encoding='utf-8')
    print('Optimized',len(reports),'GLBs; before',sum(r['before'] for r in reports),'after',sum(r['after'] for r in reports))
