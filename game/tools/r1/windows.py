"""Windows for the facade sheets, photographed from a modelled kit.

A facade sheet (`r1.surfaces`) is one bay by one storey of a wall. Its window
used to be painted: five flat rectangles of grey, which up close read as
exactly that. The windows now come from Poly Haven's *Modular Urban
Apartments Facade* (CC0): modelled frames, glazing bars, surrounds and sills,
textured from scans, at the grade of every other surface in this world.

They stay textures, never geometry: a city block holds thousands of windows
and a tile has a vertex budget (CLAUDE.md rule 5). `windows_blender.py` sees
each window module from the street through an orthographic camera framed on
its wall module, once per map -- colour, normal, roughness and metalness,
depth and glazing -- and the sheet takes the window from those maps: its
relief through the engine's parallax occlusion mapping, its glass through a
low roughness the sky can shine in.

    python -m r1.windows             # fetch the kit and photograph its windows

The photographs are cached under `game/cache/windows/`; the kit itself is
pinned under `data/source-assets/models/`.
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GAME = ROOT / "game"
KIT = "modular_urban_apartments_facade"
KIT_ROOT = ROOT / "data" / "source-assets" / "models" / KIT
KIT_RESOLUTION = "2k"
BAKE_ROOT = GAME / "cache" / "windows"
BLENDER = Path(os.environ.get("BLENDER", r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe"))
USER_AGENT = "R1World-generator/1.0"
GLASS_MATERIAL = f"{KIT}_glass"
OPACITY_MAP = f"textures/{KIT}_objects_alpha_{KIT_RESOLUTION}.jpg"
PASSES = ("color", "normal", "rm", "depth", "glass")
# A sheet's window is photographed at its own pixels over the kit's 3 m bay.
PIXELS_PER_METRE = 1024 / 3.0
SAMPLES = 32

# The kit's modules this world uses, each in the wall module it was modelled
# for. `small` is the tall single window with its surround and sill; `plain`
# the same opening without a surround; `wide` two casements in one frame.
MODULES = {
    "small": "window_centered_small_02",
    "plain": "window_centered_small_03",
    "wide": "window_centered_large_03",
}


def _get(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=300) as response:
        return response.read()


def fetch() -> Path:
    """The kit's glTF, its buffer and its textures, each checked against the
    md5 Poly Haven publishes and pinned in `pins.json` on first fetch."""
    KIT_ROOT.mkdir(parents=True, exist_ok=True)
    pins_path = KIT_ROOT / "pins.json"
    pins = json.loads(pins_path.read_text()) if pins_path.exists() else {}
    gltf = KIT_ROOT / f"{KIT}_{KIT_RESOLUTION}.gltf"
    files = None
    model = lambda f: f["gltf"][KIT_RESOLUTION]["gltf"]

    def ensure(relative: str, entry_of) -> None:
        nonlocal files
        target = KIT_ROOT / relative
        pin = pins.get(relative)
        if target.exists() and pin and hashlib.md5(target.read_bytes()).hexdigest() == pin:
            return
        if files is None:
            files = json.loads(_get(f"https://api.polyhaven.com/files/{KIT}"))
        entry = entry_of(files)
        payload = _get(entry["url"])
        digest = hashlib.md5(payload).hexdigest()
        if digest != entry["md5"] or (pin and digest != pin):
            raise RuntimeError(f"{KIT}/{relative}: download does not match its md5")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(payload)
        pins[relative] = digest

    ensure(gltf.name, model)
    document = json.loads(gltf.read_text(encoding="utf-8"))
    for item in document.get("buffers", []) + document.get("images", []):
        ensure(item["uri"], lambda f, uri=item["uri"]: model(f)["include"][uri])
    # The glTF's textures are JPEGs: the glazing's opacity is fetched apart.
    ensure(OPACITY_MAP, lambda f: f["objects_alpha"][KIT_RESOLUTION]["jpg"])
    pins_path.write_text(json.dumps(pins, indent=1, sort_keys=True) + "\n")
    return gltf


def _baked(module: str) -> bool:
    return all((BAKE_ROOT / f"{module}_{p}.npy").exists() for p in PASSES)


def photograph(modules: list[str]) -> None:
    """Run Blender on the modules not yet photographed."""
    todo = [m for m in modules if not _baked(m)]
    if not todo:
        return
    if not BLENDER.exists():
        raise SystemExit(f"Blender 4.2 not found at {BLENDER}; set BLENDER")
    gltf = fetch()
    BAKE_ROOT.mkdir(parents=True, exist_ok=True)
    job = {"gltf": str(gltf), "out": BAKE_ROOT.as_posix(), "report": str(BAKE_ROOT / "report.json"),
           "pixelsPerMetre": PIXELS_PER_METRE, "samples": SAMPLES, "glassMaterials": [GLASS_MATERIAL],
           "glassOpacity": str(KIT_ROOT / OPACITY_MAP),
           "modules": [{"name": m, "frame": "wall_" + m} for m in todo]}
    job_path = BAKE_ROOT / "job.json"
    job_path.write_text(json.dumps(job), encoding="utf-8")
    result = subprocess.run([str(BLENDER), "-b", "--factory-startup", "-P",
                             str(Path(__file__).with_name("windows_blender.py")), "--", str(job_path)],
                            capture_output=True, text=True)
    if result.returncode != 0 or not all(_baked(m) for m in todo):
        raise SystemExit(f"windows: Blender failed\n{result.stdout[-3000:]}\n{result.stderr[-3000:]}")


def layer(kind: str, pixels: int) -> dict:
    """The window of `kind` over one bay, `pixels` square: coverage, linear
    colour, unit normal (wall frame), roughness, metalness, metres in front of
    the wall plane, and glazing, each resized from the photograph."""
    import numpy as np
    from PIL import Image
    module = MODULES[kind]
    photograph([module])

    def load(name: str):
        array = np.load(BAKE_ROOT / f"{module}_{name}.npy")
        channels = [np.asarray(Image.fromarray(array[..., c]).resize((pixels, pixels), Image.LANCZOS))
                    for c in range(4)]
        return np.stack(channels, axis=-1).astype(np.float64)

    color, normal, rm, depth, glass = (load(p) for p in PASSES)
    coverage = np.clip(color[..., 3], 0.0, 1.0)
    # Premultiplied by the film's coverage: an edge pixel holds part of a
    # window and part of nothing, so each map is divided back by it.
    inside = np.maximum(coverage, 1e-4)[..., None]
    n = normal[..., :3] / inside * 2.0 - 1.0
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
    return {
        "coverage": coverage,
        "color": np.clip(color[..., :3] / inside, 0.0, 1.0),
        "normal": n,
        "rough": np.clip(rm[..., 1] / inside[..., 0], 0.0, 1.0),
        "metal": np.clip(rm[..., 2] / inside[..., 0], 0.0, 1.0),
        "depth": depth[..., 0] / inside[..., 0],
        "glass": np.clip(glass[..., 0] / inside[..., 0], 0.0, 1.0),
        "opacity": np.clip(glass[..., 1] / inside[..., 0], 0.0, 1.0),
    }


def main() -> int:
    photograph(list(MODULES.values()))
    print("windows photographed:", ", ".join(MODULES.values()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
