"""Build conservative animal rigs and LODs, or preserve existing author rigs.

New rigs ship explicit Rig_Check clips, not fabricated walk/run animations.
All output remains in candidate staging until comparative visual review.
"""
import json
import math
from pathlib import Path
import sys
import traceback
import bpy
import bmesh
import numpy as np
from mathutils import Matrix,Vector
sys.path.insert(0,str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT,QA,bounds,render,export,quiet

DEST=QA/'expansion'
STAGE=DEST/'candidates'
STAGE.mkdir(exist_ok=True)
CONFIG={
 'grab_cat':dict(id='cat_tabby',size=.75,kind='cat',budget=8000),
 'grab_bicolor_sphynx_cat':dict(id='cat_sphynx',size=.6,kind='quadruped',budget=7500),
 'street_rat':dict(id='rat_street',size=.35,kind='rat',budget=6500),
 'grab_bird':dict(id='robin',size=.15,kind='bird',budget=14000),
 'grab_frog':dict(id='frog_blue',size=.05,kind='frog',budget=4500),
 'grab_gorilla':dict(id='gorilla',size=1.7,kind='ape',budget=10000),
 'grab_dolphin':dict(id='dolphin',size=2.8,kind='dolphin',budget=7000),
 'grab_sea_turtle':dict(id='turtle_sea',size=1.2,kind='turtle',budget=6500),
 'grab_owl':dict(id='owl_eagle',size=.6,kind='bird',budget=12000),
 'grab_parrot':dict(id='parrot_macaw',size=.85,kind='parrot',budget=12000),
 'grab_eagle':dict(id='eagle_bald',size=.8,kind='bird',budget=12000),
 'bk_lizard_template':dict(id='lizard',size=.25,kind='lizard',budget=4500),
 'bk_wasp_rigged_animated_fur_particles':dict(id='wasp_realistic',size=.035,kind='existing',budget=3000),
 'bk_spider_rigged_animated_fur':dict(id='spider_garden',size=.035,kind='existing',budget=3000),
 'bk_reeve_s_pond_turtle_v_1_2':dict(id='turtle_pond',size=.25,kind='existing',budget=8000),
 'obj_frog_amazon':dict(id='frog_amazon',size=.045,kind='frog',budget=6000),
 'obj_frog_red_eye':dict(id='frog_red_eye',size=.07,kind='frog',budget=7000,rotate_z=90),
 'obj_pigeon_rock':dict(id='pigeon_rock',size=.34,kind='bird',budget=4000),
 'obj_mouse_brown':dict(id='mouse_brown',size=.2,kind='existing',budget=4000),
 'shrike':dict(id='shrike',size=.22,kind='existing',budget=2000),
}

def template(kind):
    """Fractions of the anatomical bounding box, facing Blender -Y, Z up."""
    specs=[]
    def bone(name,a,b,parent=None): specs.append((name,a,b,parent))
    if kind in {'cat','quadruped','rat','lizard','frog','ape'}:
        low=kind in {'rat','lizard','frog'}
        body=.48 if low else (.36 if kind=='cat' else .55)
        hip=.53 if kind in {'rat','lizard'} else .72
        bone('root',(.5,hip,body),(.5,.52,body),'')
        bone('spine',(.5,.52,body),(.5,.3,body+.04),'root')
        bone('neck',(.5,.3,body+.04),(.5,.15,min(body+.16,.85)),'spine')
        bone('head',(.5,.15,min(body+.16,.85)),(.5,.01,min(body+.2,.92)),'neck')
        for side,x in [('L',.25),('R',.75)]:
            for prefix,y,parent in [('fore',.24,'spine'),('hind',hip,'root')]:
                if kind=='frog':
                    outer=.02 if side=='L' else .98
                    knee=(outer,.72 if prefix=='hind' else .2,.25)
                    foot=(.08 if side=='L' else .92,.95 if prefix=='hind' else .03,.03)
                elif kind=='ape' and prefix=='fore':
                    knee=(.10 if side=='L' else .90,.2,.32)
                    foot=(.12 if side=='L' else .88,.1,.03)
                elif kind=='lizard':
                    knee=(.02 if side=='L' else .98,y+.03,.35)
                    foot=(.04 if side=='L' else .96,y-.03,.02)
                else:
                    knee=(x,y+(.06 if prefix=='fore' else -.04),body*.48)
                    foot=(x,y-.05,.02)
                bone(prefix+'_upper.'+side,(x,y,body-.04),knee,parent)
                bone(prefix+'_lower.'+side,knee,foot,prefix+'_upper.'+side)
                bone(prefix+'_foot.'+side,foot,(foot[0],foot[1]-.05,foot[2]),prefix+'_lower.'+side)
        if kind not in {'frog','ape'}:
            points=[(.5,hip+.08,body),(.5,.78,body*.65),(.5,.92,body*.42),(.5,.995,body*.2)]
            if kind=='cat': points=[(.5,.84,.34),(.5,.94,.55),(.5,.94,.78),(.5,.85,.99)]
            for i,(a,b) in enumerate(zip(points,points[1:])): bone('tail_'+str(i+1),a,b,'root' if i==0 else 'tail_'+str(i))
    elif kind in {'bird','parrot'}:
        bone('root',(.5,.6,.4),(.5,.42,.55),'')
        bone('neck',(.5,.42,.55),(.5,.25,.76),'root')
        bone('head',(.5,.25,.76),(.5,.06,.82),'neck')
        for side,x in [('L',.25),('R',.75)]:
            bone('wing_upper.'+side,(x,.4,.64),(x*.5 if side=='L' else 1-(1-x)*.5,.62,.49),'root')
            bone('wing_tip.'+side,(x*.5 if side=='L' else 1-(1-x)*.5,.62,.49),(x,.87,.34),'wing_upper.'+side)
            z=.32 if kind=='parrot' else .03
            bone('leg.'+side,(x,.45,.42),(x,.38,z),'root')
            bone('foot.'+side,(x,.38,z),(x,.18,z),'leg.'+side)
        bone('tail_1',(.5,.62,.42),(.5,.82,.22),'root')
        bone('tail_2',(.5,.82,.22),(.5,.99,.03),'tail_1')
    elif kind in {'dolphin','turtle'}:
        bone('root',(.5,.58,.5),(.5,.4,.5),'')
        bone('neck',(.5,.4,.5),(.5,.18,.5),'root')
        bone('head',(.5,.18,.5),(.5,.01,.5),'neck')
        for side,x in [('L',.02),('R',.98)]:
            bone('flipper.'+side,(.3 if side=='L' else .7,.34,.46),(x,.43,.13),'root')
            if kind=='turtle': bone('rear_flipper.'+side,(.35 if side=='L' else .65,.72,.4),(x,.92,.22),'root')
        points=[(.5,.6,.5),(.5,.78,.5),(.5,.95,.5),(.5,.995,.5)]
        for i,(a,b) in enumerate(zip(points,points[1:])): bone('tail_'+str(i+1),a,b,'root' if i==0 else 'tail_'+str(i))
    return specs

def create_rig(meshes,kind):
    lo,hi=bounds(meshes)
    size=hi-lo
    specs=template(kind)
    arm=bpy.data.armatures.new('Animal_Deform_Rig')
    rig=bpy.data.objects.new('Animal_Deform_Rig',arm)
    bpy.context.scene.collection.objects.link(rig)
    bpy.context.view_layer.objects.active=rig
    rig.select_set(True)
    bpy.ops.object.mode_set(mode='EDIT')
    for name,a,b,parent in specs:
        eb=arm.edit_bones.new(name)
        eb.head=lo+Vector(a)*size
        eb.tail=lo+Vector(b)*size
        if (eb.tail-eb.head).length<1e-5: eb.tail.z+=max(size)*.005
        if parent: eb.parent=arm.edit_bones[parent]
    bpy.ops.object.mode_set(mode='OBJECT')
    starts=np.array([b.head_local[:] for b in arm.bones],dtype=float)
    ends=np.array([b.tail_local[:] for b in arm.bones],dtype=float)
    vectors=ends-starts
    lengths=np.sum(vectors*vectors,axis=1)
    names=[b.name for b in arm.bones]
    adjacent=np.eye(len(names),dtype=bool)
    for i,b in enumerate(arm.bones):
        if b.parent:
            j=names.index(b.parent.name)
            adjacent[i,j]=adjacent[j,i]=True
    for mesh in meshes:
        coords=np.array([v.co[:] for v in mesh.data.vertices],dtype=float)
        delta=coords[:,None,:]-starts[None,:,:]
        alpha=np.clip(np.einsum('nbi,bi->nb',delta,vectors)/lengths,0,1)
        dist=np.linalg.norm(delta-alpha[:,:,None]*vectors,axis=2)
        closest=np.argmin(dist,axis=1)
        allowed=adjacent[closest]
        # Only neighboring anatomical segments can blend at a joint; distant
        # paws, the opposite side and adjacent folded wings never share weights.
        scores=np.where(allowed,1/np.maximum(dist,max(size)*.015)**4,0)
        scores/=scores.sum(axis=1,keepdims=True)
        groups=[mesh.vertex_groups.new(name=n) for n in names]
        for i,row in enumerate(scores):
            keep=np.argsort(row)[-4:]
            total=sum(row[j] for j in keep)
            for j in keep:
                if row[j]>.0001: groups[j].add([i],float(row[j]/total),'REPLACE')
        mod=mesh.modifiers.new('Animal_skin','ARMATURE')
        mod.object=rig
    return rig

def rig_checks(rig):
    rig.animation_data_create()
    for label,selector,angle in [('Rig_Check_Head',['head','neck','kopf','hals'],12),('Rig_Check_Limbs',['fore_lower','leg.','flipper','tail_2','wing_tip'],8)]:
        selected=[b for b in rig.pose.bones if any(s in b.name.lower() for s in selector)]
        if not selected: selected=list(rig.pose.bones)[1:3]
        action=bpy.data.actions.new(label)
        rig.animation_data.action=action
        for b in selected:
            b.rotation_mode='XYZ'
            for frame,multiplier in [(0,0),(12,1),(24,0)]:
                b.rotation_euler=(math.radians(angle)*multiplier,0,0)
                b.keyframe_insert('rotation_euler',frame=frame,group=b.name)
        action.use_fake_user=True
        t=rig.animation_data.nla_tracks.new()
        t.name=label
        t.strips.new(label,0,action)
        t.mute=True
    rig.animation_data.action=None
    for b in rig.pose.bones: b.matrix_basis=Matrix.Identity(4)

def portable_materials(meshes,ident):
    """Use source albedo images directly where Cycles-only groups hide them.

    No recoloring or generated textures: retain author albedo/UV and supported
    normal maps. Particle fur and Cycles bump microdetails are omitted.
    """
    if ident not in {'wasp_realistic','spider_garden','shrike'}:return
    for material in {slot.material for o in meshes for slot in o.material_slots if slot.material}:
        images=[n.image for n in material.node_tree.nodes if n.type=='TEX_IMAGE' and n.image]
        color=next((im for im in images if 'albedo' in im.name.lower() or '_diff' in im.name.lower()),None)
        if not color:continue
        normal=next((im for im in images if '_norm' in im.name.lower()),None)
        material.node_tree.nodes.clear()
        nodes=material.node_tree.nodes;links=material.node_tree.links
        out=nodes.new('ShaderNodeOutputMaterial');bsdf=nodes.new('ShaderNodeBsdfPrincipled')
        links.new(bsdf.outputs['BSDF'],out.inputs['Surface'])
        tex=nodes.new('ShaderNodeTexImage');tex.image=color
        links.new(tex.outputs['Color'],bsdf.inputs['Base Color'])
        bsdf.inputs['Roughness'].default_value=.65
        if ident=='shrike':
            links.new(tex.outputs['Alpha'],bsdf.inputs['Alpha'])
            material.surface_render_method='DITHERED'
        if normal:
            normal.colorspace_settings.name='Non-Color'
            texn=nodes.new('ShaderNodeTexImage');texn.image=normal
            bump=nodes.new('ShaderNodeNormalMap');links.new(texn.outputs['Color'],bump.inputs['Color']);links.new(bump.outputs['Normal'],bsdf.inputs['Normal'])


def prepare(record,config):
    ident=config['id']
    path=ROOT/record['source_file']
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.suffix.lower()=='.blend':
        bpy.ops.wm.open_mainfile(filepath=str(path),load_ui=False,use_scripts=False)
        if bpy.context.object and bpy.context.object.mode!='OBJECT': bpy.ops.object.mode_set(mode='OBJECT')
        for o in bpy.data.objects:
            if o.name not in bpy.context.scene.objects: bpy.context.scene.collection.objects.link(o)
    else:
        with quiet(): bpy.ops.import_scene.gltf(filepath=str(path))
    source_names={m['name'] for m in record['meshes'] if not any(s in m['name'].lower() for s in ['not rendered','hair-base'])}
    meshes=[o for o in bpy.data.objects if o.type=='MESH' and o.name in source_names]
    if not meshes: raise ValueError('No candidate meshes')
    rigs=list({o.find_armature() for o in meshes if o.find_armature()})
    rig=max(rigs,key=lambda o:len(o.data.bones)) if rigs else None
    new_rig=rig is None
    if config['kind']=='existing' and new_rig: raise ValueError('Expected author rig absent')
    for c in bpy.data.collections: c.hide_render=False
    for o in bpy.data.objects:
        o.hide_render=o not in meshes and o!=rig
    for o in meshes:
        o.hide_render=False
        o.hide_set(False)
        for mod in list(o.modifiers):
            if mod.type in {'SUBSURF','PARTICLE_SYSTEM','NODES'}:
                o.modifiers.remove(mod)
    if rig:
        rig.hide_set(False)
        rig.data.pose_position='REST'
    portable_materials(meshes,ident)
    original_triangles=sum(sum(len(p.vertices)-2 for p in o.data.polygons) for o in meshes)
    # Bake only static transforms before assigning any new skin weights.
    if new_rig:
        for o in meshes:
            world=Matrix.Rotation(math.radians(config.get('rotate_z',0)),4,'Z') @ o.matrix_world.copy()
            o.parent=None
            o.data=o.data.copy()
            o.data.transform(world)
            o.matrix_world=Matrix.Identity(4)
            # AI/scanned GLBs may duplicate position vertices at every normal
            # or UV seam. Weld geometry while retaining loop UVs before QEM;
            # otherwise a decimator deletes isolated triangles and opens holes.
            bm=bmesh.new()
            bm.from_mesh(o.data)
            diameter=max(o.dimensions)
            if config.get('weld',False): bmesh.ops.remove_doubles(bm,verts=list(bm.verts),dist=max(diameter*1e-6,1e-8))
            bm.to_mesh(o.data)
            bm.free()
            o.data.update()
    for o in meshes:
        count=sum(len(p.vertices)-2 for p in o.data.polygons)
        target=max(100,config['budget']*count/max(original_triangles,1))
        if count>target*1.1 and count>500:
            bpy.context.view_layer.objects.active=o
            mod=o.modifiers.new('LOD0_budget_preserve_uv','DECIMATE')
            mod.ratio=target/count
            mod.use_collapse_triangulate=True
            with quiet(): bpy.ops.object.modifier_apply(modifier=mod.name)
        for p in o.data.polygons: p.use_smooth=True
    if new_rig:
        rig=create_rig(meshes,config['kind'])
        bpy.context.scene.render.fps=24
        rig_checks(rig)
    else:
        # Attach static eyes to the source head, retaining author deformation.
        head=next((b.name for b in rig.data.bones if 'head' in b.name.lower() or 'kopf' in b.name.lower()),rig.data.bones[0].name)
        for o in meshes:
            if not any(m.type=='ARMATURE' and m.object==rig for m in o.modifiers):
                world=o.matrix_world.copy()
                o.parent=None
                o.matrix_world=world
                group=o.vertex_groups.new(name=head)
                group.add(list(range(len(o.data.vertices))),1.,'REPLACE')
                mod=o.modifiers.new('Author_rig_attachment','ARMATURE')
                mod.object=rig
        rig.animation_data_create()
        rig.animation_data.action=None
        for t in list(rig.animation_data.nla_tracks): rig.animation_data.nla_tracks.remove(t)
        for action in list(bpy.data.actions):
            if not any(f.data_path.startswith('pose.bones[') for f in action.fcurves): continue
            action.use_fake_user=True
            track=rig.animation_data.nla_tracks.new()
            track.name=action.name
            strip=track.strips.new(action.name,0,action)
            if any(f.modifiers for f in action.fcurves): strip.action_frame_end=max(48,action.frame_range[1])
            track.mute=True
        if not rig.animation_data.nla_tracks or ident=='turtle_pond': rig_checks(rig)
    objects=set(meshes+[rig])
    for o in list(objects):
        parent=o.parent
        while parent: objects.add(parent); parent=parent.parent
    for o in list(bpy.data.objects):
        if o not in objects: bpy.data.objects.remove(o,do_unlink=True)
    for image in bpy.data.images:
        if image.type!='IMAGE' or not image.size[0]: continue
        if max(image.size)>1024:
            factor=1024/max(image.size)
            image.scale(round(image.size[0]*factor),round(image.size[1]*factor))
        if not image.packed_file:
            try: image.pack()
            except RuntimeError: pass
    rig.data.pose_position='REST'
    lo,hi=bounds(meshes)
    scale=config['size']/max(hi-lo)
    carrier=bpy.data.objects.new(ident,None)
    bpy.context.scene.collection.objects.link(carrier)
    for obj in objects:
        if obj.parent not in objects:
            matrix=obj.matrix_world.copy()
            obj.parent=carrier
            obj.matrix_world=matrix
    carrier.scale=(scale,)*3
    carrier.location.z=-lo.z*scale
    rig['asset_license']=record['license']
    rig['asset_author']=record['author']
    rig['asset_source']=record['source_url']
    rig['rig_origin']='created_for_R1World' if new_rig else 'original_author'
    bpy.context.scene.frame_set(0)
    paths=[STAGE/(ident+'.glb'),STAGE/(ident+'-lod1.glb')]
    export(paths[0],list(objects)+[carrier])
    rig.data.pose_position='REST'
    render(meshes,DEST/(ident+'-lod0.png'))
    for o in meshes:
        bpy.context.view_layer.objects.active=o
        mod=o.modifiers.new('LOD1_preserve_skin','DECIMATE')
        mod.ratio=.55
        mod.use_collapse_triangulate=True
        with quiet(): bpy.ops.object.modifier_apply(modifier=mod.name)
    export(paths[1],list(objects)+[carrier])
    rig.data.pose_position='REST'
    render(meshes,DEST/(ident+'-lod1.png'))
    return {'id':ident,'status':'candidate','files':[p.name for p in paths],'source_file':record['source_file'],'source_url':record['source_url'],'author':record['author'],'license':record['license'],'bones':len(rig.data.bones),'source_triangles':original_triangles,'rig_origin':'created_for_R1World' if new_rig else 'original_author','rig_template':config['kind'] if new_rig else None,'animation_status':'rig_validation_only; no authored locomotion' if all(t.name.startswith('Rig_Check_') for t in rig.animation_data.nla_tracks) else ('source_pose_library_and_rig_validation; no authored locomotion' if ident=='turtle_pond' else 'author_actions_preserved'),'animations':[{'name':t.name} for t in rig.animation_data.nla_tracks],'scale':{'max_dimension_metres':config['size'],'basis':'approximate species size; source proportions preserved'},'limitations':['Folded wings: flight unfolding is not authored.'] if config['kind'] in {'bird','parrot'} else []}

if __name__=='__main__':
    selected=sys.argv[sys.argv.index('--')+1:] if '--' in sys.argv else list(CONFIG)
    for source_id,config in CONFIG.items():
        if source_id not in selected and config['id'] not in selected: continue
        record=json.loads((DEST/(source_id+'.json')).read_text())
        try: result=prepare(record,config)
        except Exception as error: result={'id':config['id'],'status':'error','reason':str(error),'traceback':traceback.format_exc()}
        (DEST/('prepared_'+config['id']+'.json')).write_text(json.dumps(result,indent=2),encoding='utf-8')
        print('PREPARED',config['id'],result['status'],result.get('reason',''),flush=True)
