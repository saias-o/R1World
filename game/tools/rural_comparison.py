"""Build a review page from the registered camera captures; photos stay remote.

Usage: python tools/rural_comparison.py [--variant registered-final]
Serve generated/rural-references/<variant> to review the page in a browser.
"""
import argparse
import html
import json
from pathlib import Path

from capture_rural_references import GAME, VIEWS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", default="registered-final")
    args = parser.parse_args()
    output = GAME / "generated/rural-references" / args.variant
    records = {v["key"]: v for v in json.loads((output / "captures.json").read_text(encoding="utf-8"))}
    cards = []
    for view in VIEWS:
        record = records.get(view["key"])
        if not record or record["returncode"] or not (output / (view["key"] + ".png")).exists():
            raise RuntimeError("Missing successful capture: " + view["key"])
        log = (output / (view["key"] + ".log")).read_text(encoding="utf-8", errors="replace")
        warnings = [line for line in log.splitlines() if "[World capture]" in line and "[error]" in line]
        camera = record["camera"]
        cloud, rain, visibility = record["weather"]
        esc = html.escape
        cards.append(f'''<article id="{view['key']}">
<h2>{esc(view['country'])} · {esc(view['village'])}</h2>
<p class="position">GPS publié : {camera[1]:.6f}, {camera[0]:.6f} · Date/heure photo : {esc(record['local'])}</p>
<div class="pair"><figure><img src="{esc(view['image'])}" alt="Photographie réelle de {esc(view['village'])}" loading="lazy"><figcaption>Photo réelle · <a href="{esc(view['reference'])}">{esc(view['author'])}</a> · {esc(view['license'])}</figcaption></figure>
<figure><img src="{view['key']}.png" alt="Jeu au point GPS de la photo"><figcaption>R1World v34 · caméra géographique fixe · <a href="{view['key']}.log">journal de capture</a></figcaption></figure></div>
<details><summary>Position, cadrage et conditions vérifiables</summary><dl>
<dt>Point de vue</dt><dd>Longitude {camera[0]}, latitude {camera[1]} ; hauteur estimée {camera[2]} m au-dessus du terrain. Le déplacement du joueur ne déplace pas cette caméra.</dd>
<dt>Orientation</dt><dd>{record['heading']:.5f}° ; {esc(record['bearing'])}</dd>
<dt>Optique</dt><dd>Champ vertical {record['verticalFov']:.3f}° ; {esc(record['optics'])}. Inclinaison non mesurée : caméra horizontale.</dd>
<dt>Météo reconstituée</dt><dd>Nuages {cloud*100:g} %, pluie {rain:g} mm/h, visibilité {visibility/1000:g} km. Estimation visuelle, pas une mesure de station ; formes des nuages différentes.</dd>
<dt>Chargement</dt><dd>{esc('; '.join(warnings) if warnings else 'Aucun avertissement de capture incomplète dans le journal.')}</dd>
</dl></details></article>''')
    page = '''<!doctype html><html lang="fr"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>R1World · Comparaisons aux points photographiés</title>
<style>body{margin:0;background:#11161b;color:#e8edf2;font:16px/1.5 system-ui}main{max-width:1600px;padding:32px;margin:auto}h1{font-size:30px;margin:0}h2{font-size:22px;margin:0 0 8px}p{max-width:1000px;color:#b7c5d1}nav{display:flex;gap:16px;flex-wrap:wrap;margin:20px 0}a{color:#86cbfa}article{border-top:1px solid #35414a;padding:28px 0;scroll-margin-top:16px}.pair{display:grid;grid-template-columns:1fr 1fr;gap:16px}figure{margin:0;background:#080b0e}img{display:block;width:100%;height:auto}figcaption{padding:10px 14px;font-size:14px;color:#b7c5d1}details{margin-top:18px;background:#1c252d;padding:12px 16px;border-radius:8px}summary{cursor:pointer;color:#b7d8f0}dt{font-weight:600;margin-top:12px}dd{margin:4px 0;font-size:14px;color:#bdc9d2}.position{font:14px ui-monospace,monospace}@media(max-width:900px){.pair{grid-template-columns:1fr}main{padding:18px}}</style>
<main><h1>Comparer au point photographié</h1><p>La caméra du jeu reprend les coordonnées publiées de chaque photo. Ces points ne sont pas déplacés pour améliorer la composition. La date et l’heure EXIF fixent le Soleil ; la météo est reconstituée visuellement.</p><p>Une coordonnée publiée n’est pas un relevé au centimètre. La hauteur et l’inclinaison sont estimées ; l’orientation américaine a été vérifiée sur la maison identifiée dans OSM, et celle du panorama marocain sur l’oued et le pont. Les emprises, le relief et la végétation restent ceux des sources du jeu.</p>
<nav>'''
    page += "".join(f'<a href="#{v["key"]}">{v["country"]}</a>' for v in VIEWS)
    page += '<a href="captures.json">Paramètres reproductibles</a></nav>' + "".join(cards) + '</main></html>'
    (output / "index.html").write_text(page, encoding="utf-8")
    print(output / "index.html")


if __name__ == "__main__":
    main()
