"""One Rocketbox avatar, baked into a game-ready GLB. Runs inside Blender 4.2.

    blender -b --factory-startup -P tools/r1/humans_blender.py -- job.json

`r1.humans` writes the job and reads the report; nothing here downloads or
decides which avatar goes where. The steps, in order:

* The avatar's FBX (3ds Max biped, centimetres) is brought to metres, feet on
  z = 0, facing Blender's -Y (glTF +Z, like every model this game places).
* The face rig -- jaw, lips, eyelids, 28 bones under the head -- is folded
  into the head: nobody in a street is close enough to see a lip move, and
  each bone is a matrix per character per frame.
* Each clip is a Rocketbox motion-capture FBX on the same biped. It is
  retargeted by copying every bone's world rotation (and the pelvis's
  position, scaled by the two skeletons' legs) and baking. Where the library
  extracted the travel onto the root (`xy` clips) the travel is removed and
  measured: the game moves the character, the clip only moves the limbs, and
  the speed it was captured at is what the game walks it at.
* The jump is not in the library. It is the flight phase of the sprint --
  both feet off the ground -- held for the time the game's jump lasts.
* Levels of detail are the scan's own mesh, decimated; the textures arrive
  already reduced by `r1.humans`.
"""
import json
import math
import sys

import bpy
import mathutils

job = json.load(open(sys.argv[sys.argv.index("--") + 1], encoding="utf-8"))
FPS = 30
HEAD = "Bip01 Head"
PELVIS = "Bip01 Pelvis"


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.scene.render.fps = FPS


def new_objects(before):
    return [o for o in bpy.data.objects if o.name not in before]


def import_fbx(path, animated):
    before = {o.name for o in bpy.data.objects}
    bpy.ops.import_scene.fbx(filepath=path, use_anim=animated, ignore_leaf_bones=False,
                             automatic_bone_orientation=False)
    return new_objects(before)


def delete(objects):
    for o in objects:
        bpy.data.objects.remove(o, do_unlink=True)


def forward_of(arm, world):
    """The way a biped faces, from its left foot to its left toe."""
    bones = arm.data.bones
    toe, foot = bones["Bip01 L Toe0"].head_local, bones["Bip01 L Foot"].head_local
    d = world.to_3x3() @ (toe - foot)
    return math.atan2(d.x, -d.y)  # 0 when facing -Y


def quarter(angle):
    return round(angle / (math.pi / 2)) * (math.pi / 2)


# ── the avatar ─────────────────────────────────────────────────────────────

reset()
imported = import_fbx(job["avatar"], False)
arm = next(o for o in imported if o.type == "ARMATURE")
body = next(o for o in imported if o.type == "MESH")
delete([o for o in imported if o not in (arm, body)])
for action in list(bpy.data.actions):
    bpy.data.actions.remove(action)
arm.animation_data_clear()

# Metres, facing -Y, no parent transform left to compose.
world = body.matrix_world.copy()
body.parent = None
body.matrix_world = world
turn = -quarter(forward_of(arm, arm.matrix_world))
for o in (arm, body):
    o.matrix_world = mathutils.Matrix.Rotation(turn, 4, "Z") @ o.matrix_world
    bpy.context.view_layer.objects.active = o
    for other in bpy.context.view_layer.objects:
        other.select_set(other is o)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
floor = min(v.co.z for v in body.data.vertices)
arm.location.z -= floor
body.location.z -= floor
for o in (arm, body):
    bpy.context.view_layer.objects.active = o
    for other in bpy.context.view_layer.objects:
        other.select_set(other is o)
    bpy.ops.object.transform_apply(location=True, rotation=False, scale=False)
body.parent = arm
height = max(v.co.z for v in body.data.vertices)

# The face rig folds into the head.
face = []
stack = [c for c in arm.data.bones[HEAD].children]
while stack:
    b = stack.pop()
    face.append(b.name)
    stack.extend(b.children)
head_group = body.vertex_groups[HEAD]
indices = {g.index: g.name for g in body.vertex_groups}
for v in body.data.vertices:
    moved = sum(g.weight for g in v.groups if indices[g.group] in face)
    if moved > 0:
        head_group.add([v.index], moved, "ADD")
for name in face:
    if name in body.vertex_groups:
        body.vertex_groups.remove(body.vertex_groups[name])
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode="EDIT")
for name in face:
    arm.data.edit_bones.remove(arm.data.edit_bones[name])
bpy.ops.object.mode_set(mode="OBJECT")
for pb in arm.pose.bones:
    pb.rotation_mode = "QUATERNION"
pelvis_height = (arm.matrix_world @ arm.data.bones[PELVIS].head_local).z


def leg(armature):
    """Thigh plus shin as posed, in world metres: a bent knee does not
    shorten it, and a clip's own rest pose is not what it was captured on."""
    world = armature.matrix_world
    head = lambda n: world @ armature.pose.bones[n].head
    return (head("Bip01 L Thigh") - head("Bip01 L Calf")).length + (head("Bip01 L Calf") - head("Bip01 L Foot")).length


leg_length = leg(arm)

# Materials: the scan's own colour maps, reduced; no specular or normal maps,
# which is what a character at this distance can spare.
for slot in body.material_slots:
    kind = next(k for k in ("opacity", "head", "body") if slot.material.name.endswith(k))
    if kind not in job["textures"]:
        raise SystemExit(f"{job['name']}: material {slot.material.name} has no texture")
    mat = bpy.data.materials.new(f"{job['name']}_{kind}")
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes["Principled BSDF"]
    tex = nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(job["textures"][kind])
    links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    bsdf.inputs["Roughness"].default_value = 0.8
    bsdf.inputs["Metallic"].default_value = 0.0
    if kind == "opacity":
        links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
        mat.blend_method = "CLIP"
        mat.alpha_threshold = 0.5
    slot.material = mat

# ── the clips ──────────────────────────────────────────────────────────────

clips = {}


def bake_clip(spec):
    imported = import_fbx(spec["fbx"], True)
    source = next(o for o in imported if o.type == "ARMATURE")
    action = source.animation_data.action
    f0, f1 = (int(round(x)) for x in action.frame_range)
    duration = (f1 - f0) / FPS
    # The travel the library extracted onto the root, removed and measured.
    travel = 0.0
    curves = {fc.array_index: fc for fc in action.fcurves if fc.data_path == "location"}
    if spec.get("inPlace"):
        start = [curves[i].evaluate(f0) if i in curves else 0.0 for i in range(3)]
        end = [curves[i].evaluate(f1) if i in curves else 0.0 for i in range(3)]
        travel = math.hypot(end[0] - start[0], end[1] - start[1])
        for i in (0, 1):
            if i not in curves:
                continue
            for k in curves[i].keyframe_points:
                shift = start[i] + (end[i] - start[i]) * (k.co.x - f0) / max(1, f1 - f0)
                for p in (k.co, k.handle_left, k.handle_right):
                    p.y -= shift
    bpy.context.scene.frame_set(f0)
    rig = mathutils.Matrix.Rotation(-quarter(forward_of(source, source.matrix_world)), 4, "Z")
    # The two skeletons' sizes, from their legs: a clip's first
    # frame may already be sitting.
    ratio = leg_length / max(1e-6, leg(source))
    holder = bpy.data.objects.new("holder", None)
    bpy.context.scene.collection.objects.link(holder)
    holder.matrix_world = rig @ mathutils.Matrix.Scale(ratio, 4)
    source.parent = holder
    delete([o for o in imported if o is not source])

    for pb in arm.pose.bones:
        if pb.name not in source.pose.bones:
            continue
        c = pb.constraints.new("COPY_ROTATION")
        c.target, c.subtarget = source, pb.name
        c.owner_space = c.target_space = "WORLD"
        if pb.name == PELVIS:
            c = pb.constraints.new("COPY_LOCATION")
            c.target, c.subtarget = source, pb.name
            c.owner_space = c.target_space = "WORLD"
    bpy.context.view_layer.objects.active = arm
    for other in bpy.context.view_layer.objects:
        other.select_set(other is arm)
    bpy.ops.object.mode_set(mode="POSE")
    bpy.ops.pose.select_all(action="SELECT")
    bpy.ops.nla.bake(frame_start=f0, frame_end=f1, only_selected=True, visual_keying=True,
                     clear_constraints=True, use_current_action=False, bake_types={"POSE"})
    bpy.ops.object.mode_set(mode="OBJECT")
    baked = arm.animation_data.action
    baked.name = spec["name"]
    arm.animation_data.action = None

    # Travel in the source's metres, at the avatar's own size.
    speed = travel * ratio / duration if duration > 0 else 0.0
    stretch = 1.0
    if spec.get("speed") and speed > 0:
        # Played at `speed` m/s by a character drawn at `scale`: the feet
        # keep pace with the ground only if the clip is retimed to it.
        stretch = speed * job.get("scale", 1.0) / spec["speed"]
        for fc in baked.fcurves:
            for k in fc.keyframe_points:
                for p in (k.co, k.handle_left, k.handle_right):
                    p.x = f0 + (p.x - f0) * stretch
        speed = spec["speed"] / job.get("scale", 1.0)
    delete([source, holder])
    for a in list(bpy.data.actions):
        if a is not baked and a.users == 0:
            bpy.data.actions.remove(a)
    baked.use_fake_user = True
    clips[spec["name"]] = {"action": baked, "start": f0, "end": f0 + (f1 - f0) * stretch,
                           "seconds": round(duration * stretch, 3), "speed": round(speed, 3),
                           "source": spec["fbx"].replace("\\", "/").split("/")[-1]}


for spec in job["clips"]:
    bake_clip(spec)


def feet_clear(action, frame):
    arm.animation_data.action = action
    bpy.context.scene.frame_set(frame)
    feet = [(arm.matrix_world @ arm.pose.bones[n].head).z for n in ("Bip01 L Toe0", "Bip01 R Toe0")]
    return min(feet)


if job.get("jump"):
    # The flight phase of the sprint: the frame where the lower foot is highest.
    src = clips[job["jump"]]
    frames = range(int(src["start"]), int(src["end"]) + 1)
    flight = max(frames, key=lambda f: feet_clear(src["action"], f))
    arm.animation_data.action = src["action"]
    bpy.context.scene.frame_set(flight)
    pose = {pb.name: (pb.location.copy(), pb.rotation_quaternion.copy()) for pb in arm.pose.bones}
    jump = bpy.data.actions.new("jump")
    arm.animation_data.action = jump
    hold = int(round(job.get("jumpSeconds", 0.74) * FPS))
    for frame in (1, 1 + hold):
        for pb in arm.pose.bones:
            pb.location, pb.rotation_quaternion = pose[pb.name]
            pb.keyframe_insert("location", frame=frame)
            pb.keyframe_insert("rotation_quaternion", frame=frame)
    arm.animation_data.action = None
    jump.use_fake_user = True
    clips["jump"] = {"action": jump, "start": 1, "end": 1 + hold, "seconds": round(hold / FPS, 3),
                     "speed": 0.0, "source": f"{src['source']} (flight frame {flight})"}

# The seated clip's pelvis, for placing a sitter on a bench of any height.
seat = None
if "sit" in clips:
    arm.animation_data.action = clips["sit"]["action"]
    bpy.context.scene.frame_set(int(clips["sit"]["start"]))
    seat = (arm.matrix_world @ arm.pose.bones[PELVIS].head)
    seat = {"pelvisHeight": round(seat.z, 3), "pelvisForward": round(-seat.y, 3)}
    arm.animation_data.action = None

# Every clip on its own NLA track: the exporter writes one animation per action.
arm.animation_data_create()
for name, clip in clips.items():
    track = arm.animation_data.nla_tracks.new()
    track.name = name
    track.strips.new(name, int(clip["start"]), clip["action"])
    track.mute = True
bpy.context.scene.frame_set(1)

# ── levels of detail ───────────────────────────────────────────────────────

lods = []
for spec in job["lods"]:
    mesh = body.copy()
    mesh.data = body.data.copy()
    mesh.name = mesh.data.name = spec["name"]
    bpy.context.scene.collection.objects.link(mesh)
    bpy.context.view_layer.objects.active = mesh
    for other in bpy.context.view_layer.objects:
        other.select_set(other is mesh)
    triangles = sum(len(p.vertices) - 2 for p in mesh.data.polygons)
    if spec.get("triangles") and spec["triangles"] < triangles:
        mod = mesh.modifiers.new("decimate", "DECIMATE")
        mod.ratio = spec["triangles"] / triangles
        mod.use_collapse_triangulate = True
        bpy.ops.object.modifier_move_to_index(modifier=mod.name, index=0)
        bpy.ops.object.modifier_apply(modifier=mod.name)
    bpy.ops.object.vertex_group_limit_total(limit=4)
    bpy.ops.object.vertex_group_normalize_all(lock_active=False)
    lods.append(mesh)
delete([body])

for other in bpy.context.scene.objects:
    other.select_set(False)
bpy.ops.export_scene.gltf(
    filepath=job["out"], export_format="GLB", use_selection=False, export_yup=True,
    export_apply=False, export_animations=True, export_animation_mode="NLA_TRACKS",
    export_force_sampling=True, export_frame_step=job.get("frameStep", 1), export_def_bones=True, export_skins=True,
    export_all_influences=False, export_morph=False, export_image_format="AUTO",
    export_texcoords=True, export_normals=True, export_tangents=False, export_materials="EXPORT",
    export_optimize_animation_size=True, export_anim_single_armature=True,
    export_reset_pose_bones=True, export_rest_position_armature=True, export_extras=False,
    export_cameras=False, export_lights=False)

report = {
    "height": round(height, 3),
    "pelvisHeight": round(pelvis_height, 3),
    "bones": len(arm.data.bones),
    "faceBonesFolded": len(face),
    "lods": [{"name": m.name, "triangles": sum(len(p.vertices) - 2 for p in m.data.polygons),
              "vertices": len(m.data.vertices)} for m in lods],
    "clips": {n: {k: v for k, v in c.items() if k not in ("action", "start", "end")} for n, c in clips.items()},
}
if seat:
    report["seat"] = seat
json.dump(report, open(job["report"], "w", encoding="utf-8"), indent=1)
print("R1 HUMAN", json.dumps(report))
