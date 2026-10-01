"""Photograph the reference viewpoints and lay them beside the previous run.

A visual regression is the one no test sees (CLAUDE.md, rule 1): only an eye
on the picture does. This takes the same six pictures every time -- the same
place, the same camera, the same solar hour, a clear sky -- so that the eye
compares a build with the last one rather than with its memory of it.

    python tools\\gallery.py                 # every viewpoint
    python tools\\gallery.py kyoto theix     # some of them
    python tools\\gallery.py --offline       # proxies closed: the cache only

Each run is a folder of `generated/gallery/`; `generated/gallery/index.html`
shows every viewpoint, latest run beside the one before.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import html
import json
import math
import os
from pathlib import Path
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
GALLERY = GAME / "generated" / "gallery"
# One day for every picture, so the Sun's height is the place's and not the
# season's: near the equinox no hemisphere is favoured.
DAY = datetime(2026, 9, 21, tzinfo=timezone.utc)
SOLAR_HOUR = 10.5  # morning light: shadows long enough to read the relief


def enu(origin: tuple[float, float], point: tuple[float, float]) -> tuple[float, float]:
    """Engine metres (x east, z south) of `point` from `origin`, both lon/lat."""
    lon0, lat0 = origin
    lon, lat = point
    east = math.radians(lon - lon0) * 6378137.0 * math.cos(math.radians(lat0))
    north = math.radians(lat - lat0) * 6378137.0
    return east, -north


def toward(origin, target, height, back, eye):
    """A camera `back` metres behind the spawn (ahead when negative), at `eye`,
    looking at `target`."""
    x, z = enu(origin, target)
    d = math.hypot(x, z)
    pos = (-x / d * back, eye, -z / d * back)
    return pos, (x, height, z)


RIVOLI_SPAWN = (2.34920, 48.85823)   # rue de Rivoli by Châtelet (way 193100626)
RIVOLI_AHEAD = (2.34750, 48.85877)   # 140 m up the street, west-north-west
LIBERTY_SPAWN = (-74.01606, 40.70236) # Battery Park, a footway south-east of Castle Clinton (way 136172862)
LIBERTY = (-74.04450, 40.68925)      # the statue, about 2.7 km away
THEIX_SPAWN = (-2.65521, 47.62936)   # the store's car park (way 33757988)

VIEWS = [
    {"name": "paris", "title": "Paris, la rue de Rivoli à hauteur d'homme",
     "spawn": RIVOLI_SPAWN,
     "camera": toward(RIVOLI_SPAWN, RIVOLI_AHEAD, 8.0, 4.0, 1.7)},
    {"name": "liberty", "title": "La statue de la Liberté, de très loin, au sol (Battery Park)",
     "spawn": LIBERTY_SPAWN,
     # 130 m on from the spawn, at the water's edge past the park's trees.
     "camera": toward(LIBERTY_SPAWN, LIBERTY, 55.0, -130.0, 1.7)},
    # The médina of Tunis has few buildings in OSM; Sousse's is mapped.
    {"name": "sousse", "title": "Sousse, la médina vue du dessus",
     "spawn": (10.6375, 35.8270),
     "camera": ((0.0, 320.0, 170.0), (0.0, 0.0, -40.0))},
    {"name": "kyoto", "title": "Kyoto, Higashiyama vers les collines",
     "spawn": (135.7787, 34.9989),
     "camera": ((-140.0, 30.0, 60.0), (80.0, 20.0, -30.0))},
    {"name": "vannes", "title": "Vannes, le port depuis la place Gambetta",
     "spawn": (-2.75795, 47.65345),
     "camera": ((0.0, 9.0, -20.0), (0.0, 0.0, 220.0))},
    {"name": "theix", "title": "Theix, la station, le parking et le Carrefour Market",
     "spawn": THEIX_SPAWN,
     # From behind the fuel station (node 966271160), looking north-west:
     # the station in front, the car park, the store at the back.
     "camera": ((75.0, 25.0, 75.0), (0.0, 0.0, -5.0))},
]


def instant(lon: float) -> float:
    """Unix seconds of SOLAR_HOUR local mean solar time on DAY at `lon`."""
    return DAY.timestamp() + (SOLAR_HOUR - lon / 15.0) * 3600.0


def vec(v) -> str:
    return ",".join(f"{c:.3f}" for c in v)


def git_head() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=GAME, capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "nogit"


def shoot(view: dict, folder: Path, env: dict) -> dict:
    lon, lat = view["spawn"]
    pos, look = view["camera"]
    png = folder / f"{view['name']}.png"
    args = [sys.executable, str(GAME / "tools" / "play_world.py"), "--smoke",
            "--spawn", str(lon), str(lat), "--screenshot", str(png),
            "--camera-pos", vec(pos), "--camera-look", vec(look), "--after-frames", "12",
            "--at", f"{instant(lon):.0f}"]
    try:
        run = subprocess.run(args, cwd=GAME, env=env, capture_output=True, text=True, timeout=600)
        log = run.stdout.strip().splitlines()[-1:] if run.stdout else []
        ok = run.returncode == 0 and png.exists()
        reason = "" if ok else f"exit {run.returncode}"
        # A picture of ground whose streets never came is not the place: the
        # game says so in its log, and so does the gallery.
        if ok and log and log[0].startswith("Logs:"):
            game_log = Path(log[0][5:].strip()) / "game.log"
            text = game_log.read_text(encoding="utf-8", errors="replace") if game_log.exists() else ""
            for line in text.splitlines():
                if "[World capture]" in line:
                    ok, reason = False, line.split("[World capture]", 1)[1].strip()
    except subprocess.TimeoutExpired:
        ok, log, reason = False, [], "no picture after 10 minutes"
    print(f"{view['name']:8} {'ok' if ok else 'FAILED: ' + reason}  {' '.join(log)}")
    return {"name": view["name"], "ok": ok, "reason": reason, "log": " ".join(log)}


def runs() -> list[Path]:
    return sorted((p for p in GALLERY.iterdir() if (p / "run.json").exists()), key=lambda p: p.name)


def page() -> None:
    """index.html: every viewpoint, the latest picture beside the one before."""
    history = runs()
    meta = {p: json.loads((p / "run.json").read_text(encoding="utf-8")) for p in history}
    rows = []
    for view in VIEWS:
        shots = [p for p in history if (p / f"{view['name']}.png").exists()]
        latest = shots[-1] if shots else None
        before = shots[-2] if len(shots) > 1 else None

        def figure(run: Path | None, label: str) -> str:
            if run is None:
                return f'<figure class="empty"><figcaption>{label} : aucune image</figcaption></figure>'
            m = meta[run]
            src = f"{run.name}/{view['name']}.png"
            return (f'<figure><a href="{src}"><img src="{src}" loading="lazy" alt=""></a>'
                    f'<figcaption>{label} · {html.escape(m["when"])} · <code>{html.escape(m["commit"])}</code>'
                    f'</figcaption></figure>')

        lon, lat = view["spawn"]
        rows.append(
            f'<section><h2>{html.escape(view["title"])}</h2>'
            f'<p class="where">{lat:.5f}, {lon:.5f} · {SOLAR_HOUR:g} h solaires, {DAY:%d/%m}, ciel clair</p>'
            f'<div class="pair">{figure(before, "Précédente")}{figure(latest, "Dernière")}</div></section>')
    (GALLERY / "index.html").write_text(f"""<!doctype html>
<html lang="fr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Galerie de référence</title>
<style>
:root {{ --bg:#f6f5f2; --fg:#1d1d1b; --muted:#6b6a65; --line:#dedcd6; --card:#fff; }}
@media (prefers-color-scheme: dark) {{ :root {{ --bg:#161614; --fg:#eceae4; --muted:#9b9992; --line:#33322e; --card:#1f1f1c; }} }}
body {{ margin:0; background:var(--bg); color:var(--fg); font:15px/1.5 system-ui, sans-serif; }}
main {{ max-width:1500px; margin:0 auto; padding:24px 16px 64px; }}
h1 {{ font-size:22px; margin:0 0 4px; }} h2 {{ font-size:17px; margin:0; }}
.lead, .where {{ color:var(--muted); margin:2px 0 12px; }}
section {{ border-top:1px solid var(--line); padding:20px 0; }}
.pair {{ display:grid; grid-template-columns:1fr 1fr; gap:12px; }}
@media (max-width:800px) {{ .pair {{ grid-template-columns:1fr; }} }}
figure {{ margin:0; background:var(--card); border:1px solid var(--line); border-radius:6px; overflow:hidden; }}
figure.empty {{ display:flex; align-items:center; justify-content:center; min-height:160px; }}
img {{ display:block; width:100%; height:auto; }}
figcaption {{ padding:6px 10px; color:var(--muted); font-size:13px; }}
</style></head><body><main>
<h1>Galerie de référence</h1>
<p class="lead">Les mêmes vues à chaque version : même caméra, même heure solaire, ciel clair. {len(history)} prise(s).</p>
{''.join(rows)}
</main></body></html>
""", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("views", nargs="*", help="viewpoint names (default: all)")
    parser.add_argument("--offline", action="store_true", help="close the proxies: cached places only")
    parser.add_argument("--size", default="1600x900", help="picture size, WIDTHxHEIGHT (default 1600x900)")
    options = parser.parse_args()
    names = [v["name"] for v in VIEWS]
    unknown = [n for n in options.views if n not in names]
    if unknown:
        parser.error(f"unknown viewpoint(s) {', '.join(unknown)}; known: {', '.join(names)}")
    chosen = [v for v in VIEWS if not options.views or v["name"] in options.views]

    env = dict(os.environ, SAIDA_WINDOW_HIDDEN="1", SAIDA_WINDOW_SIZE=options.size)
    if options.offline:
        env.update(HTTP_PROXY="http://127.0.0.1:9", HTTPS_PROXY="http://127.0.0.1:9")
    now = datetime.now()
    folder = GALLERY / f"{now:%Y%m%d-%H%M%S}-{git_head()}"
    folder.mkdir(parents=True)
    results = [shoot(v, folder, env) for v in chosen]
    (folder / "run.json").write_text(json.dumps({
        "when": f"{now:%d/%m/%Y %H:%M}", "commit": git_head(), "offline": options.offline,
        "views": results}, ensure_ascii=False, indent=2), encoding="utf-8")
    page()
    print("Gallery:", GALLERY / "index.html")
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
