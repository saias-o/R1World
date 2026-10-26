# Extension réaliste des animaux — 26 octobre 2026

**20 modèles ajoutés, 17 types supplémentaires, 15 rigs créés et 5 rigs d’origine conservés.** Les 338 GLB précédents sont conservés ; l’ensemble a ensuite été compacté sans changement de géométrie, de squelette, de mouvement visible ni de pixels utilisés par les matériaux (voir `OPTIMISATION.md`). L’ensemble passe de 169 à 189 modèles/variantes et de 45 à 62 types ; 378 fichiers GLB au total.

Les ajouts occupent **34.6 Mio**, textures et clips compris. Chaque modèle possède un LOD1 avec le même squelette et les mêmes noms de clips. Les nombres ci-dessous sont des triangles mesurés dans les GLB finaux.

## Modèles retenus

| Animal / variante | Fichier LOD0 | Triangles LOD0 | Triangles LOD1 | Os | Rig | Animations | Licence |
|---|---|---:|---:|---:|---|---|---|
| Ara | [parrot_macaw](parrot_macaw.glb) | 12000 | [6600](parrot_macaw-lod1.glb) | 13 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Araignée | [spider_garden](spider_garden.glb) | 1744 | [958](spider_garden-lod1.glb) | 52 | Auteur | Clip auteur | CC0-1.0 |
| Chat domestique | [cat_sphynx](cat_sphynx.glb) | 7500 | [4125](cat_sphynx-lod1.glb) | 19 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Chat domestique | [cat_tabby](cat_tabby.glb) | 8000 | [4399](cat_tabby-lod1.glb) | 19 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Dauphin | [dolphin](dolphin.glb) | 7000 | [3850](dolphin-lod1.glb) | 8 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Gorille | [gorilla](gorilla.glb) | 10000 | [5500](gorilla-lod1.glb) | 16 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Grenouilles / dendrobates | [frog_amazon](frog_amazon.glb) | 5376 | [2955](frog_amazon-lod1.glb) | 16 | Créé ici | Contrôle uniquement | CC-BY-4.0 |
| Grenouilles / dendrobates | [frog_blue](frog_blue.glb) | 4500 | [2475](frog_blue-lod1.glb) | 16 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Grenouilles / dendrobates | [frog_red_eye](frog_red_eye.glb) | 6362 | [3498](frog_red_eye-lod1.glb) | 16 | Créé ici | Contrôle uniquement | CC-BY-4.0 |
| Guêpe | [wasp_realistic](wasp_realistic.glb) | 1832 | [1006](wasp_realistic-lod1.glb) | 40 | Auteur | Clip auteur | CC0-1.0 |
| Hibou | [owl_eagle](owl_eagle.glb) | 12000 | [6600](owl_eagle-lod1.glb) | 13 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Lézard | [lizard](lizard.glb) | 4500 | [2474](lizard-lod1.glb) | 19 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Pie-grièche à bec jaune | [shrike](shrike.glb) | 996 | [546](shrike-lod1.glb) | 65 | Auteur | Contrôle uniquement | CC0-1.0 |
| Pigeon | [pigeon_rock](pigeon_rock.glb) | 2508 | [1378](pigeon_rock-lod1.glb) | 13 | Créé ici | Contrôle uniquement | CC-BY-4.0 |
| Pygargue à tête blanche | [eagle_bald](eagle_bald.glb) | 12000 | [6600](eagle_bald-lod1.glb) | 13 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Rat | [rat_street](rat_street.glb) | 6500 | [3575](rat_street-lod1.glb) | 19 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Rouge-gorge | [robin](robin.glb) | 14000 | [7700](robin-lod1.glb) | 13 | Créé ici | Contrôle uniquement | CC0-1.0 |
| Souris | [mouse_brown](mouse_brown.glb) | 3020 | [1660](mouse_brown-lod1.glb) | 25 | Auteur | Clip auteur | CC-BY-4.0 |
| Tortue de Reeves | [turtle_pond](turtle_pond.glb) | 7998 | [4396](turtle_pond-lod1.glb) | 25 | Auteur | Pose statique + contrôles | CC0-1.0 |
| Tortue marine | [turtle_sea](turtle_sea.glb) | 6500 | [3575](turtle_sea-lod1.glb) | 10 | Créé ici | Contrôle uniquement | CC0-1.0 |

## Cohérence visuelle

Les références actuelles et les nouveaux GLB ont été réimportés et rendus avec la même caméra, le même cadrage relatif et le même éclairage, en CPU et sans fenêtre. Les modèles sont présentés à taille comparable pour juger le style ; leur échelle relative n’est donc pas celle du monde.

Le chat, le rat, la souris et le gorille gardent des volumes naturels et des textures proches du renard, du lapin et du loup existants. Les oiseaux ajoutés ont des textures détaillées compatibles avec la poule actuelle ; leur détail est supérieur aux oiseaux les plus légers de 0 A.D. Les teintes vives des aras et dendrobates correspondent à des animaux réels. Aucune proportion de personnage ou de jouet n’a été retenue.

- [Mammifères et oiseaux communs : actuel à gauche, ajout à droite](comparaison-ajouts-1.jpg).
- [Rapaces, ara et amphibiens](comparaison-ajouts-2.jpg).
- [Tortues, invertébrés et autres ajouts](comparaison-ajouts-3.jpg).
- [Aperçu des vingt ajouts](nouveaux-animaux.jpg).

Les modèles très denses ont été réduits avant rigging : jusqu’à 382 505 triangles pour le chat source, 8 000 au proche et 4 400 au LOD1. Les oiseaux détaillés restent à 12 000–14 000 triangles plutôt que de perdre leur plumage. Les matériaux de la guêpe, de l’araignée et de la pie-grièche utilisent les images originales dans une structure PBR portable.

## Rigs et limites réelles

Les quinze rigs créés comprennent des joints de corps, tête, membres, queue ou nageoires selon l’animal, avec poids de déformation normalisés. Ils sont basiques : une animation de production peut demander une reprise ciblée des poids ou des articulations. Les clips `Rig_Check_Head` et `Rig_Check_Limbs` servent au contrôle de déformation. **Ils ne sont pas des animations de marche, course, nage ou vol prêtes à jouer.** Les ailes repliées ne sont pas préparées pour un déploiement complet.

La guêpe, l’araignée et la souris conservent un clip d’origine. La pie-grièche garde son squelette d’auteur mais reçoit des clips de contrôle. La tortue de Reeves conserve sa PoseLib statique et reçoit deux clips de contrôle ; ses yeux sont attachés au joint de tête. Le lézard a un matériau brun simple, sans albédo détaillé. Les particules de fourrure et micro-bumps propres à Cycles sont omis.

## Candidats écartés et manques

- Chat noir et pieuvre Grab3D : apparence cartoon, yeux disproportionnés.
- Pigeon, moineau, canard et écureuil Innerscene : surfaces et proportions de jouets.
- Nombreux modèles du corpus public : canards en plastique, mascottes, figurines et statues.
- Gecko DigitalLife : archive marquée CC BY, page actuelle CC BY-NC ; preuve de version historique insuffisante pour le retenir.
- Grenouille verte et autres candidats : licence actuelle absente ou version non confirmée.
- Escargot OpenGameArt : coquille seule, sans corps.

**Manques courants prioritaires :** canard colvert, oie, moineau, écureuil, hérisson, blaireau, loutre, castor, héron, cygne, grenouille commune, serpent et escargot complet. Leur absence reste explicite plutôt que d’être comblée par des figurines ou des personnages.

Les preuves de licence et métadonnées individuelles sont conservées dans `licenses/provenance.zip`. Les sources brutes et rendus de travail ont été retirés du projet et placés dans la corbeille à la demande de l’utilisateur. Les données du corpus Objaverse ne remplacent jamais la licence de chaque œuvre ; quatre ajouts sont sous CC BY 4.0, les seize autres sous CC0 1.0. Voir les [attributions](ATTRIBUTION.md).

## Vérification et périmètre

Validateur Khronos sur les 378 GLB : **0 erreur, 0 avertissement**. Smoke des buffers, textures embarquées, poids, indices, rigs identiques entre LOD et skinning à cinq instants pour chacun des 2 732 clips (variantes et LOD compris). Les vingt ajouts ont aussi été contrôlés visuellement après réimport : LOD0, LOD1 et pose au milieu de chaque clip exporté.

Aucune fenêtre ouverte, aucun lancement du jeu. Uniquement imports, rendus Blender en arrière-plan et vérifications de fichiers. Aucun comportement d’animal, code du jeu, placement ou sélection automatique de LOD n’a été ajouté.

- [Tableau complet des 189 modèles/variantes](TABLEAU_COMPLET.md).
- [CSV mesuré et statut des animations](catalog.csv).
- [Manifeste, provenance et empreintes SHA-256](manifest.json).
