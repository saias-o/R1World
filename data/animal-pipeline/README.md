# Pipeline des animaux

Les assets livrés sont sous `game/assets/animals/`. Leur
[manifeste](../../game/assets/animals/manifest.json) conserve mesures,
provenance, corrections et empreintes. Le placement en jeu dépend de
`game/assets/animals/fauna.json` ; tous les modèles du catalogue ne sont
pas utilisés.

## Vérifier et produire le catalogue

Depuis la racine du dépôt :

```powershell
python -B data/animal-pipeline/smoke_assets.py
npm.cmd ci --prefix data/animal-pipeline/deps --no-audit --no-fund
node data/animal-pipeline/validate_glb.cjs
python -B data/animal-pipeline/build_catalog.py
python -B data/animal-pipeline/make_inventory_table.py
python -B data/animal-pipeline/build_expansion_report.py
```

Le smoke vérifie buffers, textures embarquées, poids, indices, squelettes,
clips et skinning à cinq instants par clip. Le validateur Khronos produit
`review/gltf-validation.json`. `build_catalog.py` consomme ces deux rapports
et les rapports individuels préparés pour actualiser manifeste, CSV,
présentation et attributions. Ne pas les supprimer comme des fichiers temporaires.

Les générateurs de planches réutilisent les images finales si les caches de
rendu sont absents. Pillow est nécessaire aux planches ; Blender sert à la
conversion et à l'inspection des modèles. Les scripts de téléchargement et
de préparation exigent de récupérer les sources qui ne sont plus présentes.

## Provenance et optimisation

`game/assets/animals/licenses/provenance.zip` conserve les licences,
métadonnées et empreintes des sources. `build_catalog.py` peut y lire le
manifeste 0 A.D. si le cache brut est absent.

`compact_storage.py` retire les canaux de contrôleurs sans influence sur les
sommets et déduplique buffers, accessors et samplers. Il conserve les os,
clés visibles, noms et durées de clips. `compress_textures_lossless.py`
recompresse les PNG et retire un alpha seulement s'il n'est pas utilisé ;
les JPEG, dimensions et pixels utilisés restent intacts.

Les audits historiques `review/storage-equivalence.json` et
`review/storage-visual.json` documentent l'équivalence avant/après.
Leurs images comparatives finales sont dans la livraison.

## Intégration au jeu

`game/tools/r1/animals.py` mesure les modèles retenus par la faune et associe
les rôles des clips. `game/native/bird_animation.hpp` complète les poses
des oiseaux. Les clips `Rig_Check_*` servent à inspecter les déformations ;
ils ne constituent pas des animations de locomotion.

Voir la [référence runtime](../../docs/WORLD.md#animals) pour les comportements
et limites, et le [guide de développement](../../docs/DEVELOPMENT.md) pour les tests.
