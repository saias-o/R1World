# Pipeline des animaux — état après nettoyage

Livraison : `game/assets/animals/`. Le catalogue et son manifeste sont les références pour les 189 modèles/variantes, 62 types et 378 GLB. Aucun comportement ni code du jeu modifié.

Les sources brutes, doublons, rendus intermédiaires, journaux et caches ont été placés dans la corbeille à la demande de l’utilisateur. Les preuves de licence, métadonnées et empreintes historiques sont dans `game/assets/animals/licenses/provenance.zip`. Les téléchargements originaux ne sont plus dans l’espace de travail. Les outils de téléchargement et préparation restent disponibles, mais leur utilisation nécessite de récupérer à nouveau les sources.

## Optimisation finale

`compact_storage.py` retire les canaux des contrôleurs qui ne peuvent influencer aucun sommet et partage les buffers, accessors et samplers identiques. Il conserve tous les os, les clés visibles, les noms et durées de clips, sans simplifier ni quantifier la géométrie.

`compress_textures_lossless.py` conserve les dimensions et pixels utilisés par les matériaux. Aucun JPEG n’est réencodé ; les PNG sont recompressés et leur alpha est retiré uniquement lorsqu’il n’est pas utilisé. Aucun décodeur supplémentaire requis.

`review/storage-equivalence.json` documente l’équivalence exacte des 378 fichiers avec leur copie avant optimisation. `review/storage-visual.json` confirme zéro différence de pixel sur huit rendus avant/après ; la planche est conservée dans la livraison. Les copies de comparaison et rendus bruts ont ensuite été nettoyés.

## Vérifier ou reconstruire le catalogue

Les rapports finaux `review/smoke.json` et `review/gltf-validation.json` sont conservés : 378 fichiers, aucune erreur ni avertissement. `smoke_assets.py` vérifie buffers, textures, poids, indices, squelettes, clips et skinning à cinq instants par clip.

```powershell
python -B data/animal-pipeline/smoke_assets.py
npm.cmd ci --prefix data/animal-pipeline/deps --no-audit --no-fund
node data/animal-pipeline/validate_glb.cjs
python -B data/animal-pipeline/build_catalog.py
python -B data/animal-pipeline/make_inventory_table.py
python -B data/animal-pipeline/build_expansion_report.py
```

Le manifeste existant porte les métadonnées de provenance après nettoyage. `build_catalog.py` peut lire le manifeste 0 A.D. depuis l’archive de preuves ; il ne recrée pas les sources brutes. Les générateurs réutilisent les planches finales déjà conservées quand les caches de rendu sont absents.

Blender a été utilisé uniquement avec `-b -t 2`, sans interface. Les rigs créés ici sont basiques avec clips de contrôle ; ils ne constituent pas des animations de locomotion prêtes à jouer. Voir `EXPANSION.md` pour les limites et `OPTIMISATION.md` pour les tailles et garanties.
