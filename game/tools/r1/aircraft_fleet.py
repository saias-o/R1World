"""Original, unbranded aircraft, authored in metres. No downloaded assets.

Run `python -m r1.aircraft_fleet` from game/tools. The same writer as the road
fleet (`vehicle_fleet.GLB`), the same two levels of detail: the far mesh drops
trim, windows and gear detail and halves the circumference samples.

Conventions the game and the generator rely on (`native/world.cpp`,
`native/gen/airports.cpp`):

- the nose points to +Z, as the road fleet's bonnet does, and the game turns the
  model half a turn so it flies toward the engine's -Z;
- Y = 0 is the ground under the wheels or skids, X = 0 the centre line, Z = 0
  the centre of gravity, which is what the game pitches and rolls around;
- the helicopter's main rotor is the node `rotor-main` (spins about Y) and its
  tail rotor `rotor-tail` (about X); everything a plane retracts is `gear`.

Every colour is an albedo, never above 0.35 (CLAUDE.md rule 2): white livery is
0.34, which is what white reads as under this world's light.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

from .vehicle_fleet import GLB, Mesh

GAME = Path(__file__).resolve().parents[2]
ROOT = GAME / "assets/models/aircraft"

MATERIALS = [
    ("paint", (.34, .34, .335), .15, .32),       # white livery
    ("trim", (.035, .07, .16), .15, .35),        # the cheatline and fin, a dark blue
    ("belly", (.20, .205, .21), .3, .4),         # wing and belly grey
    ("olive", (.085, .10, .06), .05, .62),       # military paint, matt
    ("glass", (.026, .044, .059), .25, .12),
    ("metal", (.28, .29, .30), .85, .30),
    ("fan", (.05, .05, .055), .6, .45),          # engine inlets and exhausts
    ("rubber", (.018, .021, .024), 0., .84),
    ("navred", (.30, .012, .010), .1, .3),
    ("navgreen", (.012, .22, .04), .1, .3),
]

# ── a little vector algebra ─────────────────────────────────────────────────


def add(a, b): return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
def sub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def mul(a, k): return (a[0]*k, a[1]*k, a[2]*k)
def dot(a, b): return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]
def cross(a, b): return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def lerp(a, b, t): return a+(b-a)*t


def normal_at(fn, u, v):
    du = sub(fn(min(1, u+1e-4), v), fn(max(0, u-1e-4), v))
    dv = sub(fn(u, min(1, v+1e-4)), fn(u, max(0, v-1e-4)))
    return cross(du, dv)


def skin(mesh, material, fn, nu, nv, outward, offset=0.):
    """`Mesh.surface`, turned so the normal at its middle faces `outward(p)`."""
    p = fn(.5, .5)
    reverse = dot(normal_at(fn, .5, .5), outward(p)) < 0
    mesh.surface(material, fn, nu, nv, reverse, offset)


def cap(mesh, material, points, outward):
    """A flat polygon, wound so its normal faces `outward`."""
    a, b, c = points[0], points[1], points[2]
    if dot(cross(sub(b, a), sub(c, a)), outward) < 0:
        points = list(reversed(points))
    mesh.face(material, points)


def obox(mesh, material, centre, size, yaw=0., pitch=0., roll=0.):
    """A box turned `roll` about Z, `pitch` about X then `yaw` about Y, faces wound outward."""
    hx, hy, hz = (s*.5 for s in size)
    cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    cr, sr = math.cos(roll), math.sin(roll)

    def at(x, y, z):
        x, y = x*cr-y*sr, x*sr+y*cr
        y, z = y*cp-z*sp, y*sp+z*cp
        x, z = x*cy+z*sy, -x*sy+z*cy
        return (centre[0]+x, centre[1]+y, centre[2]+z)
    for axis in range(3):
        for sign in (-1, 1):
            corners = []
            for a, b in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                q = [0, 0, 0]
                q[axis] = sign
                q[(axis+1) % 3] = a
                q[(axis+2) % 3] = b
                corners.append(at(q[0]*hx, q[1]*hy, q[2]*hz))
            n = [0, 0, 0]
            n[axis] = sign
            cap(mesh, material, corners, sub(at(n[0]*hx, n[1]*hy, n[2]*hz), centre))


def wheel(mesh, material, hub_material, centre, radius, width, segments):
    """A tyre with its axle along X, baked where it stands (no node of its own)."""
    part = Mesh()
    part.cylinder(material, radius, width, segments)
    part.cylinder(hub_material, radius*.55, width+.01, segments, 1)
    for mat, faces in part.parts.items():
        for points, normals in faces:
            mesh.parts[mat].append(([add(p, centre) for p in points], normals))


def strut(mesh, material, top, bottom, thickness):
    """A gear leg between two points, as a box leaning from one to the other."""
    centre = mul(add(top, bottom), .5)
    d = sub(top, bottom)
    length = math.sqrt(dot(d, d))
    roll = -math.asin(d[0]/length)
    pitch = math.atan2(d[2], d[1])
    obox(mesh, material, centre, (thickness, length, thickness), 0., pitch, roll)


# ── bodies ──────────────────────────────────────────────────────────────────


class Fuselage:
    """An elliptic tube: a blunt nose, a parallel cabin and an upswept tail cone.

    `section(z)` gives the half-width, half-height and centre height at z, so
    windows, liveries and wing roots can be laid on the same surface.
    """

    def __init__(self, length, radius, centre_y, nose=2.1, tail=3.3, upsweep=.9, height_ratio=1.):
        self.length, self.radius, self.cy = length, radius, centre_y
        self.front, self.back = length*.5, -length*.5
        self.nose_start = self.front-nose*radius
        self.tail_start = self.back+tail*radius
        self.upsweep, self.hr = upsweep, height_ratio

    def section(self, z):
        r, cy = self.radius, self.cy
        if z > self.nose_start:
            t = min(1., (z-self.nose_start)/(self.front-self.nose_start))
            s = max(0., 1-t**2.2)**.5
            return r*max(s, .015), r*self.hr*max(s, .015), cy-.18*r*t**1.5
        if z < self.tail_start:
            t = min(1., (self.tail_start-z)/(self.tail_start-self.back))
            a = r*(1-.84*t**1.25)
            b = r*self.hr*(1-.80*t)
            return a, b, cy+(r*self.hr-b)*self.upsweep
        return r, r*self.hr, cy

    def point(self, theta, z, grow=0.):
        a, b, cy = self.section(z)
        return ((a+grow)*math.cos(theta), cy+(b+grow)*math.sin(theta), z)

    def outward(self, p):
        return (p[0], p[1]-self.section(p[2])[2], 0.)

    def build(self, mesh, material, segments, far):
        spans = ((self.back, self.tail_start, 4 if far else 9),
                 (self.tail_start, self.nose_start, 2),
                 (self.nose_start, self.front, 5 if far else 10))
        for z0, z1, nv in spans:
            skin(mesh, material, lambda u, v, z0=z0, z1=z1: self.point(2*math.pi*u, lerp(z0, z1, v)),
                 segments, nv, self.outward)
        # The tail cone's open end: a small closing face (the APU exhaust).
        ring = [self.point(2*math.pi*i/segments, self.back) for i in range(segments)]
        cap(mesh, "fan", ring, (0, 0, -1))

    def band(self, mesh, material, theta0, theta1, z0, z1, nu, nv, grow=.012):
        """A strip laid on the skin, for windows, cheatlines and windscreens."""
        skin(mesh, material, lambda u, v: self.point(lerp(theta0, theta1, u), lerp(z0, z1, v), grow),
             nu, nv, self.outward)


def lifting(mesh, material, root, span, root_chord, tip_chord, sweep, dihedral,
            thickness, nu, nv, side=1, vertical=False, tip=True):
    """A wing, stabiliser or fin: an aerofoil slab from `root` (its leading edge).

    Horizontal surfaces run along X (`side` picks the wing), vertical ones up Y.
    `thickness` is a fraction of the chord. Sweep and dihedral are degrees.
    """
    tan_sweep, d = math.tan(math.radians(sweep)), math.radians(dihedral)

    def chord(v): return lerp(root_chord, tip_chord, v)

    def edge(v):
        if vertical:
            return add(root, (0., v*span, -v*span*tan_sweep))
        return add(root, (side*v*span*math.cos(d), v*span*math.sin(d), -v*span*tan_sweep))

    def profile(u): return 2.6*math.sqrt(max(u, 0.))*(1-u)

    def surface(sign):
        def fn(u, v):
            c = chord(v)
            p = add(edge(v), (0., 0., -u*c))
            t = sign*thickness*c*profile(u)*(1. if sign > 0 else .45)
            return add(p, (t, 0., 0.) if vertical else (0., t, 0.))
        return fn
    for sign in (1, -1):
        normal = (sign, 0., 0.) if vertical else (0., sign, 0.)
        skin(mesh, material, surface(sign), nu, nv, lambda p, n=normal: n)
    if tip:
        steps = [i/nu for i in range(nu+1)]
        points = [surface(1)(u, 1.) for u in steps]+[surface(-1)(u, 1.) for u in reversed(steps[1:-1])]
        outward = (0., 1., 0.) if vertical else (float(side), 0., 0.)
        cap(mesh, material, points, outward)
    return edge


def revolve(mesh, material, centre, z0, z1, radius, nu, nv):
    """A body of revolution about the Z axis through `centre` (x, y)."""
    cx, cy = centre

    def fn(u, v):
        a, z = 2*math.pi*u, lerp(z0, z1, v)
        r = radius(min(1., max(0., v)))  # the normals' finite differences step past the ends
        return (cx+r*math.cos(a), cy+r*math.sin(a), z)
    skin(mesh, material, fn, nu, nv, lambda p: (p[0]-cx, p[1]-cy, 0.))
    return fn


def nacelle(mesh, centre, z_front, length, radius, segments, far, paint="paint"):
    """A turbofan pod: lip, cowl, a dark fan face and a cone in the exhaust."""
    z_back = z_front-length
    def cowl(v):
        # Narrow exhaust, full body a quarter back from the lip, then the lip.
        return radius*(lerp(1., .92, (v-.75)/.25) if v > .75 else lerp(.66, 1., (v/.75)**.6))
    revolve(mesh, paint, centre, z_back, z_front, cowl, segments, 3 if far else 6)
    fan = [(centre[0]+radius*.9*math.cos(2*math.pi*i/segments), centre[1]+radius*.9*math.sin(2*math.pi*i/segments),
            z_front-.12) for i in range(segments)]
    cap(mesh, "fan", fan, (0, 0, 1))
    exhaust = [(centre[0]+radius*.64*math.cos(2*math.pi*i/segments), centre[1]+radius*.64*math.sin(2*math.pi*i/segments),
                z_back+.02) for i in range(segments)]
    cap(mesh, "fan", exhaust, (0, 0, -1))
    if not far:
        revolve(mesh, "metal", centre, z_back-radius*.6, z_back+.05,
                lambda v: radius*.45*v**.8+.02, segments//2, 2)
        revolve(mesh, "metal", centre, z_front-.3, z_front-.1, lambda v: radius*.25*(1-v)+.02, segments//2, 1)


# ── the fleet ───────────────────────────────────────────────────────────────


def airliner(far, *, length, radius, centre_y, span, root_chord, tip_chord, sweep, engine_span,
             engine_radius, engine_length, fin_height, stab_span, gear_track, main_wheels):
    """A twin-engined, low-winged jet airliner, in the proportions it is given."""
    body, gear = Mesh(), Mesh()
    seg = 12 if far else 24
    f = Fuselage(length, radius, centre_y)
    f.build(body, "paint", seg, far)
    if not far:
        cabin0, cabin1 = f.tail_start+radius*.4, f.nose_start-radius*.25
        for side in (-1, 1):
            a = math.radians(18) if side > 0 else math.pi-math.radians(18)
            f.band(body, "glass", a-.05*side, a+.05*side, cabin0, cabin1, 1, 6)
            f.band(body, "trim", a-.21*side, a-.15*side, cabin0-radius, f.nose_start+radius*.4, 1, 6)
        f.band(body, "glass", math.radians(28), math.radians(152),
               f.nose_start+radius*.62, f.nose_start+radius*1.05, 6, 2, .02)
        f.band(body, "belly", math.radians(235), math.radians(305), f.tail_start, f.nose_start, 3, 4)
    # Low wing, its root at the lower third of the fuselage.
    root_y = centre_y-radius*.55
    root_z = root_chord*.45
    semi = span*.5-radius*.7
    edges = {}
    for side in (-1, 1):
        edges[side] = lifting(body, "belly", (side*radius*.7, root_y, root_z), semi, root_chord, tip_chord,
                              sweep, 5.5, .11, 4 if far else 8, 3 if far else 6, side)
        if not far:
            tip = edges[side](1.)
            obox(body, "navgreen" if side < 0 else "navred", add(tip, (side*.05, 0., -.15)), (.12, .1, .25))
        # The engine hangs forward of the leading edge at `engine_span`.
        v = (engine_span-radius*.7)/semi
        le = edges[side](v)
        centre = (le[0], le[1]-engine_radius*1.15)
        nacelle(body, centre, le[2]+engine_length*.62, engine_length, engine_radius, 10 if far else 20, far)
        obox(body, "paint", (le[0], le[1]-engine_radius*.45, le[2]-engine_length*.05),
             (engine_radius*.22, engine_radius*.9, engine_length*.75))
    # Tail: a fin and two stabilisers on the tail cone.
    fin_root_chord = fin_height*.95
    fin_z = f.back+fin_root_chord*1.2
    lifting(body, "trim", (0., centre_y+radius*.6, fin_z), fin_height, fin_root_chord, fin_root_chord*.38,
            38, 0, .10, 4 if far else 6, 3 if far else 5, vertical=True)
    for side in (-1, 1):
        lifting(body, "belly", (side*radius*.28, centre_y+radius*.15, fin_z-fin_root_chord*.05), stab_span*.5,
                fin_root_chord*.62, fin_root_chord*.26, 32, 6, .09, 3 if far else 6, 2 if far else 4, side)
    # Gear: two main legs at the wing root, a nose leg under the cockpit.
    tyre = radius*.17
    legs = [((side*gear_track*.5, 0., -root_chord*.08), main_wheels) for side in (-1, 1)]
    legs.append(((0., 0., f.nose_start+radius*.1), 2))
    for (x, _, z), wheels in legs:
        top = (x, root_y if abs(x) > 0 else centre_y-radius*.8, z)
        strut(gear, "metal", top, (x, tyre, z), tyre*.35)
        rows = [0.] if wheels <= 2 else [-tyre*1.15, tyre*1.15]
        for dz in rows:
            for dx in (-1, 1):
                wheel(gear, "rubber", "metal", (x+dx*tyre*.62, tyre, z+dz), tyre, tyre*.7, 8 if far else 16)
    glb = GLB(MATERIALS, "R1World original aircraft")
    glb.node("body", body)
    glb.node("gear", gear)
    return glb


def bizjet(far):
    """A small business jet: T-tail, two engines on the rear fuselage, winglets."""
    body, gear = Mesh(), Mesh()
    seg = 12 if far else 22
    length, radius, cy = 20.3, .98, 1.72
    f = Fuselage(length, radius, cy, nose=2.9, tail=4.2, upsweep=.75)
    f.build(body, "paint", seg, far)
    if not far:
        for side in (-1, 1):
            a = math.radians(14) if side > 0 else math.pi-math.radians(14)
            for i in range(7):
                z = f.nose_start-2.4-i*.95
                f.band(body, "glass", a-.09*side, a+.09*side, z-.18, z+.18, 1, 1)
            f.band(body, "trim", a-.34*side, a-.27*side, f.tail_start-2., f.nose_start+1.1, 1, 6)
        f.band(body, "glass", math.radians(30), math.radians(150),
               f.nose_start+radius*.85, f.nose_start+radius*1.45, 6, 2, .02)
    root_y, root_z = cy-radius*.65, 1.2
    semi = 19.4*.5-radius*.8
    for side in (-1, 1):
        edge = lifting(body, "paint", (side*radius*.8, root_y, root_z), semi, 3.4, 1.15, 27, 4, .11,
                       4 if far else 8, 3 if far else 5, side, tip=False)
        tip = edge(1.)
        lifting(body, "trim", tip, 1.3, 1.15, .45, 40, 0, .09, 3, 2, vertical=True)
        if not far:
            obox(body, "navgreen" if side < 0 else "navred", add(tip, (side*.05, .05, -.2)), (.08, .08, .18))
        # Rear engines on short pylons.
        centre = (side*(radius+.62), cy+.32)
        nacelle(body, centre, -3.9, 3.1, .5, 10 if far else 18, far)
        obox(body, "paint", (side*(radius+.18), cy+.3, -5.2), (.75, .16, 1.5))
    fin_z = f.back+4.9
    top = lifting(body, "trim", (0., cy+radius*.55, fin_z), 3.0, 3.1, 1.6, 45, 0, .1, 4 if far else 6, 3, vertical=True)
    head = top(1.)
    for side in (-1, 1):
        lifting(body, "paint", (head[0], head[1]-.05, head[2]+.1), 3.4, 1.6, .75, 20, -2, .09, 3 if far else 5, 2, side)
    tyre = .32
    for x, z in ((-1.35, -.6), (1.35, -.6), (0., f.nose_start+.5)):
        top_point = (x, root_y if x else cy-radius*.8, z)
        strut(gear, "metal", top_point, (x, tyre, z), .12)
        pair = (-1, 1) if x == 0 else (0,)
        for dx in pair:
            wheel(gear, "rubber", "metal", (x+dx*.17, tyre, z), tyre if x else tyre*.75, .2, 8 if far else 14)
    glb = GLB(MATERIALS, "R1World original aircraft")
    glb.node("body", body)
    glb.node("gear", gear)
    return glb


def helicopter(far):
    """A military utility helicopter: cabin pod, tail boom, skids, four blades."""
    body = Mesh()
    seg = 10 if far else 20
    # Cabin and boom are one elliptic tube with a very long tail cone.
    f = Fuselage(12.6, 1.18, 1.75, nose=2.3, tail=6.4, upsweep=.55, height_ratio=1.02)
    f.front, f.back = 4.4, -8.2
    f.nose_start, f.tail_start = 4.4-2.3*1.18, -1.1
    f.build(body, "olive", seg, far)
    f.band(body, "glass", math.radians(-5), math.radians(185), f.nose_start+.35, f.nose_start+1.95, 8 if not far else 4, 3, .02)
    if not far:
        for side in (-1, 1):
            a = 0. if side > 0 else math.pi
            f.band(body, "glass", a-.25, a+.25, -.2, 1.0, 1, 1)
    # Engine housing and mast.
    obox(body, "olive", (0., 3.05, .2), (1.35, .55, 3.6))
    obox(body, "metal", (0., 3.55, .3), (.28, .6, .28))
    # Fin and stabiliser at the end of the boom.
    tail = f.point(math.pi/2, f.back+.5)
    lifting(body, "olive", (0., tail[1]-.2, f.back+1.3), 1.9, 1.3, .8, 30, 0, .12, 3, 2, vertical=True)
    for side in (-1, 1):
        lifting(body, "olive", (side*.15, tail[1]-.35, f.back+2.2), 1.5, .8, .55, 8, 0, .1, 3, 2, side)
    if not far:
        obox(body, "navred", (1.2, 1.2, 3.0), (.08, .08, .12))
        obox(body, "navgreen", (-1.2, 1.2, 3.0), (.08, .08, .12))
    # Skids.
    for side in (-1, 1):
        obox(body, "metal", (side*1.25, .07, .6), (.1, .1, 5.6))
        obox(body, "metal", (side*1.25, .16, 3.45), (.1, .1, .5), 0., -.55)
        for z in (-.5, 2.0):
            strut(body, "metal", (side*.85, .95, z), (side*1.25, .1, z), .09)
    glb = GLB(MATERIALS, "R1World original aircraft")
    glb.node("body", body)
    main = Mesh()
    obox(main, "metal", (0., 0., 0.), (.55, .22, .55))
    for k in range(4):
        yaw = math.pi/4+k*math.pi/2
        obox(main, "rubber", (4.2*math.sin(yaw), .08, 4.2*math.cos(yaw)), (.5, .06, 7.8), yaw)
    glb.node("rotor-main", main, [0., 3.95, .3])
    rear = Mesh()
    obox(rear, "metal", (0., 0., 0.), (.16, .2, .2))
    obox(rear, "rubber", (.08, 0., 0.), (.04, 2.7, .2))
    obox(rear, "rubber", (.08, 0., 0.), (.04, .2, 2.7))
    glb.node("rotor-tail", rear, [.32, tail[1]+.55, f.back+.9])
    return glb


# Handling is arcade and says so (README, "The aircraft"): speeds in m/s,
# rates in degrees a second, accelerations in m/s².
FLEET = {
    "widebody": dict(klass="airliner", length=64., span=60.3, height=17.3, cg=5.2,
                     build=lambda far: airliner(far, length=64., radius=2.82, centre_y=5.1, span=60.3, root_chord=10.5,
                                                tip_chord=2.4, sweep=31, engine_span=10.2, engine_radius=1.55,
                                                engine_length=6.4, fin_height=9.4, stab_span=19.4, gear_track=10.7,
                                                main_wheels=4),
                     handling=dict(top=88., rotate=58., stall=46., accel=3.0, spool=.22, brake=3.2, turnRadius=42.,
                                   rollRate=24., maxBank=32., pitchRate=5.5, maxPitch=18., climb=0.)),
    "airliner": dict(klass="airliner", length=37.6, span=34.1, height=11.8, cg=3.6,
                     build=lambda far: airliner(far, length=37.6, radius=1.98, centre_y=3.55, span=34.1, root_chord=6.1,
                                                tip_chord=1.5, sweep=27, engine_span=5.75, engine_radius=1.03,
                                                engine_length=4.3, fin_height=6.1, stab_span=12.4, gear_track=7.6,
                                                main_wheels=2),
                     handling=dict(top=90., rotate=55., stall=43., accel=3.6, spool=.3, brake=3.8, turnRadius=26.,
                                   rollRate=32., maxBank=36., pitchRate=7., maxPitch=20., climb=0.)),
    "bizjet": dict(klass="jet", length=20.3, span=19.4, height=6.0, cg=1.7,
                   build=bizjet,
                   handling=dict(top=97., rotate=42., stall=34., accel=6.2, spool=.8, brake=5.5, turnRadius=11.,
                                 rollRate=95., maxBank=70., pitchRate=20., maxPitch=35., climb=0.)),
    "helicopter": dict(klass="helicopter", length=15.3, span=16.0, height=4.4, cg=1.8,
                       build=helicopter,
                       handling=dict(top=45., rotate=0., stall=0., accel=7., spool=.35, brake=8., turnRadius=0.,
                                     rollRate=75., maxBank=24., pitchRate=0., maxPitch=16., climb=10.)),
}


def model(name: str, far: bool = False) -> str:
    return f"assets/models/aircraft/{name}{'_far' if far else ''}.glb"


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    fleet = []
    for name, spec in FLEET.items():
        levels = {}
        for level, far in (("near", False), ("far", True)):
            glb = spec["build"](far)
            glb.save(GAME/model(name, far))
            levels[level] = {"path": model(name, far), "vertices": glb.vertices, "triangles": glb.triangles}
        fleet.append({"name": name, "class": spec["klass"], "length": spec["length"], "span": spec["span"],
                      "height": spec["height"], "cg": spec["cg"], **levels, "handling": spec["handling"]})
    manifest = {"author": "R1World", "license": "CC0-1.0", "source": "game/tools/r1/aircraft_fleet.py",
                "description": "Original generic aircraft; no airline, livery, logo or registration.",
                "totalVertices": sum(a[level]["vertices"] for a in fleet for level in ("near", "far")),
                "aircraft": fleet}
    (ROOT/"fleet.json").write_text(json.dumps(manifest, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({a["name"]: [a["near"]["vertices"], a["far"]["vertices"]] for a in fleet}))


if __name__ == "__main__":
    main()
