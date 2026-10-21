"""The sounds of the arrival, synthesised by the project.

    python -m r1.arrival_sounds       (from game/tools; standard library only)

The fall is a sound before it is a picture: at 200 km/h the air roars past
the ears, and the canopy opening is a rustle, then a crack as the wing bites,
then quiet -- the fabric's flutter and the wind of 9 m/s. No CC0 recording in
the project's sources covers any of it, so these are built from noise the way
`combat_sounds` builds a gunshot (CLAUDE.md rule 1: nothing on disk is better).

Everything is seeded: the same files come out on every machine. Mono, 16-bit,
44.1 kHz WAV in `assets/audio/arrival/`. The two winds are seamless loops; the
game sets their volume from the speed of the fall (native/world.cpp).
"""
from __future__ import annotations

import json
import math
import random
from pathlib import Path

from .combat_sounds import RATE, add, bandpass, envelope, highpass, lowpass, mix, noise, sweep, write

GAME = Path(__file__).resolve().parents[2]
OUT = GAME / "assets/audio/arrival"


def drift(n: int, seed: int, rate: float, depth: float) -> list[float]:
    """A slow random gain around 1: gusts at about `rate` Hz."""
    rng = random.Random(seed)
    knots = int(n / RATE * rate) + 3
    points = [rng.uniform(-1, 1) for _ in range(knots)]
    out = []
    for i in range(n):
        t = i / RATE * rate
        k = int(t)
        f = t - k
        f = f * f * (3 - 2 * f)
        out.append(1 + depth * (points[k] + (points[k + 1] - points[k]) * f))
    return out


def seamless(x: list[float], seconds: float, overlap: float) -> list[float]:
    """The first `seconds` of `x`, its extra tail crossfaded into its head
    with equal power, so that the loop has no seam."""
    n, m = int(seconds * RATE), int(overlap * RATE)
    out = x[:n]
    for i in range(m):
        a = math.sin(.5 * math.pi * i / m)
        out[i] = out[i] * a + x[n + i] * math.cos(.5 * math.pi * i / m)
    return out


def normalised(x: list[float], peak: float) -> list[float]:
    x = [math.tanh(1.3 * v) for v in x]
    top = max(1e-9, max(abs(v) for v in x))
    return [v * peak / top for v in x]


def wind_freefall() -> list[float]:
    """The roar of free fall: broadband air, a low rumble, a little hiss,
    gusting slowly and buffeting fast."""
    seconds, overlap = 8., 1.
    n = int((seconds + overlap) * RATE)
    roar = bandpass(noise(n, 101), 140, 1600)
    rumble = lowpass(noise(n, 102), 110)
    hiss = bandpass(noise(n, 103), 2500, 6000)
    gust, buffet = drift(n, 104, .45, .28), drift(n, 105, 11., .12)
    out = [gust[i] * buffet[i] * (roar[i] + 1.6 * rumble[i] + .18 * hiss[i]) for i in range(n)]
    return normalised(seamless(out, seconds, overlap), .62)


def canopy_flutter() -> list[float]:
    """Under the canopy: a light wind and the fabric's trailing edge
    flapping a few times a second."""
    seconds, overlap = 6., 1.
    n = int((seconds + overlap) * RATE)
    air = bandpass(noise(n, 201), 90, 700)
    cloth = bandpass(noise(n, 202), 400, 2200)
    flap = [.55 + .45 * math.sin(2 * math.pi * 7.3 * i / RATE + 1.7 * math.sin(2 * math.pi * .4 * i / RATE)) ** 4
            for i in range(n)]
    gust = drift(n, 203, .3, .3)
    out = [gust[i] * (air[i] + .5 * cloth[i] * flap[i]) for i in range(n)]
    return normalised(seamless(out, seconds, overlap), .4)


def canopy_open() -> list[float]:
    """The pull: the bag leaves and the lines unstow in a rising rustle, the
    wing cracks open, then flutters as it settles."""
    seconds = 2.2
    n = int(seconds * RATE)
    rustle_raw = bandpass(noise(n, 301), 500, 3500)
    rate = [18 + 22 * min(1., i / (.7 * RATE)) for i in range(n)]
    phase, rustle_mod = 0., []
    for r in rate:
        phase += 2 * math.pi * r / RATE
        rustle_mod.append(.5 + .5 * math.sin(phase))
    rise = [min(1., (i / RATE) / .7) ** 2 * (1 if i / RATE < .78 else math.exp(-(i / RATE - .78) / .05)) for i in range(n)]
    rustle = [rustle_raw[i] * rustle_mod[i] * rise[i] for i in range(n)]
    crack = mix((1.0, sweep(n, 120, 55, .06), envelope(n, .11, .004, .78)),
                (.7, lowpass(noise(n, 302), 1800), envelope(n, .05, .002, .78)),
                (.35, highpass(noise(n, 303), 2500), envelope(n, .012, .0005, .78)))
    settle_raw = bandpass(noise(n, 304), 300, 1800)
    settle = [settle_raw[i] * (.5 + .5 * math.sin(2 * math.pi * 9 * i / RATE) ** 2)
              * (0 if i / RATE < .82 else math.exp(-(i / RATE - .82) / .5)) for i in range(n)]
    out = add(add([.55 * v for v in rustle], crack), [.35 * v for v in settle])
    out = normalised(out, .9)
    fade = int(.05 * RATE)
    for i in range(fade):
        out[n - 1 - i] *= i / fade
    return out


SOUNDS = {"wind_freefall": wind_freefall, "canopy_open": canopy_open, "canopy_flutter": canopy_flutter}
LOOPS = {"wind_freefall", "canopy_flutter"}


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = {"generatedBy": "game/tools/r1/arrival_sounds.py", "license": "CC0-1.0 (project-made)",
                "rate": RATE, "sounds": {}}
    for name, make in SOUNDS.items():
        samples = make()
        write(OUT / f"{name}.wav", samples)
        manifest["sounds"][name] = {"file": f"assets/audio/arrival/{name}.wav",
                                    "seconds": round(len(samples) / RATE, 3), "loop": name in LOOPS}
        print(f"{name:15s} {len(samples) / RATE:5.2f} s")
    (OUT / "sounds.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
