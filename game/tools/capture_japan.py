"""Photographic Japan audit using the existing runtime capture API.

Build with Play.ps1 -BuildOnly first. No game/generator changes are required.
python tools/capture_japan.py before [--only jp-kyoto] [--offline]
python tools/capture_japan.py after --offline
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import subprocess

from capture_rural_references import GAME, capture_args

VIEWS = json.loads(Path(__file__).with_name("japan_views.json").read_text(encoding="utf-8"))
ROOT = GAME / "generated/japan-update"


def report():
    esc = html.escape
    cards = []
    for view in VIEWS:
        key = view["key"]
        figures = [f'<figure><img src="{esc(view["image"])}"><figcaption>Photographie · '
                   f'<a href="{esc(view["reference"])}">{esc(view["author"])}</a> · {view["license"]}</figcaption></figure>']
        for stage in ("before", "after"):
            record = ROOT / stage / (key + ".json")
            data = json.loads(record.read_text(encoding="utf-8")) if record.exists() else {}
            same_view = all(data.get(k) == view.get(k) for k in
                            ("camera", "heading", "pitch", "local", "weather", "focal35", "size", "reference"))
            if data.get("verified") and same_view:
                figures.append(f'<figure><img src="{stage}/{key}.png"><figcaption>R1World · '
                               f'{"Avant" if stage == "before" else "Après"} · <a href="{stage}/{key}.json">paramètres</a> · '
                               f'<a href="{stage}/{key}.log">journal</a></figcaption></figure>')
            else:
                reason = "capture complète non disponible"
                if data and not same_view:
                    reason = "ancienne caméra écartée"
                elif any("GeometryRegistry" in p for p in data.get("problems", [])):
                    reason = "capacité géométrique dépassée ; capture refusée"
                detail = f' · <a href="{stage}/{key}.json">diagnostic</a>' if record.exists() else ''
                figures.append(f'<figure class="missing">{stage} : {reason}{detail}</figure>')
        cards.append(f'<article><h2>{esc(view["village"])}</h2><p>{esc(view["note"])}</p>'
                     f'<p>GPS {view["camera"][1]:.6f}, {view["camera"][0]:.6f} · cap {view["heading"]:.3f}° · '
                     f'{view["local"]}</p><div class="grid">{"".join(figures)}</div></article>')
    page = '''<!doctype html><html lang="fr"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>R1World — Japan update</title><style>
body{background:#101719;color:#e9e8df;margin:0;font:16px/1.5 system-ui}main{max-width:1800px;margin:auto;padding:32px}
h1{font-size:38px;margin-bottom:8px}p{color:#bec7c5;max-width:1100px}article{border-top:1px solid #384441;padding:24px 0}
.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}figure{margin:0;background:#070b0c}
img{width:100%;height:420px;object-fit:contain;display:block}figcaption,.missing{padding:12px}a{color:#b0d8c0}
@media(max-width:900px){.grid{grid-template-columns:1fr}main{padding:16px}}
</style><main><h1>Japan update</h1><p>Photos localisées et captures du jeu aux mêmes coordonnées. Avant/après :
mêmes caméra, date et météo ; versions de l'exécutable et de l'Atlas consignées dans chaque fiche. Hauteur de l'appareil et inclinaison estimées ; le GPS publié
n'est pas une garantie centimétrique. Les photos restent hébergées par Wikimedia Commons, avec leur attribution.</p>
<p>La comparaison porte sur les règles d'architecture. Relief, emprises, couverture OSM et détails absents restent visibles.
Les vieux clichés ne décrivent pas nécessairement les bâtiments actuels. Les couleurs proposées sont des estimations de matériau,
pas des albédos mesurés dans ces photographies.</p>'''
    tree = '''<article><h2>Cerisier retenu : Innerscene</h2>
<p>Aperçu Saida : arbre actuel, cerisier détaillé, version intermédiaire, version lointaine.
Tous sont montrés à la même taille pour inspection ; le dernier est normalement vu de loin.
Floraison fixe, sans simulation saisonnière. Les cerisiers renseignés sont
reconnus partout ; depuis la v52, une sélection
urbaine déterministe ajoute 15 % au Japon et 5 % dans le reste de l'hémisphère nord,
parmi les arbres sans espèce renseignée.</p>
<img style="height:auto" src="sakura-preview/comparison.png">
<p><a href="https://www.innerscene.com/tools/library/3d-parts/flowering-cherry-tree-6d80049e">Innerscene · CC0</a></p></article>'''
    world_record = ROOT / "kyoto-sakura-world.json"
    if world_record.exists() and json.loads(world_record.read_text(encoding="utf-8")).get("verified"):
        tree += '''<article><h2>Cerisiers identifiés à Kyoto — dans R1World</h2>
<p>Près de Kiyomizu-dera. Caméra : 34.999830, 135.788440 ; cap 6°.
Deux arbres observés dans OSM sous Cerasus × yedoensis. Cette vue illustre le
placement du modèle ; elle n'est pas appariée à une photographie réelle.</p>
<img style="height:auto" src="kyoto-sakura-world.png"></article>'''
    (ROOT / "index.html").write_text(page + "".join(cards) + tree + "</main></html>", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=["before", "after"])
    parser.add_argument("--only", choices=[v["key"] for v in VIEWS])
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    output = ROOT / args.stage
    output.mkdir(parents=True, exist_ok=True)
    atlas = (GAME / "assets/world/atlas.json").read_bytes()
    (output / "atlas.json").write_bytes(atlas)
    env = {k.upper(): v for k, v in os.environ.items()}
    env["PATH"] = "C:/msys64/ucrt64/bin;" + env.get("PATH", "")
    env["SAIDA_WINDOW_HIDDEN"] = "1"
    if args.offline:
        env["HTTP_PROXY"] = env["HTTPS_PROXY"] = "http://127.0.0.1:9"
    failures = 0
    for view in VIEWS:
        if args.only and args.only != view["key"]:
            continue
        env["SAIDA_WINDOW_SIZE"] = "x".join(map(str, view["size"]))
        instant, fov, cmd = capture_args(view, output)
        logpath = output / (view["key"] + ".log")
        try:
            with logpath.open("w", encoding="utf-8") as log:
                result = subprocess.run(cmd, cwd=GAME, env=env, stdout=log, stderr=log, timeout=480)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = -1
        logtext = logpath.read_text(encoding="utf-8", errors="replace")
        problems = [line for line in logtext.splitlines() if "[error]" in line or "FAIL" in line
                    or ("[World capture]" in line and ("pending" in line.lower() or "provisional" in line.lower()))]
        verified = code == 0 and not problems and (output / (view["key"] + ".png")).exists()
        data = dict(view, instant=instant, verticalFov=fov, command=cmd, returncode=code,
                    verified=verified, problems=problems, atlasSHA256=hashlib.sha256(atlas).hexdigest(),
                    exeSHA256=hashlib.sha256(Path(cmd[0]).read_bytes()).hexdigest())
        (output / (view["key"] + ".json")).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
        failures += not verified
        print(view["key"], args.stage, "verified", verified, "exit", code, flush=True)
        report()
    return int(failures > 0)


if __name__ == "__main__":
    raise SystemExit(main())
