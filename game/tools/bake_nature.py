"""Bake layered vegetation cards from the original CC0 scans with Blender 4.2.
Run: blender --background --python game/tools/bake_nature.py
The original assets are never modified. Each slice preserves scanned albedo/alpha.
"""
import bpy, math, json, sys, hashlib
from pathlib import Path
from mathutils import Vector
import numpy as np
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from r1.external_assets import TREES, SOURCE_ROOT
TREES=(*TREES,(SOURCE_ROOT,"scene","urban_tree"))
OUT=ROOT/'assets/models/external/nature_cards'
OUT.mkdir(parents=True,exist_ok=True)
SLICES=4
RES=384
for folder,stem,name in TREES:
    target=OUT/(name+'.glb')
    if target.exists() and '--force' not in sys.argv:continue
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(folder/(stem+'.gltf')))
    objects=[o for o in bpy.context.scene.objects if o.type=='MESH']
    corners=[o.matrix_world@Vector(c) for o in objects for c in o.bound_box]
    lo=Vector(tuple(min(p[i] for p in corners) for i in range(3)))
    hi=Vector(tuple(max(p[i] for p in corners) for i in range(3)))
    mid=(lo+hi)/2
    extent=max(hi-lo)*1.04
    # Bake albedo, without a fixed sun or ambient shadows.
    for mat in bpy.data.materials:
        if not mat.use_nodes:continue
        nodes=mat.node_tree.nodes; links=mat.node_tree.links
        p=next((n for n in nodes if n.type=='BSDF_PRINCIPLED'),None)
        out=next((n for n in nodes if n.type=='OUTPUT_MATERIAL'),None)
        if not p or not out:continue
        emission=nodes.new('ShaderNodeEmission')
        if p.inputs['Base Color'].is_linked:links.new(p.inputs['Base Color'].links[0].from_socket,emission.inputs[0])
        else:emission.inputs[0].default_value=p.inputs['Base Color'].default_value
        transparent=nodes.new('ShaderNodeBsdfTransparent');mix=nodes.new('ShaderNodeMixShader')
        links.new(transparent.outputs[0],mix.inputs[1]);links.new(emission.outputs[0],mix.inputs[2])
        if p.inputs['Alpha'].is_linked:links.new(p.inputs['Alpha'].links[0].from_socket,mix.inputs[0])
        else:mix.inputs[0].default_value=p.inputs['Alpha'].default_value
        links.new(mix.outputs[0],out.inputs['Surface'])
    scene=bpy.context.scene;scene.render.engine='CYCLES';scene.cycles.samples=8
    scene.cycles.transparent_max_bounces=32
    scene.render.resolution_x=RES;scene.render.resolution_y=RES;scene.render.resolution_percentage=100
    scene.render.film_transparent=True;scene.view_settings.view_transform='Standard'
    camdata=bpy.data.cameras.new('Bake');cam=bpy.data.objects.new('Bake',camdata);scene.collection.objects.link(cam);scene.camera=cam
    camdata.type='ORTHO';camdata.ortho_scale=extent
    atlas=np.zeros((RES*2,RES*SLICES,4),dtype=np.float32)
    vertices=[];faces=[];uvs=[]
    for axis in range(2):
        direction=Vector((0,-1,0)) if axis==0 else Vector((1,0,0))
        cam.location=mid-direction*(extent*2)
        cam.rotation_euler=direction.to_track_quat('-Z','Y').to_euler()
        depth_axis=1 if axis==0 else 0
        for i in range(SLICES):
            step=(hi[depth_axis]-lo[depth_axis])/SLICES
            # Camera-front depth to this thin volume slab.
            start=(extent*2-(hi[depth_axis]-lo[depth_axis])/2)+i*step
            camdata.clip_start=max(.01,start);camdata.clip_end=start+step+.001
            tmp=OUT/(name+'-slice.png');scene.render.filepath=str(tmp)
            bpy.ops.render.render(write_still=True)
            im=bpy.data.images.load(str(tmp),check_existing=False)
            pixels=np.empty(RES*RES*4,dtype=np.float32);im.pixels.foreach_get(pixels)
            atlas[axis*RES:(axis+1)*RES,i*RES:(i+1)*RES]=pixels.reshape(RES,RES,4)
            bpy.data.images.remove(im)
            # Plane located halfway through the sampled source volume.
            center=-direction*(extent*2-start-step/2)
            right=cam.rotation_euler.to_quaternion()@Vector((1,0,0))
            center.z=mid.z-lo.z
            base=len(vertices)
            vertices.extend([tuple(center+right*x+Vector((0,0,z))) for x,z in ((-extent/2,-extent/2),(extent/2,-extent/2),(extent/2,extent/2),(-extent/2,extent/2))])
            faces.append((base,base+1,base+2,base+3))
            uvs.extend([(i/SLICES,axis/2),((i+1)/SLICES,axis/2),((i+1)/SLICES,(axis+1)/2),(i/SLICES,(axis+1)/2)])
    for o in list(scene.objects):bpy.data.objects.remove(o,do_unlink=True)
    texture=bpy.data.images.new(name+' scanned canopy',width=RES*SLICES,height=RES*2,alpha=True)
    texture.pixels.foreach_set(atlas.reshape(-1));texture.filepath_raw=str(OUT/(name+'.png'));texture.file_format='PNG';texture.save()
    mesh=bpy.data.meshes.new(name);mesh.from_pydata(vertices,[],faces);mesh.update()
    uv=mesh.uv_layers.new()
    for poly in mesh.polygons:
        for loop in poly.loop_indices:uv.data[loop].uv=uvs[mesh.loops[loop].vertex_index]
    obj=bpy.data.objects.new(name,mesh);scene.collection.objects.link(obj)
    mat=bpy.data.materials.new('Scanned foliage');mat.use_nodes=True;mat.surface_render_method='DITHERED';mat.use_backface_culling=False
    bsdf=mat.node_tree.nodes.get('Principled BSDF');tex=mat.node_tree.nodes.new('ShaderNodeTexImage');tex.image=texture
    mat.node_tree.links.new(tex.outputs['Color'],bsdf.inputs['Base Color']);mat.node_tree.links.new(tex.outputs['Alpha'],bsdf.inputs['Alpha'])
    bsdf.inputs['Roughness'].default_value=.9;obj.data.materials.append(mat)
    bpy.context.view_layer.objects.active=obj;obj.select_set(True)
    bpy.ops.export_scene.gltf(filepath=str(target),export_format='GLB',use_selection=True)
    # Enforce alpha testing, not sorted blending, for dense vegetation.
    raw=target.read_bytes();import struct
    n=struct.unpack_from('<I',raw,12)[0];doc=json.loads(raw[20:20+n]);tail=raw[20+n:]
    for m in doc['materials']:m['alphaMode']='MASK';m['alphaCutoff']=.35;m['doubleSided']=True
    payload=json.dumps(doc,separators=(',',':')).encode();payload+=b' '*((-len(payload))%4)
    target.write_bytes(struct.pack('<4sII',b'glTF',2,20+len(payload)+len(tail))+struct.pack('<I4s',len(payload),b'JSON')+payload+tail)
    (OUT/(name+'.source.json')).write_text(json.dumps({'source':str(folder/(stem+'.gltf')),'license':'CC-BY-4.0 / Daniel (Sketchfab Realistic Tree)' if name=='urban_tree' else 'CC0 / Poly Haven','sha256':hashlib.sha256((folder/(stem+'.gltf')).read_bytes()).hexdigest(),'method':'8 orthographic volume slices, scanned albedo and alpha','vertices':len(vertices),'height':hi.z-lo.z},indent=2))
    print('NATURE BAKED',name,flush=True)
