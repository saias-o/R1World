"""Bake normal-map tangent frames into GLB without changing existing data."""
import json
from pathlib import Path
import numpy as np
from glb_tools import read,write,accessor

ROOT=Path(__file__).resolve().parents[2]
results=[]
for path in sorted((ROOT/'game/assets/animals').glob('*.glb')):
    doc,binary=read(path)
    mutable=bytearray(binary)
    generated=0
    for mesh in doc['meshes']:
        for p in mesh['primitives']:
            attrs=p['attributes']
            mat=doc.get('materials',[])[p['material']] if 'material' in p else {}
            if 'normalTexture' not in mat or 'TANGENT' in attrs:
                continue
            uv_name='TEXCOORD_'+str(mat['normalTexture'].get('texCoord',0))
            assert uv_name in attrs
            pos=accessor(doc,binary,attrs['POSITION']).astype(float)
            normal=accessor(doc,binary,attrs['NORMAL']).astype(float)
            uv=accessor(doc,binary,attrs[uv_name]).astype(float)
            ix=accessor(doc,binary,p['indices']).astype(int).reshape(-1,3)
            e1=pos[ix[:,1]]-pos[ix[:,0]]
            e2=pos[ix[:,2]]-pos[ix[:,0]]
            d1=uv[ix[:,1]]-uv[ix[:,0]]
            d2=uv[ix[:,2]]-uv[ix[:,0]]
            determinant=d1[:,0]*d2[:,1]-d1[:,1]*d2[:,0]
            valid=np.abs(determinant)>1e-12
            divisor=np.zeros_like(determinant)
            divisor[valid]=1/determinant[valid]
            st=(e1*d2[:,1,None]-e2*d1[:,1,None])*divisor[:,None]
            bt=(e2*d1[:,0,None]-e1*d2[:,0,None])*divisor[:,None]
            tangent=np.zeros_like(pos)
            bitangent=np.zeros_like(pos)
            for column in range(3):
                np.add.at(tangent,ix[:,column],st)
                np.add.at(bitangent,ix[:,column],bt)
            normal/=np.maximum(np.linalg.norm(normal,axis=1,keepdims=True),1e-15)
            tangent-=normal*np.sum(tangent*normal,axis=1,keepdims=True)
            length=np.linalg.norm(tangent,axis=1)
            fallback=length<1e-10
            axis=np.zeros_like(pos)
            axis[:,0]=1
            axis[np.abs(normal[:,0])>0.9]=[0,1,0]
            tangent[fallback]=np.cross(normal[fallback],axis[fallback])
            tangent/=np.maximum(np.linalg.norm(tangent,axis=1,keepdims=True),1e-15)
            handedness=np.where(np.sum(np.cross(normal,tangent)*bitangent,axis=1)<0,-1.,1.)
            values=np.column_stack((tangent,handedness)).astype('<f4')
            assert np.isfinite(values).all() and np.allclose(np.linalg.norm(values[:,:3],axis=1),1,atol=1e-5)
            mutable.extend(b'\0'*((-len(mutable))%4))
            view_index=len(doc['bufferViews'])
            doc['bufferViews'].append({'buffer':0,'byteOffset':len(mutable),'byteLength':values.nbytes,'target':34962})
            mutable.extend(values.tobytes())
            attrs['TANGENT']=len(doc['accessors'])
            doc['accessors'].append({'bufferView':view_index,'componentType':5126,'count':len(values),'type':'VEC4'})
            generated+=1
    if generated:
        write(path,doc,bytes(mutable))
        results.append({'file':path.name,'primitives':generated})
(ROOT/'data/animal-pipeline/review/tangents.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
print('Baked tangents in',len(results),'GLBs')
