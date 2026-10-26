# Animaux préparés — 26 octobre 2026

189 modèles/variantes, regroupés en **62 types d’animaux**, avec un GLB et un GLB LOD1 chacun. Les robes, sexes, âges et races ne sont pas comptés comme des espèces distinctes.

**378 GLB, 234.9 Mio au total.** LOD0 : 56–14000 triangles ; LOD1 : 38–7700 triangles. Le LOD1 conserve en moyenne 57.0 % des triangles.

Les textures, squelettes et clips sont intégrés aux GLB. `nom.glb` est le modèle proche ; `nom-lod1.glb` est sa réduction avec le même squelette et les mêmes noms de clips. Textures limitées à 1024 px au proche et 512 px au LOD1. Pas de Draco, Meshopt ou KTX requis. Les tangentes des normal maps sont intégrées. Échelle en mètres approximative, axe vertical +Y ; les orientations de départ sont conservées.

## Catalogue

| Type | Modèles / variantes | Triangles LOD0 |
|---|---:|---:|
| Ara | 1 | 12000–12000 |
| Araignée | 1 | 1744–1744 |
| Bovins | 23 | 768–932 |
| Brochet | 1 | 1632–1632 |
| Cervidés | 5 | 306–4924 |
| Chat domestique | 2 | 7500–8000 |
| Chauve-souris | 1 | 7732–7732 |
| Cheval / poney | 70 | 1666–1714 |
| Chien | 3 | 849–856 |
| Chèvre / bouc | 8 | 366–508 |
| Cochon / porcelet | 6 | 303–303 |
| Corbeau | 1 | 542–542 |
| Crocodile | 1 | 700–700 |
| Dauphin | 1 | 7000–7000 |
| Dindon | 1 | 798–798 |
| Dromadaire | 3 | 2294–2294 |
| Fourmi | 1 | 1080–1080 |
| Gazelle | 1 | 306–306 |
| Girafe | 4 | 690–690 |
| Gnou | 1 | 638–638 |
| Gorille | 1 | 10000–10000 |
| Grenouilles / dendrobates | 3 | 4500–6362 |
| Guêpe | 1 | 1832–1832 |
| Hibou | 1 | 12000–12000 |
| Hippopotame | 2 | 1422–1422 |
| Lapin | 1 | 2640–2640 |
| Limule | 1 | 4368–4368 |
| Lion / lionne | 2 | 820–980 |
| Loup | 3 | 696–696 |
| Lézard | 1 | 4500–4500 |
| Mouette | 1 | 420–420 |
| Mouton | 2 | 254–254 |
| Ours brun | 1 | 2530–2530 |
| Ours noir | 1 | 2530–2530 |
| Ours polaire | 1 | 2528–2528 |
| Paon | 1 | 846–846 |
| Papillon monarque | 1 | 240–240 |
| Pie-grièche à bec jaune | 1 | 996–996 |
| Pigeon | 1 | 2508–2508 |
| Poisson générique / banc | 2 | 56–728 |
| Poule / coq | 2 | 2724–3452 |
| Pygargue à tête blanche | 1 | 12000–12000 |
| Rapace | 2 | 134–134 |
| Rat | 1 | 6500–6500 |
| Renard arctique | 1 | 640–640 |
| Renard roux | 1 | 640–640 |
| Requin | 2 | 292–292 |
| Rhinocéros | 1 | 792–792 |
| Rorqual | 1 | 872–872 |
| Rotengle | 1 | 1476–1476 |
| Rouge-gorge | 1 | 14000–14000 |
| Sanglier | 1 | 362–362 |
| Souris | 1 | 3020–3020 |
| Thon | 1 | 310–310 |
| Tigre | 1 | 686–686 |
| Tortue de Reeves | 1 | 7998–7998 |
| Tortue marine | 1 | 6500–6500 |
| Zèbre | 1 | 636–636 |
| Âne | 1 | 302–302 |
| Éléphant asiatique | 1 | 1538–1538 |
| Éléphant de forêt | 1 | 1666–1666 |
| Éléphant de savane | 2 | 1602–1666 |

## Mesures et vérifications

Validateur Khronos glTF : **0 erreur, 0 avertissement**. Smoke des 378 fichiers : buffers et textures embarqués, triangles, poids normalisés, indices d’os, conservation des rigs/clips entre LOD et déformation numérique sur cinq instants de chacun des 2732 clips (variantes et LOD compris).

Des réimports Blender ont aussi servi à vérifier visuellement les mouvements et les découpes alpha. Tous les rendus et imports ont tourné en arrière-plan, sans fenêtre ni lancement du jeu. Aucun code du jeu ou du moteur n’a été modifié.

## Fichiers utiles

- [Catalogue CSV](catalog.csv) : coût, licence, auteur, clips et source par animal.
- [Manifeste complet](manifest.json) : mesures par LOD, provenance, corrections et SHA-256.
- [Attributions](ATTRIBUTION.md) et [licences](licenses/).
- [Aperçu](preview.jpg) : sélection représentative issue des GLB finaux.
- [Provenance archivée](licenses/provenance.zip) : preuves de licence, métadonnées et empreintes historiques ; les sources brutes ont été nettoyées.
- Outils et rapports de smoke/revue : `data/animal-pipeline/`.

## Optimisation et nettoyage

Les textures conservent leurs dimensions et leurs pixels utilisés par les matériaux. La compaction retire uniquement les pistes de contrôleurs sans influence sur aucun sommet, en conservant tous les os, noms et durées de clips ; les données de géométrie et de mouvement visibles restent identiques. Les buffers, accessors et samplers strictement identiques sont partagés. Aucun décodeur supplémentaire n’est requis. Les sources brutes, doublons de GLB, rendus de travail et journaux ont été retirés du projet et placés dans la corbeille Windows. Voir `OPTIMISATION.md`.

## Sélection et limites

Priorité aux silhouettes naturelles et aux textures réalistes cohérentes. Les modèles de 0 A.D. forment le socle ; plusieurs animaux CC0 de Kelgar apportent des modèles plus détaillés et de nouveaux types. Les candidats cartoon, les figurines, les monstres et les textures de cadavres sont exclus. Des sources CC0 et CC BY 4.0 complètent le lot avec des silhouettes naturelles ; des rigs de déformation ont été créés pour les modèles statiques retenus. Les variantes de lapin de 184 triangles ont été remplacées par un lapin CC0 plus détaillé.

Les ajouts couvrent chats domestiques, rongeurs, oiseaux, amphibiens, tortues, primate et invertébrés. Le canard, l’oie, le moineau et l’écureuil restent à compléter : les candidats vérifiés étaient trop cartoon, des figurines ou sans preuve de licence suffisante. Voir `EXPANSION.md` pour les comparaisons et limites des nouveaux rigs.

Les GLB sont préparés et vérifiés comme fichiers. Leur compatibilité visuelle avec le rendu du jeu reste à vérifier lors d’une future intégration ; aucun comportement, placement ou changement automatique de LOD n’a été ajouté.
