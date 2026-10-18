"""Finalise motorcycle exports: material roles, independent pivots and metadata.

Geometry is authored by motorcycles_blender.py from the hash-pinned CC0 sources.
This pass keeps its embedded images and shading; it never restores a dense source
on top of an optimised asset. It is safe to run repeatedly.
"""
from __future__ import annotations
import argparse
import json
import struct
import io
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
FLEET = GAME / "assets/models/vehicles/fleet.json"


def _chunks(data: bytes):
    if data[:4] != b"glTF" or struct.unpack_from("<I", data, 4)[0] != 2:
        raise ValueError("Expected GLB 2.0")
    if struct.unpack_from("<I", data, 8)[0] != len(data):
        raise ValueError("Truncated GLB")
    result=[]; offset=12
    while offset<len(data):
        length,kind=struct.unpack_from("<II",data,offset);offset+=8
        result.append((kind,data[offset:offset+length]));offset+=length
    return result


def _glb(doc, chunks):
    packed=[]
    for kind,payload in chunks:
        if kind==0x4e4f534a:
            payload=json.dumps(doc,separators=(",",":"),ensure_ascii=False).encode("utf-8")
            payload+=b" "*(-len(payload)%4)
        packed.extend((struct.pack("<II",len(payload),kind),payload))
    body=b"".join(packed)
    return b"glTF"+struct.pack("<II",2,12+len(body))+body


def _stats(doc):
    vertices=triangles=0
    for mesh in doc["meshes"]:
        for primitive in mesh["primitives"]:
            vertices+=doc["accessors"][primitive["attributes"]["POSITION"]]["count"]
            triangles+=doc["accessors"][primitive["indices"]]["count"]//3
    return dict(vertices=vertices,triangles=triangles,drawTriangles=triangles)


def role(name):
    lower=name.lower()
    if lower.startswith(("paint-","rubber-","alloy-","trim-","headlamp-","taillamp-","indicator-")):
        return lower.split("-",1)[0]
    if "paint" in lower or lower=="red":return "paint"
    if "rubber" in lower:return "rubber"
    if any(x in lower for x in ("steel","chrome","aluminium","iron")):return "alloy"
    if lower=="tail" or "optical lens" in lower:return "taillamp"
    if lower=="lamp":return "headlamp"
    if lower=="orange":return "indicator"
    return "trim"


def finalise(path, texture_size=512):
    chunks=_chunks(path.read_bytes())
    doc=json.loads(next(payload for kind,payload in chunks if kind==0x4e4f534a))
    if doc.get('images'):
        from PIL import Image
        binary=next(payload for kind,payload in chunks if kind==0x004e4942)
        replacements={}
        for image in doc['images']:
            index=image['bufferView'];view=doc['bufferViews'][index]
            start=view.get('byteOffset',0)
            source=Image.open(io.BytesIO(binary[start:start+view['byteLength']]))
            if max(source.size)<=texture_size:continue
            source.thumbnail((texture_size,texture_size),Image.Resampling.LANCZOS)
            output=io.BytesIO();source.save(output,format='PNG',compress_level=7)
            replacements[index]=output.getvalue();image['mimeType']='image/png'
        if replacements:
            packed=bytearray()
            for index,view in enumerate(doc['bufferViews']):
                start=view.get('byteOffset',0)
                data=replacements.get(index,binary[start:start+view['byteLength']])
                packed.extend(b'\0'*(-len(packed)%4))
                view['byteOffset']=len(packed);view['byteLength']=len(data)
                packed.extend(data)
            packed.extend(b'\0'*(-len(packed)%4));doc['buffers'][0]['byteLength']=len(packed)
            chunks=[(kind,bytes(packed) if kind==0x004e4942 else data) for kind,data in chunks]
    for material in doc["materials"]:
        name=material.get("name","material")
        if name.split("-",1)[0] in ("paint","rubber","alloy","trim","headlamp","taillamp","indicator"):
            name=name.split("-",1)[1]
        material["name"]=role(name)+"-"+name
    # Mesh primitives need named children so the runtime's paint/deformation
    # roles never recolour tyres, and wheel pivots enclose all wheel materials.
    if not doc.get("extras",{}).get("r1MotorcycleFinalised"):
        meshes=[]
        for node in list(doc["nodes"]):
            if node.get("name")=="wheel-rear":node["name"]="wheel-back"
            if "mesh" not in node:continue
            source=doc["meshes"][node.pop("mesh")]
            children=node.setdefault("children",[])
            for primitive in source["primitives"]:
                material=doc["materials"][primitive["material"]]
                name=material["name"]+"-"+node.get("name","part")
                child={"name":name,"mesh":len(meshes)}
                meshes.append({"name":name,"primitives":[primitive]})
                children.append(len(doc["nodes"]));doc["nodes"].append(child)
        doc["meshes"]=meshes
        doc.setdefault("extras",{})["r1MotorcycleFinalised"]=True
    path.write_bytes(_glb(doc,chunks))
    return _stats(doc)


def update_manifest():
    fleet=json.loads(FLEET.read_text(encoding="utf-8"))
    for entry in fleet["vehicles"]:
        if not entry["name"].startswith("moto_"):continue
        entry["authoredForward"]="+Z"
        for level in ("near","far"):
            entry[level].update(finalise(GAME/entry[level]["path"],512 if level=='near' else 128))
            print(entry["name"],level,entry[level]["triangles"],"triangles")
    fleet["totalVertices"]=sum(v[level]["vertices"] for v in fleet["vehicles"] for level in ("near","far"))
    FLEET.write_text(json.dumps(fleet,indent=2)+"\n",encoding="utf-8")
    path=GAME/"assets/THIRD_PARTY_ASSETS.json"
    credits=json.loads(path.read_text(encoding="utf-8"))
    for entry in credits["assets"]:
        if any("vehicles/moto_" in f for f in entry.get("files",[])):
            entry["generatedBy"]="game/tools/r1/motorcycles_blender.py"
            entry["modifications"]="Pinned source models, reduced Near/Far geometry with preserved textures and smooth shading; +Z forward, independent wheel pivots and separated paint/rubber/metal materials."
    path.write_text(json.dumps(credits,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")


if __name__=="__main__":update_manifest()
