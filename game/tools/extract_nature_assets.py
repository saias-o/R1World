"""Extract only selected plants from user-supplied scenes; preserve attribution."""
import bpy,json,hashlib
from pathlib import Path
from mathutils import Vector
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT.parent/'data/source-assets'
OUT=ROOT/'assets/models/external/nature_selected';OUT.mkdir(parents=True,exist_ok=True)
items=[('scene.gltf',None,'urban_tree'),('grass_pack_lowpoly_game-ready.glb','Grass main fresh_','grass_fresh'),('grass_pack_lowpoly_game-ready.glb','Grass main dry_','grass_dry'),('grass_pack_lowpoly_game-ready.glb','Grass tall high_','grass_tall')]
manifest=[]
for filename,prefix,name in items:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(SOURCE/filename))
    chosen=[o for o in bpy.context.scene.objects if o.type=='MESH' and (prefix is None or o.name.startswith(prefix))]
    assert chosen,(filename,prefix)
    # Apply the scene hierarchy before removing unrelated ground/man/props.
    for o in chosen:
        matrix=o.matrix_world.copy();o.parent=None;o.matrix_world=matrix
    for o in list(bpy.context.scene.objects):
        if o not in chosen:bpy.data.objects.remove(o,do_unlink=True)
    points=[o.matrix_world@Vector(c) for o in chosen for c in o.bound_box]
    low=Vector(tuple(min(p[i] for p in points) for i in range(3)));high=Vector(tuple(max(p[i] for p in points) for i in range(3)))
    height=high.z-low.z;center=Vector(((low.x+high.x)/2,(low.y+high.y)/2,low.z))
    for o in chosen:
        for v in o.data.vertices:v.co=(o.matrix_world@v.co-center)/height
        o.matrix_world.identity()
    for im in bpy.data.images:
        if max(im.size)>1024:
            ratio=1024/max(im.size);im.scale(max(1,int(im.size[0]*ratio)),max(1,int(im.size[1]*ratio)))
    for o in chosen:o.select_set(True)
    bpy.context.view_layer.objects.active=chosen[0]
    bpy.ops.export_scene.gltf(filepath=str(OUT/(name+'.glb')),export_format='GLB',use_selection=True)
    # Asset extras hold the user's source author/license in these glTF files.
    import struct
    raw=(SOURCE/filename).read_bytes();doc=json.loads(raw[20:20+struct.unpack_from('<I',raw,12)[0]]) if filename.endswith('.glb') else json.loads(raw)
    info={'output':name+'.glb','input':filename,'selectedChildren':[o.name for o in chosen], 'sourceSha256':hashlib.sha256(raw).hexdigest(),'attribution':doc.get('asset',{}).get('extras',{}),'normalizedHeight':1,'vertices':sum(len(o.data.vertices) for o in chosen)}
    manifest.append(info)
    # Render a neutral preview of just the extracted children.
    scene=bpy.context.scene;scene.render.engine='CYCLES';scene.cycles.samples=16
    scene.world=bpy.data.worlds.new('World');scene.world.use_nodes=True;scene.world.node_tree.nodes['Background'].inputs[0].default_value=(.18,.18,.18,1)
    light=bpy.data.lights.new('Softbox','AREA');light.energy=250;light.size=4
    obj=bpy.data.objects.new('Softbox',light);scene.collection.objects.link(obj);obj.location=(2,-3,4)
    obj.rotation_euler=(Vector((0,0,.5))-obj.location).to_track_quat('-Z','Y').to_euler()
    data=bpy.data.cameras.new('Camera');cam=bpy.data.objects.new('Camera',data);scene.collection.objects.link(cam);scene.camera=cam
    cam.location=(1.4,-2,1);cam.rotation_euler=(Vector((0,0,.5))-cam.location).to_track_quat('-Z','Y').to_euler();data.type='ORTHO';data.ortho_scale=1.6
    scene.render.resolution_x=512;scene.render.resolution_y=512;scene.render.resolution_percentage=100;scene.render.filepath=str(OUT/(name+'.png'))
    bpy.ops.render.render(write_still=True)
    print('EXTRACTED',name,info['vertices'],flush=True)
(OUT/'SOURCES.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf-8')
