"""Photograph the reference viewpoints and lay them beside the previous run.

A visual regression is the one no test sees (CLAUDE.md, rule 1): only an eye
on the picture does. This takes the same pictures every time -- the same
place, the same camera, the same solar hour, fixed weather -- so that the eye
compares a build with the last one rather than with its memory of it.

Some views are also a real photograph's: the camera stands where the
photographer stood, looks where the photograph looks, through the same lens and
frame, at its recorded or explicitly calibrated instant. Those are laid beside
the photograph, which says how far the world still is from the place.

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
import re
import subprocess
import sys

GAME = Path(__file__).resolve().parents[1]
GALLERY = GAME / "generated" / "gallery"
# One day for every picture, so the Sun's height is the place's and not the
# season's: near the equinox no hemisphere is favoured.
DAY = datetime(2026, 9, 21, tzinfo=timezone.utc)
SOLAR_HOUR = 10.5  # morning light: shadows long enough to read the relief
CLEAR_WEATHER = (0.0, 0.0, 150000.0)  # cloud fraction, rain mm/h, visibility m
# The runtime logger prints six significant digits.
LOG_NUMBER_REL_TOL = 1e-5


def enu(origin: tuple[float, float], point: tuple[float, float]) -> tuple[float, float]:
    """Engine metres (x east, z south) of `point` from `origin`, both lon/lat."""
    lon0, lat0 = origin
    lon, lat = point
    east = math.radians(lon - lon0) * 6378137.0 * math.cos(math.radians(lat0))
    north = math.radians(lat - lat0) * 6378137.0
    return east, -north


def aim(heading: float, pitch: float, eye: float):
    """A camera at the spawn, `eye` metres up, looking along a compass
    `heading` and `pitch` (degrees): a photograph's line of sight."""
    h, p = math.radians(heading), math.radians(pitch)
    d = 1000.0
    return (0.0, eye, 0.0), (math.sin(h) * d, eye + math.tan(p) * d, -math.cos(h) * d)


def lens(focal_35mm: float, width: int, height: int) -> float:
    """The vertical field of view, in degrees, of a 35 mm-equivalent focal
    length on a frame of this shape (the equivalence holds on the diagonal)."""
    half_diagonal = math.atan(math.hypot(36.0, 24.0) / 2.0 / focal_35mm)
    return math.degrees(2.0 * math.atan(math.tan(half_diagonal) * height / math.hypot(width, height)))


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
    {"name": "lasne", "title": "Croix de Lasné, route de Saint-Colombier",
     "spawn": (-2.71559, 47.56272),
     "camera": ((0.0, 1.7, 0.0), (-25.0, 1.7, 150.0)), "frame": (1334, 646),
     "at": "2026-04-20T10:30:00Z",
     "timeNote": "Avril 2026 comme la référence fournie ; jour et heure choisis pour une comparaison reproductible."},
    {"name": "saint_armel", "title": "Saint-Armel, maisons et annexes de Lasné",
     "spawn": (-2.716508, 47.563261),
     # User-reported regression point; look south-west over the irregular
     # cadastral house (171538265) and the small adjacent footprints.
     "camera": ((0.0, 3.0, 0.0), (-15.0, 4.5, 14.0))},
    # ── Mountains: each one a real photograph (Wikimedia Commons) ────────────
    # Position and lens from the file's camera location and EXIF; the heading
    # and pitch fitted on the summits it shows (OSM peaks), so that a summit
    # the world draws in the right place stands where the photograph has it.
    {"name": "grenoble", "title": "Grenoble, le Vercors et le Moucherotte depuis la Bastille",
     "spawn": (5.723786, 45.197956),
     # Moucherotte (1 901 m, 8.7 km, bearing 229.8°) at x 810 of 1 280.
     "camera": aim(224.97, 1.0, 1.7), "fov": lens(55, 2972, 2197), "frame": (2972, 2197),
     "at": "2008-10-12T08:31:12Z",
     "photo": {"file": "Moucherotte.jpg", "author": "Eusebius", "licence": "CC BY 3.0",
               "thumb": "https://upload.wikimedia.org/wikipedia/commons/thumb/1/1d/Moucherotte.jpg/1280px-Moucherotte.jpg"}},
    {"name": "lecap", "title": "Le Cap, la montagne de la Table depuis Bloubergstrand",
     "spawn": (18.468255, -33.810255),
     # Devil's Peak (16.3 km) at x 400 and Lion's Head (15.7 km) at x 1 015
     # agree on the heading to 0.2°.
     "camera": aim(196.4, 1.8, 1.7), "fov": lens(52, 2048, 1340), "frame": (2048, 1340),
     "at": "2007-02-25T08:08:46Z",
     "photo": {"file": "Table_Mountain_DanieVDM.jpg", "author": "Danie van der Merwe", "licence": "CC BY 2.0",
               "thumb": "https://upload.wikimedia.org/wikipedia/commons/thumb/d/dc/Table_Mountain_DanieVDM.jpg/1280px-Table_Mountain_DanieVDM.jpg"}},
    {"name": "rio", "title": "Rio, le Corcovado et le Christ depuis le Pain de Sucre",
     "spawn": (-43.156605, -22.949413),
     # On the summit (396 m, OSM), whatever this build's relief says: the
     # Christ (5.5 km, bearing 267.1°) at x 775.
     "camera": aim(258.5, -4.9, 1.7), "altitude": 396.0, "fov": lens(25, 6000, 4000), "frame": (6000, 4000),
     # EXIF 17:53 has no timezone. Treating it as Rio civil time puts the
     # Sun 8.7 degrees below the horizon, although the photo shows it above
     # the skyline at the right edge. A camera clock one hour ahead gives
     # 4.35 degrees elevation and 294.27 degrees azimuth, consistent with
     # that position. This is a visual calibration, not a measured UTC time.
     "at": "2015-05-22T19:53:16Z",
     "timeNote": "EXIF 17:53 sans fuseau ; correction d’horloge −1 h inférée du Soleil visible (UTC estimée).",
     "weather": (0.08, 0.0, 150000.0),
     "weatherNote": "Réanalyse Open-Meteo, 22/05/2015 à 20 h UTC : 8 % nuages, 0 mm/h ; visibilité 150 km estimée visuellement.",
     "photo": {"file": "Cidade_maravilhosa.JPG", "author": "Brunno Monteiro Lira", "licence": "CC BY-SA 3.0",
               "thumb": "https://upload.wikimedia.org/wikipedia/commons/thumb/6/60/Cidade_maravilhosa.JPG/1280px-Cidade_maravilhosa.JPG"}},
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


def size_of(view: dict, size: str) -> str:
    """The capture's size: the photograph's frame at the gallery's width."""
    if "frame" not in view:
        return size
    width = int(size.split("x")[0])
    fw, fh = view["frame"]
    return f"{width}x{round(width * fh / fw)}"


def moment(view: dict) -> float:
    if "at" in view:
        return datetime.fromisoformat(view["at"].replace("Z", "+00:00")).timestamp()
    return instant(view["spawn"][0])


def shoot(view: dict, folder: Path, env: dict) -> dict:
    lon, lat = view["spawn"]
    pos, look = view["camera"]
    png = folder / f"{view['name']}.png"
    args = [sys.executable, str(GAME / "tools" / "play_world.py"), "--smoke",
            "--spawn", str(lon), str(lat), "--screenshot", str(png),
            "--camera-pos", vec(pos), "--camera-look", vec(look), "--after-frames", "12",
            "--at", f"{moment(view):.0f}"]
    weather = view.get("weather", CLEAR_WEATHER)
    applied_weather = None
    args += ["--weather", *(str(value) for value in weather)]
    if "fov" in view:
        args += ["--camera-fov", f"{view['fov']:.3f}"]
    if "altitude" in view:
        args += ["--camera-altitude", f"{view['altitude']:.2f}"]
    env = dict(env, SAIDA_WINDOW_SIZE=size_of(view, env["SAIDA_WINDOW_SIZE"]))
    try:
        run = subprocess.run(args, cwd=GAME, env=env, capture_output=True, text=True, timeout=600)
        log = run.stdout.strip().splitlines()[-1:] if run.stdout else []
        ok = run.returncode == 0 and png.exists()
        reason = "" if ok else f"exit {run.returncode}"
        # A picture of ground whose streets never came is not the place: the
        # game says so in its log, and so does the gallery.
        if ok:
            text = ""
            if log and log[0].startswith("Logs:"):
                game_log = Path(log[0][5:].strip()) / "game.log"
                if game_log.exists():
                    text = game_log.read_text(encoding="utf-8", errors="replace")
            readings = re.findall(r"\[World inspection\] cloud=(\S+) rain_mm_h=(\S+) visibility_m=(\S+)", text)
            if readings:
                applied_weather = [float(value) for value in readings[-1]]
            if applied_weather is None or any(not math.isclose(got, want, rel_tol=LOG_NUMBER_REL_TOL)
                                               for got, want in zip(applied_weather, weather)):
                ok, reason = False, "inspection weather differs from the requested conditions or is unconfirmed"
            for line in text.splitlines():
                if "[World capture]" in line:
                    ok, reason = False, line.split("[World capture]", 1)[1].strip()
    except subprocess.TimeoutExpired:
        ok, log, reason = False, [], "no picture after 10 minutes"
    print(f"{view['name']:8} {'ok' if ok else 'FAILED: ' + reason}  {' '.join(log)}")
    return {"name": view["name"], "ok": ok, "reason": reason, "log": " ".join(log),
            "instant": moment(view), "weather": weather, "timeNote": view.get("timeNote", ""),
            "appliedWeather": applied_weather,
            "weatherNote": view.get("weatherNote", "Conditions estimées visuellement, sans relevé météo historique.")}


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
            condition = next((item for item in m["views"] if item["name"] == view["name"]), {})
            recorded = condition.get("instant")
            caption = (datetime.fromtimestamp(recorded, timezone.utc).strftime("%d/%m/%Y %H:%M UTC")
                       if recorded is not None else "conditions non enregistrées")
            if "weather" in condition:
                applied = condition.get("appliedWeather")
                cloud, rain, visibility = applied or condition["weather"]
                if applied is None:
                    caption += " · météo demandée, application non contrôlée"
                caption += f" · nuages {cloud:.0%} · {rain:g} mm/h · {visibility / 1000:g} km"
            return (f'<figure><a href="{src}"><img src="{src}" loading="lazy" alt=""></a>'
                    f'<figcaption>{label} · {html.escape(m["when"])} · <code>{html.escape(m["commit"])}</code>'
                    f'<br>{caption}'
                    f'</figcaption></figure>')

        lon, lat = view["spawn"]
        photo = view.get("photo")
        reference = ""
        when = f"{SOLAR_HOUR:g} h solaires, {DAY:%d/%m}"
        if photo:
            page_url = f"https://commons.wikimedia.org/wiki/File:{photo['file']}"
            original = photo["thumb"].replace("/thumb/", "/").rsplit("/", 1)[0]
            reference = (f'<figure><a href="{page_url}"><img src="{photo["thumb"]}" loading="lazy" '
                         f'onerror="this.onerror=null;this.src=\'{original}\'" alt=""></a>'
                         f'<figcaption>Photographie réelle · {html.escape(photo["author"])} · '
                         f'{html.escape(photo["licence"])} · <a href="{page_url}">Wikimedia Commons</a>'
                         f'</figcaption></figure>')
            when = view["at"][:16].replace("T", " ") + " UTC"
            if view.get("timeNote"):
                when += " · " + html.escape(view["timeNote"])
        weather = view.get("weather", CLEAR_WEATHER)
        weather_note = view.get("weatherNote", "Conditions estimées visuellement, sans relevé météo historique.")
        rows.append(
            f'<section><h2>{html.escape(view["title"])}</h2>'
            f'<p class="where">{lat:.5f}, {lon:.5f} · {when}. '
            f'Nuages {weather[0]:.0%}, pluie {weather[1]:g} mm/h, visibilité {weather[2] / 1000:g} km. '
            f'{html.escape(weather_note)}</p>'
            f'<div class="pair{" trio" if photo else ""}">{reference}{figure(before, "Précédente")}'
            f'{figure(latest, "Dernière")}</div></section>')
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
.pair.trio {{ grid-template-columns:1fr 1fr 1fr; }}
@media (max-width:800px) {{ .pair, .pair.trio {{ grid-template-columns:1fr; }} }}
figure {{ margin:0; background:var(--card); border:1px solid var(--line); border-radius:6px; overflow:hidden; }}
figure.empty {{ display:flex; align-items:center; justify-content:center; min-height:160px; }}
img {{ display:block; width:100%; height:auto; }}
figcaption {{ padding:6px 10px; color:var(--muted); font-size:13px; }}
</style></head><body><main>
<h1>Galerie de référence</h1>
<p class="lead">Caméra, heure solaire et météo fixes, enregistrées avec chaque capture. Les photos de montagne sont à gauche ; les incertitudes d’horloge et de météo sont indiquées. {len(history)} prise(s).</p>
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
