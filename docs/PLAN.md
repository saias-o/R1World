# R1World

**Reproduire la Terre entière à l'échelle 1:1, jouable, sur SaidaEngine.**

Ce document garde ce qui ne change pas : la thèse, les contraintes, les
invariants et les décisions prises. Il ne contient ni feuille de route ni liste
de chantiers.

**Deux critères priment sur tous les autres, et ils sont en tension :**

1. **Le monde doit ressembler à la Terre.** Pas à une terre plausible : à *la*
   Terre, à l'endroit précis où le joueur se tient.
2. **Cela doit tourner sur un Core i5 avec une GTX 1060.** C'est la machine de
   référence, celle qui contraint le design. Une RTX 4070 n'est qu'un preset
   supérieur.

Ce qui produit la ressemblance (§2) coûte peu, et ce qui coûte cher produit peu.

---

## 1. La thèse

> **Le monde n'est pas stocké. Il est décrit, puis régénéré à l'identique chez
> chaque joueur.**

Ce qui transite n'est jamais de la géométrie, mais une description sémantique
compacte (emprise, niveaux, toit, matériau, mitoyenneté). La géométrie est
fabriquée par le client, de façon déterministe. Trois conséquences :

1. **Le volume s'effondre** : un rapport de 100 à 1 000 entre la géométrie et sa
   description.
2. **Le déterminisme remplace la synchronisation** : même générateur, même
   tuile, même ville au triangle près. Le multijoueur ne transporte que des
   acteurs.
3. **Le monde s'améliore sans être re-livré** : améliorer un générateur repeint
   la planète.

Ce qui distingue R1World d'une extrusion OSM de plus :

- **(a) La frontière données/génération est au zoom 14.** En deçà, données
  réelles ; au-delà, tout est synthétisé et jamais stocké.
- **(b) L'Atlas de styles régionaux.** Un bâtiment sans attributs hérite de la
  distribution de *sa* région, pas d'un défaut mondial. C'est le premier facteur
  de ressemblance, loin devant la qualité des modèles 3D.
- **(c) L'inférence comble le vide sans le masquer**, et elle est marquée comme
  telle.
- **(d) La boucle de retour vers OSM** : une correction faite en jeu part en
  proposition OSM.

---

## 2. Ce que « ressembler à la Terre » veut dire

Classement par contribution réelle à « je reconnais cet endroit ». Il gouverne
l'allocation de l'effort.

| Rang | Facteur | Origine | Précision |
|---|---|---|---|
| 1 | Silhouette du relief et horizon | Copernicus DEM 30 m | Exacte |
| 2 | Trame urbaine : routes, îlots, orientation | OSM | Exacte |
| 3 | Emprises des bâtiments | OSM + Google/Microsoft | Exacte |
| 4 | Lumière : soleil réel, atmosphère | Éphémérides + date | Exacte |
| 5 | Hauteurs et gabarits | OSM partiel → Atlas | Mesurée ou inférée |
| 6 | Matériaux et couleurs de façade | Atlas | Inférée — *le point critique* |
| 7 | Toitures | OSM partiel → Atlas | Inférée |
| 8 | Végétation | WorldCover + Atlas | Inférée par biome |
| 9 | Sols et revêtements | OSM partiel → règles | Inférée |
| 10 | Mobilier urbain régional | Atlas + kits | Inférée |
| 11 | Détail de façade | Kit-of-parts + Atlas | Synthétisée |
| 12 | Micro-détail | Procédural | Synthétisée |

Les quatre premiers rangs sont **mesurés**, donc exacts et quasi gratuits. Le
travail créatif commence au rang 5 et il est porté par l'Atlas.

**On ne reproduit pas :** les intérieurs, les personnes réelles et les plaques,
les enseignes de marques, la photogrammétrie (elle ne se corrige pas), ni
l'exactitude au-delà de la donnée disponible.

---

## 3. Les invariants

### I0 — La fidélité arbitre
En cas de conflit, on réduit la portée (zone, densité), jamais la justesse. Seule
exception : les plafonds de I4.

### I1 — Aucune coordonnée ECEF n'atteint le moteur
Le moteur ne voit que de l'ENU local, ancré près du joueur. Mesuré : le
`float32` n'est pas ce qui limite la vue (2,4 cm de pas à 200 km). Ce qui limite,
c'est le jitter caméra/physique, d'où une **origine flottante rebasée**
(aujourd'hui tous les 350 m dans `native/world.cpp`).

| Distance à l'ancre | Pas float32 | Chute due à la courbure |
|---|---|---|
| 1 km | 0,12 mm | 0,08 m |
| 10 km | 1,2 mm | 7,8 m |
| 200 km | 2,4 cm | 3 140 m |

### I2 — La courbure est du contenu, pas une erreur
L'ENU est rigide. Aucune « correction de courbure » nulle part. Horizons :
1,7 m → 4,7 km · 50 m → 25 km · 4 000 m → 226 km · 10 000 m → 357 km.

### I3 — Le déterminisme est un contrat versionné
`géométrie = G(version_générateur, quadkey, données_tuile, atlas)`. Aucune
horloge, aucun aléa non seedé. Le seed dérive du quadkey. Changer un générateur
incrémente sa version et invalide le cache, jamais en silence.

### I4 — Le budget est un contrat
Machine de référence : **Core i5 4 cœurs, GTX 1060 6 Go, 1080p / 60 fps.**

| Ressource | 1060, 1080p | 4070, 1440p |
|---|---|---|
| VRAM | **4,5 Go** | 7 Go |
| RAM | **6 Go** | 10 Go |
| Triangles visibles | **4 M** | 12 M |
| Draw calls CPU | **1 500** | 4 000 |
| Frame CPU, thread principal | **10 ms** | 8 ms |
| Streaming + génération | **1,5 ms/frame**, 2 threads | 2 ms, 6 threads |

Sur i5, **c'est le CPU la ressource rare**, pas le GPU. L'arène de géométrie du
moteur est fixée à **1 048 576 sommets** (`GeometryRegistry::kDefaultMaxVertices`)
et tous les budgets de sommets en découlent (voir `CLAUDE.md` §5).

### I5 — Le jeu sait toujours ce qu'il ignore
Chaque élément porte sa provenance : `mesuré`, `inféré` ou `synthétisé`. Ce qui
est mesuré n'est jamais écrasé par ce qui est inféré, et le manifeste de chaque
tuile dit lequel a répondu.

---

## 4. Les données sources

| Couche | Source | Licence |
|---|---|---|
| Bâti, voirie, eau, usage du sol | OpenStreetMap | ODbL |
| Élévation | Copernicus DEM GLO-30 | libre, attribution |
| Bathymétrie | GEBCO | libre |
| Occupation du sol | ESA WorldCover | CC BY 4.0 |
| Bâti hors OSM | Google Open Buildings, Microsoft Footprints | CC BY / ODbL |
| Population | GHSL | libre |
| Toponymes | GeoNames | CC BY |
| Photos de validation | Mapillary | CC BY-SA, consultées, non redistribuées |

Les données de tuiles dérivées d'OSM sont publiées sous ODbL avec attribution
visible en jeu. Aucune source `CC BY-NC`. Les assets sont **CC0 exclusivement**
(Poly Haven, ambientCG, Quaternius, Kenney), normalisés avant d'entrer : échelle
métrique, albédos mesurés, un asset non conforme n'entre pas.

---

## 5. Le tuilage

Grille Web Mercator standard (z/x/y + quadkey). La distorsion en cos(lat) n'est
pas corrigée : on charge plus de tuiles en haute latitude, et elles sont plus
légères.

Six couches au contenu différent, chargées indépendamment. **Seuls les rayons
changent entre presets, jamais le contenu** (sinon deux joueurs ne verraient pas
le même monde, I3).

| Couche | Zoom | Tuile (équateur) | Rayon 1060 | Rayon 4070 | Contenu |
|---|---|---|---|---|---|
| L0 globe | 5 | 1 252 km | planète | planète | Coque, océan, relief grossier |
| L1 région | 9 | 78 km | 300 km | 300 km | Relief lointain, littoral, neige |
| L2 paysage | 12 | 9,8 km | 40 km | 60 km | Terrain, sols, canopée, villes en masses |
| L3 local | 14 | 2,4 km | 6 km | 12 km | Routes, bâti en blocs |
| L4 rue | 16 | 611 m | 1 km | 2 km | Bâtiments détaillés, arbres individuels |
| L5 détail | 17 | 305 m | 200 m | 400 m | Mobilier, véhicules, clutter |

**Pourquoi z14 est la frontière :** 2²⁸ ≈ 268 M tuiles z14 (≈ 78 M émergées)
contre 2³² ≈ 4,3 Md en z16. Pré-générer z16 est hors de portée ; z14 tient.

**Le détail suit la vitesse.** On ne voit pas les poignées de porte à 130 km/h :
au-delà de 15 km/h le mobilier n'est plus importé et les rayons de végétation
baissent, tout revient quand on ralentit.

---

## 6. Décisions prises

1. **Licence et distribution.** Jeu gratuit, en ligne. Données OSM dérivées sous
   ODbL avec attribution visible.
2. **Moteur : dépôt Saida direct, sans fork.** Une modification moteur n'est
   admise que si elle améliore le moteur en général ; aucune exception
   spécifique à R1World. Ce qui est propre au jeu se compile dans le binaire du
   jeu (ex. `engine/plugins/traffic`), jamais dans la bibliothèque du moteur.
3. **L'heure est réelle.** Arriver de nuit à Tokyo est l'heure qu'il y est.
   L'instant est un paramètre de scène (`WORLD_EPOCH`), pas du modèle.
4. **Le ciel suit l'heure.** Une journée de ciels photographiés (série Qwantani,
   Poly Haven, CC0), fondus deux à deux selon la vraie hauteur du soleil. Chaque
   photo est placée à la hauteur mesurée de son soleil, tournée vers le vrai
   azimut, et calée sur la couleur d'horizon du modèle (le brouillard). Le
   soleil des photos est retiré ; le moteur dessine un seul disque, là d'où
   vient la lumière (`r1/skies.py`).
5. **La voiture n'est pas un corps rigide.** Le joueur, à pied ou en voiture, est
   une longitude, une latitude et une altitude ; une seule autorité sur la
   position, dont dépendent le rebasage, le streamer et le soleil.

---

## 7. Ce qui existe aujourd'hui

- **Le monde entier** : carte sélectionnable (`ui/world.html`), worker qui cuit
  les tuiles demandées (`r1/world_service.py`), anneaux métriques sans coupure
  polaire (`r1/world_tiles.py`), runtime à origine flottante
  (`native/world.cpp`). Les tuiles du monde sont un LOD d'aperçu.
- **Soleil et atmosphère** : `r1/solar.py` (NOAA/Meeus) et `scripts/sun_cycle.js`
  tenus l'un à l'autre à 1e-9 par un test ; le soleil suit le joueur partout.
- **Ciel** : onze HDRI Qwantani sans soleil (`assets/skies/`), fondu et disque
  solaire dans la skybox du moteur (`scene.setSkybox`, `skyboxBlend`,
  `skySunDirection`).
- **Atlas** : `r1/atlas.py`, douze régions nommées et vingt-deux bandes
  continentales. Le manifeste dit si c'est une `region`, une `band` ou `none`.
- **Sols** : `r1/ground.py` partitionne le terrain par classe, sans sommet
  supplémentaire ; albédos mesurés ; l'eau bloque la marche.
- **Matières** : `r1/surfaces.py` habille sols, rues, murs et toits de textures
  photographiées CC0 (Poly Haven, ambientCG) à leur taille réelle. Le sol suit
  le climat de la région (tropical, aride, méditerranéen, tempéré, boréal,
  polaire) et la limite des neiges ; la texture apporte le détail, la palette
  garde l'albédo mesuré.
- **Mobilier** : props OSM posés comme nœuds de scène, budget réparti par espèce.
  Églises et mosquées reçoivent une flèche ou un minaret générés depuis
  l'emprise.
- **Voiture et circulation** : `F` pour monter/descendre, n'importe quelle voiture
  se prend ; trafic sur le graphe OSM, `maxspeed` tagué d'abord.
- **Cache hors ligne** : un lieu déjà visité ne touche plus le réseau
  (`OSM_QUERY_VERSION` invalide une réponse posée à une ancienne question).

Mesures de résidence (sommets, budget 900 000) : Amsterdam 840 351 · Paris
764 705 · Tunis 165 905.

---

## Annexe — Mesures de référence

Vérifiées contre WGS84 le 3 septembre 2026.

- Aller-retour géodésique ↔ ECEF : erreur maximale **0,0000 mm** sur huit points.
- Emprise des tuiles à l'équateur : z5 = 1 252 km · z9 = 78,3 km · z12 = 9,78 km
  · z14 = 2,45 km · z16 = 611 m · z17 = 305 m.
