"""The sky follows the hour: a day of photographed skies, placed by measurement.

The world used to sit under one HDRI, a single photograph with its own Sun
baked in, dimmed as the real Sun went down. A night under a daytime sky is a
daytime sky that has been turned down, and that was the one visible failure of
the solar model (plan §2).

This module replaces it with the Qwantani series from Poly Haven: eleven
photographs of one sky taken from one hilltop over five days of August 2024,
from before dawn to after dusk. Three things are measured, never chosen:

**Where each sky belongs.** Every photograph carries the instant it was taken
and the place it was taken from, so `solar.sun_angles` gives the Sun's true
elevation in it. The Sun visible in the images agrees with that computation
within 0.7° on every daytime frame, which is the check that the timestamps are
local time (UTC+2) and the site is right. A sky is shown when the real Sun
stands where it stood in the photograph, and two neighbours are crossfaded in
between. Morning and evening are separate branches: a sunrise is not a sunset
played backwards, and the photographs say so.

**Which way each sky faces.** Poly Haven turned every image so its Sun sits at
the same column. `sunU` records it, and the runtime rotates each sky so that
column faces the real Sun's azimuth.

**How bright each sky is.** Poly Haven normalises every photograph, so the
night is as bright as noon in the files. Brightness therefore comes from the
atmosphere model, never from the image, and from the one term of it the sky
must meet: the horizon band, which is also the fog (`solar.py`, "one scattering
column"). Each photograph's band from 0° to 4° is measured, and the sky is
scaled so that band is the model's horizon colour — so the dome meets the fog
without a seam at every hour, darkens as the model's horizon darkens, and keeps
the photograph's own gradient from horizon to zenith above it.

**The Sun is taken out of the photographs.** Crossfading two photographs whose
Suns stand at different elevations would show two Suns, neither where the light
comes from. The disc and its aureole are removed here and filled from the sky
around them, and the engine draws one Sun disc where the model puts the Sun
(`SceneSettings::skySunDirection`).

`python -m r1.skies` rebuilds everything from the pinned downloads: the
normalised skies in `assets/skies/`, `assets/skies/skies.json`, and the table
`scripts/sun_cycle.js` carries, which a test holds equal to the JSON.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np

from . import solar


ROOT = Path(__file__).resolve().parents[3]
GAME_ROOT = ROOT / "game"
SOURCE_ROOT = ROOT / "data" / "source-assets" / "skies"
SKY_ROOT = GAME_ROOT / "assets" / "skies"
TABLE_PATH = SKY_ROOT / "skies.json"
SCRIPT_PATH = GAME_ROOT / "scripts" / "sun_cycle.js"
USER_AGENT = "R1World-generator/1.0"

# Poly Haven's coordinates for the Qwantani series, and the offset of the
# timestamps it publishes: they are the camera's local time, South African
# Standard Time. Read as UTC, the "dawn" photograph would be taken with the
# Sun 25° up; read as SAST, it is taken 15 minutes before sunrise, and every
# daytime frame's visible Sun lands within 0.7° of the computed one.
SITE_LAT = -28.48533209827116
SITE_LON = 29.012639559934758
CAPTURE_UTC_OFFSET = timedelta(hours=2)


@dataclass(frozen=True)
class SkySource:
    name: str
    branch: str          # "rising", "setting", or "both" for night and noon
    date_taken: int      # Poly Haven's timestamp: local time written as Unix seconds
    md5: str

    @property
    def slug(self) -> str:
        return f"qwantani_{self.name}_puresky"

    @property
    def url(self) -> str:
        return f"https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/{self.slug}_1k.hdr"

    @property
    def download(self) -> Path:
        return SOURCE_ROOT / f"{self.slug}_1k.hdr"

    @property
    def texture(self) -> str:
        """The normalised sky the game loads, project-relative."""
        return f"assets/skies/qwantani_{self.name}.hdr"

    @property
    def captured(self) -> datetime:
        local = datetime.fromtimestamp(self.date_taken, timezone.utc).replace(tzinfo=None)
        return (local - CAPTURE_UTC_OFFSET).replace(tzinfo=timezone.utc)


# The "pure sky" variants: the same photographs with the ground replaced by a
# plain continuation of the sky, because the ground here is the world's own.
# The moonlit frames of the series are left out: a Moon fixed in the sky would
# be wrong on almost every night, and a moonless night is wrong on none.
SOURCES = (
    SkySource("night", "both", 1724352180, "e8691211295e505f77c8c3bdcf3055d9"),
    SkySource("dawn", "rising", 1724048880, "ad24f073518565d702b22b354b17db83"),
    SkySource("sunrise", "rising", 1724049780, "9803c0c384d5175805c28f83b8489338"),
    SkySource("morning", "rising", 1724141340, "0458a3c3dcb2c4b6e19d469a16611335"),
    SkySource("mid_morning", "rising", 1724148300, "798068e149c607fa1639cbbcd5f6b66a"),
    SkySource("noon", "both", 1724242680, "ecd559633c4a70e069ab3bdd2e9a58f8"),
    SkySource("afternoon", "setting", 1724249040, "82dcd1fd609c7235d34e7b8c24035834"),
    SkySource("late_afternoon", "setting", 1723997040, "80b4eae23e4984fc1e643f3bb147c6d1"),
    SkySource("sunset", "setting", 1724000940, "fe79b1b213166a70871eaddf7fce8c4e"),
    SkySource("dusk_1", "setting", 1724348820, "cb1f0fbae91740c0379ec1a5d646df74"),
    SkySource("dusk_2", "setting", 1724349960, "d400530e33f683654e70b4a087b6fe8d"),
)

# The horizon band each photograph is scaled by, in degrees of elevation.
HORIZON_BAND = (0.0, 4.0)

# The Sun disc's radiance for an overhead Sun through a clear column. Not the
# photographs' ratio: the noon frame's Sun is 600 000 times its sky, and the
# renderer's HDR target is half-float, which stops at 65 504. This is the
# brightest value that leaves the disc white after tonemapping at every hour
# the beam is not already red, with headroom under that ceiling.
SUN_DISC_RADIANCE = 20000.0

# Real angular radius of the Sun, in degrees.
SUN_ANGULAR_RADIUS = 0.265

# A photograph has a Sun to remove when its brightest spot outshines the sky
# five degrees away by this much. The daytime frames measure 135 to 14 000;
# dawn, dusk and night measure under 2, and their glow is sky, not Sun.
SUN_PRESENT_RATIO = 20.0

# Where the aureole ends: the first radius whose one-degree ring is no more
# than this multiple of the ring two degrees further out. That is where the
# circumsolar brightening has flattened into the sky's own gradient — 3° at
# sunrise, 7° at noon, measured on these files.
AUREOLE_FALLOFF = 1.25
AUREOLE_MIN_RADIUS = 1.5
AUREOLE_MAX_RADIUS = 12.0


# ── Radiance .hdr I/O ───────────────────────────────────────────────────────

def read_hdr(path: Path) -> np.ndarray:
    """Linear RGB float64, rows top to bottom, of a Radiance RGBE file."""
    data = Path(path).read_bytes()
    i = 0
    while True:
        j = data.index(b"\n", i)
        line = data[i:j]
        i = j + 1
        if line.startswith((b"-Y", b"+Y")):
            parts = line.split()
            height, width = int(parts[1]), int(parts[3])
            break
    rgbe = np.zeros((height, width, 4), np.uint8)
    for y in range(height):
        if data[i] != 2 or data[i + 1] != 2:
            raise ValueError(f"{path}: only run-length encoded scanlines are read")
        i += 4
        for channel in range(4):
            x = 0
            while x < width:
                count = data[i]
                i += 1
                if count > 128:
                    count -= 128
                    rgbe[y, x:x + count, channel] = data[i]
                    i += 1
                else:
                    rgbe[y, x:x + count, channel] = np.frombuffer(data[i:i + count], np.uint8)
                    i += count
                x += count
    exponent = rgbe[..., 3].astype(np.int32)
    scale = np.where(exponent > 0, np.ldexp(1.0, exponent - 136), 0.0)
    return rgbe[..., :3].astype(np.float64) * scale[..., None]


def _encode_channel(values: np.ndarray) -> bytes:
    out = bytearray()
    n = len(values)
    x = 0
    while x < n:
        run = 1
        while x + run < n and run < 127 and values[x + run] == values[x]:
            run += 1
        if run >= 4:
            out += bytes((128 + run, values[x]))
            x += run
            continue
        start = x
        while x < n and x - start < 128:
            if x + 3 < n and values[x] == values[x + 1] == values[x + 2] == values[x + 3]:
                break
            x += 1
        out.append(x - start)
        out += bytes(values[start:x])
    return bytes(out)


def write_hdr(path: Path, image: np.ndarray) -> None:
    """Radiance RGBE, run-length encoded; deterministic for a given image."""
    height, width, _ = image.shape
    peak = image.max(axis=2)
    mantissa, exponent = np.frexp(peak)
    rgbe = np.zeros((height, width, 4), np.uint8)
    lit = peak > 1e-32
    scale = np.zeros_like(peak)
    scale[lit] = mantissa[lit] * 256.0 / peak[lit]
    rgbe[..., :3] = np.clip(np.floor(image * scale[..., None]), 0, 255).astype(np.uint8)
    rgbe[..., 3] = np.where(lit, exponent + 128, 0).astype(np.uint8)
    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode()
    body = bytearray()
    for y in range(height):
        body += bytes((2, 2, width >> 8, width & 255))
        for channel in range(4):
            body += _encode_channel(rgbe[y, :, channel].tobytes())
    path.write_bytes(header + bytes(body))


# ── measurement ─────────────────────────────────────────────────────────────

def _directions(height: int, width: int) -> tuple[np.ndarray, np.ndarray]:
    """Unit vector and solid-angle weight of each texel of an equirect map."""
    lat = (0.5 - (np.arange(height) + 0.5) / height) * math.pi
    lon = (np.arange(width) + 0.5) / width * 2.0 * math.pi
    lat, lon = np.meshgrid(lat, lon, indexing="ij")
    vectors = np.stack([np.cos(lat) * np.cos(lon), np.sin(lat), np.cos(lat) * np.sin(lon)], -1)
    return vectors, np.cos(lat)


def luminance(image: np.ndarray) -> np.ndarray:
    return image @ np.array([0.2126, 0.7152, 0.0722])


def sky_luminance(image: np.ndarray) -> float:
    """Solid-angle mean luminance of the upper hemisphere."""
    vectors, weights = _directions(*image.shape[:2])
    upper = vectors[..., 1] > 0.0
    return float((luminance(image)[upper] * weights[upper]).sum() / weights[upper].sum())


def horizon_luminance(image: np.ndarray) -> float:
    """Solid-angle mean luminance of the band just above the horizon."""
    vectors, weights = _directions(*image.shape[:2])
    elevation = np.degrees(np.arcsin(vectors[..., 1]))
    band = (elevation >= HORIZON_BAND[0]) & (elevation < HORIZON_BAND[1])
    return float((luminance(image)[band] * weights[band]).sum() / weights[band].sum())


def find_sun(image: np.ndarray) -> tuple[int, int]:
    """Row and column of the brightest region of the upper sky."""
    lum = luminance(image)
    height, width = lum.shape
    k = 4
    padded = np.pad(lum, ((k, k), (k, k)), mode="wrap")
    summed = padded.cumsum(0).cumsum(1)
    blurred = (summed[2 * k:, 2 * k:] - summed[:-2 * k, 2 * k:]
               - summed[2 * k:, :-2 * k] + summed[:-2 * k, :-2 * k])[:height, :width]
    blurred[height // 2:, :] = -1.0
    row, column = np.unravel_index(int(np.argmax(blurred)), blurred.shape)
    return int(row), int(column)


# How much brighter than the sky model around it a pixel may be before it is
# taken for a lens streak and pulled down: the starburst of the camera's
# aperture reaches well past the aureole, a pixel wide, in six directions.
STREAK_FACTOR = 1.05


def remove_sun(image: np.ndarray, row: int, column: int) -> tuple[np.ndarray, float]:
    """The sky with the Sun, its aureole and its lens streaks taken out.

    Nothing is removed from a photograph with no Sun in it (`SUN_PRESENT_RATIO`).
    Three steps, out to `reach`, two and a half times the radius where the
    aureole flattens (`AUREOLE_FALLOFF`):

    - **The aureole is divided out.** The median of each half-degree ring is the
      circumsolar brightening at that distance — a median, so a streak a pixel
      wide never enters it — and dividing by it, relative to the ring at
      `reach`, flattens the glow while keeping whatever structure the sky has
      around the Sun: the clouds of a sunset survive, their halo does not.
    - **Streaks are pulled down.** On the flattened sky, a pixel above the
      median of its 15° sector by more than `STREAK_FACTOR` is the aperture's
      starburst, and is brought back to it.
    - **The disc is filled** from the flattened ring just outside the radius,
      sector by sector, so a horizon glow on one side of a low Sun runs through
      the hole rather than being averaged away.

    Returns the image and the radius, in degrees (0 when there was no Sun).
    """
    vectors, _ = _directions(*image.shape[:2])
    centre = vectors[row, column]
    angle = np.degrees(np.arccos(np.clip(vectors @ centre, -1.0, 1.0)))
    lum = luminance(image)

    def ring(inner: float) -> float:
        return float(np.mean(lum[(angle >= inner) & (angle < inner + 1.0)]))

    if float(lum[angle < 1.0].max()) < SUN_PRESENT_RATIO * ring(5.0):
        return image.copy(), 0.0

    radius = AUREOLE_MIN_RADIUS
    while radius < AUREOLE_MAX_RADIUS and ring(radius) > AUREOLE_FALLOFF * ring(radius + 2.0):
        radius += 0.5
    reach = min(2.5 * radius + 2.0, 24.0)

    # Position angle around the Sun, in a tangent frame at its centre.
    up = np.array([0.0, 1.0, 0.0])
    east = np.cross(up, centre)
    east /= np.linalg.norm(east)
    north = np.cross(centre, east)
    theta = np.arctan2(vectors @ north, vectors @ east)

    sectors, step = 24, 0.5
    bins = int(math.ceil((reach + 1.0) / step))
    near = angle < reach + 1.0
    sector_of = ((theta + math.pi) / (2.0 * math.pi) * sectors).astype(int) % sectors
    bin_of = np.minimum((angle / step).astype(int), bins - 1)
    first = int(math.ceil(radius / step))
    last = bins - 1

    def radial_lookup(values: np.ndarray) -> np.ndarray:
        position = np.clip(angle / step - 0.5, 0.0, bins - 1.0)
        low = np.floor(position).astype(int)
        high = np.minimum(low + 1, bins - 1)
        t = position - low
        return values[low] * (1.0 - t) + values[high] * t

    # The aureole: each ring's median against the ring at `reach`.
    profile = np.ones(bins)
    for b in range(first, bins):
        profile[b] = np.median(lum[near & (bin_of == b)])
    profile[:first] = profile[first]
    aureole = np.maximum(profile / profile[last], 1.0)
    out = image.copy()
    region = angle < reach
    out[region] = image[region] / radial_lookup(aureole)[region][..., None]

    # The sector model of the flattened sky, smoothed so it has no facets.
    flat = luminance(out)
    model = np.zeros((bins, sectors, 3))
    for b in range(first, bins):
        in_bin = near & (bin_of == b)
        whole = np.median(out[in_bin], axis=0)
        for sct in range(sectors):
            chosen = in_bin & (sector_of == sct)
            model[b, sct] = np.median(out[chosen], axis=0) if chosen.sum() >= 3 else whole
    model[:first] = model[first]
    model = (np.roll(model, 1, axis=1) + 2.0 * model + np.roll(model, -1, axis=1)) / 4.0
    model[1:-1] = (model[:-2] + 2.0 * model[1:-1] + model[2:]) / 4.0
    model[:first] = model[first]

    radial = np.clip(angle / step - 0.5, 0.0, bins - 1.0)
    r0 = np.floor(radial).astype(int)
    r1 = np.minimum(r0 + 1, bins - 1)
    tr = (radial - r0)[..., None]
    around = (theta + math.pi) / (2.0 * math.pi) * sectors - 0.5
    s0 = np.floor(around).astype(int) % sectors
    s1 = (s0 + 1) % sectors
    ts = (around - np.floor(around))[..., None]
    smooth = ((model[r0, s0] * (1.0 - ts) + model[r0, s1] * ts) * (1.0 - tr)
              + (model[r1, s0] * (1.0 - ts) + model[r1, s1] * ts) * tr)

    halo = (angle >= radius) & region
    limit = STREAK_FACTOR * luminance(smooth)
    excess = halo & (flat > limit) & (limit > 0.0)
    out[excess] *= (limit[excess] / flat[excess])[..., None]

    inside = angle < radius
    out[inside] = smooth[inside]
    return out, radius


def clean_sky(image: np.ndarray) -> tuple[np.ndarray, float, int]:
    """The Sun removed; (image, radius, Sun column).

    The lower hemisphere keeps the soft reflection a low Sun leaves in a "pure
    sky": it is under the terrain wherever the world has one, and it is too
    diffuse to pass `SUN_PRESENT_RATIO` anyway.
    """
    row, column = find_sun(image)
    cleaned, radius = remove_sun(image, row, column)
    return cleaned, radius, column


def capture_sun(source: SkySource) -> tuple[float, float]:
    """(elevation, azimuth) of the real Sun when the photograph was taken."""
    azimuth, elevation, *_ = solar.sun_angles(SITE_LON, SITE_LAT, source.captured)
    return elevation, azimuth


# ── the runtime half, which `sun_cycle.js` ports ────────────────────────────

def load_table(path: Path = TABLE_PATH) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def branch_frames(frames: list[dict], rising: bool) -> list[int]:
    """Indices of one branch's skies, lowest Sun first."""
    wanted = ("rising", "both") if rising else ("setting", "both")
    chosen = [i for i, frame in enumerate(frames) if frame["branch"] in wanted]
    return sorted(chosen, key=lambda i: frames[i]["elevation"])


def select_skies(frames: list[dict], elevation: float, azimuth: float) -> tuple[int, int, float]:
    """The two skies to show and how far the first has faded into the second.

    Morning is the eastern half of the sky: before local noon the Sun's
    azimuth lies between 0° and 180° at every latitude.
    """
    order = branch_frames(frames, 0.0 < azimuth < 180.0)
    if elevation <= frames[order[0]]["elevation"]:
        return order[0], order[0], 0.0
    for low, high in zip(order, order[1:]):
        if elevation < frames[high]["elevation"]:
            span = frames[high]["elevation"] - frames[low]["elevation"]
            t = (elevation - frames[low]["elevation"]) / span
            return low, high, t * t * (3.0 - 2.0 * t)
    return order[-1], order[-1], 0.0


def sky_rotation(frame: dict, azimuth: float) -> float:
    """Radians turning a sky so its photographed Sun faces `azimuth`.

    The skybox shader looks a direction up at u = atan2(z, x) / 2π + 0.5 after
    turning it by -rotation, and a direction toward azimuth `a` is
    (sin a, ·, -cos a) in the engine frame (x east, z south).
    """
    a = math.radians(azimuth)
    real_u = math.atan2(-math.cos(a), math.sin(a)) / (2.0 * math.pi) + 0.5
    turn = (real_u - frame["sunU"]) * 2.0 * math.pi
    return turn % (2.0 * math.pi)


@dataclass(frozen=True)
class SkyState:
    texture: str
    blend_texture: str
    blend: float
    rotation: float
    blend_rotation: float
    exposure: float
    sun_direction: tuple[float, float, float]
    sun_color: tuple[float, float, float]


def sky_state(sun: solar.SunState, table: dict | None = None) -> SkyState:
    """Everything the skybox needs at the instant `sun` describes."""
    table = table if table is not None else load_table()
    frames = table["frames"]
    a, b, blend = select_skies(frames, sun.elevation, sun.azimuth)
    band = frames[a]["horizon"] * (1.0 - blend) + frames[b]["horizon"] * blend
    horizon = sun.horizon_color
    exposure = (0.2126 * horizon[0] + 0.7152 * horizon[1] + 0.0722 * horizon[2]) / band
    radiance = table["sunDiscRadiance"] * sun.intensity / solar.PEAK_INTENSITY
    return SkyState(
        texture=frames[a]["texture"],
        blend_texture=frames[b]["texture"],
        blend=blend,
        rotation=sky_rotation(frames[a], sun.azimuth),
        blend_rotation=sky_rotation(frames[b], sun.azimuth),
        exposure=exposure,
        sun_direction=tuple(-c for c in sun.direction),
        sun_color=tuple(radiance * c for c in sun.color),
    )


# ── rebuilding ──────────────────────────────────────────────────────────────

def _fetch(source: SkySource) -> None:
    if source.download.exists() and _md5(source.download) == source.md5:
        return
    SOURCE_ROOT.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(source.url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        payload = response.read()
    if hashlib.md5(payload).hexdigest() != source.md5:
        raise RuntimeError(f"{source.slug}: download does not match its pinned md5")
    source.download.write_bytes(payload)


def _md5(path: Path) -> str:
    return hashlib.md5(path.read_bytes()).hexdigest()


def build_table() -> dict:
    frames = []
    for source in SOURCES:
        _fetch(source)
        image = read_hdr(source.download)
        cleaned, radius, column = clean_sky(image)
        SKY_ROOT.mkdir(parents=True, exist_ok=True)
        write_hdr(GAME_ROOT / source.texture, cleaned)
        elevation, azimuth = capture_sun(source)
        frames.append({
            "name": source.name,
            "texture": source.texture,
            "branch": source.branch,
            "elevation": round(elevation, 4),
            "sunU": round((column + 0.5) / image.shape[1], 5),
            "horizon": round(horizon_luminance(cleaned), 5),
            "luminance": round(sky_luminance(cleaned), 5),
            "sunRemovedDegrees": radius,
            "captured": source.captured.isoformat(),
            "capturedAzimuth": round(azimuth, 3),
        })
    return {
        "schema": 1,
        "source": "Poly Haven, Qwantani series (CC0), photographed by Greg Zaal",
        "site": [SITE_LAT, SITE_LON],
        "sunDiscRadiance": SUN_DISC_RADIANCE,
        "sunAngularRadius": SUN_ANGULAR_RADIUS,
        "frames": frames,
    }


# `sun_cycle.js` cannot read files, so it carries the table as a literal
# between these markers; `write_script_table` rewrites it and a test holds it
# equal to `skies.json`.
SCRIPT_BEGIN = "// BEGIN SKIES — generated by `python -m r1.skies`, do not edit by hand."
SCRIPT_END = "// END SKIES"


def script_table(table: dict) -> str:
    runtime = {
        "sunDiscRadiance": table["sunDiscRadiance"],
        "sunAngularRadius": table["sunAngularRadius"],
        "frames": [
            {key: frame[key] for key in ("name", "texture", "branch", "elevation", "sunU", "horizon")}
            for frame in table["frames"]
        ],
    }
    return "const SKIES = " + json.dumps(runtime, indent=4) + ";"


def write_script_table(table: dict, path: Path = SCRIPT_PATH) -> None:
    source = path.read_text(encoding="utf-8")
    pattern = re.compile(re.escape(SCRIPT_BEGIN) + r".*?" + re.escape(SCRIPT_END), re.S)
    if not pattern.search(source):
        raise RuntimeError(f"{path}: the SKIES markers are missing")
    block = SCRIPT_BEGIN + "\n" + script_table(table) + "\n" + SCRIPT_END
    path.write_text(pattern.sub(lambda _match: block, source), encoding="utf-8")


def main() -> int:
    table = build_table()
    TABLE_PATH.write_text(json.dumps(table, indent=2) + "\n", encoding="utf-8")
    write_script_table(table)
    for frame in table["frames"]:
        print(f"{frame['name']:15s} elevation {frame['elevation']:7.2f}  "
              f"sun removed within {frame['sunRemovedDegrees']:4.1f}  "
              f"horizon {frame['horizon']:.4f}  sky {frame['luminance']:.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
