# Optimisation du stockage des animaux

La compaction conserve géométrie, squelette complet, mouvement visible,
noms et durées des clips :

- Retrait des pistes de contrôleurs sans influence directe ou indirecte sur
  un sommet ; les os restent présents.
- Partage des buffers, accessors et samplers identiques, sans quantification
  ni rééchantillonnage.
- Recompression PNG et retrait d'alpha inutilisé, sans modifier résolution
  ni pixels utilisés par les matériaux ; JPEG inchangés.
- Aucun décodeur supplémentaire requis.

L'audit du 26 octobre 2026 porte sur 378 GLB : 313,5 Mio avant,
234,9 Mio après. Le contrôle exact est conservé dans
`data/animal-pipeline/review/storage-equivalence.json` ;
huit rendus avant/après donnent zéro pixel différent dans
`review/storage-visual.json` et la [planche comparative](optimisation-comparaison.jpg).

Ces résultats concernent cette opération de stockage. Les adaptations de clips
et comportements ultérieures sont décrites dans la [référence du jeu](../../../docs/WORLD.md#animals).

Les licences et preuves de source restent dans
[provenance.zip](licenses/provenance.zip). Les procédures de préparation et
de vérification sont dans le [guide du pipeline](../../../data/animal-pipeline/README.md).
