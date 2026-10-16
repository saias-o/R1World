"""Authoring check: compare LOD silhouettes/albedos from identical directions.

Orthographic, alpha-tested renders isolate representation differences from
runtime shadows and lighting. Billboard views follow the shipped atlas layout.
This tool is offline authoring QA; it does not replace the game's renderer.
"""
import io
import math
from pathlib import Path

import moderngl
import numpy as np
from PIL import Image

from .tree_lod import read_glb
from .tree_impostor import transformed_parts

VERTEX = """#version 330
in vec3 position; in vec2 texcoord;
uniform mat4 projection;
out vec2 uv;
void main(){ gl_Position=projection*vec4(position,1); uv=texcoord; }
"""
FRAGMENT = """#version 330
uniform sampler2D albedo;
uniform vec4 color;
uniform float cutoff;
uniform vec4 atlas;
in vec2 uv; out vec4 pixel;
void main(){
 vec4 s;
 if(atlas.x>0){
  vec2 size=vec2(atlas.x,5);
  vec4 a=texture(albedo,(uv+atlas.yz)/size);
  vec4 b=texture(albedo,(uv+vec2(mod(atlas.y+1,atlas.x),atlas.z))/size);
  float alpha=mix(a.a,b.a,atlas.w);
  s=vec4(mix(a.rgb*a.a,b.rgb*b.a,atlas.w)/max(alpha,.0001),alpha);
 }else s=texture(albedo,uv);
 if(s.a*color.a<cutoff)discard;
 pixel=vec4(s.rgb*color.rgb,1);
}
"""


def compare(reference: Path, variants: dict[str, Path], output: Path, resolution=256, azimuth_step=45):
    output.mkdir(parents=True, exist_ok=True)
    ctx=moderngl.create_standalone_context(require=330)
    program=ctx.program(vertex_shader=VERTEX,fragment_shader=FRAGMENT)
    program['albedo'].value=0
    size=resolution*2
    fbo=ctx.framebuffer([ctx.texture((size,size),4,dtype='f4')],ctx.depth_renderbuffer((size,size)))
    ctx.enable(moderngl.DEPTH_TEST);ctx.depth_func='<='
    resources=[]

    def model(path):
        doc,binary=read_glb(path);parts=transformed_parts(doc,binary);draws=[]
        for v,i,m,_ in parts:
            material=doc['materials'][m];pbr=material.get('pbrMetallicRoughness',{})
            ref=pbr.get('baseColorTexture')
            if ref:
                im=doc['images'][doc['textures'][ref['index']]['source']]
                view=doc['bufferViews'][im['bufferView']];offset=view.get('byteOffset',0)
                picture=Image.open(io.BytesIO(binary[offset:offset+view['byteLength']])).convert('RGBA')
                pixels=np.asarray(picture,dtype='f4')/255
                rgb=pixels[:,:,:3]
                pixels[:,:,:3]=np.where(rgb<=.04045,rgb/12.92,((rgb+.055)/1.055)**2.4)
                texture=ctx.texture(picture.size,4,pixels.tobytes(),dtype='f4')
            else:texture=ctx.texture((1,1),4,bytes([255]*4))
            texture.filter=(moderngl.LINEAR,moderngl.LINEAR)
            vertex=ctx.buffer(reserve=len(v)*5*4,dynamic=True)
            element=ctx.buffer(i.astype('u4').tobytes())
            vao=ctx.vertex_array(program,[(vertex,'3f 2f','position','texcoord')],element)
            resources.extend((texture,vertex,element,vao))
            atlas=material.get('extras',{}).get('SAIDA_billboard')
            draws.append((v,vertex,vao,texture,pbr.get('baseColorFactor',[1]*4),
                          material.get('alphaCutoff',.5) if material.get('alphaMode')=='MASK' else 0,
                          material.get('doubleSided',False),atlas))
        return parts,draws

    models={'LOD0':model(reference),**{k:model(v) for k,v in variants.items()}}
    points=np.concatenate([p[0][:,:3] for p in models['LOD0'][0]])
    center=(points.min(0)+points.max(0))/2
    extent=float(2*np.linalg.norm(points-center,axis=1).max()*1.06)

    def render(draws,yaw,pitch):
        direction=np.array([math.sin(yaw)*math.cos(pitch),math.sin(pitch),math.cos(yaw)*math.cos(pitch)])
        right=np.array([math.cos(yaw),0,-math.sin(yaw)])
        frame=np.stack((right,np.cross(direction,right),direction),axis=1)
        projection=np.eye(4,dtype='f4');projection[:3,:3]=frame.T*2/extent;projection[2,:3]*=-1
        projection[:3,3]=-projection[:3,:3]@center
        program['projection'].write(projection.T.tobytes())
        fbo.use();fbo.clear(0,0,0,0,depth=1)
        for v,vertex,vao,texture,color,cutoff,double,atlas in draws:
            positions=v[:,:3]
            if atlas:
                pivot=(positions.min(0)+positions.max(0))/2
                positions=(positions-pivot)@frame.T+pivot
                column=(yaw/(2*math.pi)%1)*atlas['columns']
                row=round((pitch/math.pi+.5)*(atlas['rows']-1))
                program['atlas'].value=(atlas['columns'],math.floor(column),row,column%1)
            else:program['atlas'].value=(0,0,0,0)
            vertex.write(np.column_stack((positions,v[:,6:8])).astype('f4').tobytes())
            ctx.disable(moderngl.CULL_FACE) if double else ctx.enable(moderngl.CULL_FACE)
            texture.use(0);program['color'].value=tuple(color);program['cutoff'].value=cutoff
            vao.render()
        pixels=np.frombuffer(fbo.read(components=4,dtype='f4'),dtype='f4').reshape(size,size,4)[::-1]
        return pixels.reshape(resolution,2,resolution,2,4).mean((1,3))

    rows=[];results={name:[] for name in variants}
    for pitch in (0,45):
        for degrees in np.arange(0,360,azimuth_step):
            degrees=float(degrees)
            pictures={name:render(draws,math.radians(degrees),math.radians(pitch))
                      for name,(_,draws) in models.items()}
            reference_pixels=pictures['LOD0'];a=reference_pixels[:,:,3]>.5
            for name in variants:
                image=pictures[name];b=image[:,:,3]>.5;joint=a&b
                results[name].append({'azimuth':degrees,'elevation':pitch,
                    'silhouetteIoU':float((a&b).sum()/max((a|b).sum(),1)),
                    'linearAlbedoMAE':float(np.abs(reference_pixels[:,:,:3]-image[:,:,:3])[joint].mean())})
            if pitch==0:
                strip=[]
                for picture in pictures.values():
                    rgb=picture[:,:,:3]/np.maximum(picture[:,:,3,None],1e-8)
                    srgb=np.where(rgb<=.0031308,rgb*12.92,1.055*np.maximum(rgb,0)**(1/2.4)-.055)
                    rgba=np.concatenate((srgb,picture[:,:,3,None]),axis=2)
                    strip.append(np.uint8(np.clip(rgba,0,1)*255+.5))
                rows.append(np.concatenate(strip,axis=1))
    sheet=Image.fromarray(np.concatenate(rows,axis=0),'RGBA')
    backdrop=Image.new('RGBA',sheet.size,(196,205,214,255));backdrop.alpha_composite(sheet)
    backdrop.convert('RGB').save(output/'silhouettes.png')
    report={name:{'minimumIoU':min(v['silhouetteIoU'] for v in views),
                  'meanIoU':float(np.mean([v['silhouetteIoU'] for v in views])),
                  'meanLinearAlbedoMAE':float(np.mean([v['linearAlbedoMAE'] for v in views])),
                  'views':views} for name,views in results.items()}
    for resource in resources:resource.release()
    fbo.release();ctx.release()
    return report
