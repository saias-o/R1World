"""Capture at the published PHOTO camera coordinates, never the player's spawn.

Clocks come from EXIF (UTC GPS clock where available). Weather is reconstructed
from the photograph, not an archived meteorological measurement. Unmeasured
camera height and pitch stay explicit. EXIF magnetic bearings are corrected
with WMM for the photo date; no arbitrary move to a prettier nearby street.
"""
import argparse
import datetime as dt
import json
import math
import os
from pathlib import Path
import subprocess

GAME = Path(__file__).resolve().parents[1]
VIEWS = [
    dict(key="us-moretown", country="US", village="1423 Route 100B, Moretown, Vermont",
         camera=[-72.758531, 44.254078, 1.65], heading=212.33998755,
         bearing="EXIF magnetic 46 + WMM2025 -13.66001245 + 180 degree lens reversal verified against mapped house 1161789689 (address and Commons photo link match)",
         local="2026-09-20T09:40:56-04:00", weather=[1, 0, 20000],
         focal35=49, size=[1064, 801], pitch=0,
         optics="EXIF 49 mm equivalent; optical diagonal preserved at original 4080:3072 aspect",
         reference="https://commons.wikimedia.org/wiki/File:1423_Vermont_Route_100B,_Moretown,_VT.jpg",
         image="https://upload.wikimedia.org/wikipedia/commons/5/59/1423_Vermont_Route_100B%2C_Moretown%2C_VT.jpg",
         author="AdamFranco", license="CC BY-SA 4.0"),
    dict(key="ru-suzdal", country="RU", village="Tikhonravov house, Gastev Street 21, Suzdal",
         camera=[40.440817, 56.428137, 1.65], heading=270,
         bearing="Published camera heading 270 degrees (manually geocoded; uncertainty unspecified)",
         local="2018-06-20T10:30:38+03:00", weather=[.12, 0, 25000],
         focal35=28.8, size=[1192, 760], pitch=0,
         optics="EXIF 18 mm on Canon APS-C; 28.8 mm equivalent estimate, cropped source 3730:2380",
         reference="https://commons.wikimedia.org/wiki/File:Suzdal_Gasteva21_192_6046.jpg",
         image="https://upload.wikimedia.org/wikipedia/commons/5/5a/Suzdal_Gasteva21_192_6046.jpg",
         author="Ludvig14", license="CC BY-SA 4.0"),
    dict(key="ma-tafraout", country="MA", village="Oued Massa bridge, Boulevard Al-Jeish Al-Malaki, Tafraout",
         camera=[-8.972913, 29.720753, 1.65], heading=340,
         bearing="Registered on the upstream channel and the bridge at the published GPS point; heading estimated, not present in EXIF",
         local="2018-08-15T13:07:38+01:00", weather=[0, 0, 40000],
         focal35=46.4249279, size=[1334, 750], pitch=0,
         optics="Stitched rectilinear panorama, Canon full frame; 46.4249279 mm PTGui focal metadata, source 6080:3420. Stitch/crop uncertainty remains",
         reference="https://commons.wikimedia.org/wiki/File:MA.SS.Tafraout_1149_16x9-R_6K.jpg",
         image="https://upload.wikimedia.org/wikipedia/commons/8/82/MA.SS.Tafraout_1149_16x9-R_6K.jpg",
         author="Roy Egloff", license="CC BY-SA 4.0"),
    dict(key="jp-tsumago", country="JP", village="Nakasendo, Tsumago-juku, Nagiso",
         camera=[137.595610, 35.577542, 1.65], heading=205,
         bearing="Published camera heading 205 degrees (uncertainty unspecified)",
         local="2016-03-29T15:05:01+09:00", weather=[.15, 0, 30000],
         focal35=27, size=[1203, 800], pitch=0,
         optics="EXIF 27 mm equivalent; original 6016:4000 aspect",
         reference="https://commons.wikimedia.org/wiki/File:Looking_up_the_Nakasend%C5%8D_in_the_central_part_of_the_village,_Tsumago-juku,_Nagiso,_2016.jpg",
         image="https://upload.wikimedia.org/wikipedia/commons/c/c0/Looking_up_the_Nakasend%C5%8D_in_the_central_part_of_the_village%2C_Tsumago-juku%2C_Nagiso%2C_2016.jpg",
         author="DimiTalen", license="CC0"),
]


def capture_args(view, output):
    instant = int(dt.datetime.fromisoformat(view["local"]).timestamp())
    azimuth, pitch = map(math.radians, [view["heading"], view["pitch"]])
    direction = [math.sin(azimuth)*math.cos(pitch), math.sin(pitch), -math.cos(azimuth)*math.cos(pitch)]
    aspect = view["size"][0]/view["size"][1]
    vertical = math.hypot(36, 24)/math.sqrt(1+aspect*aspect)
    fov = math.degrees(2*math.atan(vertical/(2*view["focal35"])))
    cmd = [str(GAME / "generated/world-windows/R1World.exe"), "--project", str(GAME),
           "--smoke", "--spawn", *map(str, view["camera"][:2]), "--at", str(instant),
           "--weather", *map(str, view["weather"]), "--camera-geo", *map(str, view["camera"]),
           "--camera-pos", "0,0,0", "--camera-look", ",".join(f"{x:.10f}" for x in direction),
           "--camera-fov", f"{fov:.8f}", "--after-frames", "90",
           "--screenshot", str(output / (view["key"] + ".png"))]
    return instant, fov, cmd


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", choices=[v["key"] for v in VIEWS])
    parser.add_argument("--variant", default="registered")
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    output = GAME / "generated" / "rural-references" / args.variant
    output.mkdir(parents=True, exist_ok=True)
    env = {k.upper(): v for k, v in os.environ.items()}
    env["PATH"] = "C:/msys64/ucrt64/bin;" + env.get("PATH", "")
    env["SAIDA_WINDOW_HIDDEN"] = "1"
    if args.offline:
        env["HTTP_PROXY"] = env["HTTPS_PROXY"] = "http://127.0.0.1:9"
    manifest = output / "captures.json"
    results = json.loads(manifest.read_text(encoding="utf-8")) if manifest.exists() else []
    for view in VIEWS:
        if args.only and view["key"] != args.only:
            continue
        env["SAIDA_WINDOW_SIZE"] = "x".join(map(str, view["size"]))
        instant, fov, cmd = capture_args(view, output)
        with (output / (view["key"] + ".log")).open("w", encoding="utf-8") as log:
            result = subprocess.run(cmd, cwd=GAME, env=env, stdout=log, stderr=log, timeout=480)
        results = [r for r in results if r["key"] != view["key"]]
        results.append(dict(view, instant=instant, verticalFov=fov, command=cmd,
                            eyeHeight="unmeasured, 1.65 m AGL", pitchSource="unmeasured, level camera",
                            weatherSource="visual reconstruction; cloud shapes are not reproduced",
                            returncode=result.returncode))
        print(view["key"], "exit", result.returncode, flush=True)
        manifest.write_text(json.dumps(results, indent=2), encoding="utf-8")
    return int(any(v["returncode"] for v in results))


if __name__ == "__main__":
    raise SystemExit(main())
