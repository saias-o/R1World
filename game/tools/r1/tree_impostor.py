"""Bake a relightable, multi-view Far from the existing Mid, without Blender.

python -m r1.tree_impostor (from game/tools; numpy, Pillow, moderngl required).
Forty orthographic views retain scanned albedo and alpha; lighting stays live.
Only the bake uses OpenGL; the shipped asset uses Saida's billboard material.
"""
from __future__ import annotations
import hashlib
import io
import json
import math
from pathlib import Path

import moderngl
import numpy as np
from PIL import Image

from .tree_lod import Builder, NATURE, read_glb, write_glb
from .decimate import _read_accessor

COLUMNS, ROWS, RESOLUTION, SUPERSAMPLE = 8, 5, 256, 2
TARGET = NATURE / "urban_tree_far.glb"

VERTEX = """#version 330
in vec3 position; in vec3 normal; in vec2 uv; in vec4 tangent;
uniform mat4 projection;
out vec2 texcoord;
void main() { gl_Position=projection*vec4(position,1); texcoord=uv; }
"""
FRAGMENT = """#version 330
in vec2 texcoord;
uniform sampler2D albedo;
uniform vec4 color; uniform float cutoff;
layout(location=0) out vec4 bakedColor;
vec3 linearColor(vec3 c) {return mix(c/12.92,pow((c+.055)/1.055,vec3(2.4)),greaterThan(c,vec3(.04045)));}
void main() {
 vec4 sampleColor=texture(albedo,texcoord); if(sampleColor.a*color.a<cutoff) discard;
 bakedColor=vec4(linearColor(sampleColor.rgb)*color.rgb,1.);

}
"""

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

def bake(source=NATURE/"urban_tree_mid.glb", target=TARGET):
    target.parent.mkdir(parents=True,exist_ok=True)
    doc,binary=read_glb(source);parts=transformed_parts(doc,binary)
    points=np.concatenate([p[0][:,:3] for p in parts]);low=points.min(axis=0);high=points.max(axis=0)
    center=(low+high)/2
    extent=float(2*np.linalg.norm(points-center,axis=1).max()*1.06)
    ctx=moderngl.create_standalone_context(require=330)
    program=ctx.program(vertex_shader=VERTEX,fragment_shader=FRAGMENT)
    program["albedo"].value=0
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
    draws=[]
    for vertices,indices,index,tangents in parts:
        mat=doc["materials"][index];pbr=mat.get("pbrMetallicRoughness",{})
        vao=ctx.vertex_array(program,[(ctx.buffer(vertices.astype("f4").tobytes()),"3f 12x 2f 16x","position","uv")],ctx.buffer(indices.tobytes()))
        draws.append((vao,texture(pbr.get("baseColorTexture"),white),
                      pbr.get("baseColorFactor",[1,1,1,1]),mat.get("alphaCutoff",.5) if mat.get("alphaMode")=="MASK" else 0.,
                      mat.get("doubleSided",False)))
    size=RESOLUTION*SUPERSAMPLE
    colors=ctx.texture((size,size),4,dtype="f4")
    fbo=ctx.framebuffer([colors],ctx.depth_renderbuffer((size,size)))
    ctx.enable(moderngl.DEPTH_TEST);ctx.depth_func="<="
    def setup_view(direction,right):
        up=np.cross(direction,right);frame=np.stack((right,up,direction),axis=1)
        projection=np.eye(4,dtype=np.float32);projection[:3,:3]=frame.T*2/extent;projection[2,:3]*=-1
        projection[:3,3]=-projection[:3,:3]@center
        program["projection"].write(projection.T.astype("f4").tobytes())
        return frame,projection
    def draw():
        for vao,color,factor,cutoff,double in draws:
            ctx.disable(moderngl.CULL_FACE) if double else ctx.enable(moderngl.CULL_FACE)
            color.use(0);program["color"].value=tuple(factor)
            program["cutoff"].value=cutoff;vao.render()
    albedo_atlas=np.zeros((ROWS*RESOLUTION,COLUMNS*RESOLUTION,4),dtype=np.float32)
    for row in range(ROWS):
        pitch=-math.pi/2+row*math.pi/(ROWS-1)
        for column in range(COLUMNS):
            yaw=column*2*math.pi/COLUMNS
            direction=np.array([math.sin(yaw)*math.cos(pitch),math.sin(pitch),math.cos(yaw)*math.cos(pitch)],dtype=np.float32)
            right=np.array([math.cos(yaw),0,-math.sin(yaw)],dtype=np.float32)
            setup_view(direction,right)
            fbo.use();fbo.clear(0,0,0,0,depth=1)
            draw()
            samples=np.frombuffer(fbo.read(components=4,attachment=0,dtype="f4"),dtype=np.float32).reshape(size,size,4)[::-1]
            pixels=samples.reshape(RESOLUTION,SUPERSAMPLE,RESOLUTION,SUPERSAMPLE,4).mean(axis=(1,3))
            alpha=pixels[:,:,3];rgb=pixels[:,:,:3]/np.maximum(alpha[:,:,None],1e-8)
            rgb=dilate(encode_color(rgb),alpha)
            y,x=row*RESOLUTION,column*RESOLUTION
            albedo_atlas[y:y+RESOLUTION,x:x+RESOLUTION]=np.concatenate((rgb,alpha[:,:,None]),axis=2)
    builder=Builder();builder.doc["asset"]["generator"]="R1World tree_impostor.py"
    for name,atlas in (("albedo",albedo_atlas),):
        builder.doc["images"].append({"bufferView":builder.store(png(atlas)),"mimeType":"image/png"})
        builder.doc["textures"].append({"source":len(builder.doc["images"])-1})
        Image.fromarray(np.uint8(np.clip(atlas,0,1)*255+.5)).save(target.with_name(target.stem+"_"+name+".png"))
    builder.doc["materials"].append({"name":"Mid canopy impostor","doubleSided":True,"alphaMode":"MASK","alphaCutoff":.35,
        "pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":.85},
        "extras":{"SAIDA_billboard":{"columns":COLUMNS,"rows":ROWS}}})
    h=extent/2;vertices=[(-h,-h,0),(h,-h,0),(h,h,0),(-h,h,0)]
    builder.mesh("Far",{"POSITION":builder.accessor(vertices,"VEC3",5126),
        "NORMAL":builder.accessor([(0,0,1)]*4,"VEC3",5126),"TANGENT":builder.accessor([(1,0,0,1)]*4,"VEC4",5126),
        "TEXCOORD_0":builder.accessor([(0,1),(1,1),(1,0),(0,0)],"VEC2",5126)},
        builder.accessor([(i,) for i in (0,1,2,0,2,3)],"SCALAR",5123),0)
    builder.doc["nodes"][0]["translation"]=[float(v) for v in center]
    target.parent.mkdir(parents=True,exist_ok=True);write_glb(target,builder.doc,bytes(builder.binary))
    origin=next(e for e in json.loads((NATURE/"SOURCES.json").read_text(encoding="utf8")) if e["output"]==source.name)
    report={"output":target.name,"derivedFrom":source.name,"sourceSha256":hashlib.sha256(source.read_bytes()).hexdigest(),
        "columns":COLUMNS,"rows":ROWS,"resolution":RESOLUTION,"supersample":SUPERSAMPLE,"vertices":4,"triangles":2,
        "extent":extent,"pivot":[float(v) for v in center],"attribution":origin["attribution"],
        "method":"multi-view orthographic Mid albedo/alpha; no baked sun; shared camera-facing quad with coarse live ambient/directional lighting"}
    target.with_suffix(".source.json").write_text(json.dumps(report,ensure_ascii=False,indent=2)+"\n",encoding="utf8")
    ctx.release();return report

if __name__=="__main__":print(json.dumps(bake(),ensure_ascii=False))
