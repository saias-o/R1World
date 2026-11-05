# R1World — règles de travail

`docs/PLAN.md` porte les objectifs, invariants et travaux ouverts.
`game/README.md` décrit l'utilisation du jeu ; `docs/WORLD.md` son fonctionnement
technique ; `docs/DEVELOPMENT.md` la compilation et les vérifications.
L'historique des changements reste dans Git.

## 1. Préserver la qualité des assets

- Examiner les assets existants et `game/assets/THIRD_PARTY_ASSETS.json` avant
  d'en choisir un nouveau. Réutiliser le meilleur asset qui couvre le besoin.
- Ne pas remplacer un asset par un modèle de qualité inférieure. Si le budget
  rend l'asset existant inutilisable, exposer la contrainte et demander un choix.
- Pour une couverture manquante, chercher d'abord un asset du même grade.
  Les kits Kenney conviennent aux objets sans meilleur équivalent, pas aux arbres
  déjà couverts par les scans Poly Haven.
- Vérifier le résultat visuellement ; les tests de fichiers ne détectent pas
  une dégradation de silhouette, de texture ou d'animation.

## 2. Utiliser des albédos mesurés

Plages de référence : eau 0,03–0,06 ; asphalte et béton 0,10–0,15 ; forêt
0,08–0,15 ; herbe 0,18–0,25 ; cultures et roche 0,15–0,25 ; sable sec
0,30–0,40 ; neige fraîche 0,80–0,90. Les couleurs peintes des kits ne sont
pas des mesures. `normalize.repaint_kit_model` refuse les matériaux absents
de sa palette.

## 3. Rendre les refus explicites

Un refus nomme sa raison dans le journal, est visible pour le joueur et fait
échouer le smoke concerné sur cette raison. Une erreur connue ne doit pas
ressembler à un chargement sans fin.

## 4. Faire primer les observations

Une hauteur, une forme de toit ou une emprise observée prime sur une estimation.
Le manifeste distingue données mesurées, inférences et observations manquantes
(PLAN §3 I5). Un monument généré conserve son emprise OSM.

## 5. Respecter les budgets du moteur

Comparer les budgets de génération et de résidence à la capacité réelle de
`GeometryRegistry` : 1 048 576 sommets par défaut. Les props sont des nœuds
sur des maillages partagés par `MeshCache`, jamais des copies dans la géométrie
des tuiles.

## 6. Placer les responsabilités au bon endroit

- Capacité générique indispensable (physique, animation, LOD, streaming) : moteur.
- Capacité générique optionnelle (trafic, faune, réseau) : plugin moteur.
- Géographie, observations OSM, tuilage et règles de R1World : jeu.

Compléter une API moteur manquante au lieu de la contourner dans le jeu.
Le moteur ne connaît pas R1World. Suivre `engine/AGENTS.md` pour le modifier.
Mesurer un problème de coût avec `--profile <trace.json>` dans le build
optimisé `engine/build-rel` (RelWithDebInfo), utilisé par `game/Play.ps1`.

## 7. Vérifier hors ligne

Un lieu déjà visité doit fonctionner sans réseau de données géographiques.
Le vérifier avec les proxys fermés et un smoke sur ce lieu, selon
`docs/DEVELOPMENT.md`. Les échanges P2P sont distincts du cache du monde.
Pour compiler sous Windows, placer `C:\msys64\ucrt64\bin` en tête du PATH ;
`Play.ps1` et `build_tools.sh` le font eux-mêmes.

## 8. Travailler sur main

Ne créer aucune branche ni worktree, dans ce dépôt comme dans `engine/`.
Travailler sur `main`, sans changement de branche ni réécriture de son historique.
