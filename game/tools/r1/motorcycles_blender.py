"""Rebake the pinned CC0 motorcycles with smooth, bounded Near/Far geometry.

Run with Blender --background --python motorcycles_blender.py. Original files
remain untouched. Decimation preserves material/UV seams and wheel silhouettes;
tiny screws/tread lugs are reduced before the body panels and tyres.
"""
from pathlib import Path
import json
import math
import bpy
from mathutils import Vector, Matrix

GAME = Path(__file__).resolve().parents[2]
SOURCE = GAME.parent / 'data/source-assets/vehicles/motorcycles'
OUTPUT = GAME / 'assets/models/vehicles'
WORK = GAME / 'generated/motorcycle-validation'
WORK.mkdir(parents=True, exist_ok=True)


def reset():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    for collection in (bpy.data.meshes, bpy.data.materials, bpy.data.images):
        for item in list(collection):
            if item.users == 0:
                collection.remove(item)


def bounds(objects):
    points = [o.matrix_world @ v.co for o in objects for v in o.data.vertices]
    return Vector([min(p[i] for p in points) for i in range(3)]), Vector([max(p[i] for p in points) for i in range(3)])


def photograph(path, label):
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 32
    scene.cycles.use_denoising = True
    scene.render.resolution_x = 1100
    scene.render.resolution_y = 720
    scene.render.resolution_percentage = 100
    scene.world.color = (.25, .25, .25)
    scene.view_settings.view_transform = 'AgX'
    bpy.ops.mesh.primitive_plane_add(size=200, location=(0,0,-.002))
    floor = bpy.context.object
    mat = bpy.data.materials.new('Studio floor')
    mat.diffuse_color = (.10, .12, .14, 1)
    floor.data.materials.append(mat)
    for pos, energy, size in [((2,-4,5),850,4),((-3,0,3),600,3),((1,3,4),1000,3)]:
        bpy.ops.object.light_add(type='AREA', location=pos)
        bpy.context.object.data.energy = energy
        bpy.context.object.data.shape = 'DISK'
        bpy.context.object.data.size = size
        bpy.context.object.rotation_euler = (Vector((0,0,.7))-bpy.context.object.location).to_track_quat('-Z','Y').to_euler()
    bpy.ops.object.camera_add(location=(3.0,-3.8,2.0))
    camera=bpy.context.object
    camera.rotation_euler=(Vector((0,0,.65))-camera.location).to_track_quat('-Z','Y').to_euler()
    camera.data.type='ORTHO'
    camera.data.ortho_scale=3.1
    scene.camera=camera
    scene.render.filepath=str(path)
    bpy.ops.render.render(write_still=True)


def rebake(name, source_name, far):
    reset()
    bpy.ops.import_scene.gltf(filepath=str(SOURCE / source_name))
    objects=[o for o in bpy.context.scene.objects if o.type=='MESH']
    # Sources are metre-scale, glTF +Z forward (Blender -Y). Bake transforms
    # before grouping so the output's only transforms are the mechanical pivots.
    for o in objects:
        world=o.matrix_world.copy()
        if name=='moto_cross':world=Matrix.Rotation(-math.pi/2,4,'Z') @ world
        o.parent=None
        if o.data.users>1:o.data=o.data.copy()
        o.data.transform(world)
        o.matrix_world=Matrix.Identity(4)
        o.data.update()
    bpy.context.view_layer.update()
    low,high=bounds(objects)
    print(name, 'source bounds', list(low), list(high), 'objects', len(objects))
    # The source has hundreds of seated fasteners. They are invisible at game
    # scale and cost more triangles than the recognisable silhouette.
    for o in list(objects):
        n=o.name.lower()
        tiny=any(word in n for word in ('fixing','nipple','bolt','hex head','seated shank'))
        if tiny:
            objects.remove(o)
            bpy.data.objects.remove(o, do_unlink=True)
    wheel_objects={}
    for side in ('front','rear'):
        keys=('tire','tyre','rim','hub','spoke','rotor','brake disc','bearing','through axle','tread block')
        wheel_objects[side]=[o for o in objects if o.name.lower().startswith(side+' ') and any(k in o.name.lower() for k in keys)]
        if not wheel_objects[side]:
            raise ValueError('Missing wheel assembly: '+name+' '+side)
    centres={}
    for side, parts in wheel_objects.items():
        tyres=[o for o in parts if 'tyre' in o.name.lower() or 'tire' in o.name.lower()]
        lo,hi=bounds(tyres or parts)
        centres[side]=(lo+hi)*.5
        print(name, side, 'wheel centre', list(centres[side]))
    # Smooth normals on curved source surfaces survive the collapse. Flat panels
    # retain their authored normals. Never turn the whole vehicle flat shaded.
    for o in objects:
        bpy.context.view_layer.objects.active=o
        n=o.name.lower()
        curved=any(k in n for k in ('tire','tyre','rim','fork','stanchion','slider','tank','exhaust','hub'))
        if curved:
            for polygon in o.data.polygons:
                polygon.use_smooth=True
        triangles=sum(len(p.vertices)-2 for p in o.data.polygons)
        if triangles>36:
            mod=o.modifiers.new('Silhouette reduction','DECIMATE')
            if name=='moto_cross':
                if 'tyre' in n or 'tire' in n:ratio=.012 if far else .045
                elif 'rim' in n:ratio=.035 if far else .12
                elif 'spring' in n:ratio=.015 if far else .055
                elif any(k in n for k in ('mudguard','body cowl','fuel tank','number panel')):ratio=.08 if far else .38
                else:ratio=.025 if far else .10
            else:ratio=(.12 if far else .6) if curved else (.10 if far else .42)
            mod.ratio=ratio
            mod.use_collapse_triangulate=True
            bpy.ops.object.modifier_apply(modifier=mod.name)
    # One primitive per material and moving assembly, not per original screw.
    wheels=set(wheel_objects['front']+wheel_objects['rear'])
    assemblies={'front':wheel_objects['front'],'rear':wheel_objects['rear'],
                'body':[o for o in objects if o not in wheels]}
    for assembly, parts in assemblies.items():
        bpy.ops.object.select_all(action='DESELECT')
        for o in parts:o.select_set(True)
        bpy.context.view_layer.objects.active=parts[0]
        bpy.ops.object.join()
        joined=bpy.context.object
        joined.name='wheel-'+assembly if assembly!='body' else 'Motorcycle body'
        if assembly!='body':
            bpy.context.scene.cursor.location=centres[assembly]
            bpy.ops.object.origin_set(type='ORIGIN_CURSOR')
        # Welding material duplicates lets glTF coalesce primitives.
        materials=[]
        remap={}
        for i,mat in enumerate(joined.data.materials):
            if mat not in materials:materials.append(mat)
            remap[i]=materials.index(mat)
        indices=[remap[p.material_index] for p in joined.data.polygons]
        joined.data.materials.clear()
        for mat in materials:joined.data.materials.append(mat)
        for p,index in zip(joined.data.polygons,indices):p.material_index=index
    combined=[o for o in bpy.context.scene.objects if o.type=='MESH']
    count=sum(len(p.vertices)-2 for o in combined for p in o.data.polygons)
    budget=1800 if far else (6500 if name=='moto_cross' else 9000)
    if count>budget:
        for o in combined:
            bpy.context.view_layer.objects.active=o
            mod=o.modifiers.new('Final geometry budget','DECIMATE')
            mod.ratio=budget/count
            mod.use_collapse_triangulate=True
            bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.object.select_all(action='SELECT')
    for image in bpy.data.images:
        if image.source=='FILE' and image.size[0] and max(image.size)>(128 if far else 512):
            target_size=128 if far else 512
            ratio=target_size/max(image.size)
            image.scale(max(1,round(image.size[0]*ratio)),max(1,round(image.size[1]*ratio)))
    target=OUTPUT/(name+('_far' if far else '')+'.glb')
    bpy.ops.export_scene.gltf(filepath=str(target),export_format='GLB',use_selection=True,
        export_animations=False,export_yup=True,export_materials='EXPORT',export_extras=True)
    if not far:photograph(WORK/(name+'-after.png'),name)


if __name__=='__main__':
    for name,source in [('moto_city','road-motorcycle.glb'),('moto_cross','dirt-motorcycle.glb')]:
        if not (WORK/(name+'-before.png')).exists():
            reset()
            bpy.ops.import_scene.gltf(filepath=str(OUTPUT/(name+'.glb')))
            photograph(WORK/(name+'-before.png'),name)
        for far in (False,True):rebake(name,source,far)
    # Manifest/provenance are refreshed by r1.motorcycle_detail after this script.
