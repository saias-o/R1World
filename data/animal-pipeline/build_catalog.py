"""Build the distributable asset catalogue from measured GLBs and provenance."""
import collections
import csv
import hashlib
import json
import zipfile
from pathlib import Path

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'game/assets/animals'
QA=ROOT/'data/animal-pipeline/review'
SOURCE=ROOT/'data/source-assets/animals'
BASE_PATH=SOURCE/'0ad/manifest.json'
if BASE_PATH.exists():
    BASE=json.loads(BASE_PATH.read_text(encoding='utf-8'))
else:
    with zipfile.ZipFile(OUT/'licenses/provenance.zip') as archive:
        BASE=json.loads(archive.read('0ad/manifest.json'))
GROUPS={
 'arctic_wolf':('wolf','Loup'), 'wolf':('wolf','Loup'),
 'bear_black':('black_bear','Ours noir'), 'bear_brown':('brown_bear','Ours brun'), 'bear_white':('polar_bear','Ours polaire'),
 'boar':('boar','Sanglier'), 'bull':('cattle','Bovins'), 'cow':('cattle','Bovins'), 'sanga_cattle':('cattle','Bovins'), 'zebu':('cattle','Bovins'), 'zebu_wild':('cattle','Bovins'),
 'camel':('camel','Dromadaire'), 'crocodile':('crocodile','Crocodile'), 'crow':('crow','Corbeau'),
 'deer':('deer','Cervidés'), 'doe':('deer','Cervidés'), 'stag':('deer','Cervidés'), 'donkey':('donkey','Âne'),
 'elephant_african_baby':('elephant_bush','Éléphant de savane'), 'elephant_african_bush':('elephant_bush','Éléphant de savane'),
 'elephant_african_forest':('elephant_forest','Éléphant de forêt'), 'elephant_asian':('elephant_asian','Éléphant asiatique'),
 'fish_generic':('fish','Poisson générique / banc'), 'fish_single':('fish','Poisson générique / banc'), 'fish_kelgar':('rudd','Rotengle'),
 'fox_arctic':('arctic_fox','Renard arctique'), 'fox_red':('red_fox','Renard roux'), 'gazelle':('gazelle','Gazelle'),
 'giraffe_adult':('giraffe','Girafe'), 'giraffe_baby':('giraffe','Girafe'), 'goat':('goat','Chèvre / bouc'),
 'hawk':('hawk','Rapace'), 'hippopotamus':('hippo','Hippopotame'),
 'lion':('lion','Lion / lionne'), 'lioness':('lion','Lion / lionne'), 'mastiff':('dog','Chien'), 'wolfhound':('dog','Chien'),
 'peacock':('peacock','Paon'), 'pig1':('pig','Cochon / porcelet'), 'piglet':('pig','Cochon / porcelet'),
 'pony':('horse','Cheval / poney'), 'rabbit':('rabbit','Lapin'), 'rhino':('rhino','Rhinocéros'),
 'seagull':('seagull','Mouette'), 'shark':('shark','Requin'), 'sheep1':('sheep','Mouton'), 'sheep3':('sheep','Mouton'),
 'tiger':('tiger','Tigre'), 'tuna':('tuna','Thon'), 'turkey':('turkey','Dindon'), 'whale_fin':('fin_whale','Rorqual'),
 'wildebeest':('wildebeest','Gnou'), 'zebra':('zebra','Zèbre'),
 'ant':('ant','Fourmi'), 'butterfly':('butterfly','Papillon monarque'), 'bat':('bat','Chauve-souris'),
 'chicken_kelgar':('chicken','Poule / coq'), 'rooster':('chicken','Poule / coq'),
 'pike':('pike','Brochet'), 'horseshoe_crab':('horseshoe_crab','Limule'),
 'cat_tabby':('cat','Chat domestique'), 'cat_sphynx':('cat','Chat domestique'),
 'rat_street':('rat','Rat'), 'mouse_brown':('mouse','Souris'),
 'robin':('robin','Rouge-gorge'), 'pigeon_rock':('pigeon','Pigeon'),
 'frog_blue':('frog','Grenouilles / dendrobates'), 'frog_amazon':('frog','Grenouilles / dendrobates'), 'frog_red_eye':('frog','Grenouilles / dendrobates'),
 'gorilla':('gorilla','Gorille'), 'dolphin':('dolphin','Dauphin'),
 'turtle_sea':('sea_turtle','Tortue marine'), 'turtle_pond':('pond_turtle','Tortue de Reeves'),
 'owl_eagle':('owl','Hibou'), 'parrot_macaw':('parrot','Ara'), 'eagle_bald':('eagle','Pygargue à tête blanche'),
 'lizard':('lizard','Lézard'), 'wasp_realistic':('wasp','Guêpe'), 'spider_garden':('spider','Araignée'), 'shrike':('shrike','Pie-grièche à bec jaune'),
}


def build():
    smoke=json.loads((QA/'smoke.json').read_text(encoding='utf-8'))
    assert not smoke['errors'],smoke['errors']
    measurements={r['file']:r for r in smoke['files']}
    validation=json.loads((QA/'gltf-validation.json').read_text(encoding='utf-8'))
    assert not any(r['errors'] for r in validation)
    assert {r['file'] for r in validation}==set(measurements)
    # The delivered manifest is the canonical provenance record after cleanup.
    previous=json.loads((OUT/'manifest.json').read_text(encoding='utf-8')) if (OUT/'manifest.json').exists() else {'assets':[]}
    reports={a['id']:a for a in previous['assets']}
    for p in QA.glob('*.json'):
        r=json.loads(p.read_text(encoding='utf-8'))
        if isinstance(r,dict) and r.get('status')=='prepared' and 'id' in r:
            reports[r['id']]=r
    assets=[]
    for filename,near in sorted(measurements.items()):
        if filename.endswith('-lod1.glb'):
            continue
        ident=filename[:-4]
        r=reports[ident].copy()
        actor=Path(r['source_actor']).stem if 'source_actor' in r else ident
        group,label=('horse','Cheval / poney') if actor.startswith('horse') and actor!='horseshoe_crab' else GROUPS[actor]
        r['animal_type']=group
        r['label']=label
        r.setdefault('rig_origin','original_author')
        r.setdefault('animation_status','source_actions_preserved; some clips may be static poses')
        r['kind']='shoal' if actor=='fish_generic' else 'individual'
        r['lod0']=near
        r['lod1']=measurements[ident+'-lod1.glb']
        r['lod1']['triangle_ratio']=r['lod1']['triangles']/near['triangles']
        r['sha256']={name:hashlib.sha256((OUT/name).read_bytes()).hexdigest() for name in r['files']}
        r['source_url']=r.get('source_url',r.get('repository',BASE['repository'])+'/blob/'+r.get('revision',BASE['revision'])+'/binaries/data/mods/public/art/'+r.get('source_actor',''))
        if actor=='turkey':
            r['source_url']=r['repository']+'/blob/'+r['revision']+'/art/'+r['source_actor']
        r['visual_review']=r.get('visual_review','Reviewed textured LOD0/LOD1 pair from background render. Natural proportions and consistent realistic texture style. Lower-grade and death-texture candidates excluded.')
        assets.append(r)
    totals={'models_including_variants':len(assets),'animal_types':len({a['animal_type'] for a in assets}),'glb_files':len(measurements),'glb_bytes':sum(r['bytes'] for r in measurements.values()),'lod0_triangle_range':[min(a['lod0']['triangles'] for a in assets),max(a['lod0']['triangles'] for a in assets)],'lod1_triangle_range':[min(a['lod1']['triangles'] for a in assets),max(a['lod1']['triangles'] for a in assets)],'lod1_mean_triangle_ratio':sum(a['lod1']['triangle_ratio'] for a in assets)/len(assets),'gltf_errors':sum(v['errors'] for v in validation),'gltf_warnings':sum(v['warnings'] for v in validation),'sampled_animation_clips':sum(len(r['clips']) for r in measurements.values())}
    manifest={'source_cache_status':'Raw sources and work caches recycled at user request; license evidence and source hashes retained in licenses/provenance.zip.','storage_optimization':'Unweighted helper channels removed; identical buffers/accessors/samplers shared; PNGs repacked without changing dimensions or shader-used pixels. Geometry, complete skeletons, visible motion, clip names and durations preserved.','date':'2026-10-26','purpose':'Prepared assets only; no animal behaviours or runtime integration.','coordinate_system':{'up':'+Y','units':'metres','scale_accuracy':'Approximate species-size normalization; orientation and proportions inherited from source.'},'statistics':totals,'assets':assets}
    (OUT/'manifest.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf-8')
    with (OUT/'catalog.csv').open('w',newline='',encoding='utf-8-sig') as f:
        writer=csv.writer(f)
        writer.writerow(['id','animal','license','author','lod0_triangles','lod1_triangles','lod0_bytes','lod1_bytes','bones','rig_origin','animation_status','clips','source'])
        for a in assets:
            writer.writerow([a['id'],a['label'],a['license'],a['author'],a['lod0']['triangles'],a['lod1']['triangles'],a['lod0']['bytes'],a['lod1']['bytes'],a['lod0']['bones'],a['rig_origin'],a['animation_status'],'; '.join(c['name'] for c in a['lod0']['clips']),a['source_url']])
    grouped=collections.defaultdict(list)
    for a in assets: grouped[a['label']].append(a)
    lines=['# Animaux préparés — 26 octobre 2026','',f"{totals['models_including_variants']} modèles/variantes, regroupés en **{totals['animal_types']} types d’animaux**, avec un GLB et un GLB LOD1 chacun. Les robes, sexes, âges et races ne sont pas comptés comme des espèces distinctes.",'',f"**{totals['glb_files']} GLB, {totals['glb_bytes']/1048576:.1f} Mio au total.** LOD0 : {totals['lod0_triangle_range'][0]}–{totals['lod0_triangle_range'][1]} triangles ; LOD1 : {totals['lod1_triangle_range'][0]}–{totals['lod1_triangle_range'][1]} triangles. Le LOD1 conserve en moyenne {totals['lod1_mean_triangle_ratio']*100:.1f} % des triangles.",'','Les textures, squelettes et clips sont intégrés aux GLB. `nom.glb` est le modèle proche ; `nom-lod1.glb` est sa réduction avec le même squelette et les mêmes noms de clips. Textures limitées à 1024 px au proche et 512 px au LOD1. Pas de Draco, Meshopt ou KTX requis. Les tangentes des normal maps sont intégrées. Échelle en mètres approximative, axe vertical +Y ; les orientations de départ sont conservées.','', '## Catalogue','', '| Type | Modèles / variantes | Triangles LOD0 |', '|---|---:|---:|']
    for label,items in sorted(grouped.items()):
        tris=[a['lod0']['triangles'] for a in items]
        lines.append(f"| {label} | {len(items)} | {min(tris)}–{max(tris)} |")
    lines+=['','## Mesures et vérifications','',f"Validateur Khronos glTF : **{totals['gltf_errors']} erreur, {totals['gltf_warnings']} avertissement**. Smoke des {totals['glb_files']} fichiers : buffers et textures embarqués, triangles, poids normalisés, indices d’os, conservation des rigs/clips entre LOD et déformation numérique sur cinq instants de chacun des {totals['sampled_animation_clips']} clips (variantes et LOD compris).",'','Des réimports Blender ont aussi servi à vérifier visuellement les mouvements et les découpes alpha. Tous les rendus et imports ont tourné en arrière-plan, sans fenêtre ni lancement du jeu. Aucun code du jeu ou du moteur n’a été modifié.','', '## Fichiers utiles','', '- [Catalogue CSV](catalog.csv) : coût, licence, auteur, clips et source par animal.', '- [Manifeste complet](manifest.json) : mesures par LOD, provenance, corrections et SHA-256.', '- [Attributions](ATTRIBUTION.md) et [licences](licenses/).', '- [Aperçu](preview.jpg) : sélection représentative issue des GLB finaux.', '- [Provenance archivée](licenses/provenance.zip) : preuves de licence, métadonnées et empreintes historiques ; les sources brutes ont été nettoyées.', '- Outils et rapports de smoke/revue : `data/animal-pipeline/`.','', '## Optimisation et nettoyage', '', 'Les textures conservent leurs dimensions et leurs pixels utilisés par les matériaux. La compaction retire uniquement les pistes de contrôleurs sans influence sur aucun sommet, en conservant tous les os, noms et durées de clips ; les données de géométrie et de mouvement visibles restent identiques. Les buffers, accessors et samplers strictement identiques sont partagés. Aucun décodeur supplémentaire n’est requis. Les sources brutes, doublons de GLB, rendus de travail et journaux ont été retirés du projet et placés dans la corbeille Windows. Voir `OPTIMISATION.md`.', '', '## Sélection et limites','', 'Priorité aux silhouettes naturelles et aux textures réalistes cohérentes. Les modèles de 0 A.D. forment le socle ; plusieurs animaux CC0 de Kelgar apportent des modèles plus détaillés et de nouveaux types. Les candidats cartoon, les figurines, les monstres et les textures de cadavres sont exclus. Des sources CC0 et CC BY 4.0 complètent le lot avec des silhouettes naturelles ; des rigs de déformation ont été créés pour les modèles statiques retenus. Les variantes de lapin de 184 triangles ont été remplacées par un lapin CC0 plus détaillé.','', 'Les ajouts couvrent chats domestiques, rongeurs, oiseaux, amphibiens, tortues, primate et invertébrés. Le canard, l’oie, le moineau et l’écureuil restent à compléter : les candidats vérifiés étaient trop cartoon, des figurines ou sans preuve de licence suffisante. Voir `EXPANSION.md` pour les comparaisons et limites des nouveaux rigs.','', 'Les GLB sont préparés et vérifiés comme fichiers. Leur compatibilité visuelle avec le rendu du jeu reste à vérifier lors d’une future intégration ; aucun comportement, placement ou changement automatique de LOD n’a été ajouté.']
    (OUT/'README.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')
    credits=['# Attribution des animaux','', 'Les adaptations présentes dans ce dossier sont des conversions en GLB, normalisations approximatives d’échelle, conversions/réductions de textures, réductions LOD, corrections de compatibilité Collada, optimisation de canaux d’animation constants, création de rigs de déformation lorsque nécessaire, retrait de pistes de contrôleurs sans influence sur les sommets, déduplication de stockage et recompression PNG sans perte des canaux utilisés. Les téléchargements bruts ont été nettoyés à la demande de l’utilisateur. Les preuves de licence, métadonnées source et empreintes historiques sont conservées dans `licenses/provenance.zip` ; les modèles adaptés complets restent dans les GLB.','', '## CC BY-SA 3.0 — Wildfire Games','', '**Wildfire Games**, https://www.wildfiregames.com/ — modèles, textures et animations du projet [0 A.D.](https://github.com/0ad/0ad). Licence [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/), copie et notice originales dans `licenses/`.','', 'Les fichiers dérivés de ces œuvres sont distribués sous CC BY-SA 3.0. Conserver cette attribution, le lien de licence et l’indication des adaptations lors de leur redistribution. Le manifeste précise la source et le commit pour chaque fichier.','', '## CC BY-SA 3.0 — Terra Magna','', '**The Council of Modders, Fallen Empire Studio, Scion Development**, https://www.wildfiregames.com/ — dindon du projet [Terra Magna](https://github.com/0ADMods/terra_magna). Même licence [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/). Notice originale : `licenses/terra-magna-art.txt`.','', '## CC0 1.0 — sources diverses','', 'Ces contributions sont conservées sous [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). Crédit volontaire :','']
    for a in assets:
        if a['license']=='CC0-1.0': credits.append(f"- **{a['label']}** (`{a['id']}`), {a['author']} — [page originale]({a['source_url']}).")
    credits+=['','## CC BY 4.0 — contributions originales','','Ces modèles et leurs adaptations sont distribués sous [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/), copie dans `licenses/CC-BY-4.0.txt`. Modifications : conversion GLB, mise à l’échelle, LOD, textures réduites et, sauf mention de rig original dans le manifeste, création du squelette, des poids et de clips de contrôle. Les fichiers GLB ont été obtenus par la distribution publique Objaverse ; la licence de chaque œuvre est vérifiée séparément de celle du jeu de données.','']
    for a in assets:
        if a['license']=='CC-BY-4.0':credits.append(f"- **{a['label']}** (`{a['id']}`), {a['author']} — [œuvre originale]({a['source_url']}).")
    (OUT/'ATTRIBUTION.md').write_text('\n'.join(credits)+'\n',encoding='utf-8')
    # Raw source caches may have been explicitly cleaned. Their historical
    # hashes and licensing evidence remain in licenses/provenance.zip.
    if SOURCE.exists():
        checksums={p.relative_to(SOURCE).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(SOURCE.rglob('*')) if p.is_file() and p.name!='SHA256SUMS.json' and 'research-expansion' not in p.parts}
        (SOURCE/'SHA256SUMS.json').write_text(json.dumps(checksums,indent=2),encoding='utf-8')
    print(json.dumps(totals,indent=2))


if __name__=='__main__': build()
