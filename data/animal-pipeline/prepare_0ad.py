"""Headless fauna preparation. Blender 4.2, CPU rendering, two threads.

Produces independent GLB + LOD1 with embedded textures and preserved skins.
No runtime code is imported, changed or launched.
"""
import contextlib
import json
import math
import os
from pathlib import Path
import re
import sys
import traceback
import xml.etree.ElementTree as ET

import bpy
from mathutils import Matrix, Vector

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'data/source-assets/animals/0ad'
OUT = ROOT / 'game/assets/animals'
QA = ROOT / 'data/animal-pipeline/review'
for p in (OUT, QA):
    p.mkdir(parents=True, exist_ok=True)
ARGS = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
ONLY = set(ARGS)


@contextlib.contextmanager
def quiet():
    # Python exporter logs only; Blender's native output remains in the log file.
    with open(os.devnull, 'w') as sink, contextlib.redirect_stdout(sink):
        yield


def imported(path):
    before = set(bpy.data.objects)
    # Some valid-for-0ad legacy files omit Collada's required texcoord name in
    # unused material effects. Repair a working copy, never the downloaded file.
    document = ET.parse(path)
    ns = 'http://www.collada.org/2005/11/COLLADASchema'
    missing = [t for t in document.getroot().iter('{'+ns+'}texture') if 'texcoord' not in t.attrib]
    if missing:
        for texture in missing:
            texture.set('texcoord','UVMap')
        ET.register_namespace('',ns)
        corrected = QA / 'converted-collada' / path.relative_to(SRC)
        corrected.parent.mkdir(parents=True,exist_ok=True)
        document.write(corrected,encoding='utf-8',xml_declaration=True)
        path = corrected
    with quiet():
        bpy.ops.wm.collada_import(filepath=str(path), import_units=True)
    return list(set(bpy.data.objects) - before)


def bounds(meshes):
    bpy.context.view_layer.update()
    depsgraph=bpy.context.evaluated_depsgraph_get()
    corners=[]
    for obj in meshes:
        evaluated=obj.evaluated_get(depsgraph)
        mesh=evaluated.to_mesh()
        corners.extend(evaluated.matrix_world @ v.co for v in mesh.vertices)
        evaluated.to_mesh_clear()
    return Vector([min(c[i] for c in corners) for i in range(3)]), Vector([max(c[i] for c in corners) for i in range(3)])


def expected_size(name):
    sizes = {'elephant_african_baby': 2.1, 'elephant_african_bush': 6.5,
             'elephant_african_forest': 5.3, 'elephant_asian': 5.5,
             'giraffe_adult': 5.2, 'giraffe_baby': 2.2,
             'whale_fin': 20.0, 'whale': 14.0, 'walrus': 3.2,
             'hippopotamus': 3.6, 'rhino': 3.8, 'crocodile': 4.2,
             'bear_white': 2.5, 'bear_brown': 2.3, 'bear_black': 1.8,
             'lion': 2.8, 'lioness': 2.4, 'tiger': 2.9,
             'wolf': 1.7, 'arctic_wolf': 1.7, 'fox_red': 1.0, 'fox_arctic': 0.85,
             'boar': 1.5, 'cow': 2.5, 'bull': 2.6, 'sanga_cattle': 2.6,
             'zebu': 2.5, 'zebu_wild': 2.7, 'muskox': 2.3, 'camel': 3.2,
             'wildebeest': 2.3, 'zebra': 2.5, 'zebra_2': 2.5,
             'deer': 2.0, 'gazelle': 1.4, 'goat': 1.2, 'sheep1': 1.3, 'sheep2': 1.3, 'sheep3': 1.3,
             'pig1': 1.5, 'piglet': 0.65, 'rabbit1': 0.45, 'mastiff': 1.1,
             'donkey': 2.0, 'pony': 1.8, 'chicken': 0.6, 'peacock': 2.0,
             'crow': 0.85, 'seagull': 1.3, 'hawk': 1.1,
             'fish_generic': 0.4, 'fish_single': 0.4, 'tuna': 1.7, 'shark': 3.5}
    sizes.update({'wolfhound':1.25,'turkey':1.1})
    return sizes.get(name, 2.7 if name.startswith('horse') else 2.0)


def set_material(meshes, texture, normal=None):
    mat = bpy.data.materials.new('animal_surface')
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get('Principled BSDF')
    bsdf.inputs['Roughness'].default_value = 0.87
    bsdf.inputs['Specular IOR Level'].default_value = 0.22
    tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
    tex.image = bpy.data.images.load(str(SRC / 'textures/skins' / texture), check_existing=True)
    # PNG is portable; DDS sources are kept unchanged in source-assets.
    if max(tex.image.size) > 1024:
        factor = 1024 / max(tex.image.size)
        tex.image.scale(max(1, round(tex.image.size[0] * factor)), max(1, round(tex.image.size[1] * factor)))
    tex.image.file_format = 'PNG'
    tex.image.pack()
    mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    # Source fauna uses transparent fur/feather/tail cards. Alpha clip also
    # works for fully opaque textures and avoids expensive blend sorting.
    mat.node_tree.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
    mat.surface_render_method = 'DITHERED'
    mat.use_backface_culling = False
    mat['gltf_alpha_mode'] = 'MASK'
    # glTF exporter in Blender 4.2 reads the legacy blend_method property.
    mat.blend_method = 'CLIP'
    mat.alpha_threshold = 0.45
    if normal:
        ntex = mat.node_tree.nodes.new('ShaderNodeTexImage')
        ntex.image = bpy.data.images.load(str(SRC / 'textures/skins' / normal), check_existing=True)
        ntex.image.colorspace_settings.name = 'Non-Color'
        ntex.image.file_format = 'PNG'
        ntex.image.pack()
        norm = mat.node_tree.nodes.new('ShaderNodeNormalMap')
        mat.node_tree.links.new(ntex.outputs['Color'], norm.inputs['Color'])
        mat.node_tree.links.new(norm.outputs['Normal'], bsdf.inputs['Normal'])
    for o in meshes:
        o.data.materials.clear()
        o.data.materials.append(mat)
        for poly in o.data.polygons:
            poly.use_smooth = True
            poly.material_index = 0


def name_key(name):
    return re.sub(r'[^a-z0-9]', '', name.lower())


def transfer_animations(rig, records):
    info, warnings = [], []
    rig.animation_data_create()
    for record in records:
        objects = imported(SRC / 'animation' / record['file'])
        anim_rigs = [o for o in objects if o.type == 'ARMATURE' and o.animation_data and o.animation_data.action]
        if not anim_rigs:
            warnings.append({'file': record['file'], 'reason': 'no armature action on import'})
        else:
            source = max(anim_rigs, key=lambda o: len(o.data.bones))
            action = source.animation_data.action.copy()
            action.name = Path(record['file']).stem
            target_names = {name_key(b.name): b.name for b in rig.data.bones}
            source_names = {b.name: target_names.get(name_key(b.name)) for b in source.data.bones}
            missing = [n for n, t in source_names.items() if t is None]
            coverage = sum(t is not None for t in source_names.values()) / max(len(source_names), 1)
            if coverage < 0.8:
                warnings.append({'file': record['file'], 'reason': 'incompatible bone names', 'coverage': coverage, 'missing': missing})
                bpy.data.actions.remove(action)
            else:
                ratios = []
                for sb in source.data.bones:
                    target_name = source_names[sb.name]
                    if target_name and sb.head_local.length > 1e-4:
                        ratios.append(rig.data.bones[target_name].head_local.length / sb.head_local.length)
                factor = sorted(ratios)[len(ratios) // 2] if ratios else 1.0
                # Blender's legacy Collada importer double-applies an armature
                # object scale to the root animation of some old 3ds Max rigs
                # (goat: 440.47x). Remove that constant carrier transform only;
                # retain the root's authored motion and all child animation.
                corrected_roots = []
                for sb in source.data.bones:
                    if sb.parent:
                        continue
                    prefix = 'pose.bones[' + json.dumps(sb.name) + ']'
                    scales = [fc for fc in action.fcurves if fc.data_path == prefix + '.scale']
                    if scales and any(abs(fc.keyframe_points[0].co.y) > 10 for fc in scales):
                        corrected_roots.append(sb.name)
                        for fc in scales:
                            carrier = fc.keyframe_points[0].co.y
                            for point in fc.keyframe_points:
                                point.co.y /= carrier
                                point.handle_left.y /= carrier
                                point.handle_right.y /= carrier
                        for fc in action.fcurves:
                            if fc.data_path == prefix + '.location':
                                carrier = fc.keyframe_points[0].co.y
                                for point in fc.keyframe_points:
                                    point.co.y -= carrier
                                    point.handle_left.y -= carrier
                                    point.handle_right.y -= carrier
                for fc in list(action.fcurves):
                    match = re.match(r'pose\.bones\["(.+)"\]\.(.+)', fc.data_path)
                    if not match or not source_names.get(match.group(1)):
                        action.fcurves.remove(fc)
                        continue
                    target_name = source_names[match.group(1)]
                    fc.data_path = 'pose.bones[' + json.dumps(target_name) + '].' + match.group(2)
                    if match.group(2) == 'location':
                        for point in fc.keyframe_points:
                            point.co.y *= factor
                            point.handle_left.y *= factor
                            point.handle_right.y *= factor
                action.use_fake_user = True
                track = rig.animation_data.nla_tracks.new()
                track.name = action.name
                strip = track.strips.new(action.name, 0, action)
                strip.action_frame_start = action.frame_range[0]
                strip.action_frame_end = action.frame_range[1]
                strip.extrapolation = 'NOTHING'
                track.mute = True
                info.append({'name': action.name, 'source': record['file'], 'source_speed_percent': record.get('speed'), 'duration_seconds': (action.frame_range[1] - action.frame_range[0]) / bpy.context.scene.render.fps, 'bone_coverage': coverage, 'translation_rebase_scale': factor, 'collada_carrier_scale_corrected_roots': corrected_roots})
        for o in objects:
            bpy.data.objects.remove(o, do_unlink=True)
    rig.animation_data.action = None
    return info, warnings


def export(path, objects):
    bpy.ops.object.select_all(action='DESELECT')
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = next(o for o in objects if o.type == 'ARMATURE')
    rigs = [o for o in objects if o.type == 'ARMATURE']
    for rig in rigs:
        rig.data.pose_position = 'POSE'
        if rig.animation_data:
            for t in rig.animation_data.nla_tracks:
                t.mute = False
    with quiet():
        bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB', use_selection=True,
            export_animations=True, export_animation_mode='NLA_TRACKS', export_frame_range=False,
            export_force_sampling=True, export_skins=True, export_def_bones=False,
            export_yup=True, export_image_format='AUTO', export_cameras=False, export_lights=False,
            export_extras=True, export_anim_slide_to_zero=True)
    for rig in rigs:
        if rig.animation_data:
            for t in rig.animation_data.nla_tracks:
                t.mute = True


def render(meshes, dest):
    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 8
    scene.cycles.use_denoising = False
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 2
    scene.render.resolution_x = 400
    scene.render.resolution_y = 320
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.world = bpy.data.worlds.new('review_world')
    scene.world.color = (0.19, 0.19, 0.19)
    scene.render.film_transparent = False
    scene.view_settings.view_transform = 'AgX'
    lo, hi = bounds(meshes)
    center = (lo + hi) / 2
    size = max(hi - lo)
    cam_data = bpy.data.cameras.new('review_camera')
    cam = bpy.data.objects.new('review_camera', cam_data)
    scene.collection.objects.link(cam)
    cam.location = center + Vector((1.4, -2.2, 1.0)) * size
    cam.rotation_euler = (center - cam.location).to_track_quat('-Z', 'Y').to_euler()
    cam_data.type = 'ORTHO'
    cam_data.clip_start = max(size / 1000, 0.000001)
    cam_data.ortho_scale = size * 1.5
    cam_data.clip_end = size * 20
    scene.camera = cam
    lamps = []
    for position, energy in [((2, -3, 4), 120), ((-3, -1, 2), 75), ((0, 3, 3), 100)]:
        ld = bpy.data.lights.new('review_softbox', 'AREA')
        lamp = bpy.data.objects.new('review_softbox', ld)
        scene.collection.objects.link(lamp)
        lamp.location = center + Vector(position) * size
        lamp.rotation_euler = (center - lamp.location).to_track_quat('-Z', 'Y').to_euler()
        ld.energy = energy * size * size
        ld.shape = 'DISK'
        ld.size = size * 3
        lamps.append(lamp)
    scene.render.filepath = str(dest)
    with quiet():
        bpy.ops.render.render(write_still=True)
    for o in lamps + [cam]:
        bpy.data.objects.remove(o, do_unlink=True)


def prepare(entry, texture_index=0, mesh_index=0):
    name = entry['id']
    if texture_index:
        name += '_coat' + str(texture_index + 1)
    if mesh_index:
        name += '_shape' + str(mesh_index + 1)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 30
    objects = imported(SRC / 'meshes' / entry['meshes'][mesh_index])
    meshes = [o for o in objects if o.type == 'MESH']
    rigs = [o for o in objects if o.type == 'ARMATURE']
    if not meshes or len(rigs) != 1 or not any(o.vertex_groups for o in meshes):
        return {'id': name, 'status': 'rejected', 'reason': 'no usable skinned mesh'}
    rig = rigs[0]
    rig['asset_license'] = 'CC-BY-SA-3.0'
    rig['asset_author'] = entry.get('author', 'Wildfire Games')
    rig['asset_source'] = entry['actor']
    clips, warnings = transfer_animations(rig, entry['animations'])
    scene.frame_set(0)
    rig.data.pose_position = 'REST'
    lo, hi = bounds(meshes)
    factor = expected_size(entry['id']) / max(hi - lo)
    container = bpy.data.objects.new(name, None)
    scene.collection.objects.link(container)
    for obj in objects:
        if obj.parent not in objects:
            world = obj.matrix_world.copy()
            obj.parent = container
            obj.matrix_world = world
    container.scale = (factor,) * 3
    container.location.z = -lo.z * factor
    set_material(meshes, entry['textures'][texture_index], entry['normals'][0] if entry['normals'] else None)
    # Keep meaningful details in LOD0; no reduction of the author's main mesh.
    main = OUT / (name + '.glb')
    export(main, objects + [container])
    rig.data.pose_position = 'REST'
    render(meshes, QA / (name + '.png'))
    source_triangles = sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in meshes)
    for obj in meshes:
        bpy.context.view_layer.objects.active = obj
        modifier = obj.modifiers.new('LOD1_preserve_skin', 'DECIMATE')
        modifier.decimate_type = 'COLLAPSE'
        modifier.ratio = 0.55 if source_triangles > 500 else 0.7
        modifier.use_collapse_triangulate = True
        # No armature evaluation is baked: weights and the original rig survive.
        with quiet():
            bpy.ops.object.modifier_apply(modifier=modifier.name)
    lod = OUT / (name + '-lod1.glb')
    export(lod, objects + [container])
    rig.data.pose_position = 'REST'
    render(meshes, QA / (name + '-lod1.png'))
    return {'id': name, 'status': 'prepared', 'source_actor': entry['actor'], 'source_mesh': entry['meshes'][mesh_index], 'source_texture': entry['textures'][texture_index], 'license': 'CC-BY-SA-3.0', 'author': entry.get('author', 'Wildfire Games'), 'repository': entry.get('repository','https://github.com/0ad/0ad'), 'revision': entry.get('revision', json.loads((SRC/'manifest.json').read_text())['revision']), 'files': [main.name, lod.name], 'bones': len(rig.data.bones), 'animations': clips, 'animation_warnings': warnings, 'scale': {'max_dimension_metres': expected_size(entry['id']), 'basis': 'approximate species size; source proportions preserved'}, 'source_triangles': source_triangles}


if __name__ == '__main__':
    manifest = json.loads((SRC / 'manifest.json').read_text())
    results = []
    seen = set()
    for entry in manifest['assets']:
        if 'skip' in entry or (ONLY and entry['id'] not in ONLY):
            continue
        for mi, mesh in enumerate(entry['meshes']):
            for ti, texture in enumerate(entry['textures']):
                if any(token in texture.lower() for token in ['_dead','_death','_blood']):
                    continue
                key = (mesh, texture)
                if key in seen:
                    continue
                seen.add(key)
                ident = entry['id'] + ('_coat' + str(ti + 1) if ti else '') + ('_shape' + str(mi + 1) if mi else '')
                report = QA / (ident + '.json')
                if report.exists() and not ONLY:
                    previous = json.loads(report.read_text())
                    if previous['status'] == 'prepared':
                        results.append(previous)
                        continue
                try:
                    result = prepare(entry, ti, mi)
                except Exception as error:
                    result = {'id': ident, 'status': 'error', 'reason': str(error), 'traceback': traceback.format_exc()}
                report.write_text(json.dumps(result, indent=2), encoding='utf-8')
                results.append(result)
                print('ANIMAL_RESULT', ident, result['status'], result.get('source_triangles', ''), flush=True)
    (QA / ('results_sample.json' if ONLY else 'results_0ad.json')).write_text(json.dumps(results, indent=2), encoding='utf-8')
