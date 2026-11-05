# Catalogue des animaux

189 modèles/variantes, 62 types et 378 GLB (237.3 Mio dans les rapports de mesure).
Les robes, sexes, âges et races ne sont pas des espèces distinctes.

## Formats

Chaque modèle possède un LOD0 et un LOD1 avec les mêmes os et noms de clips. Textures et animations sont embarquées ; limites de texture : 1024 px au proche, 512 px au LOD1. Axe vertical +Y, unités en mètres, échelle approximative. Les orientations de source sont conservées. Aucun décodeur Draco, Meshopt ou KTX requis.

Triangles : 56–14000 au proche, 38–7700 au LOD1.

## Références

- [Tableau complet](TABLEAU_COMPLET.md) : variantes et coûts par modèle.
- [Catalogue CSV](catalog.csv) : clips, auteur, licence et source.
- [Manifeste](manifest.json) : mesures par LOD, corrections et SHA-256.
- [Attributions](ATTRIBUTION.md) et [licences](licenses/).
- [Aperçu](preview.jpg).
- [Optimisation du stockage](OPTIMISATION.md).
- [Ajouts et limites des rigs](EXPANSION.md).
- [Pipeline de préparation](../../../data/animal-pipeline/README.md).

## Mesures et utilisation

Les rapports de fichiers sont conservés sous data/animal-pipeline/review/. Le smoke contrôle les buffers, textures, poids, indices, rigs et clips entre LOD, ainsi que le skinning à cinq instants par clip. Le validateur Khronos contrôle les deux LOD de chaque modèle.

Le catalogue inventorie les assets disponibles. Le placement dépend de fauna.json ; les mesures et rôles des clips utilisés sont dans animals.json. Certains rigs ne fournissent que des clips de contrôle, sans locomotion. Voir la [référence runtime](../../../docs/WORLD.md#animals) pour les comportements et limites.
