# R1World — règles de travail

`docs/PLAN.md` dit **ce qu'est** le projet : thèse, contraintes, invariants,
décisions. Ce fichier dit ce qu'il ne faut pas faire, parce que chaque règle ci-dessous a été apprise en la
cassant.

---

## 1. Ne jamais remplacer un asset existant par un moins bon

**C'est la règle numéro un et elle a été cassée deux fois.**

Le projet embarque des arbres **photoscannés Poly Haven** — `fir_sapling`,
`pine_sapling_small`, `quiver_tree_01`, `quiver_tree_02`. En ajoutant les props au monde
(9 septembre 2026), un kit Kenney basse-poly a été branché pour la végétation
alors que ces arbres-là étaient déjà sur le disque, déjà normalisés, déjà
inscrits dans `assets/THIRD_PARTY_ASSETS.json`.

**La barre visuelle du projet est fixée par son meilleur asset existant, pas par
ce qui est commode pour la fonctionnalité en cours.** Une fonctionnalité qui
abaisse cette barre est une régression vendue comme un progrès, et aucun test ne
la voit : seul le joueur la voit, et seulement en ouvrant le jeu.

En pratique :

- Avant de choisir un asset, **regarder d'abord ce qui est déjà sur le disque**
  et dans `assets/THIRD_PARTY_ASSETS.json`. Le réutiliser.
- Un nouvel asset peut **élargir** la couverture (une essence qui manque, un
  objet sans équivalent). Il ne peut jamais **déplacer** un meilleur asset qui
  couvre déjà le cas.
- Si l'asset existant ne tient pas une contrainte (budget, arène, nombre), **le
  dire et demander** — jamais substituer quelque chose de moins cher en silence.
- Quand la couverture manque vraiment, chercher un asset **du même grade**
  (CC0 photoscanné, Poly Haven) avant de descendre vers un kit stylisé.
- Les kits Kenney conviennent aux objets sans meilleur équivalent dans le projet
  — lampadaires, bancs, arrêts de bus, poteaux. **Pas aux arbres.**

`tools/r1/tests/test_props.py` tient cette règle : un test échoue si une essence
d'arbre pointe ailleurs que sur les modèles photoscannés.

## 2. Les couleurs d'un kit sont de la peinture, pas des albédos

Appris deux fois le même jour, sur les sols puis sur les arbres. Kenney peint le
feuillage `(0,16 · 0,79 · 0,67)` — un turquoise à quatre fois l'albédo d'une
feuille — et la première table de sols donnait 0,38 à l'asphalte et 0,78 au sable
sec. À la lumière de ce monde, tout ce qui dépasse ~0,35 sature : la palette
entière s'effondre en un seul blanc.

Toute valeur de couleur entrant dans le jeu est un **albédo mesuré** : eau
0,03–0,06 · asphalte et béton 0,10–0,15 · forêt 0,08–0,15 · herbe 0,18–0,25 ·
cultures et roche 0,15–0,25 · sable sec 0,30–0,40 · neige fraîche 0,80–0,90.
`normalize.repaint_kit_model` refuse tout matériau que la palette ne nomme pas
(PLAN §4 : *un asset non conforme n'entre pas*).

## 3. Un refus doit se dire, jamais ressembler à de la lenteur

Trois fois dans `native/world.cpp` : le compteur de tuiles figé, la tuile qui ne
tenait pas dans l'arène, le spawn refusé dans l'eau. Chaque fois, le code
*savait* et se taisait, et le symptôme était un délai de trois minutes qui
accusait le réseau.

Tout refus écrit une ligne de journal nommant la raison, fait échouer le test de
fumée sur cette raison-là, et dit quelque chose au joueur.

## 4. Ce qui est mesuré bat toujours ce qui est inféré

PLAN §3 I5. Une hauteur taguée, une forme de toit taguée, une emprise relevée : rien
d'inféré ne les écrase, et le manifeste écrit lequel des deux a répondu. C'est
aussi pourquoi une église garde son emprise OSM et reçoit une flèche générée
plutôt qu'un modèle d'église posé par-dessus.

## 5. L'arène fait 1 048 576 sommets et ce n'est pas un choix

`GeometryRegistry::kDefaultMaxVertices`. R1World se lie au moteur, il ne le
recompile pas. Toute mesure de géométrie se compare à ce nombre, et les budgets
(`TILE_VERTEX_BUDGET`, `kResidentVertices`) en découlent au lieu d'être choisis.

Les props sont des **nœuds de scène**, jamais de la géométrie de tuile : le
`MeshCache` indexe par asset, donc six cents nœuds pointant sur un arbre
téléversent cet arbre une fois.

## 6. Vérifier hors ligne

Le cache est une promesse : un lieu déjà visité ne touche plus le réseau.

```powershell
$env:HTTP_PROXY = "http://127.0.0.1:9"; $env:HTTPS_PROXY = "http://127.0.0.1:9"
$env:SAIDA_WINDOW_HIDDEN = "1"
cd game
generated\python-runtime\python.exe tools\play_world.py --smoke --spawn 2.3522 48.8566
python -m unittest discover -s tools\r1\tests -t tools
```

Le test de fumée tourne sous le Python embarqué du jeu : ses dépendances
compilées (shapely) ne se chargent pas sous un autre interpréteur. Pour
recompiler le moteur, `C:\msys64\ucrt64\bin` doit être **en tête** du PATH ;
sinon le lien de `SaidaEngineRuntime.exe` échoue avec un `ld returned 116` sans
autre message (`engine/AGENTS.md`).

Un journal de worker vide à la fin d'un run est ce qui prouve la promesse.
