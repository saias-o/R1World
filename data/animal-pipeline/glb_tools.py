"""Small GLB reader used only by the asset preparation smoke checks."""
import json
import struct
import numpy as np

COMPONENT = {5120: 'i1', 5121: 'u1', 5122: '<i2', 5123: '<u2', 5125: '<u4', 5126: '<f4'}
WIDTH = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}


def read(path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from('<III', data)
    assert magic == 0x46546c67 and version == 2 and length == len(data), path
    offset, doc, binary = 12, None, b''
    while offset < len(data):
        size, kind = struct.unpack_from('<II',data,offset)
        chunk = data[offset+8:offset+8+size]
        if kind == 0x4e4f534a:
            doc = json.loads(chunk)
        elif kind == 0x004e4942:
            binary = chunk
        offset += 8 + size
    assert doc
    return doc, binary


def write(path, doc, binary):
    doc['buffers'][0]['byteLength'] = len(binary)
    j = json.dumps(doc, separators=(',',':'),ensure_ascii=True).encode()
    j += b' ' * ((-len(j))%4)
    binary += b'\0' * ((-len(binary))%4)
    result = struct.pack('<III',0x46546c67,2,28+len(j)+len(binary)) + struct.pack('<II',len(j),0x4e4f534a)+j+struct.pack('<II',len(binary),0x004e4942)+binary
    path.write_bytes(result)


def accessor(doc, binary, index):
    a = doc['accessors'][index]
    v = doc['bufferViews'][a['bufferView']]
    dtype = np.dtype(COMPONENT[a['componentType']])
    width = WIDTH[a['type']]
    offset = v.get('byteOffset',0) + a.get('byteOffset',0)
    stride = v.get('byteStride',width*dtype.itemsize)
    arr = np.ndarray((a['count'],width),dtype=dtype,buffer=binary,offset=offset,strides=(stride,dtype.itemsize)).copy()
    if a.get('normalized'):
        info = np.iinfo(dtype)
        arr = np.clip(arr.astype(float)/info.max, -1, 1)
    assert np.isfinite(arr).all()
    return arr


def quaternion(q):
    x,y,z,w = q / max(np.linalg.norm(q),1e-15)
    return np.array([[1-2*y*y-2*z*z,2*x*y-2*z*w,2*x*z+2*y*w],
                     [2*x*y+2*z*w,1-2*x*x-2*z*z,2*y*z-2*x*w],
                     [2*x*z-2*y*w,2*y*z+2*x*w,1-2*x*x-2*y*y]])


def globals_for(doc, overrides):
    local = []
    for idx,node in enumerate(doc['nodes']):
        if 'matrix' in node and idx not in overrides:
            mat = np.array(node['matrix']).reshape(4,4).T
        else:
            channels = overrides.get(idx,{})
            mat = np.eye(4)
            mat[:3,:3] = quaternion(np.array(channels.get('rotation',node.get('rotation',[0,0,0,1])))) @ np.diag(channels.get('scale',node.get('scale',[1,1,1])))
            mat[:3,3] = channels.get('translation',node.get('translation',[0,0,0]))
        local.append(mat)
    parents = {c:i for i,n in enumerate(doc['nodes']) for c in n.get('children',[])}
    cache = {}
    def get(i):
        if i not in cache:
            cache[i] = get(parents[i]) @ local[i] if i in parents else local[i]
        return cache[i]
    return np.stack([get(i) for i in range(len(local))])


def animation_overrides(doc,binary,animation,time):
    out = {}
    for channel in animation['channels']:
        s = animation['samplers'][channel['sampler']]
        times = accessor(doc,binary,s['input']).ravel()
        values = accessor(doc,binary,s['output'])
        j = max(0,min(len(times)-2,np.searchsorted(times,time,side='right')-1))
        k = min(j+1,len(times)-1)
        alpha = np.clip((time-times[j]) / max(times[k]-times[j],1e-12),0,1)
        if s.get('interpolation') == 'STEP':
            alpha = 0
        a,b = values[j].astype(float),values[k].astype(float)
        target = channel['target']
        if target['path'] == 'rotation' and np.dot(a,b)<0:
            b = -b
        value = a*(1-alpha)+b*alpha
        if target['path'] == 'rotation':
            value /= max(np.linalg.norm(value),1e-15)
        out.setdefault(target['node'],{})[target['path']] = value
    return out


def vertices_for(doc,binary,transforms):
    clouds = []
    for node in doc['nodes']:
        if 'mesh' not in node or 'skin' not in node:
            continue
        skin = doc['skins'][node['skin']]
        inverse = accessor(doc,binary,skin['inverseBindMatrices']).reshape(-1,4,4).transpose(0,2,1)
        matrices = transforms[skin['joints']] @ inverse
        for primitive in doc['meshes'][node['mesh']]['primitives']:
            attrs = primitive['attributes']
            positions = accessor(doc,binary,attrs['POSITION'])
            weights = accessor(doc,binary,attrs['WEIGHTS_0'])
            joints = accessor(doc,binary,attrs['JOINTS_0']).astype(int)
            assert np.allclose(weights.sum(axis=1),1,atol=0.002)
            assert joints.min()>=0 and joints.max()<len(matrices)
            v = np.column_stack((positions,np.ones(len(positions))))
            transformed = np.einsum('nwij,nj->nwi',matrices[joints],v)
            clouds.append((transformed*weights[:,:,None]).sum(axis=1)[:,:3])
    assert clouds, 'no skinned vertices'
    return np.concatenate(clouds)
