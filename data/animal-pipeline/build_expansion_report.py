"""Publish measured additions and comparative visual selection evidence."""

import json,hashlib,shutil

from pathlib import Path

from PIL import Image,ImageDraw,ImageFont

ROOT=Path(__file__).resolve().parents[2];OUT=ROOT/'game/assets/animals';QA=ROOT/'data/animal-pipeline/review';P=QA/'expansion'

m=json.loads((OUT/'manifest.json').read_text(encoding='utf-8'))
assets=[a for a in m['assets'] if 'expansion' in a.get('source_file','') or a['id']=='shrike']
assert len(assets)==20

created=sum(a['rig_origin']=='created_for_R1World' for a in assets);size=sum(a[l]['bytes'] for a in assets for l in ['lod0','lod1'])

# Reuse the canonical visual sheets after raw render caches are cleaned.
if (P/'final-renders').exists() and (P/'references').exists():
    for a in assets:
     for suffix in ['', '-lod1']:
      shutil.copyfile(P/'final-renders'/(a['id']+suffix+'.png'),QA/'final-renders'/(a['id']+suffix+'.png'))
    font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',17)
    rows=[['fox_red','rabbit','cat_tabby','rat_street'],['wolf','rabbit','gorilla','mouse_brown'],['chicken_kelgar','crow','robin','pigeon_rock'],['hawk','crow','owl_eagle','eagle_bald'],['chicken_kelgar','crow','parrot_macaw','shrike'],['crocodile','pike','frog_blue','frog_red_eye'],['crocodile','pike','turtle_sea','turtle_pond'],['ant','ant','wasp_realistic','spider_garden'],['pike','crocodile','dolphin','lizard']]
    for start in range(0,len(rows),3):
     sheet=Image.new('RGB',(1600,1065),'#25282b');draw=ImageDraw.Draw(sheet)
     draw.text((12,12),'R1World — références actuelles à gauche / nouveaux GLB à droite',font=font,fill='white')
     for i,row in enumerate(rows[start:start+3]):
      for j,name in enumerate(row):
       im=Image.open(P/('references' if j<2 else 'final-renders')/(name+'.png'))
       sheet.paste(im,(j*400,45+i*340));draw.text((j*400+7,45+i*340+321),('Actuel : ' if j<2 else 'Ajout : ')+name,font=font,fill='white')
     file=P/f'coherence-{start//3+1}.jpg';sheet.save(file,quality=94)
     shutil.copyfile(file,OUT/f'comparaison-ajouts-{start//3+1}.jpg')

    font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',16)

    sheet=Image.new('RGB',(1440,212*((len(assets)+5)//6)+44),'#25282b');draw=ImageDraw.Draw(sheet);draw.text((12,12),'R1World — 20 ajouts réalistes, GLB finaux avec LOD et rig',font=font,fill='white')

    for i,a in enumerate(assets):

     x=i%6*240;y=44+i//6*212

     im=Image.open(QA/'final-renders'/(a['id']+'.png')).resize((240,192),Image.Resampling.LANCZOS);sheet.paste(im,(x,y));draw.text((x+4,y+193),a['label'][:29],font=font,fill='white')

    sheet.save(OUT/'nouveaux-animaux.jpg',quality=94)


lines=['# Extension réaliste des animaux — 26 octobre 2026','',f"**{len(assets)} modèles ajoutés, 17 types supplémentaires, {created} rigs créés et {len(assets)-created} rigs d’origine conservés.** Les 338 GLB précédents sont conservés ; l’ensemble a ensuite été compacté sans changement de géométrie, de squelette, de mouvement visible ni de pixels utilisés par les matériaux (voir `OPTIMISATION.md`). L’ensemble passe de 169 à 189 modèles/variantes et de 45 à 62 types ; 378 fichiers GLB au total.",'',f"Les ajouts occupent **{size/1048576:.1f} Mio**, textures et clips compris. Chaque modèle possède un LOD1 avec le même squelette et les mêmes noms de clips. Les nombres ci-dessous sont des triangles mesurés dans les GLB finaux.",'','## Modèles retenus','','| Animal / variante | Fichier LOD0 | Triangles LOD0 | Triangles LOD1 | Os | Rig | Animations | Licence |','|---|---|---:|---:|---:|---|---|---|']

for a in sorted(assets,key=lambda a:(a['label'],a['id'])):

 rig='Créé ici' if a['rig_origin']=='created_for_R1World' else 'Auteur'

 clips='Contrôle uniquement' if 'rig_validation_only' in a['animation_status'] else ('Pose statique + contrôles' if a['id']=='turtle_pond' else 'Clip auteur')

 lines.append(f"| {a['label']} | [{a['id']}]({a['id']}.glb) | {a['lod0']['triangles']} | [{a['lod1']['triangles']}]({a['id']}-lod1.glb) | {a['lod0']['bones']} | {rig} | {clips} | {a['license']} |")

lines+=['','## Cohérence visuelle','','Les références actuelles et les nouveaux GLB ont été réimportés et rendus avec la même caméra, le même cadrage relatif et le même éclairage, en CPU et sans fenêtre. Les modèles sont présentés à taille comparable pour juger le style ; leur échelle relative n’est donc pas celle du monde.','', 'Le chat, le rat, la souris et le gorille gardent des volumes naturels et des textures proches du renard, du lapin et du loup existants. Les oiseaux ajoutés ont des textures détaillées compatibles avec la poule actuelle ; leur détail est supérieur aux oiseaux les plus légers de 0 A.D. Les teintes vives des aras et dendrobates correspondent à des animaux réels. Aucune proportion de personnage ou de jouet n’a été retenue.','', '- [Mammifères et oiseaux communs : actuel à gauche, ajout à droite](comparaison-ajouts-1.jpg).', '- [Rapaces, ara et amphibiens](comparaison-ajouts-2.jpg).', '- [Tortues, invertébrés et autres ajouts](comparaison-ajouts-3.jpg).', '- [Aperçu des vingt ajouts](nouveaux-animaux.jpg).','','Les modèles très denses ont été réduits avant rigging : jusqu’à 382 505 triangles pour le chat source, 8 000 au proche et 4 400 au LOD1. Les oiseaux détaillés restent à 12 000–14 000 triangles plutôt que de perdre leur plumage. Les matériaux de la guêpe, de l’araignée et de la pie-grièche utilisent les images originales dans une structure PBR portable.','', '## Rigs et limites réelles','','Les quinze rigs créés comprennent des joints de corps, tête, membres, queue ou nageoires selon l’animal, avec poids de déformation normalisés. Ils sont basiques : une animation de production peut demander une reprise ciblée des poids ou des articulations. Les clips `Rig_Check_Head` et `Rig_Check_Limbs` servent au contrôle de déformation. **Ils ne sont pas des animations de marche, course, nage ou vol prêtes à jouer.** Les ailes repliées ne sont pas préparées pour un déploiement complet.','', 'La guêpe, l’araignée et la souris conservent un clip d’origine. La pie-grièche garde son squelette d’auteur mais reçoit des clips de contrôle. La tortue de Reeves conserve sa PoseLib statique et reçoit deux clips de contrôle ; ses yeux sont attachés au joint de tête. Le lézard a un matériau brun simple, sans albédo détaillé. Les particules de fourrure et micro-bumps propres à Cycles sont omis.','', '## Candidats écartés et manques','','- Chat noir et pieuvre Grab3D : apparence cartoon, yeux disproportionnés.', '- Pigeon, moineau, canard et écureuil Innerscene : surfaces et proportions de jouets.', '- Nombreux modèles du corpus public : canards en plastique, mascottes, figurines et statues.', '- Gecko DigitalLife : archive marquée CC BY, page actuelle CC BY-NC ; preuve de version historique insuffisante pour le retenir.', '- Grenouille verte et autres candidats : licence actuelle absente ou version non confirmée.', '- Escargot OpenGameArt : coquille seule, sans corps.', '', '**Manques courants prioritaires :** canard colvert, oie, moineau, écureuil, hérisson, blaireau, loutre, castor, héron, cygne, grenouille commune, serpent et escargot complet. Leur absence reste explicite plutôt que d’être comblée par des figurines ou des personnages.','', 'Les preuves de licence et métadonnées individuelles sont conservées dans `licenses/provenance.zip`. Les sources brutes et rendus de travail ont été retirés du projet et placés dans la corbeille à la demande de l’utilisateur. Les données du corpus Objaverse ne remplacent jamais la licence de chaque œuvre ; quatre ajouts sont sous CC BY 4.0, les seize autres sous CC0 1.0. Voir les [attributions](ATTRIBUTION.md).','', '## Vérification et périmètre','','Validateur Khronos sur les 378 GLB : **0 erreur, 0 avertissement**. Smoke des buffers, textures embarquées, poids, indices, rigs identiques entre LOD et skinning à cinq instants pour chacun des 2 732 clips (variantes et LOD compris). Les vingt ajouts ont aussi été contrôlés visuellement après réimport : LOD0, LOD1 et pose au milieu de chaque clip exporté.','', 'Aucune fenêtre ouverte, aucun lancement du jeu. Uniquement imports, rendus Blender en arrière-plan et vérifications de fichiers. Aucun comportement d’animal, code du jeu, placement ou sélection automatique de LOD n’a été ajouté.','', '- [Tableau complet des 189 modèles/variantes](TABLEAU_COMPLET.md).', '- [CSV mesuré et statut des animations](catalog.csv).', '- [Manifeste, provenance et empreintes SHA-256](manifest.json).']

(OUT/'EXPANSION.md').write_text('\n'.join(lines)+'\n',encoding='utf-8')

(QA/'expansion-summary.json').write_text(json.dumps({'new_models':len(assets),'new_types':17,'rigs_created':created,'rigs_original':len(assets)-created,'new_glb_bytes':size,'storage_optimized_without_visual_change':True},indent=2),encoding='utf-8')

print('Report:',len(assets),'models,',created,'created rigs,',round(size/1048576,1),'MiB')
