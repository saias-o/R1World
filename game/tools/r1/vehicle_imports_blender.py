"""Blender worker: preserve source shading/UVs, bake units/pivots, reduce LODs."""
import bpy
import json
import math
import sys
from collections import defaultdict
from pathlib import Path
from mathutils import Vector
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from r1.vehicle_fleet import GLB, MATERIALS, ROOT

PBR={name:(rgb,metal,rough) for name,rgb,metal,rough in MATERIALS}

def role(name,obj):
    s=name.lower().split('.')[0]
    if obj.lower()=='circle' and s=='glass':return 'headlamp'
    if 'mirror' in s or ('mirrors' in obj.lower() and s=='chrome'):return 'mirror'
    if s in ('body','carpaint','lowbody','lowbodyarch','handledoor','red-cabin','accentcolor'):return 'paint'
    if 'glass' in s and 'red' not in s:return 'glass'
    if s in ('rearlights','rearlighttop','glass_red'):return 'taillamp'
    if 'orange' in s or 'yellow' in s:return 'indicator'
    if 'lights' in s and s!='lights':return 'headlamp'
    if 'chrome' in s or s in ('rim','lowbodymetallic','grey-metal-wheels','grey-metalic-frame','hitchball'):return 'alloy'
    return 'rubber' if any(k in s for k in ('black','rubber','tires','wheelarch','underside','lines','roof','interior','seating','backwheels','axe')) else 'trim'

def wheel_object(name,category):
    s=name.lower()
    if category=='city':return s.startswith('wheel_')
    if category=='truck':return s.startswith('truck.003')
    if category=='bus':return s.startswith(('tires_','texturewheels_'))
    return s.startswith('wheels')

def reset():
    bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
    # Avoid accumulating imported duplicate material/image names across jobs.
    for collection in (bpy.data.meshes,bpy.data.materials,bpy.data.images):
        for data in list(collection):
            if data.users==0:collection.remove(data)

def truck_wheel_vertices(mesh):
    """The truck OBJ mixes tyres and chassis in one textured object."""
    parent=list(range(len(mesh.vertices)))
    def find(i):
        while parent[i]!=i:parent[i]=parent[parent[i]];i=parent[i]
        return i
    for edge in mesh.edges:
        a,b=edge.vertices;parent[find(a)]=find(b)
    components=defaultdict(list)
    for v in mesh.vertices:components[find(v.index)].append(v.index)
    result=set()
    for indices in components.values():
        pts=[mesh.vertices[i].co for i in indices]
        low=[min(p[i] for p in pts) for i in range(3)]
        high=[max(p[i] for p in pts) for i in range(3)]
        if high[0]-low[0]<.5 and .65<high[1]-low[1]<1.3 and .65<high[2]-low[2]<1.3 and low[2]<.05:
            result.update(indices)
    return result

def bake(job,far=False):
    reset();bpy.ops.wm.obj_import(filepath=job['source'])
    category=job['name']
    meshes=[o for o in bpy.context.scene.objects if o.type=='MESH']
    for o in list(meshes):
        # Hidden interiors cannot be seen through the opaque reflective glazing.
        # Remove branding and tyre lettering, retaining the body and wheel geometry.
        s=o.name.lower()
        if any(k in s for k in ('interior','steering','seating','seats','logo_','wheel_text')) or s.startswith('axe_'):
            bpy.data.objects.remove(o,do_unlink=True);meshes.remove(o)
    points=[o.matrix_world@v.co for o in meshes for v in o.data.vertices]
    lo=Vector([min(p[i] for p in points) for i in range(3)])
    hi=Vector([max(p[i] for p in points) for i in range(3)])
    origin=Vector(((lo.x+hi.x)/2,(lo.y+hi.y)/2,lo.z))
    # Sources are authored in centimetres. Blender Z-up, front -Y -> glTF Y-up,+Z.
    wheel_pts=[]
    mixed_wheels={}
    for o in meshes:
        transform=o.matrix_world.copy()
        for v in o.data.vertices:v.co=(transform@v.co-origin)*.01
        source_low=Vector([min(v.co[i] for v in o.data.vertices) for i in range(3)])
        source_high=Vector([max(v.co[i] for v in o.data.vertices) for i in range(3)])
        o.matrix_world.identity()
        if wheel_object(o.name,category):wheel_pts.extend(v.co.copy() for v in o.data.vertices)
        if category=='truck' and o.name.startswith('truck-textured'):
            selected=truck_wheel_vertices(o.data)
            wheel_pts.extend(o.data.vertices[i].co.copy() for i in selected)
            # A vertex group survives decimation and keeps the photographed
            # tyres attached to the rotating hubs instead of to the chassis.
            group=o.vertex_groups.new(name='rolling-tyres');group.add(list(selected),1.,'REPLACE')
            mixed_wheels[o.name]=group.index
        bpy.context.view_layer.objects.active=o;o.select_set(True)
        if far:
            # Weld geometric duplicates before reducing distance geometry;
            # OBJ corner UVs remain per-loop. Near preserves the source mesh.
            bpy.ops.object.mode_set(mode='EDIT');bpy.ops.mesh.select_all(action='SELECT')
            bpy.ops.mesh.remove_doubles(threshold=.000001);bpy.ops.object.mode_set(mode='OBJECT')
            ratio=.035 if category=='city' else .12
            o.data.calc_loop_triangles()
            if len(o.data.loop_triangles)>100:
                mod=o.modifiers.new('Distance detail','DECIMATE');mod.ratio=ratio
                mod.use_collapse_triangulate=True
                bpy.ops.object.modifier_apply(modifier=mod.name)
            # Open source panels can overshoot under quadric decimation.
            # A distance LOD must stay inside the original component's bounds.
            for v in o.data.vertices:
                v.co=Vector([max(source_low[i],min(source_high[i],v.co[i])) for i in range(3)])
        if far:
            o.data.set_sharp_from_angle(angle=math.radians(50))
            for p in o.data.polygons:p.use_smooth=True
            bpy.ops.mesh.customdata_custom_splitnormals_clear()
        o.select_set(False)
    # Tyres determine physical pivot height; decorative arches stay on the body.
    if not wheel_pts:raise ValueError('Missing wheels: '+category)
    groups=defaultdict(list)
    for p in wheel_pts:groups[(p.x>0,p.y>0)].append(p)
    pivots={}
    for key,pts in groups.items():
        pivots[key]=Vector([(min(p[i] for p in pts)+max(p[i] for p in pts))/2 for i in range(3)])
    radius=sum(p.z for p in pivots.values())/4
    parts=defaultdict(lambda:([],[],[],[],{}))
    seen_triangles=defaultdict(set)
    materials={};textures={}
    for o in meshes:
        mesh=o.data;mesh.calc_loop_triangles()
        uv=mesh.uv_layers.active.data if mesh.uv_layers.active else None
        normals=mesh.corner_normals
        is_wheel=wheel_object(o.name,category)
        for tri in mesh.loop_triangles:
            if tri.area<1e-10:continue
            source=mesh.materials[tri.material_index]
            surface=role(source.name,o.name)
            if category=='bus' and o.name.startswith('textureWheels'):surface='alloy'
            image=None
            if source.use_nodes:
                image=next((n.image for n in source.node_tree.nodes if n.type=='TEX_IMAGE' and n.image),None)
            # Transparent handle decals become solid modelled dark handles.
            if source.name.lower().startswith('handle'):image=None
            material=surface+'-'+source.name.split('.')[0]
            if material not in materials:
                rgb,metal,rough=PBR.get(surface,(tuple(min(.65,c) for c in source.diffuse_color[:3]),0.,.42))
                if image:
                    rgb=(1.,1.,1.)
                    image_path=Path(bpy.path.abspath(image.filepath))
                    if not image_path.is_file():raise ValueError('Missing source texture: '+str(image_path))
                    textures[material]=image_path
                materials[material]={'name':material,'pbrMetallicRoughness':{
                    'baseColorFactor':[*rgb,1.],'metallicFactor':metal,'roughnessFactor':rough}}
            center=sum((mesh.vertices[i].co for i in tri.vertices),Vector())/3
            rolling=is_wheel or (o.name in mixed_wheels and all(
                any(g.group==mixed_wheels[o.name] and g.weight>.5 for g in mesh.vertices[i].groups)
                for i in tri.vertices))
            key=(center.x>0,center.y>0) if rolling else None
            part=(key,material)
            positions,ns,uvs,indices,lookup=parts[part]
            face=[]
            for loop in tri.loops:
                p=mesh.vertices[mesh.loops[loop].vertex_index].co
                if key is not None:
                    p=p-pivots[key]
                    if far:
                        # Decimation may move a tyre's lowest support point.
                        # Keep the original axle and contact radius at both LODs.
                        r=math.hypot(p.y,p.z)
                        source_radius=pivots[key].z
                        if r>source_radius:p=Vector((p.x,p.y*source_radius/r,p.z*source_radius/r))
                n=normals[loop].vector
                tex=tuple(uv[loop].uv) if uv else (0.,0.)
                vals=tuple(round(float(v),7) for v in (p.x,p.z,-p.y,n.x,n.z,-n.y,*tex))
                if vals not in lookup:
                    lookup[vals]=len(positions)//3
                    positions.extend(vals[:3]);ns.extend(vals[3:6]);uvs.extend((vals[6],1-vals[7]))
                face.append(lookup[vals])
            # Several source faces are duplicated in the OBJ. They must not
            # spend draw triangles or z-fight in the engine.
            triangle=tuple(sorted(face))
            if len(set(face))==3 and triangle not in seen_triangles[part]:
                seen_triangles[part].add(triangle);indices.extend(face)
    glb=GLB(generator='R1World licensed vehicle importer')
    glb.doc['materials']=list(materials.values())
    mat_ids={name:i for i,name in enumerate(materials)}
    image_ids={}
    for material,path in textures.items():
        if path not in image_ids:
            while len(glb.binary)%4:glb.binary.append(0)
            data=path.read_bytes();view=len(glb.doc['bufferViews'])
            glb.doc['bufferViews'].append({'buffer':0,'byteOffset':len(glb.binary),'byteLength':len(data)})
            glb.binary.extend(data)
            image_ids[path]=len(glb.doc.setdefault('images',[]))
            glb.doc['images'].append({'bufferView':view,'mimeType':'image/png' if path.suffix.lower()=='.png' else 'image/jpeg'})
            glb.doc.setdefault('textures',[]).append({'source':image_ids[path]})
        glb.doc['materials'][mat_ids[material]]['pbrMetallicRoughness']['baseColorTexture']={'index':image_ids[path]}
    parents={None:glb.node('body')}
    for key,p in pivots.items():
        parents[key]=glb.node('wheel-'+('back' if key[1] else 'front')+'-'+('right' if key[0] else 'left'),
                              position=(float(p.x),float(p.z),float(-p.y)))
    intern={}
    for (key,material),(positions,ns,uvs,indices,_) in parts.items():
        if not indices:continue
        # Deduplication can leave unused corners. Arena budgets count only
        # vertices referenced by the final triangles, including every UV seam.
        used=sorted(set(indices));remap={old:new for new,old in enumerate(used)}
        positions=[positions[3*i+j] for i in used for j in range(3)]
        ns=[ns[3*i+j] for i in used for j in range(3)]
        uvs=[uvs[2*i+j] for i in used for j in range(2)]
        indices=[remap[i] for i in indices]
        mesh_key=(material,tuple(positions),tuple(ns),tuple(uvs),tuple(indices))
        if mesh_key in intern:mi=intern[mesh_key]
        else:
            primitive={'attributes':{'POSITION':glb.accessor(positions,5126,3,True),
                'NORMAL':glb.accessor(ns,5126,3)},'indices':glb.accessor(indices,5125,1),'material':mat_ids[material]}
            if material in textures:primitive['attributes']['TEXCOORD_0']=glb.accessor(uvs,5126,2)
            mi=len(glb.doc['meshes']);glb.doc['meshes'].append({'primitives':[primitive]});intern[mesh_key]=mi
            glb.vertices+=len(positions)//3;glb.triangles+=len(indices)//3
        node=glb.node(material+'-'+('body' if key is None else str(key)),parent=parents[key])
        glb.doc['nodes'][node]['mesh']=mi
    path=ROOT/(category+('_far' if far else '')+'.glb');glb.save(path)
    draw=sum(len(data[3])//3 for data in parts.values())
    print(category,'far' if far else 'near',glb.vertices,'vertices',draw,'triangles')
    dims={'length':float((hi.y-lo.y)*.01),'width':float((hi.x-lo.x)*.01),
          'height':float((hi.z-lo.z)*.01),'wheelbase':float(abs(pivots[(True,True)].y-pivots[(True,False)].y)),
          'wheelRadius':float(radius)}
    return {'path':'assets/models/vehicles/'+path.name,'vertices':glb.vertices,
            'triangles':glb.triangles,'drawTriangles':draw},dims

config=Path(sys.argv[sys.argv.index('--')+1])
ROOT=config.parent/'output';ROOT.mkdir(parents=True,exist_ok=True)
jobs=json.loads(config.read_text(encoding='utf-8'))
fleet=[]
for job in jobs:
    near,dims=bake(job);far,_=bake(job,True)
    fleet.append({**job,**dims,'near':near,'far':far})
    fleet[-1].pop('source')
manifest={'description':'Licensed realistic road vehicles; opaque PBR, original curved silhouettes and UV detail, two LODs.',
          'source':'game/tools/r1/vehicle_imports.py','totalVertices':sum(v[l]['vertices'] for v in fleet for l in ('near','far')),
          'vehicles':fleet}
(ROOT/'fleet.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print('Fleet resident geometry:',manifest['totalVertices'])
