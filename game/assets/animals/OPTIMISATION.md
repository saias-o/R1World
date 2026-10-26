# Optimisation et nettoyage — 26 octobre 2026

**240,6 Mio environ pour l’ensemble conservé**, livraison, outils et rapports compris. Le dossier `animals` représente environ 238,9 Mio, dont **234,9 Mio pour les 378 GLB**. Objectif de 250 Mio atteint.

Les GLB pesaient 313,5 Mio avant ce nettoyage. La compaction du stockage économise 75,6 Mio ; la recompression PNG économise encore près de 3 Mio. Tous les 189 modèles/variantes, 62 types, squelettes et LOD sont conservés.

- Suppression des pistes animées de contrôleurs qui n’influencent aucun sommet, directement ou par un descendant. Tous les os restent présents.
- Partage des buffers, accessors et samplers strictement identiques. Aucun rééchantillonnage, aucune quantification, aucun triangle supprimé.
- PNG recompressés et alpha retiré uniquement lorsqu’il est inutilisé par les matériaux. Aucune baisse de résolution, aucun changement des pixels utilisés par les shaders. Les JPEG restent intacts.
- Aucun décodeur de compression supplémentaire nécessaire.

L’audit exact des 378 fichiers confirme la conservation des positions, indices, poids, UV, normales, squelettes, clés d’animation visibles, noms et durées des clips, dimensions des textures et pixels utilisés. Huit comparaisons de rendu Blender avant/après donnent une différence de **zéro pixel** : [planche comparative](optimisation-comparaison.jpg).

Validateur Khronos : **0 erreur, 0 avertissement** sur les 378 GLB. Smoke complet réussi, avec déformation à cinq instants par clip. Aucun code du jeu modifié, aucune fenêtre ouverte.

Les sources brutes, sauvegardes temporaires, doublons, rendus intermédiaires, journaux et caches ont été retirés du projet et **placés dans la corbeille Windows**, donc récupérables. Ils occupent encore de l’espace dans la corbeille jusqu’à son vidage. Les licences et preuves de provenance sont conservées dans [provenance.zip](licenses/provenance.zip), avec auteurs, pages source et empreintes historiques. Les GLB complets restent disponibles et éditables.

Le catalogue, le tableau et les empreintes SHA-256 ont été actualisés. Les outils et rapports finaux restent dans `data/animal-pipeline/`.
