"""The people of the world: the player and the crowd, from Microsoft Rocketbox.

Rocketbox (MIT, 115 rigged scanned-and-painted characters on one 3ds Max
biped, and 400-odd motion-capture clips for that biped) is what a street
of real proportions needs: the same grade for the player and for everyone
he walks past, in everyday clothes, with walks, waits, telephones and
benches captured on people rather than keyed by hand.

    python -m r1.humans            (from game/tools; needs Blender 4.2)

Everything is fetched at one pinned commit into `cache/downloads/rocketbox/`
and nothing downloaded is committed except what this bakes:

* `assets/models/humans/<name>.glb` -- one skinned mesh per level of detail,
  the library's own colour maps reduced (1024/512 px for the player,
  512/256 for the crowd), the face rig folded into the head but for the
  eyes, which the game turns toward what a person looks at, and the clips
  retargeted onto the avatar (`humans_blender.py`).
* `assets/models/humans/humans.json` -- heights, clip speeds, vertex counts
  and where every file came from, which the game reads at start-up.

The game draws people at `SCALE` of their scanned size and moves them at the
speed each clip was captured at, so a foot never slides.
"""
from __future__ import annotations

import concurrent.futures
import json
import os
import struct
import subprocess
import urllib.parse
import urllib.request
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
OUT = GAME / "assets/models/humans"
MANIFEST = OUT / "humans.json"
REPO = "microsoft/Microsoft-Rocketbox"
COMMIT = "0943055db6ec570bcef9f2c8b41c9e5467c808f9"
CACHE = GAME / "cache/downloads/rocketbox" / COMMIT
BLENDER = Path(os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe"))
LICENSE = "assets/licenses/Microsoft-Rocketbox-MIT.txt"

# People are drawn at this fraction of their scanned size, and so are the
# road vehicles (native/world.cpp): the user's call, measured against the
# streets the player walks.
SCALE = 0.8

PLAYER = "Male_Adult_08"
# Keep the original everyday scans and add darker ones that were previously
# absent. Pilot_Female_02 is the library's only other dark-skinned adult woman
# compatible with this rig; her pilot uniform is the tradeoff for that mix.
CROWD = [
    ("Male_Adult_01", "m"), ("Male_Adult_03", "m"), ("Male_Adult_05", "m"),
    ("Male_Adult_06", "m"), ("Male_Adult_13", "m"), ("Business_Male_02", "m"),
    ("Female_Adult_01", "f"), ("Female_Adult_02", "f"), ("Female_Adult_05", "f"),
    ("Female_Adult_08", "f"), ("Female_Adult_14", "f"), ("Business_Female_03", "f"),
    ("Male_Adult_04", "m"), ("Male_Adult_07", "m"),
    ("Male_Adult_12", "m"), ("Male_Adult_18", "m"),
    ("Female_Adult_11", "f"),
    ("Business_Female_01", "f"), ("Pilot_Female_02", "f"),
]

# Visual tone of the supplied scans, reviewed from their original colour maps.
# This describes an asset's appearance, not a person's ethnicity or identity.
SKIN_TONES = {
    "Male_Adult_04": "dark", "Male_Adult_07": "medium",
    "Male_Adult_12": "dark", "Male_Adult_18": "dark",
    "Female_Adult_11": "medium",
    "Business_Female_01": "dark", "Pilot_Female_02": "dark",
}

STATIC = "Assets/Animations/all_animations_max_motextr_static/"
TRAVEL = "Assets/Animations/all_animations_max_motextr_xy/"
# (clip, file for a man, file for a woman, travels, speed the game plays it at)
PLAYER_CLIPS = [
    ("idle", STATIC + "m_idle_neutral_01.max.fbx", None, False, None),
    ("walk", TRAVEL + "m_walk_neutral_01.max.fbx", None, True, None),
    # native/world.cpp: 2.8 m/s on foot, 7 m/s with Shift.
    ("run", TRAVEL + "m_run_slow_01.max.fbx", None, True, 2.8),
    ("sprint", TRAVEL + "m_run_fast_01.max.fbx", None, True, 7.0),
]
CROWD_CLIPS = [
    ("idle", STATIC + "m_idle_neutral_01.max.fbx", STATIC + "f_idle_neutral_01.max.fbx", False, None),
    ("walk", TRAVEL + "m_walk_neutral_01.max.fbx", TRAVEL + "f_walk_neutral_01.max.fbx", True, None),
    ("wait", STATIC + "m_idle_waiting_01.max.fbx", STATIC + "f_idle_waiting_01.max.fbx", False, None),
    ("phone", STATIC + "m_cell_phone_talk_01.max.fbx", STATIC + "f_cell_phone_textmessage.max.fbx", False, None),
    ("talk", STATIC + "m_gestic_talk_neutral_01.max.fbx", STATIC + "f_gestic_talk_neutral_01.max.fbx", False, None),
    ("sit", STATIC + "m_sit_chair_idle_neutral_01.max.fbx", STATIC + "f_sit_chair_idle_neutral_01.max.fbx", False, None),
    ("run", TRAVEL + "m_run_slow_01.max.fbx", TRAVEL + "f_run_slow_01.max.fbx", True, None),
    # What someone does after the player walks into them (gen/crowd.cpp):
    # a shrug for a brush, a telling-off or dusting themselves down for a
    # shove. The library has no fall and no stagger; the stagger itself is
    # the engine's ImpactModifier, a spring on the spine.
    ("shrug", STATIC + "m_gestic_shrug_01.max.fbx", STATIC + "f_gestic_shrug_01.max.fbx", False, None),
    ("angry", STATIC + "m_idle_angry_01.max.fbx", STATIC + "f_idle_angry_01.max.fbx", False, None),
    ("dust", STATIC + "m_idle_dust_01.max.fbx", STATIC + "f_idle_dust_01.max.fbx", False, None),
]
# The reactions play for a few seconds and are cut there: a minute of
# telling-off would be a minute of keys in every crowd model.
CLIP_SECONDS = {"shrug": 4.0, "angry": 6.0, "dust": 6.0}
# Triangles per level of detail. A street pedestrian of the PS2's last years
# was 1.5-3 k triangles; the player keeps the whole scan (7.4 k).
CROWD_LODS = [{"name": "Near", "triangles": 2400}, {"name": "Far", "triangles": 500}]
PLAYER_LODS = [{"name": "Body"}]
# Texture sides in pixels: body, head, hair/eyelash cards, and optional hat.
PLAYER_TEXTURES = (1024, 512, 512)
CROWD_TEXTURES = (512, 256, 256, 256)
# Rule 2 (CLAUDE.md): what enters the game is an albedo. Skin, cloth and hair
# average well under this; a map above it was painted, not measured.
MAX_MEAN_ALBEDO = 0.35


def _tree() -> list[dict]:
    path = CACHE / "tree.json"
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        url = f"https://api.github.com/repos/{REPO}/git/trees/{COMMIT}?recursive=1"
        with urllib.request.urlopen(url, timeout=60) as r:
            path.write_bytes(r.read())
    return json.loads(path.read_text(encoding="utf-8"))["tree"]


def _fetch(path: str) -> Path:
    local = CACHE / path
    if not local.exists():
        local.parent.mkdir(parents=True, exist_ok=True)
        url = f"https://raw.githubusercontent.com/{REPO}/{COMMIT}/{urllib.parse.quote(path)}"
        tmp = local.with_suffix(local.suffix + ".part")
        urllib.request.urlretrieve(url, tmp)
        tmp.replace(local)
    return local


def _avatar_files(tree: list[dict], name: str) -> dict:
    folder = next(t["path"] for t in tree if t["type"] == "tree" and t["path"].endswith("/" + name)
                  and t["path"].startswith("Assets/Avatars/"))
    files = {"fbx": f"{folder}/Export/{name}.fbx"}
    for t in tree:
        p = t["path"]
        if not p.startswith(folder + "/Textures/"):
            continue
        for kind in ("body", "head", "opacity", "hat"):
            if p.endswith(f"_{kind}_color.tga"):
                files[kind] = p
    # Hair and eyelash cards, where the avatar has any (a shaved head has none).
    missing = {"fbx", "body", "head"} - set(files)
    if missing:
        raise SystemExit(f"{name}: the library has no {', '.join(sorted(missing))} for it")
    return files


def _mean_albedo(image) -> float:
    import numpy as np
    a = np.asarray(image.convert("RGB")).astype(np.float64) / 255.0
    linear = np.where(a <= 0.04045, a / 12.92, ((a + 0.055) / 1.055) ** 2.4)
    return float((linear @ [0.2126, 0.7152, 0.0722]).mean())


def _textures(name: str, files: dict, sides, work: Path) -> tuple[dict, dict]:
    from PIL import Image
    out, albedo = {}, {}
    for kind, side in zip(("body", "head", "opacity", "hat"), sides):
        if kind not in files:
            continue
        image = Image.open(_fetch(files[kind]))
        if kind == "opacity":
            target = work / f"{name}_{kind}.png"
            image.convert("RGBA").resize((side, side), Image.LANCZOS).save(target, optimize=True)
        else:
            rgb = image.convert("RGB")
            albedo[kind] = round(_mean_albedo(rgb), 3)
            if albedo[kind] > MAX_MEAN_ALBEDO:
                raise SystemExit(f"{name}: {kind} map averages {albedo[kind]} albedo, above {MAX_MEAN_ALBEDO}")
            target = work / f"{name}_{kind}.jpg"
            rgb.resize((side, side), Image.LANCZOS).save(target, quality=88, optimize=True)
        out[kind] = str(target)
    return out, albedo


def glb_vertices(path: Path) -> dict:
    """Vertices per mesh, as the engine will upload them (seams included)."""
    data = path.read_bytes()
    length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + length])
    return {m.get("name", str(i)): sum(doc["accessors"][p["attributes"]["POSITION"]]["count"] for p in m["primitives"])
            for i, m in enumerate(doc.get("meshes", []))}


def _bake(name: str, sex: str, player: bool, work: Path, tree: list[dict]) -> dict:
    files = _avatar_files(tree, name)
    textures, albedo = _textures(name, files, PLAYER_TEXTURES if player else CROWD_TEXTURES, work)
    clips = []
    for clip, male, female, travels, speed in (PLAYER_CLIPS if player else CROWD_CLIPS):
        source = female if sex == "f" and female else male
        clips.append({"name": clip, "fbx": str(_fetch(source)), "inPlace": travels, "speed": speed,
                      "seconds": CLIP_SECONDS.get(clip), "file": source})
    model = OUT / ("player.glb" if player else f"{name.lower()}.glb")
    job = {"name": name, "avatar": str(_fetch(files["fbx"])), "textures": textures, "scale": SCALE,
           "clips": clips, "jump": "sprint" if player else None, "jumpSeconds": 0.74,
           "lods": PLAYER_LODS if player else CROWD_LODS, "frameStep": 1 if player else 2,
           "out": str(model), "report": str(work / f"{name}.json")}
    job_path = work / f"{name}_job.json"
    job_path.write_text(json.dumps(job), encoding="utf-8")
    result = subprocess.run([str(BLENDER), "-b", "--factory-startup", "-P", str(Path(__file__).with_name("humans_blender.py")),
                             "--", str(job_path)], capture_output=True, text=True)
    if result.returncode != 0 or not Path(job["report"]).exists():
        raise SystemExit(f"{name}: Blender failed\n{result.stdout[-3000:]}\n{result.stderr[-3000:]}")
    report = json.loads(Path(job["report"]).read_text(encoding="utf-8"))
    for clip in clips:
        report["clips"][clip["name"]]["source"] = clip["file"]
    counts = glb_vertices(model)
    for lod in report["lods"]:
        lod["exportedVertices"] = counts.get(lod["name"], 0)
    entry = {"name": name, "sex": sex, "skinTone": SKIN_TONES.get(name, "light"),
             "model": model.relative_to(GAME).as_posix(),
             "source": files["fbx"], "albedo": albedo, **report}
    print(f"{name:20s} {model.stat().st_size / 1e6:5.2f} MB  "
          + "  ".join(f"{l['name']} {l['triangles']} tri / {l['exportedVertices']} v" for l in report["lods"]))
    return entry


def main() -> int:
    if not BLENDER.exists():
        raise SystemExit(f"Blender 4.2 not found at {BLENDER}; set BLENDER")
    OUT.mkdir(parents=True, exist_ok=True)
    work = CACHE / "work"
    work.mkdir(parents=True, exist_ok=True)
    tree = _tree()
    jobs = [(PLAYER, "m", True)] + [(n, s, False) for n, s in CROWD]
    # Every source once, before any bake asks for it: two bakes sharing a
    # clip must not both be writing it.
    sources = {c[1] for c in PLAYER_CLIPS + CROWD_CLIPS} | {c[2] for c in CROWD_CLIPS if c[2]}
    for name, _, _ in jobs:
        sources |= set(_avatar_files(tree, name).values())
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        list(pool.map(_fetch, sorted(sources)))
    previous = {}
    if MANIFEST.exists():
        old = json.loads(MANIFEST.read_text(encoding="utf-8"))
        previous = {e["name"]: e for e in [old["player"], *old["crowd"]]}
    def bake_or_reuse(job):
        name, _, player = job
        old = previous.get(name)
        target = OUT / ("player.glb" if player else f"{name.lower()}.glb")
        if old and target.exists() and old.get("model") == target.relative_to(GAME).as_posix():
            return {**old, "skinTone": SKIN_TONES.get(name, "light")}
        return _bake(*job, work, tree)
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        entries = list(pool.map(bake_or_reuse, jobs))
    player, crowd = entries[0], entries[1:]
    shared = sum(l["exportedVertices"] for e in entries for l in e["lods"])
    manifest = {
        "source": f"https://github.com/{REPO}", "commit": COMMIT, "license": "MIT", "licenseFile": LICENSE,
        "author": "Microsoft (Rocketbox Studios)", "scale": SCALE, "generatedBy": "game/tools/r1/humans.py",
        "sharedVertices": shared, "player": player, "crowd": crowd,
    }
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    _register([e["model"] for e in entries] + [MANIFEST.relative_to(GAME).as_posix()])
    print(f"{len(entries)} people, {shared} shared vertices")
    return 0


def _register(files: list[str]) -> None:
    path = GAME / "assets/THIRD_PARTY_ASSETS.json"
    doc = json.loads(path.read_text(encoding="utf-8"))
    name = "Microsoft Rocketbox Avatar Library - player and crowd"
    doc["assets"] = [a for a in doc["assets"] if a.get("name") != name]
    doc["assets"].append({
        "name": name, "author": "Microsoft (Rocketbox Studios)", "source": f"https://github.com/{REPO}/tree/{COMMIT}",
        "license": "MIT", "licenseFile": LICENSE, "generatedBy": "game/tools/r1/humans.py",
        "modifications": "retargeted motion-capture clips, face rig folded into the head but for the eyes, "
                         "decimated levels of detail, colour maps reduced; normal and specular maps not used",
        "files": sorted(files),
    })
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
