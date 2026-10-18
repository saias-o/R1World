"""Bake a relightable, multi-view Far from the existing Mid, without Blender.

python -m r1.tree_impostor (from game/tools; numpy, Pillow, moderngl required).
Forty views retain scanned albedo, normals and static visibility; sunlight stays live.
Only the bake uses OpenGL; the shipped asset uses Saida's billboard material.
"""
from __future__ import annotations
import hashlib
import io
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image

from .tree_lod import Builder, NATURE, read_glb, write_glb
from .decimate import _read_accessor

COLUMNS, ROWS, RESOLUTION, SUPERSAMPLE = 8, 5, 256, 2
OCCLUSION_DIRECTIONS = 32
OCCLUSION_RESOLUTION = 512
# One shadow texel avoids self-intersection without opening canopy-sized gaps.
OCCLUSION_BIAS_TEXELS = 1.0
TARGET = NATURE / "urban_tree_far.glb"

VERTEX = """#version 330
in vec3 position; in vec3 normal; in vec2 uv; in vec4 tangent;
uniform mat4 projection;
out vec2 texcoord; out vec3 surfaceNormal; out vec3 surfaceTangent;
out vec3 surfaceBitangent; out vec3 surfacePosition;
void main() {
 gl_Position=projection*vec4(position,1); texcoord=uv;
 surfaceNormal=normal; surfaceTangent=tangent.xyz;
 surfaceBitangent=cross(normal,tangent.xyz)*tangent.w; surfacePosition=position;
}
"""
FRAGMENT = """#version 330
in vec2 texcoord;
in vec3 surfaceNormal; in vec3 surfaceTangent; in vec3 surfaceBitangent;
in vec3 surfacePosition;
uniform sampler2D albedo; uniform sampler2D normalMap;
uniform vec4 color; uniform float cutoff;
uniform bool mappedNormal; uniform float normalScale;
layout(location=0) out vec4 bakedColor;
layout(location=1) out vec4 bakedNormal;
layout(location=2) out vec4 bakedPosition;
vec3 linearColor(vec3 c) {return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),greaterThan(c,vec3(.04045)));}
void main() {
 vec4 sampleColor=texture(albedo,texcoord); if(sampleColor.a*color.a<cutoff) discard;
 bakedColor=vec4(linearColor(sampleColor.rgb)*color.rgb,1.);
 vec3 n=normalize(surfaceNormal);
 if(mappedNormal) {
  vec3 detail=texture(normalMap,texcoord).rgb*2.-1.; detail.xy*=normalScale;
  n=normalize(mat3(normalize(surfaceTangent),normalize(surfaceBitangent),n)*detail);
 }
 if(!gl_FrontFacing) n=-n;
 bakedNormal=vec4(n,1.); bakedPosition=vec4(surfacePosition,1.);
}
"""


def sphere_directions(count):
    """Equal-area directions; no selected sun direction enters the bake."""
    indices = np.arange(count, dtype=np.float32)
    y = 1 - 2 * (indices + .5) / count
    angle = indices * math.pi * (3 - math.sqrt(5))
    radius = np.sqrt(1 - y * y)
    return np.stack((radius * np.cos(angle), y, radius * np.sin(angle)), axis=1)


def ambient_visibility(positions, normals, shadows):
    """Cosine-weighted hemisphere visibility against the source alpha geometry."""
    visible = np.zeros(positions.shape[:2], dtype=np.float32)
    weights = np.zeros_like(visible)
    bias = OCCLUSION_BIAS_TEXELS / OCCLUSION_RESOLUTION
    for direction, projection, depth in shadows:
        weight = np.maximum(normals @ direction, 0)
        projected = positions @ projection[:3, :3].T + projection[:3, 3]
        uv = projected[:, :, :2] * .5 + .5
        texels = np.clip((uv * OCCLUSION_RESOLUTION).astype(np.int32), 0, OCCLUSION_RESOLUTION - 1)
        nearest = depth[texels[:, :, 1], texels[:, :, 0]]
        visible += weight * (projected[:, :, 2] * .5 + .5 <= nearest + bias)
        weights += weight
    return visible / np.maximum(weights, 1e-8)


def covered_attachment(fbo, index, alpha, resolution=RESOLUTION):
    """Downsample surface attributes without treating transparent samples as data."""
    size = resolution * SUPERSAMPLE
    samples = np.frombuffer(fbo.read(components=4, attachment=index, dtype="f4"), dtype=np.float32)
    samples = samples.reshape(size, size, 4)[::-1]
    reduced = samples.reshape(resolution, SUPERSAMPLE, resolution, SUPERSAMPLE, 4).mean(axis=(1, 3))
    return reduced[:, :, :3] / np.maximum(alpha[:, :, None], 1e-8)

def transformed_parts(doc, binary):
    """Resolve glTF hierarchy once, so the atlas and its bounds share a frame."""
    parts = []
    def visit(index, parent):
        node = doc["nodes"][index]
        if "matrix" in node:
            matrix = np.asarray(node["matrix"], dtype=np.float32).reshape(4,4).T
        else:
            x,y,z,w = node.get("rotation", [0,0,0,1])
            matrix = np.eye(4, dtype=np.float32)
            matrix[:3,:3] = [[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                             [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                             [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]]
            matrix[:3,:3] *= np.asarray(node.get("scale", [1,1,1]))
            matrix[:3,3] = node.get("translation", [0,0,0])
        world = parent @ matrix
        if "mesh" in node:
            for primitive in doc["meshes"][node["mesh"]]["primitives"]:
                attributes = primitive["attributes"]
                read = lambda key: np.asarray(_read_accessor(doc, [binary], attributes[key]),dtype=np.float32)
                positions = read("POSITION") @ world[:3,:3].T + world[:3,3]
                normals = read("NORMAL") @ np.linalg.inv(world[:3,:3])
                normals /= np.maximum(np.linalg.norm(normals,axis=1,keepdims=True),1e-8)
                tangents = read("TANGENT") if "TANGENT" in attributes else np.zeros((len(positions),4),dtype=np.float32)
                tangents[:,:3] = tangents[:,:3] @ world[:3,:3].T
                tangents[:,:3] /= np.maximum(np.linalg.norm(tangents[:,:3],axis=1,keepdims=True),1e-8)
                indices = np.asarray(_read_accessor(doc,[binary],primitive["indices"]),dtype=np.uint32).flatten()
                parts.append((np.concatenate((positions,normals,read("TEXCOORD_0"),tangents),axis=1),indices,
                              primitive["material"],"TANGENT" in attributes))
        for child in node.get("children",[]):visit(child,world)
    for root in doc["scenes"][doc.get("scene",0)]["nodes"]:visit(root,np.eye(4,dtype=np.float32))
    return parts

def encode_color(linear):
    return np.where(linear<=.0031308,linear*12.92,1.055*np.maximum(linear,0)**(1/2.4)-.055)

def png(array):
    stream=io.BytesIO();Image.fromarray(np.uint8(np.clip(array,0,1)*255+.5)).save(stream,format="PNG")
    return stream.getvalue()

def dilate(rgb, alpha, steps=8):
    """Extend edge colour without alpha; transparent mip texels stay leaf green."""
    covered=alpha>0
    rgb=rgb.copy()
    for _ in range(steps):
        total=np.zeros_like(rgb);count=np.zeros_like(alpha)
        for dy,dx in ((-1,0),(1,0),(0,-1),(0,1)):
            mask=np.roll(covered,(dy,dx),(0,1))
            total+=np.roll(rgb,(dy,dx),(0,1))*mask[:,:,None];count+=mask
        fill=(~covered)&(count>0)
        rgb[fill]=total[fill]/count[fill,None];covered|=fill
    return rgb

def bake(source=NATURE/"urban_tree_mid.glb", target=TARGET, *, reference_level="Mid", columns=COLUMNS, alpha_cutoff=.35, resolution=RESOLUTION):
    import moderngl  # Only baking needs an OpenGL context; asset checks do not.
    target.parent.mkdir(parents=True,exist_ok=True)
    doc,binary=read_glb(source);parts=transformed_parts(doc,binary)
    points=np.concatenate([p[0][:,:3] for p in parts]);low=points.min(axis=0);high=points.max(axis=0)
    center=(low+high)/2
    extent=float(2*np.linalg.norm(points-center,axis=1).max()*1.06)
    ctx=moderngl.create_standalone_context(require=330)
    program=ctx.program(vertex_shader=VERTEX,fragment_shader=FRAGMENT)
    program["albedo"].value=0;program["normalMap"].value=1
    textures={}
    def texture(ref, fallback):
        if not ref:return fallback
        index=ref["index"]
        if index not in textures:
            image=doc["images"][doc["textures"][index]["source"]];view=doc["bufferViews"][image["bufferView"]]
            offset=view.get("byteOffset",0)
            im=Image.open(io.BytesIO(binary[offset:offset+view["byteLength"]])).convert("RGBA")
            textures[index]=ctx.texture(im.size,4,im.tobytes())
            textures[index].filter=(moderngl.LINEAR,moderngl.LINEAR)
        return textures[index]
    white=ctx.texture((1,1),4,bytes([255]*4))
    neutral=ctx.texture((1,1),4,bytes([128,128,255,255]))
    draws=[]
    for vertices,indices,index,tangents in parts:
        mat=doc["materials"][index];pbr=mat.get("pbrMetallicRoughness",{})
        vao=ctx.vertex_array(program,[(ctx.buffer(vertices.astype("f4").tobytes()),"3f 3f 2f 4f","position","normal","uv","tangent")],ctx.buffer(indices.tobytes()))
        draws.append((vao,texture(pbr.get("baseColorTexture"),white),
                      texture(mat.get("normalTexture"),neutral),
                      pbr.get("baseColorFactor",[1,1,1,1]),mat.get("alphaCutoff",.5) if mat.get("alphaMode")=="MASK" else 0.,
                      tangents and "normalTexture" in mat,mat.get("normalTexture",{}).get("scale",1),
                      mat.get("doubleSided",False)))
    size=resolution*SUPERSAMPLE
    colors=ctx.texture((size,size),4,dtype="f4")
    normals=ctx.texture((size,size),4,dtype="f4")
    positions=ctx.texture((size,size),4,dtype="f4")
    fbo=ctx.framebuffer([colors,normals,positions],ctx.depth_renderbuffer((size,size)))
    ctx.enable(moderngl.DEPTH_TEST);ctx.depth_func="<="
    def setup_view(direction,right):
        up=np.cross(direction,right);frame=np.stack((right,up,direction),axis=1)
        projection=np.eye(4,dtype=np.float32);projection[:3,:3]=frame.T*2/extent;projection[2,:3]*=-1
        projection[:3,3]=-projection[:3,:3]@center
        program["projection"].write(projection.T.astype("f4").tobytes())
        return frame,projection
    def draw():
        for vao,color,normal,factor,cutoff,mapped,scale,double in draws:
            ctx.disable(moderngl.CULL_FACE) if double else ctx.enable(moderngl.CULL_FACE)
            color.use(0);normal.use(1);program["color"].value=tuple(factor)
            program["cutoff"].value=cutoff;program["mappedNormal"].value=mapped
            program["normalScale"].value=scale;vao.render()
    shadow_depth=ctx.depth_texture((OCCLUSION_RESOLUTION,OCCLUSION_RESOLUTION))
    shadow_fbo=ctx.framebuffer(depth_attachment=shadow_depth)
    shadows=[]
    for direction in sphere_directions(OCCLUSION_DIRECTIONS):
        right=np.array([direction[2],0,-direction[0]],dtype=np.float32)
        right/=np.linalg.norm(right)
        _,projection=setup_view(direction,right)
        shadow_fbo.use();shadow_fbo.clear(depth=1);draw()
        depth=np.frombuffer(shadow_depth.read(),dtype=np.float32).reshape(OCCLUSION_RESOLUTION,OCCLUSION_RESOLUTION)
        shadows.append((direction,projection,depth))
    albedo_atlas=np.zeros((ROWS*resolution,columns*resolution,4),dtype=np.float32)
    normal_atlas=np.zeros_like(albedo_atlas)
    for row in range(ROWS):
        pitch=-math.pi/2+row*math.pi/(ROWS-1)
        for column in range(columns):
            yaw=column*2*math.pi/columns
            direction=np.array([math.sin(yaw)*math.cos(pitch),math.sin(pitch),math.cos(yaw)*math.cos(pitch)],dtype=np.float32)
            right=np.array([math.cos(yaw),0,-math.sin(yaw)],dtype=np.float32)
            setup_view(direction,right)
            fbo.use();fbo.clear(0,0,0,0,depth=1)
            draw()
            samples=np.frombuffer(fbo.read(components=4,attachment=0,dtype="f4"),dtype=np.float32).reshape(size,size,4)[::-1]
            pixels=samples.reshape(resolution,SUPERSAMPLE,resolution,SUPERSAMPLE,4).mean(axis=(1,3))
            alpha=pixels[:,:,3];rgb=pixels[:,:,:3]/np.maximum(alpha[:,:,None],1e-8)
            rgb=dilate(encode_color(rgb),alpha)
            mapped=covered_attachment(fbo,1,alpha,resolution)
            mapped/=np.maximum(np.linalg.norm(mapped,axis=2,keepdims=True),1e-8)
            visibility=ambient_visibility(covered_attachment(fbo,2,alpha,resolution),mapped,shadows)
            visibility=dilate(visibility[:,:,None],alpha)[:,:,0]
            normal_rgb=dilate(mapped*.5+.5,alpha)
            y,x=row*resolution,column*resolution
            albedo_atlas[y:y+resolution,x:x+resolution]=np.concatenate((rgb,alpha[:,:,None]),axis=2)
            normal_atlas[y:y+resolution,x:x+resolution]=np.concatenate((normal_rgb,visibility[:,:,None]),axis=2)
    builder=Builder();builder.doc["asset"]["generator"]="R1World tree_impostor.py"
    for name,atlas in (("albedo",albedo_atlas),("normal",normal_atlas)):
        builder.doc["images"].append({"bufferView":builder.store(png(atlas)),"mimeType":"image/png"})
        builder.doc["textures"].append({"source":len(builder.doc["images"])-1})
        Image.fromarray(np.uint8(np.clip(atlas,0,1)*255+.5)).save(target.with_name(target.stem+"_"+name+".png"))
    builder.doc["materials"].append({"name":f"{reference_level} canopy impostor","doubleSided":True,"alphaMode":"MASK","alphaCutoff":alpha_cutoff,
        "pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":.85},
        "normalTexture":{"index":1},
        "extras":{"SAIDA_billboard":{"columns":columns,"rows":ROWS}}})
    h=extent/2;vertices=[(-h,-h,0),(h,-h,0),(h,h,0),(-h,h,0)]
    builder.mesh("Far",{"POSITION":builder.accessor(vertices,"VEC3",5126),
        "NORMAL":builder.accessor([(0,0,1)]*4,"VEC3",5126),"TANGENT":builder.accessor([(1,0,0,1)]*4,"VEC4",5126),
        "TEXCOORD_0":builder.accessor([(0,1),(1,1),(1,0),(0,0)],"VEC2",5126)},
        builder.accessor([(i,) for i in (0,1,2,0,2,3)],"SCALAR",5123),0)
    builder.doc["nodes"][0]["translation"]=[float(v) for v in center]
    target.parent.mkdir(parents=True,exist_ok=True);write_glb(target,builder.doc,bytes(builder.binary))
    origin=next(e for e in json.loads((NATURE/"SOURCES.json").read_text(encoding="utf8")) if e["output"]==source.name)
    report={"output":target.name,"derivedFrom":source.name,"sourceSha256":hashlib.sha256(source.read_bytes()).hexdigest(),
        "columns":columns,"rows":ROWS,"resolution":resolution,"supersample":SUPERSAMPLE,"vertices":4,"triangles":2,
        "extent":extent,"pivot":[float(v) for v in center],"attribution":origin["attribution"],
        "occlusionDirections":OCCLUSION_DIRECTIONS,"occlusionResolution":OCCLUSION_RESOLUTION,
        "method":f"multi-view {reference_level} albedo/alpha, authored-frame mapped normals and cosine-weighted hemisphere visibility in normal alpha; no baked sun; shared relightable quad"}
    target.with_suffix(".source.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf8",newline="\n")
    manifest_path=NATURE/"SOURCES.json"
    manifest=json.loads(manifest_path.read_text(encoding="utf8"))
    manifest=[report if entry.get("output")==target.name else entry for entry in manifest]
    if not any(entry.get("output")==target.name for entry in manifest):manifest.append(report)
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n",encoding="utf8",newline="\n")
    ctx.release();return report

if __name__=="__main__":print(json.dumps(bake(),ensure_ascii=False))
