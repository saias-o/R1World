"""Write a readable complete inventory from the measured delivery manifest."""
import json
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'game/assets/animals'
catalog=json.loads((OUT/'manifest.json').read_text(encoding='utf-8'))
assets=catalog['assets']
groups={}
for asset in assets:
    groups.setdefault(asset['label'],[]).append(asset)

NOTES={
    'cat':'Tigré et sphynx ; rigs créés ici',
    'frog':'Dendrobate bleu, Ranitomeya amazonica et rainette aux yeux rouges ; rigs créés ici',
    'mouse':'Souris brune, rig et clip de l’auteur',
    'cattle':'2 taureaux, 6 vaches, 10 Sanga, 5 zébus',
    'deer':'3 robes du modèle léger, 1 biche détaillée, 1 cerf à bois détaillé',
    'horse':'70 variantes de chevaux et poneys : robes, tachetés, Lusitanien, Marwari, Persan, Thessalien, etc.',
    'dog':'2 robes de mastiff, 1 lévrier irlandais',
    'goat':'4 robes × 2 morphologies, dont une à grandes cornes',
    'pig':'3 robes de cochon + 3 robes de porcelet',
    'camel':'3 robes',
    'giraffe':'Adulte et petit, chacun en 2 robes',
    'hippo':'2 textures',
    'lion':'Mâle et femelle',
    'wolf':'Arctique, brun et gris',
    'sheep':'2 variantes de texture',
    'fish':'1 poisson individuel et 1 banc de poissons',
    'chicken':'1 poule et 1 coq détaillés',
    'hawk':'2 textures de rapace',
    'shark':'2 textures',
    'elephant_bush':'Adulte et petit',
}


def interval(values):
    lo,hi=min(values),max(values)
    return str(lo) if lo==hi else f'{lo}–{hi}'


lines=['# Tableau complet des animaux','',
       f'**{len(assets)} modèles/variantes, {len(groups)} types d’animaux, {len(assets)*2} GLB.** Chaque variante possède LOD0 et LOD1, avec rig et clips. Aucun LOD2 n’est fourni. Le banc de poissons est signalé comme tel.','',
       'Les nombres de polygones sont exprimés en **triangles réellement présents dans les GLB**, pas en quads source. Les plages indiquent les minimum et maximum des variantes. Les os comprennent les contrôleurs conservés dans le squelette exporté ; les clips peuvent inclure des poses statiques.','',
       '## Vue par type d’animal','',
       '| Animal | Nb modèles/variantes | Variantes | Triangles LOD0 | Triangles LOD1 | Os | Clips par modèle |',
       '|---|---:|---|---:|---:|---:|---:|']
for label,items in sorted(groups.items()):
    key=items[0]['animal_type']
    values=[interval([a[lod]['triangles'] for a in items]) for lod in ['lod0','lod1']]
    bones=interval([a['lod0']['bones'] for a in items])
    clips=interval([len(a['lod0']['clips']) for a in items])
    lines.append(f"| {label} | {len(items)} | {NOTES.get(key,'Modèle unique')} | {values[0]} | {values[1]} | {bones} | {clips} |")

lines+=['',f'## Détail exact des {len(assets)} modèles / variantes','',
        'Les liens des identifiants ouvrent le GLB LOD0 ; le lien du nombre de triangles LOD1 ouvre sa réduction. Les poids sont ceux des fichiers autonomes, textures et animations incluses. **1 Kio = 1 024 octets.**','',
        '| Animal | Identifiant / fichier | Triangles LOD0 | Triangles LOD1 | Réduction triangles | LOD0 Kio | LOD1 Kio | Os | Clips | Rig | Licence |',
        '|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|']
for a in sorted(assets,key=lambda a:(a['label'],a['id'])):
    near,lod=a['lod0'],a['lod1']
    reduction=(1-lod['triangles']/near['triangles'])*100
    lines.append(f"| {a['label']} | [{a['id']}]({a['id']}.glb) | {near['triangles']} | [{lod['triangles']}]({a['id']}-lod1.glb) | {reduction:.1f} % | {near['bytes']/1024:.1f} | {lod['bytes']/1024:.1f} | {near['bones']} | {len(near['clips'])} | {'Créé ici : contrôle uniquement' if a['rig_origin']=='created_for_R1World' else 'Auteur'} | {a['license']} |")
lines+=['','## Informations complémentaires','',
        '- [CSV complet](catalog.csv) : mêmes variantes, noms précis des clips et URL source.',
        '- [Manifeste JSON](manifest.json) : mesures, empreintes SHA-256, dimensions, clips, provenance et corrections.',
        '- [Licences et attributions](ATTRIBUTION.md).',
        '- [Aperçu visuel](preview.jpg).','',
        'Tous les fichiers sont riggés. Les LOD1 gardent les mêmes os et noms de clips que leur LOD0. Les textures sont embarquées, limitées à 1024 px pour LOD0 et 512 px pour LOD1 ; une texture source plus petite reste à sa résolution d’origine. Les réductions exactes figurent dans chaque ligne. Les rigs créés ici incluent des clips de contrôle, sans locomotion de production. Les poses statiques et clips d’origine sont identifiés dans le CSV et le manifeste.','',
        'Licences du lot livré : CC0 1.0, CC BY-SA 3.0 et CC BY 4.0. Les variantes ne constituent pas autant d’espèces différentes. Les modèles restent des assets préparés : aucun comportement ou changement de LOD automatique n’est intégré au jeu.']
(OUT/'TABLEAU_COMPLET.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
assert len(assets)>0 and len(groups)>0
assert sum(len(items) for items in groups.values())==len(assets)
print(f'Wrote {len(groups)} summary rows and {len(assets)} detailed rows.')
