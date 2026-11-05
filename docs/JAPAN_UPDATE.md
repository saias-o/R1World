# Japon — règles de contenu et références

Les profils sont dans `game/assets/world/atlas.json`. Les observations de
dimensions, usages et formes de toit priment sur les règles ci-dessous.

## Références photographiques

Les paramètres reproductibles et les crédits sont dans
`game/tools/japan_views.json`. La planche locale se trouve dans
`game/generated/japan-update/index.html` après exécution de
`python tools/capture_japan.py before`, puis `after` après modification de l'Atlas.
Le programme réutilise l'API de capture existante, avec caméra géographique fixe,
soleil daté, météo imposée et fenêtre cachée. Il refuse les captures dont le
journal signale des erreurs ou des observations provisoires.

| Lieu | Position publiée (latitude, longitude) | Source |
|---|---|---|
| Tsumago-juku, Nakasendo | 35.577542, 137.595610 | [DimiTalen, 29 mars 2016, CC0](https://commons.wikimedia.org/wiki/File:Looking_up_the_Nakasend%C5%8D_in_the_central_part_of_the_village,_Tsumago-juku,_Nagiso,_2016.jpg) |
| Kyoto, Sannenzaka | 34.997847, 135.780992 | [Soramimi, 17 décembre 2013, CC BY-SA 3.0](https://commons.wikimedia.org/wiki/File:Sanneizaka_(Sannenzaka)_near_Kiyomizudera_Temple.JPG) |
| Tokyo, Shibuya Crossing | 35.659350, 139.700867 | [Hide1228, 24 février 2016, CC BY-SA 4.0](https://commons.wikimedia.org/wiki/File:Shibuya_crossing_1.jpg) |

Les photographies restent hébergées sur Wikimedia Commons ; la planche les
affiche sans les retoucher. GPS, cap et focale viennent des fiches originales.
Hauteur de caméra : 1,65 m estimé ; inclinaison : horizontale, non mesurée.
La météo est une estimation visuelle, pas une station historique. Le HDR et
Photoshop déclarés sur le cliché de Shibuya interdisent d'en extraire des
albédos mesurés. Les photos de 2013–2016 ne garantissent pas l'état actuel des lieux.

## Réglages

- Profils `JAPAN` et `TOKYO` : palettes des façades, toits et menuiseries moins
  claires. Ce sont des estimations prudentes de matériau, pas une mesure
  radiométrique. Les distributions urbaines de hauteurs sont conservées.
- Profil `JP_TSUMAGO` : petit secteur de Tsumago-juku, murs d'un à deux niveaux,
  bois brun sombre mêlé d'enduit, toits gris et menuiseries sombres.
- Profil `JP_HIGASHIYAMA` : secteur Sannenzaka/Ninenzaka, mêmes principes,
  étages de 2,55 m, rez-de-chaussée de 2,70 m, débords de 0,75 m et fenêtres
  plus larges que hautes. Il ne s'applique pas à tout Kyoto ni à tout le Japon.
- Règle résidentielle Kiso existante : menuiseries sombres, palette harmonisée
  et majorité de maisons à deux niveaux. Variante locale de Higashiyama.
  Ces deux règles utilisent `architectureOnly` : maisons et petits commerces
  historiques sont éligibles dans des rangées plus denses, sans jardin inventé.
  Les bâtiments publics, les usages incompatibles et les dimensions observées
  restent protégés. Surface maximale : 600 m² ; couverture locale maximale :
  40 % pour Kiso, 55 % pour Higashiyama. Les donneurs de hauteur voisins sont
  restreints aux logements compatibles ; ceux déclarant plus de deux étages
  sont exclus. Des hauteurs empruntées restent néanmoins possibles.

Les rectangles sont des secteurs approximatifs, pas des limites patrimoniales
officielles. La sélection du profil principal s'effectue à l'échelle de la
tuile ; la règle résidentielle utilise la position du bâtiment. Les données
observées de dimensions et formes de toit gardent la priorité. Aucun nouveau
jardin n'est autorisé dans une rangée dense. Aucune emprise OSM n'est retouchée.

## Limites visibles

Le système existant ne reproduit pas les treillis de machiya, les doubles
avant-toits, les enseignes de chaque commerce, les équipements propres aux
rues japonaises ni les façades vitrées exactes des grands immeubles. Les
textures communes de toiture gardent leur comportement spéculaire : changer
la rugosité d'un swatch ne remplace pas leur carte de matériau. Une comparaison
de couleurs et de gabarits ne prouve pas une reconstitution fidèle de chaque
bâtiment. Les décalages du relief et les approximations des sources restent
visibles et ne sont pas corrigés par déplacement de caméra.

## Cerisiers japonais

Le cerisier retenu est le modèle Innerscene. Son identité et son empreinte
sont vérifiées par `tools/r1/sakura.py` ; ne pas lui substituer un autre asset.

[Flowering Cherry Tree, Innerscene, CC0](https://www.innerscene.com/tools/library/3d-parts/flowering-cherry-tree-6d80049e)
est enregistré dans `THIRD_PARTY_ASSETS.json` et `nature_selected/SOURCES.json`.
URL reproductible : `https://www.innerscene.com/api/library/flowering-cherry-tree-6d80049e/download`.
SHA-256 : `2412baf8c41da00801512b56d1dbcba6025f983e2470cdd7a2c3f9703ba91b12`.

`tools/r1/sakura.py` produit trois niveaux à partir de ce modèle :

| Niveau | Sommets | Triangles | Traitement |
|---|---:|---:|---|
| Near | 155 285 | 56 580 | Géométrie et textures originales ; 95 meshes réunis en 6 matériaux |
| Mid | 46 445 | 20 300 | Fleurs et feuilles entières choisies selon leur couverture visuelle ; branches conservées |
| Far | 4 | 2 | Atlas de 40 vues du Mid, éclairage dynamique, procédé des arbres actuels |

Le pivot reste au pied du tronc, hauteur normalisée à un mètre. La hauteur
observée dans OSM prime ; sinon, estimation de 6 m avec la variation existante.
Near et Mid coûtent ensemble environ 19,2 % de l'arène de sommets, partagés
entre instances. Il s'agit donc d'une nouvelle essence relativement lourde.

Le sélecteur reconnaît, **partout dans le monde**, `Prunus serrulata`,
`Prunus [×/x] yedoensis`, `Prunus subhirtella`, et tout genre ou espèce
`Cerasus` renseigné. Ces observations utilisent toujours le cerisier choisi,
en ville comme à la campagne, y compris dans l'hémisphère sud.
Un simple `Prunus`, un prunier ou un pêcher ne suffit pas.
Deux observations concrètes à Kyoto : nœuds OSM
9706471830 (135.7887284, 34.9997382) et 9706484898 (135.7884534, 34.9999324),
étiquetés `Cerasus × yedoensis` dans les données en cache.

Les arbres urbains sans genre ni espèce renseignés reçoivent une sélection inférée :

- **Japon : 15 %** de probabilité par arbre éligible.
- **Reste de l'hémisphère nord (latitude > 0) : 5 %**.
- Aucun taux supplémentaire dans l'hémisphère sud ni à l'équateur. Les 15 %
  japonais remplacent les 5 %, sans cumul.

**Uniquement les villes, hors villages.** Il faut un lieu OSM `place=city` ou `place=town` à moins de 1500 m
de l'arbre, sans `place=village/hamlet` aussi proche ou plus proche. Sans
identification locale de ville dans les observations, aucun ajout par taux.
Ce critère conservateur ne prétend pas reconstituer les limites municipales ;
il peut omettre les quartiers dont le centre de ville n'est pas observé à proximité.

Cette identification de ville est obligatoire **en plus** du caractère urbain
à la position de chaque arbre : emprise OSM
`landuse=residential/commercial/retail/industrial`, ou au moins 25 bâtiments
principaux dans un carré de 350 m. Les garages, granges, serres et autres
annexes agricoles ne comptent pas. Ni une zone résidentielle seule, ni sa
densité, ni un parc seul ne suffisent. Les emprises boisées, forêts, vergers, terres agricoles, vignobles,
prairies et autres espaces naturels exclus, ainsi que les arbres inférés de
bords de route ruraux, ne reçoivent pas ces pourcentages. Un parc urbain peut
en recevoir si son emplacement répond aux critères urbains.

Les espèces et genres renseignés, ainsi que les arbres explicitement à aiguilles,
restent prioritaires. Le tirage dépend de l'identifiant de l'arbre et conserve
le même résultat à chaque visite ; les proportions locales peuvent donc varier
autour de 15 % ou 5 %. Les cerisiers observés s'ajoutent aux cerisiers inférés.
Les statistiques distinguent `botanical-tag`, `urban-cherry-japan-inference`
et `urban-cherry-north-inference`.

La sélection des forêts de résineux est prioritaire :
à l'intérieur d'une forêt explicitement identifiée comme conifère, tous les arbres
utilisent le pin adulte, y compris les points contradictoires étiquetés cerisier.
Les règles des cerisiers restent applicables ailleurs (voir [WORLD.md](WORLD.md#trees)).

**Limite : le modèle représente une floraison fixe.** Il n'existe pas encore
de cycle saisonnier de végétation dans l'Atlas ; cette modification n'en invente
pas un. Les arbres existants ne sont pas recolorés ; les sélections ci-dessus
utilisent les trois niveaux du modèle de cerisier.
