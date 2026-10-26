"""The animals as the game draws them: measured from their GLBs.

`assets/animals/` holds the prepared models (`data/animal-pipeline`): one GLB
and one `-lod1` GLB per variant, skinned, with their clips inside. Their
orientation and the pose their clips stand in are inherited from each source,
and nothing in a GLB says which way the head is, where the hooves touch the
ground, or how fast a walk cycle carries the body. The game needs all three,
or an animal floats, walks backwards or skates.

    python -m r1.animals            (from game/tools; pure Python)

This samples every clip of every model `assets/animals/fauna.json` uses and
writes `assets/animals/animals.json`, which the game reads at start-up:

* **ground**: the lowest skinned point of the standing clip, so the node is
  set down by that much and the hooves rest on the ground;
* **forward**: the yaw of the head in the model's own frame, from where the
  feet push in the walk (the stance foot slides backwards under a body that
  walks in place) or, for a model without a gait, from its head bone;
* **speed** of each gait: the speed the stance feet slide backwards relative
  to the body, which is the ground speed at which they stay planted;
* **rig**: that the LOD1 file has the same joints and inverse binds as the
  LOD0, since one animator drives both levels.

Measured, never inferred (CLAUDE.md §4): a clip whose feet never touch the
ground gets no speed, and a model without any gait says so.
"""
from __future__ import annotations

import json
import math
import struct
import sys
from array import array
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
ANIMALS = GAME / "assets/animals"
FAUNA = ANIMALS / "fauna.json"
OUT = ANIMALS / "animals.json"

# Samples per second when a clip is measured; a 0.33 s run stride still gets ten.
RATE = 30
# A vertex is on the ground within this share of the standing height of the
# lowest point of its frame (2 % of a cow is 3 cm, of a mouse 0.6 mm).
CONTACT = 0.02
# Vertices tracked for contact: those in the lowest share of the bind pose.
LOW_SHARE = 0.2
MAX_TRACKED = 2500

_COMPONENT = {5120: ("b", 1, 127.0), 5121: ("B", 1, 255.0), 5122: ("h", 2, 32767.0),
              5123: ("H", 2, 65535.0), 5125: ("I", 4, None), 5126: ("f", 4, None)}
_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path: Path):
    data = path.read_bytes()
    magic, version, length = struct.unpack_from("<III", data)
    if magic != 0x46546C67 or version != 2 or length != len(data):
        raise ValueError(f"{path}: not a glTF 2.0 binary")
    offset, doc, binary = 12, None, b""
    while offset < len(data):
        size, kind = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8:offset + 8 + size]
        if kind == 0x4E4F534A:
            doc = json.loads(chunk)
        elif kind == 0x004E4942:
            binary = chunk
        offset += 8 + size
    if doc is None:
        raise ValueError(f"{path}: no JSON chunk")
    return doc, binary


def accessor(doc, binary, index):
    """The accessor's elements as tuples of floats (normalized when flagged)."""
    a = doc["accessors"][index]
    code, size, scale = _COMPONENT[a["componentType"]]
    width = _WIDTH[a["type"]]
    view = doc["bufferViews"][a["bufferView"]]
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = view.get("byteStride", width * size)
    fmt = "<" + code * width
    norm = a.get("normalized", False) and scale
    out = []
    for i in range(a["count"]):
        values = struct.unpack_from(fmt, binary, start + i * stride)
        out.append(tuple(max(-1.0, v / scale) for v in values) if norm else values)
    return out


# ── 4x4 matrices, column-major tuples like glTF ────────────────────────────
def mat_mul(a, b):
    return tuple(sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4))


def mat_point(m, p):
    x, y, z = p
    return (m[0] * x + m[4] * y + m[8] * z + m[12],
            m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14])


def trs(t, r, s):
    x, y, z, w = r
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    sx, sy, sz = s
    return (
        (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0.0,
        (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0.0,
        (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0.0,
        t[0], t[1], t[2], 1.0)


def _slerp(a, b, u):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0:
        b, dot = tuple(-v for v in b), -dot
    if dot > 0.9995:
        q = tuple(x + (y - x) * u for x, y in zip(a, b))
    else:
        theta = math.acos(min(1.0, dot))
        sa, sb = math.sin((1 - u) * theta), math.sin(u * theta)
        q = tuple((x * sa + y * sb) / math.sin(theta) for x, y in zip(a, b))
    n = math.sqrt(sum(v * v for v in q)) or 1.0
    return tuple(v / n for v in q)


class Model:
    """One GLB: its node tree, skin, skinned vertices and clips."""

    def __init__(self, path: Path):
        self.path = path
        self.doc, self.binary = read_glb(path)
        doc = self.doc
        self.parent = {}
        for i, node in enumerate(doc["nodes"]):
            for c in node.get("children", []):
                self.parent[c] = i
        self.rest = []
        for node in doc["nodes"]:
            self.rest.append((tuple(node.get("translation", (0.0, 0.0, 0.0))),
                              tuple(node.get("rotation", (0.0, 0.0, 0.0, 1.0))),
                              tuple(node.get("scale", (1.0, 1.0, 1.0)))))
        if not doc.get("skins"):
            raise ValueError(f"{path.name}: no skin")
        skin = doc["skins"][0]
        self.joints = skin["joints"]
        self.joint_set = set(self.joints)
        self.joint_names = [doc["nodes"][j].get("name", str(j)) for j in self.joints]
        self.inverse_binds = accessor(doc, self.binary, skin["inverseBindMatrices"])
        self.clips = {}
        for a, anim in enumerate(doc.get("animations", [])):
            channels = []
            duration = 0.0
            for c, ch in enumerate(anim["channels"]):
                sampler = anim["samplers"][ch["sampler"]]
                times = [t[0] for t in accessor(doc, self.binary, sampler["input"])]
                values = accessor(doc, self.binary, sampler["output"])
                if sampler.get("interpolation") == "CUBICSPLINE":
                    values = values[1::3]
                duration = max(duration, times[-1] if times else 0.0)
                channels.append((ch["target"].get("node"), ch["target"]["path"], times, values,
                                 sampler.get("interpolation", "LINEAR"), a, c))
            self.clips[anim["name"]] = (duration, channels)
        self._vertices()

    def _vertices(self):
        """Every skinned vertex: bind position, four joints, four weights."""
        doc = self.doc
        positions, joints, weights = [], [], []
        for node in doc["nodes"]:
            if "mesh" not in node or "skin" not in node:
                continue
            for prim in doc["meshes"][node["mesh"]]["primitives"]:
                attrs = prim["attributes"]
                if "JOINTS_0" not in attrs or "WEIGHTS_0" not in attrs:
                    continue
                positions += accessor(doc, self.binary, attrs["POSITION"])
                joints += [tuple(int(v) for v in j) for j in accessor(doc, self.binary, attrs["JOINTS_0"])]
                weights += accessor(doc, self.binary, attrs["WEIGHTS_0"])
        if not positions:
            raise ValueError(f"{self.path.name}: no skinned primitive")
        self.positions, self.vjoints, self.vweights = positions, joints, weights
        low = min(p[1] for p in positions)
        high = max(p[1] for p in positions)
        cut = low + (high - low) * LOW_SHARE
        tracked = [i for i, p in enumerate(positions) if p[1] <= cut]
        step = max(1, len(tracked) // MAX_TRACKED)
        self.tracked = tracked[::step]
        step = max(1, len(positions) // MAX_TRACKED)
        self.sampled = list(range(0, len(positions), step))

    def pose(self, clip: str | None, t: float):
        """World matrices of every node at time `t` of `clip` (rest without)."""
        local = list(self.rest)
        if clip is not None:
            _, channels = self.clips[clip]
            local = [list(v) for v in local]
            for node, path, times, values, interpolation, *_ in channels:
                if node is None or not times:
                    continue
                if t <= times[0]:
                    value = values[0]
                elif t >= times[-1]:
                    value = values[len(times) - 1]
                else:
                    lo, hi = 0, len(times) - 1
                    while hi - lo > 1:
                        mid = (lo + hi) // 2
                        if times[mid] <= t:
                            lo = mid
                        else:
                            hi = mid
                    if interpolation == "STEP":
                        value = values[lo]
                    else:
                        u = (t - times[lo]) / max(times[hi] - times[lo], 1e-9)
                        a, b = values[lo], values[hi]
                        value = _slerp(a, b, u) if path == "rotation" else tuple(x + (y - x) * u for x, y in zip(a, b))
                slot = {"translation": 0, "rotation": 1, "scale": 2}.get(path)
                if slot is not None:
                    local[node][slot] = tuple(value)
        world = [None] * len(local)

        def get(i):
            if world[i] is None:
                m = trs(*local[i])
                world[i] = mat_mul(get(self.parent[i]), m) if i in self.parent else m
            return world[i]

        for i in range(len(local)):
            get(i)
        return world

    def skinned(self, world, indices):
        palette = [mat_mul(world[j], ib) for j, ib in zip(self.joints, self.inverse_binds)]
        out = []
        for i in indices:
            p = self.positions[i]
            x = y = z = 0.0
            for j, w in zip(self.vjoints[i], self.vweights[i]):
                if w <= 0:
                    continue
                q = mat_point(palette[j], p)
                x += q[0] * w
                y += q[1] * w
                z += q[2] * w
            out.append((x, y, z))
        return out

    def frames(self, clip):
        duration = self.clips[clip][0]
        count = max(2, int(math.ceil(duration * RATE)) + 1)
        return [duration * k / (count - 1) for k in range(count)]


def _median(values):
    values = sorted(values)
    if not values:
        return None
    n = len(values)
    return values[n // 2] if n % 2 else 0.5 * (values[n // 2 - 1] + values[n // 2])


def measure_clip(model: Model, clip: str):
    """Ground, height and the stance feet's slide in one clip."""
    lows, highs, slides = [], [], []
    previous = None
    for t in model.frames(clip):
        world = model.pose(clip, t)
        whole = model.skinned(world, model.sampled)
        low_points = model.skinned(world, model.tracked)
        low = min(min(p[1] for p in whole), min(p[1] for p in low_points))
        high = max(p[1] for p in whole)
        lows.append(low)
        highs.append(high)
        contact = (high - low) * CONTACT
        on = {i: p for i, p in zip(model.tracked, low_points) if p[1] <= low + contact}
        if previous is not None:
            dt, before = t - previous[0], previous[1]
            if dt > 0:
                for i, p in on.items():
                    if i in before:
                        q = before[i]
                        slides.append(((p[0] - q[0]) / dt, (p[2] - q[2]) / dt))
        previous = (t, on)
    ground = _median(lows)
    height = _median(highs) - ground
    result = {"seconds": round(model.clips[clip][0], 4), "ground": round(ground, 4), "height": round(height, 4)}
    result["slides"] = slides
    return result


def foot_speed(model: Model, clip, forward, height, deforming):
    """Ground speed from the foot joints: while a foot is at the bottom of its
    own travel it is planted, and slides backwards at the body's speed."""
    rest = model.pose(None, 0.0)
    low = [j for j in model.joints if j in deforming and rest[j][13] < 0.2 * height]
    feet = [j for j in low if not any(model.parent.get(k) == j for k in low)]
    if not feet:
        return None
    fx, fz = math.sin(forward), math.cos(forward)
    duration = model.clips[clip][0]
    n = max(8, int(duration * 60))
    tracks = {j: [] for j in feet}
    for k in range(n + 1):
        world = model.pose(clip, duration * k / n)
        for j in feet:
            tracks[j].append((world[j][12] * fx + world[j][14] * fz, world[j][13]))
    dt = duration / n
    speeds = []
    for track in tracks.values():
        lo = min(h for _, h in track)
        hi = max(h for _, h in track)
        planted = lo + max(0.15 * (hi - lo), 0.005 * height)
        for (p0, h0), (p1, h1) in zip(track, track[1:]):
            if h0 <= planted and h1 <= planted:
                speeds.append(-(p1 - p0) / dt)
    return _median(speeds) if len(speeds) >= 4 else None


def root_travel(model: Model, clip: str):
    """How far the skeleton's root moves over the clip (root motion)."""
    root = model.joints[0]
    while root in model.parent:
        root = model.parent[root]
    times = model.frames(clip)
    a = model.pose(clip, times[0])[root]
    b = model.pose(clip, times[-1])[root]
    return math.hypot(b[12] - a[12], b[14] - a[14])


def head_yaw(model: Model):
    """Yaw of the head, rest pose: from the tail joints (else the body's
    centre) to the head joints; None without a head joint."""
    world = model.pose(None, 0.0)
    names = [n.lower() for n in model.joint_names]

    def mean(test):
        js = [j for j, n in zip(model.joints, names) if test(n) and "ik" not in n and "nub" not in n and "target" not in n]
        if not js:
            return None
        return (sum(world[j][12] for j in js) / len(js), sum(world[j][14] for j in js) / len(js))

    head = mean(lambda n: "head" in n)
    if head is None:
        return None
    tail = mean(lambda n: "tail" in n)
    if tail is None:
        tail = (sum(p[0] for p in model.positions) / len(model.positions), sum(p[2] for p in model.positions) / len(model.positions))
    if math.hypot(head[0] - tail[0], head[1] - tail[1]) < 1e-6:
        return None
    return math.atan2(head[0] - tail[0], head[1] - tail[1])


# ── clips made for another skeleton ─────────────────────────────────────────
#
# The 0 A.D. clips were baked, through Collada, on skeletons of other
# proportions and frames than the meshes they now drive: a fox whose pelvis
# carries a constant 2.5 scale in every clip (it doubles in size the moment it
# moves), a wolf's legs translated to another wolf's lengths, a deer's root
# turned out of its rest frame, a goat's idle with a key fifty metres away.
# Retargeting onto the mesh's own skeleton is the usual cure: a joint below
# the roots keeps its rotations and its rest length, and a root keeps its
# motion relative to its first key, re-anchored on its rest transform. A
# repair stays only where it measurably helps (`clip_score`), and a clip still
# broken after it plays no part (`clip_defect`).


def _q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _q_inv(q):
    n = sum(v * v for v in q) or 1.0
    return (-q[0] / n, -q[1] / n, -q[2] / n, q[3] / n)


def _q_rotate(q, v):
    x, y, z, _ = _q_mul(_q_mul(q, (v[0], v[1], v[2], 0.0)), _q_inv(q))
    return (x, y, z)


def _q_angle(a, b):
    d = abs(sum(x * y for x, y in zip(a, b))) / ((math.sqrt(sum(x * x for x in a)) * math.sqrt(sum(y * y for y in b))) or 1)
    return 2 * math.acos(min(1.0, d))


def deforming_joints(model: Model):
    """Joints that move a vertex, themselves or through a descendant."""
    used = set()
    for js, ws in zip(model.vjoints, model.vweights):
        for j, w in zip(js, ws):
            if w > 0:
                used.add(model.joints[j])
    out = set(used)
    for j in used:
        while j in model.parent and model.parent[j] in model.joint_set:
            j = model.parent[j]
            out.add(j)
    return out


def clip_score(model: Model, clip, deforming=None, samples=12):
    """How far a clip carries the body from its own rest skeleton.

    The deforming joints' spread, their centre and their lowest point against
    the rest pose, as ratios of the rest body: a fox twice its size scores
    0.9, a goat fifty metres off scores 85, a lion lying down 0.1."""
    deforming = deforming or deforming_joints(model)
    joints = [j for j in model.joints if j in deforming]
    rest = model.pose(None, 0.0)

    def stats(world):
        pts = [(world[j][12], world[j][13], world[j][14]) for j in joints]
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        cz = sum(p[2] for p in pts) / len(pts)
        spread = math.sqrt(sum((p[0] - cx) ** 2 + (p[1] - cy) ** 2 + (p[2] - cz) ** 2 for p in pts) / len(pts))
        return (cx, cz), spread, min(p[1] for p in pts)

    (rc, rs, rl) = stats(rest)
    rs = max(rs, 1e-6)
    worst = 0.0
    duration = model.clips[clip][0]
    for k in range(samples + 1):
        c, sp, low = stats(model.pose(clip, duration * k / samples))
        worst = max(worst, abs(math.log(max(sp, 1e-9) / rs)), math.hypot(c[0] - rc[0], c[1] - rc[1]) / rs,
                    abs(low - rl) / rs)
    return worst


def clip_defect(model: Model, clip, deforming=None, samples=24):
    """What makes a clip unplayable, or None.

    A body that changes size, jumps between two frames or sinks through the
    ground is a broken clip; a bear rearing up or a rabbit in mid-bound is
    not, however far it is from the rest pose."""
    deforming = deforming or deforming_joints(model)
    joints = [j for j in model.joints if j in deforming]

    def stats(world):
        pts = [(world[j][12], world[j][13], world[j][14]) for j in joints]
        c = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
        spread = math.sqrt(sum(sum((p[i] - c[i]) ** 2 for i in range(3)) for p in pts) / len(pts))
        return c, spread, min(p[1] for p in pts)

    rc, rs, rl = stats(model.pose(None, 0.0))
    rs = max(rs, 1e-6)
    duration = model.clips[clip][0]
    previous = None
    for k in range(samples + 1):
        c, spread, low = stats(model.pose(clip, duration * k / samples))
        ratio = spread / rs
        if ratio < 0.625 or ratio > 1.6:
            return f"size x{ratio:.2f}"
        if math.hypot(c[0] - rc[0], c[2] - rc[2]) > 3 * rs:
            return f"{math.hypot(c[0] - rc[0], c[2] - rc[2]):.1f} m off its place"
        if low < rl - 1.5 * rs:
            return "under the ground"
        if previous and math.dist(c, previous) > rs:
            return "jumps between frames"
        previous = c
    return None


def _retargeted_values(model: Model, channels, node, path, values, anchor_root, floor):
    """New key values for one channel of a clip, or None to keep it."""
    t_rest, r_rest, s_rest = model.rest[node]

    def first(kind):
        for n, p, _, v, *_ in channels:
            if n == node and p == kind and v:
                return v[0]
        return None
    root = model.parent.get(node) not in model.joint_set
    if path == "rotation":
        if not (root and anchor_root):
            return None
        correction = _q_mul(r_rest, _q_inv(values[0]))
        if _q_angle(correction, (0, 0, 0, 1)) < math.radians(1):
            return None
        return [_q_mul(correction, v) for v in values]
    if path == "scale":
        if all(abs(v - r) <= 0.02 * max(abs(r), 1e-6) for val in values for v, r in zip(val, s_rest)):
            return None
        if root and anchor_root:
            return [tuple(r * v / max(abs(v0), 1e-9) for r, v, v0 in zip(s_rest, val, values[0])) for val in values]
        return [tuple(s_rest) for _ in values]
    if path == "translation":
        length = math.sqrt(sum(v * v for v in t_rest))
        deviation = max(abs(v - r) for val in values for v, r in zip(val, t_rest))
        if not root and deviation <= max(floor, 0.2 * length):
            return None
        s0 = first("scale")
        scale = s_rest[0] / s0[0] if s0 and abs(s0[0]) > 1e-9 else 1.0
        if root:
            if not anchor_root:
                return None
            rotation = first("rotation")
            correction = _q_mul(r_rest, _q_inv(rotation)) if rotation else (0, 0, 0, 1)
            t0 = values[0]
            return [tuple(r + d * scale for r, d in zip(t_rest, _q_rotate(correction, tuple(v - w for v, w in zip(val, t0)))))
                    for val in values]
        if abs(scale - 1.0) > 0.02:
            # A body joint scaled with its clip moves with it: its motion, at rest size.
            t0 = values[0]
            return [tuple(r + (v - w) * scale for r, v, w in zip(t_rest, val, t0)) for val in values]
        return [tuple(t_rest) for _ in values]
    return None


def retarget_clip(model: Model, clip, anchor_root):
    """The channel values that put `clip` back on the model's skeleton."""
    deforming = deforming_joints(model)
    lengths = sorted(math.sqrt(sum(v * v for v in model.rest[j][0])) for j in model.joints)
    floor = 0.05 * (lengths[len(lengths) // 2] if lengths else 0.0)
    changes = {}
    channels = model.clips[clip][1]
    for node, path, times, values, interpolation, a, c in channels:
        if node not in deforming or interpolation == "CUBICSPLINE":
            continue
        new = _retargeted_values(model, channels, node, path, values, anchor_root, floor)
        if new is not None:
            changes[(a, c)] = new
    return changes


def _apply(model: Model, clip, changes):
    duration, channels = model.clips[clip]
    model.clips[clip] = (duration, [ch[:3] + (changes.get((ch[5], ch[6]), ch[3]),) + ch[4:] for ch in channels])


def needs_retarget(model: Model, clip, deforming):
    """Whether a clip carries the Collada faults retargeting cures, and which:
    a body drawn at another size than its rest (a fox at 2.5), a root turned
    out of its rest frame, or a body standing off its place. A clip without
    them is the author's and is left as it is, however far its poses go."""
    joints = [j for j in model.joints if j in deforming]

    def stats(world):
        pts = [(world[j][12], world[j][13], world[j][14]) for j in joints]
        c = tuple(sum(p[i] for p in pts) / len(pts) for i in range(3))
        return c, math.sqrt(sum(sum((p[i] - c[i]) ** 2 for i in range(3)) for p in pts) / len(pts))

    rc, rs = stats(model.pose(None, 0.0))
    duration = model.clips[clip][0]
    ratios = [stats(model.pose(clip, duration * k / 8))[1] / max(rs, 1e-9) for k in range(9)]
    mean = sum(ratios) / len(ratios)
    if mean < 0.85 or mean > 1.18:
        return f"size x{mean:.2f}"
    c0, _ = stats(model.pose(clip, 0.0))
    if math.hypot(c0[0] - rc[0], c0[2] - rc[2]) > 0.6 * rs:
        return "off its place"
    for node, path, _, values, *_ in model.clips[clip][1]:
        if path == "rotation" and node in deforming and model.parent.get(node) not in model.joint_set and values:
            if _q_angle(model.rest[node][1], values[0]) > math.radians(45):
                return "root turned"
    return None


def repair_model(model: Model, clips=None):
    """Retarget the clips (all, or those the game plays) that carry Collada
    faults (`needs_retarget`); the report says what changed."""
    deforming = deforming_joints(model)
    report, all_changes = {}, {}
    for clip in list(clips if clips is not None else model.clips):
        if not needs_retarget(model, clip, deforming):
            continue
        original = model.clips[clip]
        before = clip_score(model, clip, deforming)
        best, best_score, best_changes = "kept", before, {}
        for label, anchor in (("rotations only", False), ("re-anchored root", True)):
            changes = retarget_clip(model, clip, anchor)
            if not changes:
                continue
            _apply(model, clip, changes)
            score = clip_score(model, clip, deforming)
            model.clips[clip] = original
            if score < best_score - 0.03:
                best, best_score, best_changes = label, score, changes
        if best_changes:
            _apply(model, clip, best_changes)
            all_changes.update(best_changes)
        report[clip] = {"before": round(before, 3), "after": round(best_score, 3), "repair": best}
    return report, all_changes


def write_repaired(model: Model, changes):
    """The GLB with each changed channel pointed at a new output accessor."""
    doc = json.loads(json.dumps(model.doc))
    binary = bytearray(model.binary)
    for (a, c), values in sorted(changes.items()):
        anim = doc["animations"][a]
        channel = anim["channels"][c]
        old = anim["samplers"][channel["sampler"]]
        width = len(values[0])
        while len(binary) % 4:
            binary.append(0)
        offset = len(binary)
        for v in values:
            binary += struct.pack("<" + "f" * width, *v)
        doc["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(binary) - offset})
        doc["accessors"].append({"bufferView": len(doc["bufferViews"]) - 1, "componentType": 5126,
                                 "count": len(values), "type": {3: "VEC3", 4: "VEC4"}[width]})
        anim["samplers"].append({"input": old["input"], "output": len(doc["accessors"]) - 1,
                                 "interpolation": old.get("interpolation", "LINEAR")})
        channel["sampler"] = len(anim["samplers"]) - 1
    # Samplers no channel uses any more go, and the indices close up.
    for anim in doc.get("animations", []):
        used = sorted({ch["sampler"] for ch in anim["channels"]})
        remap = {old: new for new, old in enumerate(used)}
        anim["samplers"] = [anim["samplers"][i] for i in used]
        for ch in anim["channels"]:
            ch["sampler"] = remap[ch["sampler"]]
    while len(binary) % 4:
        binary.append(0)
    doc["buffers"][0]["byteLength"] = len(binary)
    text = json.dumps(doc, separators=(",", ":"), ensure_ascii=True).encode()
    text += b" " * ((-len(text)) % 4)
    out = struct.pack("<III", 0x46546C67, 2, 28 + len(text) + len(binary))
    out += struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(binary), 0x004E4942) + bytes(binary)
    _write_bytes(model.path, out)


def _write_bytes(path: Path, data: bytes, tries=8):
    """Write, retrying: a synced folder (OneDrive) sometimes answers EINVAL
    to an open while it holds the file."""
    import time
    for attempt in range(tries):
        try:
            path.write_bytes(data)
            return
        except OSError:
            if attempt == tries - 1:
                raise
            time.sleep(0.5 * (attempt + 1))


def same_rig(a: Model, b: Model, tolerance=1e-3):
    if a.joint_names != b.joint_names:
        return False
    for m, n in zip(a.inverse_binds, b.inverse_binds):
        if any(abs(x - y) > tolerance * max(1.0, abs(x)) for x, y in zip(m, n)):
            return False
    return True


def assign_roles(clips, override=None):
    """Which clips play which part, from their names.

    The sources name their clips after what they show (`bovidae_walk_relax`,
    `Idle.000`, `fox_feeding`); deaths and attacks have no part here. A
    species may name its clips itself in fauna.json, and then only those play.
    Models whose rigs were made for R1World carry only `Rig_Check_*` clips:
    the head one is their idle, and they have no gait (`data/animal-pipeline`).
    """
    if override:
        return {role: list(names) for role, names in override.items()}
    lower = {name: name.lower() for name in clips}

    def pick(test):
        return [name for name in clips if test(lower[name])]

    roles = {
        "idle": pick(lambda n: ("idle" in n and "transition" not in n) or n.startswith("lookaround")),
        "eat": pick(lambda n: ("feeding" in n or n.startswith("eat")) and "walk" not in n and "transition" not in n),
        "walk": pick(lambda n: "walk" in n and "feeding" not in n),
        "run": pick(lambda n: "run" in n or "gallop" in n),
    }
    if not roles["run"]:
        roles["run"] = pick(lambda n: "trot" in n)
    if not roles["idle"]:
        roles["idle"] = pick(lambda n: n.startswith("sitting") or n.startswith("guarding")) or pick(lambda n: n == "rig_check_head")
    if not any(roles.values()) and len(clips) == 1:
        roles["idle"] = list(clips)
    return {role: names for role, names in roles.items() if names}


def geometry_key(model: Model):
    """A hash of everything but the images: coat variants share it."""
    import hashlib
    images = {img.get("bufferView") for img in model.doc.get("images", [])}
    h = hashlib.sha1()
    for i, view in enumerate(model.doc["bufferViews"]):
        if i in images:
            continue
        start = view.get("byteOffset", 0)
        h.update(model.binary[start:start + view["byteLength"]])
    return h.hexdigest()


def measure_model(model_id: str, override=None, cache=None):
    """The game's numbers for one model.

    `override` names its clips per role when fauna.json does; otherwise
    `assign_roles` reads them from the clip names. A named clip the model
    lacks is an error, never a guess.
    """
    near = Model(ANIMALS / f"{model_id}.glb")
    far_path = ANIMALS / f"{model_id}-lod1.glb"
    far = Model(far_path)
    roles = assign_roles(list(near.clips), override)
    key = geometry_key(near) + json.dumps(roles, sort_keys=True)
    if cache is not None and key in cache:
        result = dict(cache[key])
        result["lod0"] = f"assets/animals/{model_id}.glb"
        result["lod1"] = f"assets/animals/{far_path.name}"
        result["sameRig"] = same_rig(near, far)
        return result
    for role, clips in roles.items():
        for clip in clips:
            if clip not in near.clips:
                raise ValueError(f"{model_id}: role {role} names clip {clip!r}, not in {sorted(near.clips)}")
            if clip not in far.clips:
                raise ValueError(f"{model_id}-lod1: no clip {clip!r}")
    # A clip still broken after retargeting plays no part.
    deforming = deforming_joints(near)
    refused = {}
    for role in list(roles):
        kept = []
        for clip in roles[role]:
            defect = clip_defect(near, clip, deforming)
            if defect:
                refused[clip] = defect
            else:
                kept.append(clip)
        if kept:
            roles[role] = kept
        else:
            del roles[role]
    stand = (roles.get("idle") or roles.get("eat") or roles.get("walk") or roles.get("fly") or [None])[0]
    if stand is None:
        raise ValueError(f"{model_id}: no idle, eat, walk or fly clip")
    standing = measure_clip(near, stand)
    gaits = {}
    # Which way the head is: the skeleton's tail-to-head axis, which every
    # source names; else where the stance feet push (they slide backwards
    # under a body walking in place), noisier; else +Z, said as such.
    head = head_yaw(near)
    pushes = []
    if head is None:
        for role in ("walk", "run"):
            if roles.get(role) and not pushes:
                pushes = measure_clip(near, roles[role][0])["slides"]
    if head is not None:
        forward, source = head, "head bone"
    elif len(pushes) >= 4:
        forward, source = math.atan2(-_median([v[0] for v in pushes]), -_median([v[1] for v in pushes])), "stance feet"
    else:
        forward, source = 0.0, "none: assumed +Z"
    # Snap to the model's axes when within 15 degrees: sources face an axis.
    for axis in (0.0, math.pi / 2, math.pi, -math.pi / 2):
        if abs(math.remainder(forward - axis, 2 * math.pi)) < math.radians(15):
            forward = axis
            break
    # A gait's speed: how fast its stance feet slide backwards along the head
    # axis, the speed at which they stay planted -- from the lowest vertices
    # and from the foot joints, which disagree when a hoof pivots. Feet that
    # push forwards walk the animal backwards: that clip plays no part, and
    # the next of its role is tried. A run slower than a third of what
    # dynamic similarity gives its legs (Froude number 1; 0.25 walking) is a
    # stride the measure missed: the inferred speed stands, said as such.
    fx, fz = math.sin(forward), math.cos(forward)
    leg = 0.55 * standing["height"]
    for role, froude in (("walk", 0.25), ("run", 1.0)):
        for clip in list(roles.get(role, [])):
            m = measure_clip(near, clip)
            back = [-(v[0] * fx + v[1] * fz) for v in m["slides"]]
            by_vertices = _median(back) if len(back) >= 4 else None
            by_feet = foot_speed(near, clip, forward, standing["height"], deforming)
            known = [v for v in (by_vertices, by_feet) if v is not None]
            floor = 0.02 * standing["height"] / max(m["seconds"], 1e-3)
            if known and max(known) < -floor and all(v < 0 for v in known):
                refused[clip] = "its feet push forwards: it walks backwards"
                roles[role].remove(clip)
                continue
            entry = {"clip": clip, "seconds": m["seconds"], "rootTravel": round(root_travel(near, clip), 4)}
            prior = math.sqrt(froude * 9.81 * leg)
            measured = max(known) if known else None
            if measured is not None and measured > floor and (role == "walk" or measured >= prior / 3):
                entry["speed"], entry["speedSource"] = round(measured, 4), "measured: stance feet"
            else:
                entry["speed"] = round(prior, 4)
                entry["speedSource"] = f"inferred: Froude {froude} on a {leg:.2f} m leg" + (
                    f" (measured {measured:.3f} m/s)" if measured is not None else "")
            gaits[role] = entry
            # The role plays its measured clip first.
            roles[role].remove(clip)
            roles[role].insert(0, clip)
            break
        if role in roles and not roles[role]:
            del roles[role]
    xs = [p[0] for p in near.positions]
    zs = [p[2] for p in near.positions]
    along = abs(math.sin(forward)) * (max(xs) - min(xs)) + abs(math.cos(forward)) * (max(zs) - min(zs))
    across = abs(math.cos(forward)) * (max(xs) - min(xs)) + abs(math.sin(forward)) * (max(zs) - min(zs))
    result = {
        "lod0": f"assets/animals/{model_id}.glb",
        "lod1": f"assets/animals/{far_path.name}",
        "ground": standing["ground"],
        "height": standing["height"],
        "length": round(along, 4),
        "width": round(across, 4),
        "forward": round(forward, 4),
        "forwardSource": source,
        "headYaw": None if head is None else round(head, 4),
        "roles": roles,
        "refusedClips": refused,
        "gaits": gaits,
        "clips": {name: round(near.clips[name][0], 4) for name in sorted(near.clips)},
        "sameRig": same_rig(near, far),
        "joints": len(near.joints),
        "vertices": {"lod0": len(near.positions), "lod1": len(far.positions)},
    }
    if cache is not None:
        cache[key] = result
    return result


def used_models(fauna: dict):
    """Each model fauna.json places, with the clips its species names (or None)."""
    out = {}
    for species in fauna["species"]:
        override = species.get("clips")
        for variant in species["models"] + species.get("indoorModels", []):
            model = variant["model"]
            if model in out and out[model] != override:
                raise ValueError(f"{model}: two species name its clips differently")
            out[model] = override
    return out


def collada_source(model_id: str):
    """Whether a model came through 0 A.D.'s Collada (Wildfire Games): the
    only source whose clips carry the faults retargeting cures. Every other
    author's clips were made on the model's own skeleton and are kept."""
    import csv
    for row in csv.DictReader((ANIMALS / "catalog.csv").open(encoding="utf-8-sig")):
        if row["id"] == model_id:
            return "Wildfire Games" in row["author"]
    return False


def repair_files(model_id: str, override=None):
    """Retarget the clips the game plays in a model's two files alike; the
    clips repaired."""
    if not collada_source(model_id):
        return {}, []
    near = Model(ANIMALS / f"{model_id}.glb")
    played = sorted({c for names in assign_roles(list(near.clips), override).values() for c in names})
    report, changes = repair_model(near, played)
    repaired = {clip: r for clip, r in report.items() if r["repair"] != "kept"}
    if not repaired:
        return {}, []
    write_repaired(near, changes)
    far = Model(ANIMALS / f"{model_id}-lod1.glb")
    far_changes = {}
    for clip, r in repaired.items():
        far_changes.update(retarget_clip(far, clip, r["repair"] == "re-anchored root"))
    write_repaired(far, far_changes)
    return repaired, [near.path, far.path]


def update_manifest(paths, repairs=None):
    """The manifest's SHA-256 of the files the repair rewrote, and which clips."""
    import hashlib
    manifest_path = ANIMALS / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    names = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
    for asset in manifest["assets"]:
        hit = False
        for name in list(asset.get("sha256", {})):
            if name in names:
                asset["sha256"][name] = names[name]
                hit = True
        if hit:
            asset["clip_retarget"] = "game/tools/r1/animals.py: clips retargeted onto the model's own skeleton"
            if repairs and asset["id"] in repairs:
                asset["clip_retarget_clips"] = repairs[asset["id"]]
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


REGISTRY = GAME / "assets/THIRD_PARTY_ASSETS.json"
LICENSE_URLS = {"CC0-1.0": "https://creativecommons.org/publicdomain/zero/1.0/",
                "CC-BY-4.0": "https://creativecommons.org/licenses/by/4.0/",
                "CC-BY-SA-3.0": "https://creativecommons.org/licenses/by-sa/3.0/"}


def update_registry(models):
    """The placed animals in assets/THIRD_PARTY_ASSETS.json, one entry per
    author and licence, from the pipeline's catalogue."""
    import csv
    rows = {r["id"]: r for r in csv.DictReader((ANIMALS / "catalog.csv").open(encoding="utf-8-sig"))}
    groups = {}
    for model in sorted(models):
        row = rows.get(model)
        if row is None:
            raise ValueError(f"{model}: not in assets/animals/catalog.csv")
        groups.setdefault((row["author"], row["license"]), []).append(row)
    registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
    registry["assets"] = [a for a in registry["assets"] if not a.get("name", "").startswith("Animals — ")]
    for (author, licence), entries in sorted(groups.items()):
        files = []
        for r in entries:
            files += [f"assets/animals/{r['id']}.glb", f"assets/animals/{r['id']}-lod1.glb"]
        registry["assets"].append({
            "name": f"Animals — {author} ({licence})",
            "author": author,
            "source": "; ".join(sorted({r["source"] for r in entries})),
            "license": licence.replace("-", " ").replace("CC BY SA", "CC BY-SA").replace(" 1.0", " 1.0"),
            "license_url": LICENSE_URLS.get(licence, ""),
            "modified": "Prepared by data/animal-pipeline (GLB, approximate species scale, LOD1, rigs where noted in "
                        "assets/animals/manifest.json); clips retargeted onto each model's own skeleton by "
                        "tools/r1/animals.py. Attribution in assets/animals/ATTRIBUTION.md and in game.",
            "files": files,
        })
    REGISTRY.write_text(json.dumps(registry, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    fauna = json.loads(FAUNA.read_text(encoding="utf-8"))
    models = used_models(fauna)
    only = set(argv)
    previous = json.loads(OUT.read_text(encoding="utf-8")) if OUT.exists() else {"models": {}}
    measured = dict(previous.get("models", {})) if only else {}
    cache = {}
    rewritten = []
    manifest = json.loads((ANIMALS / "manifest.json").read_text(encoding="utf-8"))
    manifest_retargets = {a["id"]: a["clip_retarget_clips"] for a in manifest["assets"] if a.get("clip_retarget_clips")}
    for model_id, override in sorted(models.items()):
        if only and model_id not in only:
            continue
        repaired, paths = repair_files(model_id, override)
        rewritten += paths
        measured[model_id] = measure_model(model_id, override, cache)
        m = measured[model_id]
        if repaired:
            m["retargeted"] = {clip: [r["before"], r["after"], r["repair"]] for clip, r in sorted(repaired.items())}
        else:
            # Already retargeted on disk: the manifest says so.
            done = manifest_retargets.get(model_id)
            if done:
                m["retargeted"] = done
        gaits = ", ".join(f"{k} {v.get('speed', '-')} m/s" for k, v in m["gaits"].items()) or "no gait"
        print(f"{model_id:28s} forward {math.degrees(m['forward']):7.1f} ({m['forwardSource']}), "
              f"ground {m['ground']:+.3f}, height {m['height']:.2f}, {gaits}, rig {'shared' if m['sameRig'] else 'DIFFERS'}")
    update_registry(models)
    if rewritten:
        update_manifest(rewritten, {k: v["retargeted"] for k, v in measured.items() if "retargeted" in v})
        print(f"retargeted clips in {len(rewritten)} files; manifest checksums updated")
    measured = {k: measured[k] for k in sorted(measured) if k in models}
    doc = {
        "generatedBy": "game/tools/r1/animals.py",
        "note": "Measured from the GLBs: ground offset and height of the standing clip, head yaw in the "
                "model frame (0 = +Z, atan2(x, z)), gait speeds from the stance feet. Do not edit.",
        "models": measured,
    }
    OUT.write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(GAME)}: {len(measured)} models")


if __name__ == "__main__":
    main()
