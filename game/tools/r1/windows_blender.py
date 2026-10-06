"""Photograph the window modules of a facade kit into texture maps. Runs
inside Blender 4.2:

    blender -b --factory-startup -P tools/r1/windows_blender.py -- job.json

`r1.windows` writes the job and composes the facade sheets from what this
writes; nothing here downloads or decides which window goes on which wall.

Each module is framed by the wall module it sits in (one bay by one storey of
the kit), seen from the street through an orthographic camera, and drawn
once per map with every material turned into an emission of one of its own
inputs, so that a pixel holds that input, unlit:

* `color`  -- the base colour (linear);
* `normal` -- the shading normal, normal map included, in the wall's frame
  (x along the wall, y up, z out of it), as 0.5 + 0.5 n;
* `rm`     -- roughness (g) and metalness (b), as the engine packs them;
* `depth`  -- metres in front of the wall plane (r), negative behind it;
* `glass`  -- on glazing, r 1 and g the glass's own opacity (the film of
  dirt the kit draws on it, through which the room behind shows); 0 elsewhere.

The film is transparent, so every map's alpha is the window's coverage.
Each map is written as a float array (`.npy`, rows top-down) beside its EXR.
"""
import json
import sys

import bpy
import mathutils
import numpy as np

job = json.load(open(sys.argv[sys.argv.index("--") + 1], encoding="utf-8"))
PASSES = ("color", "normal", "rm", "depth", "glass")


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = job.get("samples", 16)
    scene.cycles.use_denoising = False
    scene.cycles.max_bounces = 0
    scene.render.film_transparent = True
    scene.view_settings.view_transform = "Raw"
    scene.view_settings.look = "None"
    scene.render.image_settings.file_format = "OPEN_EXR"
    scene.render.image_settings.color_depth = "32"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.resolution_percentage = 100


def principled(material):
    for node in material.node_tree.nodes:
        if node.type == "BSDF_PRINCIPLED":
            return node
    raise RuntimeError(f"{material.name}: no principled BSDF")


def source(node, name):
    """The socket feeding `node`'s input `name`, or None when unlinked."""
    socket = node.inputs[name]
    return socket.links[0].from_socket if socket.is_linked else None


def emit(material, pass_name):
    """Turn `material` into an emission of one of its inputs."""
    tree = material.node_tree
    nodes, links = tree.nodes, tree.links
    bsdf = principled(material)
    out = next(n for n in nodes if n.type == "OUTPUT_MATERIAL")
    emission = nodes.new("ShaderNodeEmission")
    emission.inputs["Strength"].default_value = 1.0
    colour = emission.inputs["Color"]
    if pass_name == "color":
        base = source(bsdf, "Base Color")
        if base: links.new(base, colour)
        else: colour.default_value = bsdf.inputs["Base Color"].default_value
    elif pass_name == "normal":
        shading = source(bsdf, "Normal")
        if shading is None:
            shading = nodes.new("ShaderNodeNewGeometry").outputs["Normal"]
        # The wall's frame from the world's: x along it, y up (world z),
        # z out of it (world -y).
        split = nodes.new("ShaderNodeSeparateXYZ")
        links.new(shading, split.inputs["Vector"])
        out_of_wall = nodes.new("ShaderNodeMath"); out_of_wall.operation = "MULTIPLY"
        out_of_wall.inputs[1].default_value = -1.0
        links.new(split.outputs["Y"], out_of_wall.inputs[0])
        wall = nodes.new("ShaderNodeCombineXYZ")
        links.new(split.outputs["X"], wall.inputs["X"])
        links.new(split.outputs["Z"], wall.inputs["Y"])
        links.new(out_of_wall.outputs["Value"], wall.inputs["Z"])
        half = nodes.new("ShaderNodeVectorMath"); half.operation = "MULTIPLY_ADD"
        half.inputs[1].default_value = (0.5, 0.5, 0.5)
        half.inputs[2].default_value = (0.5, 0.5, 0.5)
        links.new(wall.outputs["Vector"], half.inputs[0])
        links.new(half.outputs["Vector"], colour)
    elif pass_name == "rm":
        combine = nodes.new("ShaderNodeCombineColor")
        for channel, name in (("Green", "Roughness"), ("Blue", "Metallic")):
            feed = source(bsdf, name)
            if feed: links.new(feed, combine.inputs[channel])
            else: combine.inputs[channel].default_value = bsdf.inputs[name].default_value
        links.new(combine.outputs["Color"], colour)
    elif pass_name == "depth":
        geometry = nodes.new("ShaderNodeNewGeometry")
        split = nodes.new("ShaderNodeSeparateXYZ")
        links.new(geometry.outputs["Position"], split.inputs["Vector"])
        # The wall faces -y: metres in front of it are -y.
        negate = nodes.new("ShaderNodeMath"); negate.operation = "MULTIPLY"
        negate.inputs[1].default_value = -1.0
        links.new(split.outputs["Y"], negate.inputs[0])
        combine = nodes.new("ShaderNodeCombineColor")
        links.new(negate.outputs["Value"], combine.inputs["Red"])
        links.new(combine.outputs["Color"], colour)
    elif pass_name == "glass":
        combine = nodes.new("ShaderNodeCombineColor")
        if material.name in job["glassMaterials"]:
            combine.inputs["Red"].default_value = 1.0
            # The kit's own opacity map, on the glazing's UVs.
            opacity = nodes.new("ShaderNodeTexImage")
            opacity.image = bpy.data.images.load(job["glassOpacity"], check_existing=True)
            opacity.image.colorspace_settings.name = "Non-Color"
            links.new(opacity.outputs["Color"], combine.inputs["Green"])
        links.new(combine.outputs["Color"], colour)
    for link in list(out.inputs["Surface"].links):
        links.remove(link)
    links.new(emission.outputs["Emission"], out.inputs["Surface"])
    # Opaque: the glazing's own alpha would let the void behind it through.
    material.blend_method = "OPAQUE"


def framed(module, frame):
    """An orthographic camera on `frame`'s bounding box, seen from -y."""
    corners = [frame.matrix_world @ mathutils.Vector(c) for c in frame.bound_box]
    lo = mathutils.Vector([min(c[i] for c in corners) for i in range(3)])
    hi = mathutils.Vector([max(c[i] for c in corners) for i in range(3)])
    width, height = hi.x - lo.x, hi.z - lo.z
    camera_data = bpy.data.cameras.new("cam")
    camera_data.type = "ORTHO"
    camera_data.ortho_scale = max(width, height)
    camera_data.clip_start = 0.01
    camera_data.clip_end = 100.0
    camera = bpy.data.objects.new("cam", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = ((lo.x + hi.x) / 2, -10.0, (lo.z + hi.z) / 2)
    camera.rotation_euler = mathutils.Euler((1.5707963, 0.0, 0.0))
    bpy.context.scene.camera = camera
    scene = bpy.context.scene
    scene.render.resolution_x = round(job["pixelsPerMetre"] * width)
    scene.render.resolution_y = round(job["pixelsPerMetre"] * height)
    return {"width": width, "height": height,
            "pixelsX": scene.render.resolution_x, "pixelsY": scene.render.resolution_y}


report = {}
for module in job["modules"]:
    report[module["name"]] = {}
    for pass_name in PASSES:
        reset()
        bpy.ops.import_scene.gltf(filepath=job["gltf"])
        window = bpy.data.objects[module["name"]]
        frame = bpy.data.objects[module["frame"]]
        for obj in bpy.data.objects:
            if obj.type == "MESH" and obj is not window:
                obj.hide_render = True
        for material in {slot.material for slot in window.material_slots if slot.material}:
            emit(material, pass_name)
        size = framed(window, frame)
        path = f"{job['out']}/{module['name']}_{pass_name}.exr"
        bpy.context.scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(path)
        pixels = np.array(image.pixels[:], dtype=np.float32).reshape(size["pixelsY"], size["pixelsX"], 4)
        # Blender's rows run bottom-up; a texture's top-down.
        np.save(path.replace(".exr", ".npy"), pixels[::-1])
        report[module["name"]] = {**size, "frame": module["frame"]}
        print("rendered", path, size)

json.dump(report, open(job["report"], "w", encoding="utf-8"), indent=1)
