"""Bake licensed road vehicles into opaque PBR GLBs with two geometry LODs.

python -m r1.vehicle_imports --blender /path/to/blender
The pinned source archives in data/source-assets/vehicles make builds offline.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import zipfile
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
SOURCES = GAME.parent / 'data/source-assets/vehicles'
IMPORTS = {
    'city': ('fordFocus', 'blendswap-cc-by', 'Ruff'),
    'sedan': ('break', 'scopia', 'Scopia Visual Interfaces Systems, s.l. / Space Mushrooms'),
    'suv': ('suv2', 'scopia', 'Scopia Visual Interfaces Systems, s.l. / Space Mushrooms'),
    'offroad': ('4x4', 'scopia', 'Scopia Visual Interfaces Systems, s.l. / Space Mushrooms'),
    'sport': ('sportive', 'scopia', 'Scopia Visual Interfaces Systems, s.l. / Space Mushrooms'),
    'truck': ('truck', 'scopia', 'Scopia Visual Interfaces Systems, s.l. / Space Mushrooms'),
    'bus': ('cityBus', 'scopia', 'Scopia Visual Interfaces Systems, s.l. / Space Mushrooms'),
}

MOTORCYCLES = {
    'moto_city': ('road-motorcycle', 2.118, .745, 1.363, 1.45, .327),
    'moto_cross': ('dirt-motorcycle', 2.264, .626, 1.218, 1.48, .35),
}


def _glb_stats(path: Path) -> dict:
    payload=path.read_bytes()
    length,kind=struct.unpack_from('<II',payload,12)
    if kind!=0x4e4f534a:
        raise ValueError(f'Not a GLB file: {path}')
    doc=json.loads(payload[20:20+length])
    vertices=sum(doc['accessors'][primitive['attributes']['POSITION']]['count']
                 for mesh in doc.get('meshes',[]) for primitive in mesh['primitives'])
    triangles=sum(doc['accessors'][primitive['indices']]['count']//3
                   for mesh in doc.get('meshes',[]) for primitive in mesh['primitives']
                   if 'indices' in primitive)
    return {'vertices':vertices,'triangles':triangles,'drawTriangles':triangles}


def _motorcycle_entries():
    source_path=SOURCES/'motorcycles'/'sources.json'
    sources=json.loads(source_path.read_text(encoding='utf-8'))
    entries=[]
    for name,(source_key,length,width,height,wheelbase,wheel_radius) in MOTORCYCLES.items():
        source=sources[source_key]
        raw=SOURCES/'motorcycles'/source['file']
        digest=hashlib.sha256(raw.read_bytes()).hexdigest()
        if digest!=source['sha256']:
            raise ValueError(f'Motorcycle source changed: {raw}')
        entry={'name':name,'author':source['author'],'license':'CC0-1.0',
            'sourceUrl':source['url'],'sourceSha256':digest,
            'length':length,'width':width,'height':height,'wheelbase':wheelbase,
            'wheelRadius':wheel_radius}
        for level,suffix in (('near',''),('far','_far')):
            model_path=f'assets/models/vehicles/{name}{suffix}.glb'
            entry[level]={'path':model_path,**_glb_stats(GAME/'assets/models/vehicles'/f'{name}{suffix}.glb')}
        entries.append(entry)
    return entries

def provenance_records():
    records=[]
    for collection,author,source in (
        ('blendswap-cc-by','Ruff','https://www.blendswap.com/blends/view/76275'),
        ('scopia','Scopia Visual Interfaces Systems, s.l. / Space Mushrooms','https://www.sweethome3d.com/free-3d-models/')):
        names=[name for name,(_,group,_) in IMPORTS.items() if group==collection]
        records.append({'name':'Road vehicles — '+author,'author':author,'source':source,
            'license':'CC BY 3.0','licenseFile':'assets/licenses/Road-vehicles-CC-BY-3.0.txt',
            'generatedBy':'game/tools/r1/vehicle_imports.py',
            'modifications':'Converted to metres and glTF; unbranded, opaque PBR materials, reduced geometry LODs, independent wheel pivots; source shading and UV detail retained.',
            'files':[f'assets/models/vehicles/{name}{suffix}.glb' for name in names for suffix in ('','_far')]})
    sources=json.loads((SOURCES/'motorcycles'/'sources.json').read_text(encoding='utf-8'))
    for name,(source_key,*_) in MOTORCYCLES.items():
        source=sources[source_key]
        records.append({'name':source['name'],'author':source['author'],
            'source':source['url'],'license':'CC0 1.0',
            'licenseFile':'assets/licenses/CC0-1.0.txt',
            'generatedBy':'game/tools/r1/vehicle_imports.py',
            'modifications':'Source GLB resized and decimated into Near/Far LODs, oriented for R1World, with independent wheel pivots and embedded textures.',
            'files':[f'assets/models/vehicles/{name}{suffix}.glb' for suffix in ('','_far')]})
    return records

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--blender', default=shutil.which('blender') or
                        'C:/Program Files/Blender Foundation/Blender 4.2/blender.exe')
    args=parser.parse_args()
    pinned=json.loads((SOURCES/'sources.json').read_text(encoding='utf-8'))
    work=GAME/'generated/vehicle-imports';work.mkdir(parents=True,exist_ok=True)
    jobs=[]
    for name,(asset,collection,author) in IMPORTS.items():
        archive=SOURCES/(asset+'.zip')
        if hashlib.sha256(archive.read_bytes()).hexdigest()!=pinned[asset]['sha256']:
            raise ValueError(f'Source archive changed: {archive}')
        target=work/asset;target.mkdir(exist_ok=True)
        with zipfile.ZipFile(archive) as z:
            # Only the checked-in, hash-pinned archives are extracted.
            if any(not (target/entry).resolve().is_relative_to(target.resolve()) for entry in z.namelist()):
                raise ValueError(f'Unsafe archive path in {asset}')
            z.extractall(target)
        jobs.append({'name':name,'source':str(next(target.rglob('*.obj'))),
                     'author':author,'license':'CC-BY-3.0','sourceUrl':pinned[asset]['url'],
                     'sourceSha256':pinned[asset]['sha256']})
    config=work/'jobs.json';config.write_text(json.dumps(jobs,indent=2),encoding='utf-8')
    subprocess.run([args.blender,'--background','--threads','2','--python-exit-code','1','--python',
                    str(Path(__file__).with_name('vehicle_imports_blender.py')),
                    '--',str(config)],check=True)
    output=work/'output'
    manifest=json.loads((output/'fleet.json').read_text(encoding='utf-8'))
    for source in output.glob('*.glb'):
        target=GAME/'assets/models/vehicles'/source.name
        temporary=target.with_suffix(target.suffix+'.tmp')
        shutil.copyfile(source,temporary)
        temporary.replace(target)
    subprocess.run([args.blender,'--background','--threads','2','--python-exit-code','1','--python',
                    str(Path(__file__).with_name('motorcycles_blender.py'))],check=True)
    manifest['vehicles'].extend(_motorcycle_entries())
    manifest['description']=('Licensed road vehicles and CC0 online motorcycles; '
        'opaque PBR materials, independent wheel pivots, and Near/Far geometry LODs.')
    manifest['totalVertices']=sum(vehicle[level]['vertices'] for vehicle in manifest['vehicles']
                                  for level in ('near','far'))
    target=GAME/'assets/models/vehicles/fleet.json'
    target.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    from r1.motorcycle_detail import update_manifest
    update_manifest()

if __name__=='__main__':main()
