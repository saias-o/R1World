"""Fleet paths and mesh authoring helpers shared with the aircraft builder.

The default build imports the licensed vehicle sources (vehicle_imports.py).
The old procedural build function is retained as an authoring reference only.
"""
from __future__ import annotations

import json
import math
import struct
from collections import defaultdict
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
ROOT = GAME / "assets/models/vehicles"
# length, width (body), height, wheelbase, tyre radius, belt height,
# front/rear cabin foot and front/rear roof positions (fraction of length).
SPECS = {
    "city": (3.60, 1.66, 1.50, 2.38, .285, .88, .29, -.43, .12, -.29),
    "sedan": (4.55, 1.80, 1.44, 2.70, .315, .91, .20, -.32, .01, -.19),
    "suv": (4.58, 1.88, 1.70, 2.72, .355, 1.08, .23, -.43, .06, -.31),
    "offroad": (4.18, 1.85, 1.89, 2.46, .39, 1.15, .20, -.44, .12, -.37),
    "sport": (4.42, 1.91, 1.22, 2.61, .325, .76, .12, -.32, -.06, -.19),
    "truck": (7.10, 2.34, 3.18, 4.20, .46, 1.39, .46, .19, .42, .22),
    "bus": (10.50, 2.50, 3.05, 5.30, .47, 1.22, .49, -.49, .47, -.47),
}

MATERIALS = [
    # Metallic flake paint, polished alloy, dielectric glazing and matte
    # rubber respond separately to direct light and the HDR sky reflection.
    ("paint", (.18, .22, .26), .35, .18),
    ("rubber", (.018, .021, .024), 0., .84),
    ("glass", (.026, .044, .059), 0., .08),
    ("alloy", (.56, .59, .62), .95, .16),
    ("headlamp", (.56, .60, .59), .15, .12),
    ("taillamp", (.24, .009, .006), 0., .18),
    ("indicator", (.40, .16, .012), 0., .2),
    ("mirror", (.83, .85, .87), 1., .06),
]


def model(name: str, far: bool = False) -> str:
    return f"assets/models/vehicles/{name}{'_far' if far else ''}.glb"


class Mesh:
    def __init__(self):
        self.parts = defaultdict(list)

    def face(self, material, points):
        # Flat faces keep seams crisp. Curved surfaces supply analytic normals.
        a, b, c = points[:3]
        u, v = [b[i]-a[i] for i in range(3)], [c[i]-a[i] for i in range(3)]
        n = (u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0])
        length = math.sqrt(sum(x*x for x in n))
        if length < 1e-9:
            return
        n = tuple(x/length for x in n)
        self.parts[material].append((points, [n]*len(points)))

    def surface(self, material, fn, nu=6, nv=3, reverse=False, offset=0.):
        def vertex(u,v):
            p=fn(u,v);du=[b-a for a,b in zip(fn(u-.0001,v),fn(u+.0001,v))]
            dv=[b-a for a,b in zip(fn(u,v-.0001),fn(u,v+.0001))]
            n=(du[1]*dv[2]-du[2]*dv[1],du[2]*dv[0]-du[0]*dv[2],du[0]*dv[1]-du[1]*dv[0])
            mag=math.sqrt(sum(x*x for x in n)) or 1
            n=tuple(x/mag*(-1 if reverse else 1) for x in n)
            return tuple(p[i]+offset*n[i] for i in range(3)),n
        for j in range(nv):
            for i in range(nu):
                corners=[vertex(i/nu,j/nv),vertex((i+1)/nu,j/nv),
                         vertex((i+1)/nu,(j+1)/nv),vertex(i/nu,(j+1)/nv)]
                if reverse:corners.reverse()
                self.parts[material].append(([p for p,n in corners],[n for p,n in corners]))

    def box(self, material, center, size, bevel=0.):
        x, y, z = center
        w, h, d = (s*.5 for s in size)
        # Bevelled footprint, flat top/bottom. Used for bumpers and mirrors.
        b = min(bevel, w*.4, d*.4)
        ring = [(-w+b,-d),(w-b,-d),(w,-d+b),(w,d-b),
                (w-b,d),(-w+b,d),(-w,d-b),(-w,-d+b)] if b else [(-w,-d),(w,-d),(w,d),(-w,d)]
        bottom = [(x+a,y-h,z+c) for a,c in ring]
        top = [(x+a,y+h,z+c) for a,c in ring]
        self.face(material,bottom)
        self.face(material,list(reversed(top)))
        for i in range(len(ring)):
            j=(i+1)%len(ring)
            self.face(material,[bottom[i],top[i],top[j],bottom[j]])

    def cylinder(self, material, radius, width, segments, inset=0.):
        # Wheel axis X. Rings round off the tyre shoulder; smooth circumference.
        rings=[(-width*.5,radius*.87),(-width*.38,radius),
               (width*.38,radius),(width*.5,radius*.87)]
        if inset:
            rings=[(-width*.5,radius),(width*.5,radius)]
        for (x0,r0),(x1,r1) in zip(rings,rings[1:]):
            for i in range(segments):
                a,b=2*math.pi*i/segments,2*math.pi*(i+1)/segments
                points=[(x0,r0*math.cos(a),r0*math.sin(a)),(x0,r0*math.cos(b),r0*math.sin(b)),
                        (x1,r1*math.cos(b),r1*math.sin(b)),(x1,r1*math.cos(a),r1*math.sin(a))]
                slope=(r0-r1)/(x1-x0)
                norm=math.sqrt(1+slope*slope)
                normals=[(slope/norm,math.cos(t)/norm,math.sin(t)/norm) for t in (a,b,b,a)]
                self.parts[material].append((points,normals))
        for side in (-1,1):
            points=[(side*width*.5,rings[0][1]*math.cos(2*math.pi*i/segments),
                     rings[0][1]*math.sin(2*math.pi*i/segments)) for i in range(segments)]
            if side<0:points.reverse()
            self.face(material,points)


class GLB:
    def __init__(self, materials=MATERIALS, generator="R1World original vehicle fleet"):
        self.binary=bytearray()
        self.materials=materials
        self.doc={"asset":{"version":"2.0","generator":generator},
                  "scene":0,"scenes":[{"nodes":[]}],"nodes":[],"meshes":[],"accessors":[],"bufferViews":[],
                  "materials":[{"name":n,"pbrMetallicRoughness":{"baseColorFactor":[*c,1],
                      "metallicFactor":m,"roughnessFactor":r}} for n,c,m,r in materials]}
        self.vertices=0
        self.triangles=0

    def accessor(self, values, kind, components, bounds=False):
        while len(self.binary)%4:self.binary.append(0)
        raw=struct.pack('<'+('f' if kind==5126 else 'I')*len(values),*values)
        view=len(self.doc['bufferViews'])
        self.doc['bufferViews'].append({'buffer':0,'byteOffset':len(self.binary),'byteLength':len(raw)})
        self.binary.extend(raw)
        result={'bufferView':view,'componentType':kind,'count':len(values)//components,
                'type':{1:'SCALAR',2:'VEC2',3:'VEC3'}[components]}
        if bounds:
            result['min']=[min(values[i::components]) for i in range(components)]
            result['max']=[max(values[i::components]) for i in range(components)]
        index=len(self.doc['accessors']);self.doc['accessors'].append(result)
        return index

    def node(self,name,mesh=None,position=None,parent=None):
        n={'name':name}
        if position:n['translation']=position
        index=len(self.doc['nodes']);self.doc['nodes'].append(n)
        if parent is None:self.doc['scenes'][0]['nodes'].append(index)
        else:self.doc['nodes'][parent].setdefault('children',[]).append(index)
        if mesh:
            for mat,faces in mesh.parts.items():
                positions=[];normals=[];indices=[];lookup={}
                for points,ns in faces:
                    face=[]
                    for p,normal in zip(points,ns):
                        key=tuple(round(v,7) for v in (*p,*normal))
                        if key not in lookup:
                            lookup[key]=len(positions)//3;positions.extend(p);normals.extend(normal)
                        face.append(lookup[key])
                    for j in range(1,len(face)-1):indices.extend((face[0],face[j],face[j+1]))
                self.vertices+=len(positions)//3;self.triangles+=len(indices)//3
                primitive={'attributes':{'POSITION':self.accessor(positions,5126,3,True),
                    'NORMAL':self.accessor(normals,5126,3)},'indices':self.accessor(indices,5125,1),
                    'material':next(i for i,m in enumerate(self.materials) if m[0]==mat)}
                mi=len(self.doc['meshes']);self.doc['meshes'].append({'primitives':[primitive]})
                child=len(self.doc['nodes']);self.doc['nodes'].append({'name':f'{mat}-{name}','mesh':mi})
                n.setdefault('children',[]).append(child)
        return index

    def save(self,path):
        self.doc['buffers']=[{'byteLength':len(self.binary)}]
        raw=json.dumps(self.doc,separators=(',',':')).encode()
        raw+=b' '*((-len(raw))%4)
        self.binary+=b'\0'*((-len(self.binary))%4)
        path.write_bytes(struct.pack('<III',0x46546c67,2,28+len(raw)+len(self.binary))+
            struct.pack('<II',len(raw),0x4e4f534a)+raw+
            struct.pack('<II',len(self.binary),0x004e4942)+self.binary)


def build(name,far=False):
    length,width,height,wb,r,belt,cf,cr,rf,rr=SPECS[name]
    half=width*.5;front=length*.5;back=-front
    body=Mesh();glb=GLB()
    commercial=name in ('bus','truck')
    # Longitudinal samples at every wheel-arch corner avoid intersecting tyres.
    stations={back,back+.06,back+.16,back+.32,front-.32,front-.16,front-.06,front,cr*length,cf*length}
    for axle in (-wb*.5,wb*.5):
        for i in range(7 if far else 13):
            stations.add(axle+(r+.055)*math.cos(math.pi*i/(6 if far else 12)))
    stations=sorted(z for z in stations if back<=z<=front)
    def shoulder(z):
        end=max(0,(abs(z)/front-.82)/.18)
        return belt-(.07 if commercial else .14)*end**1.4, half*(1-.10*end**1.8)
    def bottom(z):
        cut=.20 if not commercial else .35
        for axle in (-wb*.5,wb*.5):
            dist=abs(z-axle)
            if dist<=r+.055:cut=max(cut,r+math.sqrt(max(0,(r+.055)**2-dist**2)))
        return cut
    for z0,z1 in zip(stations,stations[1:]):
        h0,w0=shoulder(z0);h1,w1=shoulder(z1)
        for side in (-1,1):
            def flank(u,v):
                z=z0+(z1-z0)*u;h,w=shoulder(z);low=bottom(z)
                return (side*w*(.955+.045*math.sin(math.pi*v*.5)),low+(h-.055-low)*v,z)
            body.surface('paint',flank,1,1 if far else 3,side>0)
            def edge(u,v):
                z=z0+(z1-z0)*u;h,w=shoulder(z)
                return (side*(w-.065*(1-math.cos(v*math.pi*.5))),h-.055+.055*math.sin(v*math.pi*.5),z)
            body.surface('paint',edge,1,1 if far else 3,side>0)
        # Smoothly crowned bonnet and deck, with rounded shoulders.
        body.surface('paint',lambda u,v:(
            (2*u-1)*((w0-.065)*(1-v)+(w1-.065)*v),
            h0*(1-v)+h1*v+.035*(1-(2*u-1)**2),z0*(1-v)+z1*v),
            4 if far else 8,1,True)
    for z,sgn in ((front,1),(back,-1)):
        h,w=shoulder(z)
        p=[(-w,.26,z),(w,.26,z),(w,h,z),(-w,h,z)]
        if sgn<0:p.reverse()
        body.face('paint',p)
        body.face('paint',[(-w,h,z),(w,h,z),(0,h+.035,z)])
        body.box('rubber',(0,.33 if not commercial else .48,z-sgn*.045),(width*.93,.15,.14),.08)
        body.box('rubber',(0,belt*.62,z+sgn*.012),(width*.43,.16 if not commercial else .28,.025))
        if not far:
            # Neutral, unlettered number plate and grille horizontal blades.
            body.box('alloy',(0,belt*.45,z+sgn*.029),(.45,.105,.018))
            for j in range(3):body.box('alloy',(0,belt*.62-.05+j*.05,z+sgn*.029),(width*.40,.008,.012))
        for side in (-1,1):
            body.box('headlamp' if sgn>0 else 'taillamp',
                     (side*width*.35,belt*.86,z-sgn*.006),
                     (width*.23,.075 if name=='sport' else .115,.035),.025)
            if not far:body.box('indicator',(side*width*.455,belt*.86,z),(.055,.07,.085))
    # Cabin trapezoid with separate inset glazing and visible A/B/C pillars.
    cfront,crear,rfront,rrear=(v*length for v in (cf,cr,rf,rr))
    roofhalf=half*(.88 if commercial or name=='offroad' else .77)
    lowerhalf=half-.075
    top=(2.50 if name=='truck' else height)-.075
    def glazed(fn,reverse=False):
        body.surface('paint',fn,4 if far else 8,2 if far else 4,reverse)
        body.surface('glass',lambda u,v:fn(.045+.91*u,.08+.84*v),
                     3 if far else 6,2 if far else 3,reverse,.004)
    for foot,roof,reverse in ((cfront,rfront,False),(crear,rrear,True)):
        glazed(lambda u,v:((2*u-1)*(lowerhalf*(1-v)+roofhalf*v+.025*math.sin(math.pi*v)),
                    belt+(top-belt)*v+.045*(1-(2*u-1)**2)*v,
                    foot*(1-v)+roof*v),reverse)
    body.surface('paint',lambda u,v:((2*u-1)*roofhalf,
        top+.045*(1-(2*u-1)**2)+.025*math.sin(math.pi*v),rrear+(rfront-rrear)*v),
        4 if far else 8,2 if far else 6,True)
    # Partition glazing in the curved cabin sides, retaining thin pillars.
    bays=7 if name=='bus' else (1 if name in ('truck','sport') else 2)
    for side in (-1,1):
        for j in range(bays):
            def side_surface(u,v):
                t=(j+u)/bays
                return (side*(lowerhalf*(1-v)+roofhalf*v+.025*math.sin(math.pi*v)),
                       belt+(top-belt+.025*math.sin(math.pi*t))*v,
                       (crear+(cfront-crear)*t)*(1-v)+(rrear+(rfront-rrear)*t)*v)
            glazed(side_surface,side>0)
        if not far:
            # Thin door shut lines and flush handles under the belt.
            for z in ([crear*.9,0,cfront*.9] if not commercial else [crear,cfront]):
                if bottom(z)<belt-.22:
                    body.box('rubber',(side*(half+.002),(belt+bottom(z))*.5,z),(.008,belt-bottom(z)-.06,.009))
            for z in ([0.08*length,-.19*length] if name not in ('city','sport','truck') else [.04*length]):
                body.box('alloy',(side*(half+.009),belt-.13,z),(.018,.025,.13),.008)
            body.box('rubber',(side*(half+.06),belt+.16,cfront-.09),(.18,.06,.08))
            body.box('paint',(side*(half+.12),belt+.19,cfront-.11),(.17,.11,.23),.035)
            body.box('glass',(side*(half+.125),belt+.19,cfront-.23),(.13,.073,.009))
        if name in ('offroad','suv'):
            body.box('rubber',(side*half,.30,0),(.07,.11,wb*.61),.025)
            if not far:body.box('alloy',(side*roofhalf*.85,top+.05,(rfront+rrear)*.5),(.035,.045,rfront-rrear))
    if name=='truck':
        # Separate cab and unbranded box body: a rigid urban delivery truck.
        cargo_front=crear-.16;cargo_back=back+.04
        body.box('paint',(0,(height+1.05)*.5,(cargo_front+cargo_back)*.5),
                 (width,height-1.05,cargo_front-cargo_back),.055)
        body.box('rubber',(0,1.06,(cargo_front+cargo_back)*.5),(width+.015,.10,cargo_front-cargo_back))
        if not far:
            for side in (-1,1):body.box('alloy',(side*(half+.005),1.30,(cargo_front+cargo_back)*.5),(.018,.065,cargo_front-cargo_back))
            for x in (-width*.22,width*.22):body.box('alloy',(x,2.02,back+.009),(.025,1.62,.04))
    if name=='bus' and not far:
        # Entry doors and ventilation slats distinguish a city bus from a van.
        for z in (front-.8,-.9):
            body.box('glass',(half+.008,1.45,z),(.022,2.08,.84))
            body.box('alloy',(half+.023,1.45,z),(.01,2.08,.025))
        for j in range(5):body.box('rubber',(-half-.004,.77+j*.075,back+.7),(.015,.022,.9))
    glb.node('body',body)
    # Identical wheel geometry reused by node references within each file.
    wheel=Mesh();segments=8 if far else 20
    tyre_width=.20 if not commercial else .28
    wheel.cylinder('rubber',r,tyre_width,segments)
    wheel.cylinder('alloy',r*.61,tyre_width+.006,segments,1)
    if not far:
        for side in (-1,1):
            for i in range(5):
                a=2*math.pi*i/5;b=a+.33
                wheel.face('rubber',[(side*(tyre_width*.5+.005),r*k*math.cos(t),r*k*math.sin(t))
                    for k,t in ((.25,a),(.54,a),(.54,b),(.25,b))][::side])
    wheel_nodes=[]
    for axle,z in (('front',wb*.5),('back',-wb*.5)):
        for side,label in ((-1,'left'),(1,'right')):
            pos=[side*(half-.055),r,z]
            if not wheel_nodes:
                ni=glb.node(f'wheel-{axle}-{label}',wheel,pos)
                wheel_nodes=list(glb.doc['nodes'][ni]['children'])
            else:
                ni=glb.node(f'wheel-{axle}-{label}',position=pos)
                for source in wheel_nodes:
                    child=dict(glb.doc['nodes'][source]);ci=len(glb.doc['nodes']);glb.doc['nodes'].append(child)
                    glb.doc['nodes'][ni].setdefault('children',[]).append(ci)
    path=GAME/model(name,far);glb.save(path)
    return {'path':model(name,far),'vertices':glb.vertices,'triangles':glb.triangles,
            'drawTriangles':glb.triangles+3*sum(len(f[0])-2 for fs in wheel.parts.values() for f in fs)}


def main():
    from .vehicle_imports import main as import_fleet
    import_fleet()


if __name__=='__main__':main()
