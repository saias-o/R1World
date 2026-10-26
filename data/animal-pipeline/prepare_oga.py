"""Convert independent OGA candidates for visual selection, without UI."""
import json
from pathlib import Path
import sys
import traceback
import bpy
sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_0ad import ROOT, OUT, QA, bounds, export, render, quiet

SIZES = {'ant': 0.012, 'butterfly': 0.10, 'wasp': 0.05, 'bat': 0.35,
         'fish_kelgar': 0.35, 'lemur': 1.1, 'rat': 0.45, 'duck': 0.6, 'duckling': 0.2, 'spider': 0.07,
         'chicken_kelgar':0.5, 'rooster':0.7, 'scorpion':0.12, 'snake':1.0, 'penguin':0.65,
         'doe':1.8,'stag':2.2,'rabbit':0.45,'pike':0.9,'horseshoe_crab':0.55}


def image_for(entry, material):
    mappings = {'butterfly': 'butterfly-skin', 'lemur': 'lemurT',
                'fish_kelgar': 'Fish', 'spider': 'Spider.png',
                'chicken_kelgar':'UV-Texture.png','rooster':'UV','scorpion':'scorpion.png','snake':'snake.png','penguin':'penguin.png'}
    name = mappings.get(entry['id'])
    material_name=material.name.lower()
    if entry['id']=='doe':
        name='doe-head' if 'head' in material_name else 'doe-body'
    if entry['id']=='stag':
        name='deer-antlers.png' if any(s in material_name for s in ['horn','antler']) else ('head' if 'head' in material_name else 'body')
    if entry['id']=='rabbit':
        name='Fur-skin' if material.name=='Fur' else 'rabbot-skinn'
    if entry['id']=='horseshoe_crab':
        name='lim_tex_by_designandshine-ch.png'
    if entry['id']=='pike':
        name='ESOX-V2.1001.png'
    if entry['id'] in {'fish_kelgar','chicken_kelgar','rooster','doe','stag'} and 'eye' in material.name.lower():
        return None
    if entry['id'] == 'rat':
        name = {'body': 'rat-body', 'head': 'rat-head', 'head.001': 'rat-head'}.get(material.name)
    if entry['id'] == 'bat':
        name = 'bat_parts.jpg' if 'parts' in material.name else 'bat_tex.jpg'
    if not name:
        return None
    image = bpy.data.images.get(name)
    if image and image.size[0] > 0:
        return image
    files = list((ROOT / entry['file']).parent.rglob(name + '*'))
    files += list((ROOT / 'data/source-assets/animals/opengameart' / entry['id']).rglob(name + '*'))
    for file in files:
        if file.is_file():
            return bpy.data.images.load(str(file), check_existing=True)
    raise ValueError('Missing source texture: ' + name)


def prepare(entry):
    name = entry['id']
    bpy.ops.wm.open_mainfile(filepath=str(ROOT / entry['file']), load_ui=False, use_scripts=False)
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    for c in bpy.data.collections:
        c.hide_render = False
        c.hide_viewport = False
    def enable(layer):
        layer.exclude = False
        layer.hide_viewport = False
        for child in layer.children:
            enable(child)
    enable(bpy.context.view_layer.layer_collection)
    meshes = [o for o in bpy.data.objects if o.type == 'MESH' and o.find_armature() and (name != 'bat' or o.name == 'Bat_LP_Anim')]
    rigs = list({o.find_armature() for o in meshes})
    if not meshes or len(rigs) != 1:
        raise ValueError('No standalone skinned animal')
    rig = rigs[0]
    rig.hide_render = False
    rig.hide_set(False)
    keep = set(meshes + [rig])
    for o in list(keep):
        parent = o.parent
        while parent:
            keep.add(parent)
            parent = parent.parent
    for o in list(bpy.data.objects):
        if o not in keep:
            bpy.data.objects.remove(o, do_unlink=True)
    for o in keep:
        o.hide_render = False
        o.hide_viewport = False
        o.hide_set(False)
    # Each source action is baked through the original IK/constraints by glTF.
    rig.animation_data_create()
    rig.animation_data.action = None
    for t in list(rig.animation_data.nla_tracks):
        rig.animation_data.nla_tracks.remove(t)
    clips = []
    for action in bpy.data.actions:
        if not any(fc.data_path.startswith('pose.bones[') for fc in action.fcurves):
            continue
        if action.frame_range[1] - action.frame_range[0] > 600:
            continue
        if name == 'bat' and not action.name.startswith('Bat_'):
            continue
        track = rig.animation_data.nla_tracks.new()
        track.name = action.name
        track.strips.new(action.name, 0, action)
        track.mute = True
        clips.append({'name': action.name, 'duration_seconds': (action.frame_range[1] - action.frame_range[0]) / bpy.context.scene.render.fps})
    rig.data.pose_position = 'REST'
    for mesh in meshes:
        for mod in list(mesh.modifiers):
            if mod.type == 'SUBSURF':
                mesh.modifiers.remove(mod)
        for p in mesh.data.polygons:
            p.use_smooth = True
    for mat in {slot.material for o in meshes for slot in o.material_slots if slot.material}:
        image = image_for(entry, mat)
        # Existing modern node-based colour materials retain their authored colour.
        if mat.use_nodes and not image:
            continue
        color = tuple(mat.diffuse_color)
        mat.use_nodes = True
        mat.node_tree.nodes.clear()
        output = mat.node_tree.nodes.new('ShaderNodeOutputMaterial')
        bsdf = mat.node_tree.nodes.new('ShaderNodeBsdfPrincipled')
        bsdf.inputs['Base Color'].default_value = color
        bsdf.inputs['Roughness'].default_value = 0.85
        bsdf.inputs['Specular IOR Level'].default_value = 0.2
        mat.node_tree.links.new(bsdf.outputs['BSDF'], output.inputs['Surface'])
        if image:
            if max(image.size) > 1024:
                factor = 1024 / max(image.size)
                image.scale(round(image.size[0]*factor), round(image.size[1]*factor))
            converted = QA / 'converted-textures'
            converted.mkdir(exist_ok=True)
            image.filepath_raw = str(converted / (name + '_' + ''.join(c if c.isalnum() else '_' for c in image.name) + '.png'))
            image.file_format = 'PNG'
            image.save()
            image.pack()
            tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
            tex.image = image
            mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
            mat.node_tree.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
            mat.blend_method = 'CLIP'
            mat.alpha_threshold = 0.45
    lo, hi = bounds(meshes)
    scale = SIZES[name] / max(hi-lo)
    container = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(container)
    for o in keep:
        if o.parent not in keep:
            world = o.matrix_world.copy()
            o.parent = container
            o.matrix_world = world
    container.scale = (scale,)*3
    container.location.z = -lo.z * scale
    provenance = json.loads((ROOT / 'data/source-assets/animals/opengameart' / name / 'provenance.json').read_text())
    rig['asset_license'] = provenance['license']
    rig['asset_author'] = provenance['author']
    rig['asset_source'] = provenance['source']
    # Candidate files stay outside game assets until visual review accepts them.
    candidate_dir = QA / 'candidates'
    candidate_dir.mkdir(exist_ok=True)
    files = [candidate_dir / (name + '.glb'), candidate_dir / (name + '-lod1.glb')]
    export(files[0], list(keep) + [container])
    rig.data.pose_position = 'REST'
    render(meshes, QA / (name + '.png'))
    triangles = sum(sum(len(p.vertices)-2 for p in o.data.polygons) for o in meshes)
    for o in meshes:
        bpy.context.view_layer.objects.active = o
        mod = o.modifiers.new('LOD1_preserve_skin', 'DECIMATE')
        mod.ratio = 0.55 if triangles > 500 else 0.7
        mod.use_collapse_triangulate = True
        with quiet():
            bpy.ops.object.modifier_apply(modifier=mod.name)
    export(files[1], list(keep) + [container])
    rig.data.pose_position = 'REST'
    render(meshes, QA / (name + '-lod1.png'))
    return {'id': name, 'status': 'candidate', 'files': [f.name for f in files], 'source_file': entry['file'], 'source_url': provenance['source'], 'author': provenance['author'], 'license': provenance['license'], 'bones': len(rig.data.bones), 'animations': clips, 'source_triangles': triangles, 'scale': {'max_dimension_metres': SIZES[name], 'basis': 'approximate species size; source proportions preserved'}}


if __name__ == '__main__':
    results = []
    for entry in json.loads((ROOT / 'data/animal-pipeline/oga-inspection.json').read_text()):
        if entry['id'] not in SIZES:
            continue
        if len(sys.argv) > 1 and '--' in sys.argv and entry['id'] not in sys.argv[sys.argv.index('--')+1:]:
            continue
        try:
            result = prepare(entry)
        except Exception as error:
            result = {'id': entry['id'], 'status': 'rejected', 'reason': str(error), 'traceback': traceback.format_exc()}
        (QA / ('oga_' + entry['id'] + '.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
        results.append(result)
        print('ANIMAL_RESULT', result['id'], result['status'], flush=True)
    (QA / 'results_oga.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
