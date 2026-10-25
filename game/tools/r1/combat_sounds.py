"""The sounds of a fight, synthesised by the project.

    python -m r1.combat_sounds        (from game/tools; standard library only)

No CC0 recording in the project's sources covers a pistol, a punch or a body
hitting a bonnet, and a stylised kit's blaster would lower the bar the
photographed world sets (CLAUDE.md rule 1). So these are built from noise and
decaying partials, the way a sound designer layers a gunshot: a crack, the
body of the blast, the low boom, and the street answering it.

Everything is seeded: the same files come out on every machine. Mono, 16-bit,
44.1 kHz WAV in `assets/audio/combat/`, read by native/combat_kit.hpp.
"""
from __future__ import annotations

import json
import math
import random
import struct
import wave
from pathlib import Path

GAME = Path(__file__).resolve().parents[2]
OUT = GAME / "assets/audio/combat"
RATE = 44100


def silence(seconds: float) -> list[float]:
    return [0.0] * int(seconds * RATE)


def noise(n: int, seed: int) -> list[float]:
    rng = random.Random(seed)
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


def lowpass(x: list[float], cutoff: float) -> list[float]:
    a = math.exp(-2 * math.pi * cutoff / RATE)
    y, out = 0.0, []
    for v in x:
        y = (1 - a) * v + a * y
        out.append(y)
    return out


def highpass(x: list[float], cutoff: float) -> list[float]:
    low = lowpass(x, cutoff)
    return [v - l for v, l in zip(x, low)]


def bandpass(x: list[float], low: float, high: float) -> list[float]:
    return lowpass(highpass(x, low), high)


def envelope(n: int, decay: float, attack: float = 0.0005, start: float = 0.0) -> list[float]:
    out = []
    for i in range(n):
        t = i / RATE - start
        if t < 0:
            out.append(0.0)
        elif t < attack:
            out.append(t / attack)
        else:
            out.append(math.exp(-(t - attack) / decay))
    return out


def sweep(n: int, f0: float, f1: float, glide: float, phase: float = 0.0) -> list[float]:
    """A sine gliding from f0 toward f1 with time constant `glide`."""
    out, p = [], phase
    for i in range(n):
        t = i / RATE
        f = f1 + (f0 - f1) * math.exp(-t / glide)
        p += 2 * math.pi * f / RATE
        out.append(math.sin(p))
    return out


def mix(*layers: tuple[float, list[float], list[float]]) -> list[float]:
    n = max(len(s) for _, s, _ in layers)
    out = [0.0] * n
    for gain, signal, env in layers:
        for i in range(min(len(signal), len(env))):
            out[i] += gain * signal[i] * env[i]
    return out


def delayed(x: list[float], seconds: float, gain: float, cutoff: float) -> list[float]:
    d = int(seconds * RATE)
    return [0.0] * d + [gain * v for v in lowpass(x, cutoff)]


def add(a: list[float], b: list[float]) -> list[float]:
    n = max(len(a), len(b))
    return [(a[i] if i < len(a) else 0.0) + (b[i] if i < len(b) else 0.0) for i in range(n)]


def finish(x: list[float], peak: float, seconds: float) -> list[float]:
    n = int(seconds * RATE)
    x = (x + [0.0] * n)[:n]
    # A soft clip shapes the transient the way a recorder's limiter does.
    x = [math.tanh(1.6 * v) for v in x]
    top = max(1e-9, max(abs(v) for v in x))
    fade = int(0.01 * RATE)
    out = [v * peak / top for v in x]
    for i in range(fade):
        out[n - 1 - i] *= i / fade
    return out


def pistol_shot() -> list[float]:
    n = int(1.1 * RATE)
    crack = highpass(noise(n, 11), 1800)
    body = lowpass(noise(n, 12), 1400)
    blast = mix((1.0, crack, envelope(n, 0.005, 0.0002)),
                (0.9, body, envelope(n, 0.028, 0.0004)),
                (1.0, sweep(n, 160, 48, 0.035), envelope(n, 0.075, 0.001)))
    tail = mix((0.22, lowpass(noise(n, 13), 850), envelope(n, 0.32, 0.012, 0.015)))
    out = add(blast, tail)
    # The facades across the street, then the next block.
    out = add(out, delayed(blast, 0.105, 0.28, 2400))
    out = add(out, delayed(blast, 0.23, 0.14, 1500))
    out = add(out, delayed(blast, 0.41, 0.06, 900))
    return finish(out, 0.97, 1.1)


def punch_hit() -> list[float]:
    n = int(0.25 * RATE)
    return finish(mix((1.0, sweep(n, 105, 52, 0.03), envelope(n, 0.05, 0.001)),
                      (0.65, lowpass(noise(n, 21), 2600), envelope(n, 0.009, 0.0003)),
                      (0.25, bandpass(noise(n, 22), 300, 1200), envelope(n, 0.04, 0.002))), 0.9, 0.25)


def swing() -> list[float]:
    n = int(0.26 * RATE)
    raw = noise(n, 31)
    # A whoosh: noise whose band climbs as the fist passes.
    first, second = bandpass(raw, 350, 900), bandpass(raw, 900, 2200)
    env = [math.sin(math.pi * i / n) ** 2 for i in range(n)]
    out = [env[i] * ((1 - i / n) * first[i] + (i / n) * second[i]) for i in range(n)]
    return finish(out, 0.45, 0.26)


def body_fall() -> list[float]:
    n = int(0.7 * RATE)
    out = mix((1.0, sweep(n, 78, 42, 0.05), envelope(n, 0.08, 0.002)),
              (0.5, sweep(n, 95, 55, 0.04, 1.3), envelope(n, 0.06, 0.002, 0.13)),
              (0.35, lowpass(noise(n, 41), 1600), envelope(n, 0.13, 0.004)),
              (0.2, lowpass(noise(n, 42), 1100), envelope(n, 0.09, 0.004, 0.14)))
    return finish(out, 0.85, 0.7)


def car_hit() -> list[float]:
    n = int(0.6 * RATE)
    layers = [(1.0, sweep(n, 90, 55, 0.05), envelope(n, 0.07, 0.001)),
              (0.45, bandpass(noise(n, 51), 1200, 4000), envelope(n, 0.03, 0.0005))]
    # Sheet metal: a few inharmonic modes ringing out.
    for k, (f, decay, gain) in enumerate([(312, 0.26, 0.32), (527, 0.2, 0.26), (889, 0.15, 0.2), (1347, 0.1, 0.14)]):
        layers.append((gain, sweep(n, f * 1.02, f, 0.02, k * 0.7), envelope(n, decay, 0.001)))
    return finish(mix(*layers), 0.92, 0.6)


def dry_fire() -> list[float]:
    n = int(0.09 * RATE)
    return finish(mix((1.0, highpass(noise(n, 61), 3000), envelope(n, 0.0025, 0.0001)),
                      (0.6, sweep(n, 2300, 2100, 0.01), envelope(n, 0.005, 0.0002))), 0.6, 0.09)


def reload() -> list[float]:
    n = int(0.75 * RATE)
    click = lambda seed, at, gain: mix((gain, highpass(noise(n, seed), 2500), envelope(n, 0.003, 0.0001, at)),
                                       (gain * 0.5, sweep(n, 1900, 1700, 0.01), envelope(n, 0.006, 0.0002, at)))
    out = add(click(71, 0.05, 0.7), click(72, 0.5, 1.0))
    out = add(out, mix((0.18, bandpass(noise(n, 73), 800, 3000), envelope(n, 0.05, 0.03, 0.24))))
    out = add(out, mix((0.9, sweep(n, 260, 180, 0.02), envelope(n, 0.02, 0.001, 0.52))))
    return finish(out, 0.7, 0.75)


def bullet_impact() -> list[float]:
    n = int(0.22 * RATE)
    return finish(mix((1.0, highpass(noise(n, 81), 2200), envelope(n, 0.004, 0.0001)),
                      (0.45, lowpass(noise(n, 82), 3200), envelope(n, 0.045, 0.002)),
                      (0.3, sweep(n, 180, 90, 0.02), envelope(n, 0.03, 0.001))), 0.75, 0.22)


def rocket_launch() -> list[float]:
    n = int(0.9 * RATE)
    return finish(mix((1.0, bandpass(noise(n, 91), 90, 3400), envelope(n, .24, .008)),
                      (.6, sweep(n, 240, 48, .16), envelope(n, .2, .002)),
                      (.45, highpass(noise(n, 92), 1800), envelope(n, .04))), .92, .9)


def rocket_explosion() -> list[float]:
    n = int(3.2 * RATE)
    return finish(mix((1.0, lowpass(noise(n, 101), 1300), envelope(n, .5, .002)),
                      (.85, sweep(n, 105, 28, .22), envelope(n, .6, .003)),
                      (.7, highpass(noise(n, 102), 1900), envelope(n, .09)),
                      (.45, bandpass(noise(n, 103), 70, 600), envelope(n, .8, .06, .14)),
                      (.22, bandpass(noise(n, 104), 120, 1800), envelope(n, .6, .04, .38))), .98, 3.2)


SOUNDS = {
    "pistol_shot": pistol_shot, "punch_hit": punch_hit, "swing": swing, "body_fall": body_fall,
    "car_hit": car_hit, "dry_fire": dry_fire, "reload": reload, "bullet_impact": bullet_impact,
    "rocket_launch": rocket_launch, "rocket_explosion": rocket_explosion,
}


def write(path: Path, samples: list[float]) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, v)) * 32767)) for v in samples))


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = {"generatedBy": "game/tools/r1/combat_sounds.py", "license": "CC0-1.0 (project-made)",
                "rate": RATE, "sounds": {}}
    for name, make in SOUNDS.items():
        samples = make()
        write(OUT / f"{name}.wav", samples)
        manifest["sounds"][name] = {"file": f"assets/audio/combat/{name}.wav",
                                    "seconds": round(len(samples) / RATE, 3)}
        print(f"{name:14s} {len(samples) / RATE:5.2f} s")
    (OUT / "sounds.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
