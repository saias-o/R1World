# R1World

**Reproduire la Terre entière à l'échelle 1:1, jouable, sur SaidaEngine.**

Ce document garde ce qui ne change pas : la thèse, les contraintes, les
invariants et les décisions prises, puis la liste des prochaines updates (§8).
État relu le 19 octobre 2026, générateur v56 : jonction des ponts à chaussées séparées,
SDK natif autonome, admission mémoire bornée, motos et radio ; réseau routier directionnel,
feux dédupliqués par approche et lignes d'arrêt partagées avec la simulation,
gabarits réels à l'apparition du trafic physique proche, chocs et déformation par instance ; maisons de village mitoyennes,
annexes cadastrales basses, toitures en pente suivant les emprises irrégulières ;
sols manquants déterminés par la
densité et la hauteur finale prédite des bâtiments, avec règles régionales et
par pays ; hauteurs des bâtiments estimées
depuis trois voisins renseignés, suppression des plafonds de hauteur,
jointures de terrain partagées,
LOD intermédiaire des arbres et Far billboard dérivé du Mid,
trois niveaux de bâtiments avec Far plafonné à douze triangles,
pavillons français, jardins,
clôtures, portails et abris de bus ruraux ; priors régionaux US/RU/MA/JP,
toitures par ailes et villages compacts ; exclusion des centres denses.
Le détail de ce qui est fait et mesuré est dans `game/README.md`.

**Deux critères priment sur tous les autres, et ils sont en tension :**

1. **Le monde doit ressembler à la Terre.** Pas à une terre plausible : à *la*
   Terre, à l'endroit précis où le joueur se tient.
2. **Le jeu doit tenir 30 fps partout dans le monde sur le Ryzen 7 5700U
   avec NVIDIA MX450.** C'est la machine de référence, celle qui contraint le
   design et les budgets.

Ce qui produit la ressemblance (§2) coûte peu, et ce qui coûte cher produit peu.

---

## 1. La thèse

> **Le monde n'est pas stocké. Il est décrit, puis régénéré à l'identique chez
> chaque joueur.**

Ce qui transite n'est jamais de la géométrie, mais des observations (OSM,
élévation). La géométrie est fabriquée par le jeu lui-même (`game/native/gen`),
de façon déterministe, à chaque visite. Trois conséquences :

1. **Le volume s'effondre** : le cache garde les observations, jamais la
   géométrie.
2. **Le déterminisme remplace la synchronisation** : même générateur, même
   tuile, même ville au triangle près.
3. **Le monde s'améliore sans être re-livré** : améliorer un générateur repeint
   la planète.

Ce qui distingue R1World d'une extrusion OSM de plus :

- **L'Atlas de styles régionaux.** Un bâtiment sans attributs hérite de la
  distribution de *sa* région, pas d'un défaut mondial. C'est le premier facteur
  de ressemblance, loin devant la qualité des modèles 3D.
- **L'inférence comble le vide sans le masquer**, et elle est marquée comme
  telle.

---

## 2. Ce que « ressembler à la Terre » veut dire

Classement par contribution réelle à « je reconnais cet endroit ». Il gouverne
l'allocation de l'effort.

| Rang | Facteur | Origine | Précision |
|---|---|---|---|
| 1 | Silhouette du relief et horizon | IGN RGE ALTI, Copernicus DEM | Exacte |
| 2 | Trame urbaine : routes, îlots, orientation | OSM | Exacte |
| 3 | Emprises des bâtiments | OSM | Exacte |
| 4 | Lumière : soleil réel, atmosphère | Éphémérides + heure réelle | Exacte |
| 5 | Hauteurs et gabarits | OSM partiel → Atlas | Mesurée ou inférée |
| 6 | Matériaux et couleurs de façade | Atlas | Inférée — *le point critique* |
| 7 | Toitures | OSM partiel → Atlas | Inférée |
| 8 | Végétation | OSM + Atlas | Inférée par biome |
| 9 | Sols et revêtements | OSM partiel → règles | Inférée |
| 10 | Mobilier urbain | OSM + kits | Mesuré ou inféré |

Les quatre premiers rangs sont **mesurés**, donc exacts et quasi gratuits. Le
travail créatif commence au rang 5 et il est porté par l'Atlas
(`assets/world/atlas.json`).

**On ne reproduit pas :** les intérieurs réels (ils sont générés, §7), les
personnes réelles et les plaques, les logos exacts des marques, la
photogrammétrie (elle ne se corrige pas), ni l'exactitude au-delà de la donnée
disponible.

---

## 3. Les invariants

### I0 — La fidélité arbitre
En cas de conflit, on réduit la portée (zone, densité), jamais la justesse. Seule
exception : les plafonds de I4.

### I1 — Aucune coordonnée ECEF n'atteint le moteur
Le moteur ne voit que de l'ENU local, ancré près du joueur. Le `float32` n'est
pas ce qui limite la vue (2,4 cm de pas à 200 km) ; ce qui limite, c'est le
jitter caméra/physique, d'où une **origine flottante rebasée** tous les 350 m
(`native/world.cpp`).

### I2 — La courbure est du contenu, pas une erreur
L'ENU est rigide. Aucune « correction de courbure » nulle part. Horizons :
1,7 m → 4,7 km · 50 m → 25 km · 4 000 m → 226 km · 10 000 m → 357 km.

### I3 — Le déterminisme est un contrat versionné
`géométrie = G(version_générateur, tuile, observations, atlas)`. Aucune
horloge, aucun aléa non seedé : les tirages d'un bâtiment dérivent de son
identifiant OSM. Le générateur est compilé sans contraction flottante
(`-ffp-contract=off`) pour donner la même ville sur toute machine. Changer un
générateur incrémente sa version (`kVersion`), jamais en silence.

### I4 — Le budget est un contrat
Machine de référence : **Ryzen 7 5700U, NVIDIA MX450 2 Gio, environ 7,4 Gio de
RAM utilisable, alimentation secteur, 1920 × 1080 / 30 fps.** Objectif fixé par
l'utilisateur le 9 octobre 2026 : **30 fps soutenus dans le jeu, partout dans
le monde**, soit un budget de **33,33 ms par image**. Il s'applique aux villes
denses, campagnes, forêts, côtes, montagnes et pôles, aux intérieurs et aux
déplacements à pied ou en véhicule, y compris pendant le streaming. Une mesure
locale ou une moyenne qui masque des ralentissements ne qualifie pas cet
objectif global ; les distributions de temps de frame et les pics sont relevés.
CPU, GPU, RAM et VRAM sont mesurés sur ce portable : aucune ressource n'est
présumée être le seul facteur limitant. Les anciens relevés sur d'autres
machines restent des observations, pas une qualification de cette cible.
L'arène de géométrie du moteur est fixée à **1 048 576 sommets**
(`GeometryRegistry::kDefaultMaxVertices`)
et tous les budgets de sommets en découlent (`CLAUDE.md` §5). **120 000 sommets
est un seuil de dégradation, pas de refus** : une tuile dont la géométrie propre
le dépasse voit sa foule divisée par quatre et ses nouveaux intérieurs bloqués.
La décision est conservée par rangée et colonne entre visites, redémarrages et
versions du générateur. Une tuile de 140 000 sommets reste visitable si l'arène
résidente le permet ; seul le plafond global peut encore refuser de la
géométrie, en disant pourquoi.

Seule la géométrie de la tuile compte, au montage, telle que le manifeste la
mesure. Les props et les arbres sont des nœuds sur des modèles partagés
(`CLAUDE.md` §5) et n'entrent pas dans ce compte, et la règle ne touche jamais
aux arbres (`CLAUDE.md` §1). Le 8 octobre, un compte par frame des instances
visibles y ajoutait chaque arbre proche (11 000 à 28 000 sommets) : une douzaine
d'arbres dépassait le seuil, et toutes les tuiles visitées avaient leurs arbres
réduits à des cartes pour toujours. Ce compte est supprimé et ses marqueurs
effacés. La foule est arrondie à l'entier inférieur après division par quatre.
Les marqueurs immuables dans `game/cache/density-policy/<row>_<col>` conservent
la décision indépendamment pour chaque tuile. Ils règlent la densité locale
sans modifier les observations ni la géométrie déterministe du monde.

Les arbres ont leurs propres niveaux de détail : le modèle, le même arbre
éclairci (`tools/r1/tree_lod.py`, 4 000 triangles de couronne, 1 800 de
branches, 160 de tronc, silhouette commune à 94 %) et sa carte. Un niveau se
fond dans le suivant en une demi-seconde (fondu tramé de Saida). Le modèle
complet est réservé aux arbres à moins de 15 m du joueur. Pour l'arbre urbain,
le Mid reste jusqu'à une couverture écran de 0,09, soit environ 80 m pour un
arbre de 9 m avec un champ vertical de 60°, puis cède à un billboard Far de
quatre sommets dérivé du même Mid. Son atlas couvre huit azimuts et cinq
élévations et conserve les normales et l'occlusion interne du Mid, avec le
soleil calculé en direct ; son éclairage simplifié et l'absence d'ombres portées au loin
réduisent le coût GPU. Les autres essences gardent leurs cartes existantes.

Les bâtiments fermés ont trois niveaux : **Near** avec détails de façade et
intérieurs chargés à proximité, **Mid** avec l'emprise réelle et une toiture
simplifiée, **Far** avec une boîte orientée fermée de **12 triangles maximum
par bâtiment**, sans textures ni ombres portées. Le Near est réservé aux
bâtiments à moins de 75 m (85 m pour en sortir) ; la couverture écran choisit
ensuite le niveau. Les transitions se fondent en 0,3 seconde et gardent un
niveau disponible pendant le chargement du suivant. Dans une tuile dégradée
au-delà de **120 000 sommets**, le Near garde l'enveloppe sans les détails
ajoutés et les nouveaux intérieurs restent bloqués. Le seuil porte toujours
sur la géométrie propre de la tuile avant LOD, pas sur les trois variantes
additionnées. Seuls les niveaux demandés occupent l'arène, avec le petit Far
en réserve ; les collisions gardent l'enveloppe réelle indépendamment du LOD.

### I5 — Le jeu sait toujours ce qu'il ignore
Chaque élément porte sa provenance : `mesuré`, `inféré` ou `synthétisé`. Ce qui
est mesuré n'est jamais écrasé par ce qui est inféré, et le manifeste de chaque
tuile dit lequel a répondu.

---

## 4. Les données sources

| Couche | Source | Licence |
|---|---|---|
| Bâti, voirie, eau, usage du sol, aéroports | OpenStreetMap (Overpass) | ODbL |
| Élévation, France | IGN RGE ALTI (Géoplateforme) | Licence Ouverte |
| Élévation, ailleurs | Terrain Tiles (Mapzen, AWS) : SRTM, 3DEP, EU-DEM, relevés nationaux | libre, attribution par source (`assets/licenses/`) |
| Élévation, en dernier recours | Copernicus DEM GLO-90 (Open-Meteo) | libre, attribution |
| Sommets (altitude relevée) | OpenStreetMap `natural=peak` avec `ele` | ODbL |
| Météo, visibilité, vent, neige au sol, courants | Open-Meteo | CC BY 4.0 |
| Banquise (glace de mer, son âge) | NOAA CoastWatch/PolarWatch, ASCAT Metop-C | libre, sans garantie |
| Trait de côte hors ligne | Natural Earth | domaine public |
| Trafic maritime moyen | Global Shipping Traffic Density (Banque mondiale) | CC BY 4.0 |
| Canopée (arbres, haies, hauteur) | High Resolution Canopy Height Maps (Meta & WRI), 1 m, images 2009-2020 | CC BY 4.0 |

Les données dérivées d'OSM sont soumises à l'ODbL, avec attribution visible en
jeu. Aucune source `CC BY-NC`. Les assets sont **CC0** (Poly Haven, ambientCG,
Kenney) ou dessinés par le projet, avec deux exceptions : les personnages sont
Microsoft Rocketbox, **MIT** ; les sept voitures de Ruff et Scopia / Space
Mushrooms sont **CC BY 3.0** ; les deux motos originales de R1World sont
**CC0**. Les notices sont conservées
(`assets/licenses/`) et les auteurs crédités en jeu. Tous sont normalisés avant
d'entrer : échelle métrique, albédos mesurés, un asset non conforme n'entre pas.

---

## 5. Le tuilage

Anneaux de latitude métriques, sans coupure polaire (`native/gen/common.hpp`) :
36 000 rangées de 0,005° et, par rangée, `72 000 · cos(lat)` colonnes. Une
tuile fait donc environ 556 m de côté à toute latitude, et sa clé
(`v<version>_<rangée>_<colonne>`) ne dépend ni de la session ni du point
d'arrivée.

À moins de 18 km d'un pôle, un anneau n'est plus que quelques quartiers :
le voisinage y est fait des tuiles les plus proches en mètres, douze au pôle.

Une requête Overpass sert un voisinage de neuf tuiles. Le jeu cuit d'abord la
tuile du joueur, puis ses voisines par distance, puis un couloir devant le
véhicule. La géométrie résidente reste bornée par l'arène.

**Le détail ne dépend pas de la vitesse.** Le mobilier continue à se charger et
la portée de la végétation reste celle du mode graphique choisi, même en véhicule.

---

## 6. Décisions prises

1. **Licence et distribution.** Jeu gratuit. Données OSM dérivées sous ODbL avec
   attribution visible.
2. **Moteur : dépôt Saida direct, sans fork.** Une modification moteur n'est
   admise que si elle améliore le moteur en général ; aucune exception
   spécifique à R1World. Ce qui est propre au jeu se compile dans le binaire du
   jeu, jamais dans la bibliothèque du moteur.
3. **Le monde est généré dans le jeu, en C++.** Pas de processus à côté, pas de
   Python au lancement ; `game/Play.ps1` compile et lance.
4. **L'heure est réelle.** Arriver de nuit à Tokyo est l'heure qu'il y est.
5. **Le ciel suit l'heure.** Une journée de ciels photographiés (série Qwantani,
   Poly Haven, CC0), fondus deux à deux selon la vraie hauteur du soleil, calés
   sur la couleur d'horizon du modèle. Le soleil des photos est retiré ; le
   moteur dessine un seul disque, là d'où vient la lumière (`r1/skies.py`).
6. **Le joueur est une position géodésique.** À pied, en voiture, en bateau ou
   en vol, il est une longitude, une latitude et une altitude ; une seule
   autorité sur la position, dont dépendent le rebasage, le streamer et le
   soleil.
7. **Le cache est une promesse.** Un lieu déjà visité ne touche plus le réseau
   (`CLAUDE.md` §7).

---

## 7. Ce qui existe aujourd'hui

- **Le monde entier** : carte sélectionnable (`ui/world.html`), service qui
  télécharge et cuit les tuiles demandées sur des threads (`native/gen/service.cpp`),
  runtime à origine flottante (`native/world.cpp`).
- **Soleil et atmosphère** : `r1/solar.py` (NOAA/Meeus) et `scripts/sun_cycle.js`
  tenus l'un à l'autre à 1e-9 par un test ; le soleil suit le joueur partout.
- **Ciel** : onze HDRI Qwantani sans soleil (`assets/skies/`), fondus dans la
  skybox du moteur.
- **Atlas et bâti** : douze régions nommées et vingt-deux bandes continentales
  (`assets/world/atlas.json`, `native/gen/buildings.cpp`). Le manifeste dit si
  c'est une `region`, une `band` ou `none`. En v41, une hauteur manquante prend
  la moyenne des trois voisins aux hauteurs renseignées à moins de 250 m,
  y compris dans les tuiles voisines ; les étages renseignés servent de repli,
  puis l'Atlas. Les estimations ne deviennent jamais des références. Les
  hauteurs et étages propres au bâtiment restent prioritaires, sans plafond
  architectural ; la provenance de chaque moyenne est conservée.
  En v43, les maisons empruntent aux voisins résidentiels de gabarit comparable,
  sans abris minuscules, bâtiments publics ou immeubles d'appartements.
  Les maisons mitoyennes françaises restent basses près d'un village observé,
  à distance des villes ; les annexes et `wall=no` ne deviennent plus des tours.
  Les toits des maisons irrégulières sont découpés selon leur emprise, les
  hauteurs et formes de toit renseignées restant prioritaires.
- **Sols et matières** : terrain partitionné par classe (`native/gen/terrain.cpp`).
  les sols inconnus sont inférés sur une fenêtre locale de 200 × 200 m depuis
  l'emprise des bâtiments pondérée par leur hauteur finale prédite, voisins
  inclus (`native/gen/ground.cpp`). Le pays et la région règlent les seuils
  et les choix de terre, sable, végétation ou sol aménagé dans l'Atlas ; les
  classes observées restent prioritaires. Le manifeste conserve la provenance.
  Il est habillé de textures photographiées CC0 à leur taille réelle
  (`r1/surfaces.py` les prépare) ; la texture apporte le détail, la palette
  garde l'albédo mesuré. Les fenêtres sont photographiées depuis des modules
  Poly Haven ; relief en parallax mapping, reflets du ciel dans les vitrages
  et variation des surfaces cassent la répétition. L'eau bloque la marche.
  Les sols herbeux portent des touffes animées jusqu'à 70 m, exclues des rues,
  parkings et bâtiments ; le manifeste compte leur couverture (`gen/grass.cpp`).
- **Mobilier et végétation** : props OSM posés comme nœuds de scène
  (`native/gen/scatter.cpp`). Églises et mosquées reçoivent une flèche ou un
  minaret générés depuis l'emprise.
- **Ports et mer** : la mer reconstruite depuis le trait de côte OSM
  (`native/gen/harbours.cpp`, `sea.cpp`) ; jetées, digues, quais et phares tels
  qu'OSM les trace. Bateaux inférés, et le manifeste le dit. `F` prend la barre.
- **Monuments** : vingt lieux qu'aucune extrusion ne peut dessiner, chacun une
  recette (`r1/landmark_recipes.py`, vocabulaire de `r1/sculpt.py`) cuite en
  trois niveaux de détail dans `assets/world/landmarks/`. Ancre et orientation
  mesurées sur l'élément OSM trouvé par son `wikidata`, hauteur officielle.
- **Modèle prédictif des détails** : ce que les cartes ne disent pas, le lieu
  le dit. Des règles par pays (`native/gen/rules_fr.cpp`) proposent panneaux et
  ouvrages depuis le graphe routier, le pays est lu dans OSM, ce qui est relevé
  fait taire ce qui est deviné, et le manifeste dit quelle règle a posé quoi
  (`native/gen/predict.hpp`). Panneaux dessinés d'après l'IISR
  (`r1/signage.py`), panneaux d'agglomération au nom de la commune lu dans les
  adresses. Rulebooks : France (IISR) et États-Unis (MUTCD : stops, yield,
  mph, sens interdits, plaques de noms de rue). Les panneaux relevés par OSM
  sont dessinés là où ils ont été relevés.
- **Rues** : chaque voie est recollée en une ligne avant d'être élargie, les
  virages et les jonctions sont arrondis, et une rue qui traverse le bord d'une
  tuile est posée jusqu'à ce bord, sans trou (`native/gen/streets.cpp`). En v30,
  les bretelles `*_link` sont carrossables ; les largeurs OSM priment, sinon
  le nombre de voies et les accotements déterminent la chaussée. Les routes
  rurales sans ces tags infèrent 6,5 m, les bretelles express à une voie 5 m.
  Marquages des voies et rives, glissières métalliques avec ouvertures aux
  raccordements, et terre-pleins herbeux entre chaussées parallèles sont
  générés avec leur provenance inférée (`gen/road_details.cpp`). Les ouvrages
  proches à la même altitude partagent un terre-plein ; les talus ne recouvrent
  plus les plateformes routières voisines. La compilation v30 est faite,
  son rendu à Theix et ses nouveaux cas de régression restent à vérifier.
  **Jamais d'eau sur une rue** : aucune eau dessinée sur la chaussée ou le
  trottoir, l'eau cachée (busée, couverte, souterraine) n'est pas dessinée, et
  les rues qui traversent une case d'eau sont sèches pour la marche et la
  voiture (`dryStreets`).
- **Ponts** : tabliers au gabarit, rampes et remblais à 5 %, piles, culées ; ce
  que le tablier ne peut pas donner est creusé dans le relief
  (`native/gen/bridges.cpp`). On y marche et on y roule, dessus comme dessous.
- **Voitures, motos et circulation** : `F` prend la voiture ou la moto la plus
  proche ; trafic sur le graphe OSM, `maxspeed` tagué d'abord. Les véhicules
  sont dessinés à 0,68 de leur taille. Sept voitures GLB sous CC BY 3.0
  remplacent la flotte simplifiée ; deux motos originales, une citadine et une
  motocross basse-poly, sont sous CC0. Toutes ont des modèles Near/Far. La part
  de motos est de 5 % par défaut et de 50 % en Afrique, en Inde et en Amérique
  latine. Les motos ont deux roues en ligne, une masse et une direction propres
  et un réglage arcade distinct des voitures. Le moteur conserve désormais le ciel HDR dans le
  descripteur PBR ; ses reflets ne retombent plus sur une texture blanche.
- **Personnes et foule** : le joueur et la foule sont des scans riggés Rocketbox
  (`r1/humans.py`), clips de capture de mouvement reciblés, deux niveaux de
  détail, dessinés à 0,8. `native/gen/crowd.cpp` cuit où l'on marche (trottoirs,
  voies piétonnes, traversées, bancs) et combien (inféré du contenu de la tuile,
  puis de l'heure solaire et de la pluie) ; on marche, attend, téléphone, discute,
  s'assoit, on s'écarte du joueur et on fuit sa voiture. Joueur et passants sont
  des corps dans la physique du moteur (Jolt) : il bute et glisse contre eux, le
  heurt les fait chanceler et répondre, et qui le voit passer devant soi le suit
  du regard.
- **Aéroports et aéronefs** : `native/gen/airports.cpp` pose pistes, voies de
  circulation, aires de trafic et hélisurfaces telles qu'OSM les trace, peintes ;
  aérogares et hangars en verre et acier sauf hauteur taguée. Les appareils
  garés sont inférés : la plus longue piste décide de ce que l'aéroport reçoit,
  les postes OSM décident où. Chaque base militaire reçoit un hélicoptère.
  Pilotage arcade propre à chaque classe, et aucun crash : un bâtiment arrête
  l'appareil.
- **Pôle Nord et banquise** : `native/gen/seaice.cpp`. Où la mer est gelée
  et l'âge de la glace sont mesurés (ASCAT, 4 km, quotidien ; le trou polaire
  prend la classe la plus proche et le manifeste le compte), le pack est
  synthétisé et le dit : floes, chenaux ouverts ou regelés, crêtes de
  compression et leurs blocs, congères, mares selon la saison de la mesure.
  On y marche, on nage dans les chenaux ; au-delà des tuiles, la banquise
  continue jusqu'à l'horizon, sous la visibilité mesurée. Le cap du joueur
  reste droit par-dessus le pôle.
- **Neige et temps visible** : neige qui tombe et neige soufflée par le vent
  mesuré ; sols et toits enneigés quand Open-Meteo mesure 3 cm au sol.
- **Magasins** (générateur v23) : devanture, vitrines, marquise et nom OSM de
  l'enseigne ; double porte coulissante sur une entrée cartographiée, sinon
  inférée vers le parking ; rayons, présentoirs et caisses selon l'usage.
  Un supermarché cartographié dans une galerie en devient l'ancre (Carrefour
  dans Le Fourchêne, Vannes). Parkings OSM peints, rattachement au commerce
  marqué comme inféré (`native/gen/retail.*`).
- **Intérieurs du bâti ordinaire** (v24-v25) : logements (séjour-cuisine,
  chambres, salles de bain meublées), écoles, commissariats et gendarmeries,
  bureaux, garages avec voitures, lieux de culte, restaurants, entrepôts,
  prisons. Usage tagué d'abord, inféré sinon. Contrat commun
  `native/gen/interiors.*` : seul le plan léger voyage avec la tuile ; la pièce
  est construite à 65 m de la porte, libérée à 85 m, deux au plus, 24 000
  sommets chacune. Portes battantes, rez-de-chaussée seulement. Une porte
  au-dessus du sol est précédée d'un perron (v29) : marches de 18 cm au plus,
  giron de 30 cm, palier, en pierre là où la maçonnerie est la tradition
  régionale, en béton ailleurs et pour les commerces ; jamais de pente.
- **Stations-service** : l'auvent est un auvent (dalle sur colonnes, 4,7 m de
  dégagement), îlots et pompes synthétisés dessous, bandeau « station-service »
  dans la langue du pays, totem à la marque et sans prix ; auvent inféré quand
  OSM n'en trace pas (`native/gen/fuel.*`).
- **Canopée mesurée** : la carte de hauteur de canopée Meta/WRI (1 m) devient
  par tuile une grille de 48 × 48 cellules codée en quelques centaines
  d'octets (~7 Go pour toutes les terres, un fichier par degré carré). Un
  arbre inféré ne reste que là où la canopée voit un arbre ; chaque cellule
  arborée vide reçoit un arbre photoscanné à la hauteur mesurée
  (`native/gen/canopy.*`).
- **Montagnes** : le relief est lu finement (41 × 41, zoom 13) et relevé aux
  sommets que OSM a mesurés (`native/gen/peaks.*`) ; au-delà des tuiles, le
  moteur dessine le relief jusqu'à 262 km en anneaux imbriqués
  (`TerrainRingsNode`, profondeur inversée), sans un sommet de l'arène, avec la
  courbure de la Terre ; la brume est la visibilité mesurée, 150 km par temps
  clair. Comparé à des photographies réelles dans la galerie (Grenoble, Le
  Cap, Rio). Les anneaux portent des matériaux photographiés à leur échelle
  réelle : paroi rocheuse de 27 m, projection sur trois axes, répétition cassée
  et détails filtrés à distance ; la forêt varie par massifs de 12 m sans
  ajouter de géométrie. L'apparence reste inférée, pas une carte de couvert
  mesurée. Chaque capture consigne heure UTC et météo ; l'horloge sans fuseau
  de la photo de Rio est corrigée d'une heure par inférence du Soleil visible,
  avec cette incertitude indiquée. La ville et le couvert détaillé au-delà des
  tuiles résidentes restent à restituer pour atteindre la fidélité des photos.
- **Pluie** : la prévision locale distingue pluie liquide et neige et pilote les
  particules Saida, le ciel nuageux et les matériaux extérieurs. Le shader
  générique du moteur ajoute les impacts circulaires et les films humides sur
  bitume, pavés, béton et parkings ; le sol sèche progressivement. Les nuages
  de pluie complètent les HDR existantes, avec masses cumuliformes et enclumes.
  Les shaders partagent leurs coordonnées monde entre les yeux en VR.
- **Relief installé** : une grille mesurée d'environ 278 m couvre les latitudes
  de −85° à +85°, en blocs compacts de 10° (`native/gen/relief.*`, `r1relief`).
  Les anneaux de 256 m et plus la lisent sans réseau ; les anneaux fins et le
  sol proche s'y replient quand les observations fines manquent. Elle n'entre
  jamais dans le cache des observations : le sol fin reprend la priorité dès
  son arrivée. Premier test hors ligne à Santiago, sans OSM ni sol en cache :
  apparition à 559,2 m en 234 ms, marche, saut, conduite et neuf anneaux vérifiés.
  Planète complète vérifiée le 5 octobre 2026 : 648 blocs, 22 856 cellules de
  terres, 859 Mo ; les 2,35 milliards de hauteurs stockées ont été décodées.
  Les exports Windows et Web conservent les fichiers `.r1relief`. Le jeu reste
  natif Windows : le chargement asynchrone de ces blocs par le système d'assets
  du moteur est encore nécessaire pour un port Web ou consoles.
- **Physique du moteur** : toutes les collisions du jeu sont des corps Saida
  (tuiles, intérieurs, portes, meubles, véhicules, troncs) ; le jeu n'a plus
  de test d'obstacle à lui. Le joueur est un `CharacterBodyNode`. Un tronc
  n'est solide qu'une fois son arbre affiché : rien d'invisible ne bloque.
- **L'eau** (moteur, `WaterNode`) : vue d'avion, la mer ne se répète plus en
  carrelage. Trains de vagues sans période commune, ce que le maillage ou le
  pixel ne peut pas porter passe aux normales puis à la rugosité (un long
  reflet du Soleil au loin), rafales de vent sur des centaines de mètres,
  hachage entier exact. Calcul seul, sans texture : même coût sur bureau,
  mobile et Web.
- **Cache hors ligne** : un lieu déjà visité ne touche plus le réseau ; une
  réponse à une ancienne question Overpass est cuite telle quelle, et ce qui
  lui manque arrive en couche séparée (`kOsmQueryVersion`, `kOsmBaseVersion`).
  Les mises à jour du sol et d'OSM ne sont plus bloquées par des données
  d'aéroport en attente, et le téléversement commence par la tuile du joueur.

---

## 8. Prochaines updates

Chacune garde les règles du projet : rien ne dégrade un asset existant
(`CLAUDE.md` §1), les couleurs sont des albédos, tout ce qui est inféré le dit
dans le manifeste (I5), et le coût se compte contre l'arène, le CPU, le GPU,
la RAM et la VRAM de la machine de référence (I4).

### Mode grille-pain — objectif Paris 40 FPS, plan du 18 octobre 2026

**Demande : atteindre 40 FPS sur Paris sur le Ryzen 7 5700U / MX450, avec
peu de perte de qualité visible.** Budget de livraison : **25 ms par image**
à **1280 × 720**, dans le mode grille-pain. Cette cible locale complète I4 ;
elle ne qualifie pas les performances du reste de la planète.

**État de réalisation, 18 octobre :** restauration du vrai mode dans les tests,
propagation incrémentale des transformations, réutilisation des branches lors
des changements de hiérarchie, rejet des ombres hors du volume de la lumière
et tri des seuls bâtiments en attente d'envoi implémentés. Ces optimisations
profitent aux deux modes sans réduire les réglages graphiques. Les vues fixes
mesurées avant le dernier ajustement donnent environ 42–48 FPS en grille-pain
et 38 FPS en standard, contre 21–22 FPS pour les témoins. La marche révélait
encore des pics ; la dernière correction cible la reconstruction du parcours.
La qualification complète à 40 FPS, notamment en déplacement et en streaming,
reste ouverte. Mesures et limites : `game/README.md`, section Performance.

#### Point de départ et règle de décision

L'audit du 18 octobre, synthétisé dans [game/README.md](../game/README.md),
mesure sur Paris une image moyenne de 50,03 ms, dont 32,54 ms de parcours CPU
des transformations, 6,80 ms de préparation du rendu et 2,03 ms de gestion
des LOD de bâtiments. Le GPU prend 23,16 ms. **Cette référence est en qualité
standard à 1600 × 900** : `smokeOptions()` réactive le standard avant la capture.
Le point de départ réel du grille-pain reste à mesurer. Les valeurs ci-dessous
sont des budgets de travail, jamais des gains déjà mesurés ou garantis.

Ordre : mesurer le bon mode, supprimer le travail inutile sans changer l'image,
traiter les pics de streaming, puis ajuster les effets seulement si le GPU
empêche encore d'atteindre la cible. Refaire une mesure après chaque lot et
arrêter les réductions graphiques dès que la cible est atteinte.

#### 1. Établir une référence grille-pain fiable

- Découpler le test des options de la mesure de performance, ou rétablir
  explicitement toutes les options demandées après ce test. Vérifier et
  journaliser le mode réellement actif, la taille de rendu effective, les
  distances, les réglages de l'herbe et des ombres pendant la mesure.
- Conserver le binaire initial et les configurations. Comparer standard et
  grille-pain avec mêmes positions, orientations, heure, météo et densités.
  Première paire à résolution identique pour isoler les effets du mode ;
  deuxième comparaison avec les résolutions réelles des deux modes.
- Mesurer CPU, GPU, temps de frame complet et présentation séparément. Relever
  le rafraîchissement de l'écran et tester les réglages de VSync déjà disponibles :
  vérifier que le mode de présentation ne maintient pas la livraison à un
  palier inférieur à 40 FPS. La validation finale utilise le réglage retenu
  pour jouer, la fenêtre visible et le temps réel, sans pas de simulation forcé.
- Choisir et figer trois cas parisiens : rue dense à façades, bord de Seine avec
  eau et pont, parc ou avenue arborée avec herbe. Archiver coordonnées, caméra,
  chemin parcouru et état de référence ; commencer par le spawn déjà audité,
  longitude 2.3522 / latitude 48.8566.
- Sur secteur, même configuration du PC, caches chauds et température stabilisée :
  trois répétitions par scénario, avec 60 s à l'arrêt, 90 s de marche et 120 s
  de conduite sur les parcours correspondants. Les compteurs de trafic et de
  foule doivent être comparables, sans supposer qu'une simulation vivante est
  identique à chaque répétition. Séparer le chargement initial des déplacements
  avec streaming ; ne pas retrancher les montages survenant pendant ces parcours.

**Sortie du lot :** référence mesurée du vrai mode et choix du premier coût à
traiter. Si la hiérarchie des coûts diffère de l'audit standard, adapter les
lots suivants aux traces.

#### 2. Réduire les parcours des transformations dans Saida — premier chantier

**But : viser ≤ 6 ms au p95 pour l'ensemble des transformations**, contre
32,54 ms moyens dans la référence standard. Ce saut exige un prototype mesuré
avant toute refonte étendue. Aucun changement visuel recherché.

- Traiter les transformations comme un chantier moteur générique, dans
  `engine/src/scene`, en reprenant l'entrée existante du `engine/ROADMAP.md`.
- Prototyper une détection compacte des modifications et une propagation
  limitée aux branches affectées. Un tableau contigu de valeurs de transformations
  à comparer est une piste pour éviter le coût des parcours de pointeurs ;
  sa comparaison reste un coût réel à mesurer, pas une suppression de tout O(n).
- Préserver les écritures par `Transform&` conservé : un simple drapeau placé
  dans un setter ne les détecte pas. Détecter aussi les modifications pendant
  les callbacks physiques avant de limiter la propagation après simulation.
  Ne supprimer aucun parcours de sécurité sans couverture de ces écritures.
- Propager les résultats des corps réellement déplacés et les changements de
  parents sans revisiter toute la scène lorsque les contrats le permettent.
  Réutiliser les branches statiques ; visibilité et activation ne suffisent
  pas à déclarer une branche immobile.
- Vérifier références conservées sur plusieurs frames, déplacement d'un parent,
  reparentage, échelles non uniformes, branches cachées ou désactivées, activation,
  rebasage, animation, ragdolls, véhicules et caméra après physique.

**Décision :** mesurer le prototype avec les mêmes scènes et vérifier ses
matrices contre le comportement actuel. S'il ne réduit pas suffisamment le
coût, revoir la représentation des branches statiques avant de réduire les
assets ou de poursuivre des optimisations secondaires. Ne pas annoncer les
40 FPS sur la seule base d'une soustraction des temps de scopes.

#### 3. Réduire la préparation du rendu et des bâtiments

- Dans le moteur, conserver en cache les données de soumission stables et
  actualiser ce qui change : transformation, matériau, géométrie, visibilité,
  LOD et état de chargement. Garder les transitions de LOD et les ressources
  encore utilisées par les images GPU en vol.
- Pour les ombres, éliminer les objets hors du volume utile de la lumière
  avant leur soumission. **Ne pas utiliser uniquement le frustum caméra** :
  un objet hors écran peut projeter une ombre visible. Vérifier soleil rasant,
  véhicules, personnages et rotations rapides de caméra.
- Pour les bâtiments, remplacer le tri complet systématique par des priorités
  réévaluées lors des changements utiles : mouvement, montage, disponibilité,
  visibilité et transitions. Conserver les seuils, l'hystérésis et les fondus
  actuels ; le joueur ne doit pas voir de retard de détail.
- Garder les mécanismes génériques dans Saida et la politique géographique ou
  les budgets propres au monde dans R1World, sans dupliquer un système moteur.

**Budgets de travail :** préparation complète du rendu CPU ≤ 4 ms au p95,
gestion des LOD de bâtiments côté jeu ≤ 1 ms au p95. L'objectif est de supprimer
le travail répété, avec une image équivalente.

#### 4. Étaler les montages pour conserver la fluidité en déplacement

Les montages actuels de 17 à 53 ms peuvent dépasser le budget à eux seuls.
Décomposer préparation, création de hiérarchie, colliders et publication en
unités bornées, avec une enveloppe de travail sur le thread principal proposée
de **4 ms par image**, incluse dans le budget CPU global.

Préparer hors du thread principal ce qui est indépendant du moteur. Publier
des ensembles cohérents et conserver l'ancienne représentation jusqu'à ce que
la nouvelle soit prête. Le budget doit tenir compte du temps des opérations
indivisibles ; un contrôle de durée entre deux grosses opérations ne suffit
pas. Prévoir les tuiles selon la vitesse sans augmenter sans borne RAM, VRAM
ou arène. Aucun trou de terrain, disparition de collision ou mur qui arrive
sur le joueur pour satisfaire le chronomètre.

**Validation :** marche, conduite, rotation à 180°, franchissement de tuiles et
rebasage. Ce lot conditionne la cible en déplacement, même si une vue fixe
atteint déjà 40 FPS.

#### 5. Ajustements graphiques conditionnels et discrets

N'ouvrir ce lot que si les traces du vrai grille-pain montrent encore une
limite GPU. Tester chaque réglage séparément et conserver le moins visible
qui atteint le budget. Les chiffres proposés sont des points d'essai.

- **Herbe volumétrique :** appliquer réellement le preset à `GrassNode`.
  Essayer d'abord un rayon de 70 → 45 m avec fondu progressif, en gardant
  l'aspect proche. Si nécessaire, réduire les touffes éloignées avant de
  toucher à la densité près des pieds. Vérifier parc, caméra basse, conduite
  et raccord avec la couleur du sol ; aucun arrêt net du tapis d'herbe.
- **Ombres :** après élimination des soumissions inutiles, comparer 2048 et
  1536 si cette taille est supportée. Conserver d'abord la portée de 100 m et
  les ombres proches. N'essayer 1024 ou une portée plus courte que si le besoin
  mesuré subsiste et si l'image reste acceptable en mouvement.
- **Arbres, façades, eau :** conserver les assets, les couleurs, les silhouettes,
  les matériaux proches et les fondus existants. Optimiser leur soumission
  avant de réduire leurs paramètres. Ne pas réduire la foule ou le trafic
  pour compenser les parcours CPU tant qu'ils restent des coûts secondaires.
- Garder le 720p, le brouillard et les distances actuelles du grille-pain au
  départ. Une baisse de résolution supplémentaire ou un brouillard encore
  rapproché ne fait pas partie de la première solution.
- Le relief lointain est déjà désactivé dans ce mode : ne compter aucune
  économie supplémentaire de sa désactivation.

**Contrôle visuel :** captures appariées à caméra/heure/météo identiques et
séquences en mouvement. Les optimisations sans changement d'image doivent
préserver les silhouettes et les ombres ; les ajustements de qualité doivent
documenter leur différence visible. Ne retenir une dégradation sensible
qu'après présentation du compromis et de son gain mesuré à l'utilisateur.

#### 6. Budgets et critères de livraison

| Poste CPU, scopes exclusifs | Budget de travail au p95 |
|---|---:|
| Transformations, tous passages cumulés | 6 ms |
| Préparation complète du rendu | 4 ms |
| Politique de LOD des bâtiments côté jeu | 1 ms |
| Simulation, animations et autres logiques | 4 ms |
| Commandes graphiques, UI et autres coûts CPU | 3 ms |
| Travail de streaming sur le thread principal | 4 ms |
| **Enveloppe CPU** | **22 ms** |

GPU : viser également **≤ 22 ms au p95**, image GPU entière. Les budgets CPU
par poste servent au diagnostic ; leurs p95 ne décrivent pas nécessairement
la même frame. CPU et GPU se recouvrent, donc leurs temps ne s'additionnent
pas. Le verdict vient toujours du temps réel d'image complet, présentation
comprise : **moyenne ≥ 40 FPS et p95 ≤ 25 ms**, dans chaque scénario parisien
retenu. Relever aussi p50/p99/max et les dépassements de 25/33,3/50 ms.

La marge entre 22 et 25 ms vise à absorber la variabilité. Tester la stabilité
sur au moins 15 minutes de jeu après chauffe, avec RAM/VRAM relevées et sans
croissance mémoire continue, dépassement de l'arène ou évictions récurrentes
des ressources visibles. Une moyenne de 40 FPS dans une rue immobile ne suffit
pas à valider la marche et la conduite ; signaler séparément tout scénario
qui manque la cible.

À chaque lot : mêmes parcours, gain mesuré, captures, compteurs de scène et
tests ciblés sur les contrats touchés. Pour les changements moteur partagés,
suivre `engine/CONTRIBUTING.md`, dont les vérifications native, authoring WASM
et player Web applicables. Mettre à jour le backlog existant du moteur et
consigner dans `game/README.md` les résultats réellement obtenus, leurs
conditions et leurs limites. Aucun changement de ces documents de vérité
ne devra présenter un budget proposé comme une performance déjà atteinte.

### Fun update — première version livrée le 13 octobre 2026

Livré (détail et preuves dans `game/README.md`, « The fight ») : poings en
combo, pistolet visé à l'épaule, ragdolls du moteur pour les passants et le
joueur, voiture qui renverse, passants qui fuient ou ripostent, santé, chute,
mort et réveil à l'hôpital OSM le plus proche. Reste : animations de relevé
enregistrées, d'autres armes, conséquences policières, qualification du temps
de frame en combat sur la machine de référence.

- **Rendre le jeu plus amusant à jouer**, avec des interactions dans l'esprit
  de GTA III.
- **Accidents de voiture physiques** : repris dans la mise à jour commune
  « Véhicules et trafic » ci-dessous ; ils ne sont plus un chantier séparé.
- **Renverser les piétons en voiture** : réactions physiques à l'impact,
  chutes et conséquences en jeu, dans l'esprit de GTA III.
- **Combat à pied** : un bouton pour frapper, puis des armes à feu et les
  interactions de combat associées, dans l'esprit de GTA.

### Detail update — couche de détails manuels, à traiter plus tard

- **Ajouter une couche au-dessus des modèles prédictifs** pour placer et
  corriger des détails à la main, sans modifier les règles de génération pour
  chaque cas particulier.
- **Décrire les placements dans un fichier JSON** : coordonnées du lieu,
  modèle à poser et orientation, par exemple « placer un lampadaire ici ».
  Les placements manuels priment sur les propositions prédictives au même
  endroit et évitent les doublons ; leur provenance reste explicite.
- **Placer les monuments dans cette couche** : reprendre les placements
  actuels dans un système commun et explicite, avec ancrage et orientation
  vérifiables, plutôt que des exceptions dispersées. Conserver les modèles
  et les ancrages mesurés existants.

### Véhicules et trafic — plan validé le 10 octobre 2026

Cette mise à jour réunit la Circulation update et les accidents automobiles
de la Fun update. Le socle est implémenté : conduite physique sans suspension
imposée, impacts issus du solveur, déformation générique par instance, colliders
adaptés, voies directionnelles, feux, changements de voie et transferts de
véhicules entre tuiles. Les preuves et limites sont dans `game/README.md`.
Correctifs du 11 octobre : redémarrage et maintien en pente, contacts de roues
arrondis, conservation des courts raccords routiers, suivi adapté au braquage
de chaque modèle, obstacles de voitures garées et feux avec boîtier, visières,
lentilles et répétiteur. Le contrôle de Paris couvre désormais une minute de
trafic, au-delà du démarrage initial.
Les poteaux des feux sont ancrés hors des contours de chaussée réellement
rendus, avec un dégagement perpendiculaire de 45 cm. Les contours sont conservés
par le générateur ; les largeurs de voies IA ne servent plus à trouver la bordure.
Les détails ci-dessous conservent la cible validée ; les résistances mécaniques
par pièce, effets de choc dédiés, règles nationales complètes et qualification
sur casque réel restent des extensions, pas des fonctionnalités déjà validées.
Objectif : des voitures qui suivent réellement les routes, respectent les
carrefours et réagissent physiquement aux chocs, avec une carrosserie qui se
déforme selon l'endroit et la violence de l'impact. Les sept modèles existants
et leur échelle actuelle sont conservés.

#### A. Architecture et autorité sur les voitures

- **Saida, `engine/src`** : informations d'impact génériques, corps dynamiques,
  service de déformation, géométrie modifiable par instance, colliders adaptés,
  outils de réglage et sérialisation. Aucun concept OSM, tuile ou R1World.
- **Plugin `engine/plugins/traffic`** : réseau de voies, conducteur IA,
  suivi des véhicules, feux, priorités, changements de voie et réaction aux
  incidents. Le plugin reste utilisable et testable sans moteur ni fenêtre.
- **R1World, `game/native`** : transformation des observations OSM en réseau,
  raccord des tuiles, profils de la flotte, streaming et intégration avec la
  foule et la position géodésique. Extraire les nouveaux modules de véhicules
  et trafic plutôt que tout ajouter à `world.cpp`.
- **Le contrôleur à suspension existant n'est pas imposé**, ni au joueur ni
  au trafic. Le choix du modèle de conduite est indépendant du système de
  chocs. Comparer en premier une conduite par forces avec adhérence simplifiée
  au contrôleur existant ; retenir le modèle qui passe les essais de tenue de
  route et le budget. Une suspension détaillée reste une option, pas une
  dépense obligatoire par voiture.
- **Trafic éloigné** : progression légère sur les voies, pose interpolée,
  sans simulation détaillée des pneus ou déformation active.
  **Véhicule du joueur et trafic exposé à un choc** : corps dynamiques, gravité,
  contacts et forces de conduite. Promotion anticipée selon distance, vitesse
  relative et trajectoire probable ; transfert de pose et de vitesses sans
  saut. L'IA commande accélérateur, frein et direction ; elle ne replace pas
  un corps dynamique sur sa courbe à chaque frame.
- Après impact, le corps reste physique tant que l'incident est actif ; aucune
  remise en place automatique sur la voie. La rétrogradation demande stabilité,
  distance et absence de contact probable. Pendant la conduite, la pose physique
  locale détermine la position géodésique du joueur pour le streamer : une seule
  autorité, rebasage et éviction compatibles avec les corps en mouvement.

#### B. Collisions et conséquences des accidents

- Étendre les événements de contact du moteur : identifiants sûrs des corps,
  points, normale, vitesse relative au point de contact, masses effectives,
  estimation de l'impulsion et de l'énergie incidente. Les callbacks physiques
  enregistrent les données ; les consommateurs les traitent à une frontière
  sûre du pas physique, sans modifier scène ou géométrie depuis un worker Jolt.
- L'impulsion estimée sert à qualifier les dégâts ; **Jolt fournit déjà la
  réponse dynamique**, donc aucun second coup artificiel ne s'ajoute. La
  version embarquée expose `EstimateCollisionResponse`, une estimation de
  collision à deux corps, pas une mesure exacte du solveur en carambolage.
  Agréger les contacts d'un même impact et borner le budget de dégâts par corps
  et par pas pour éviter de compter plusieurs fois la même énergie.
- Intensité fondée sur la vitesse de rapprochement suivant la normale et
  la masse effective, avec rotation et bras de levier : un frottement latéral,
  un choc frontal et un choc excentré ne produisent pas les mêmes effets.
  Le proxy `E = 0,5 × masse_effective × vitesse_normale²` règle la gravité ;
  l'absorption dépend ensuite de la résistance des zones touchées.
- Chocs entre voitures, avec murs, arbres, mobilier et sol : déplacement,
  rotation, glissade et éventuel retournement selon les contacts. Activer la
  détection continue sur les véhicules rapides pour limiter les traversées.
- Profils propres aux neuf véhicules : masse, centre de masse, freinage,
  adhérence, dimensions et résistance des zones. Le réglage tient compte de
  l'échelle visuelle actuelle ; les propriétés mécaniques sont des choix de
  simulation explicitement réglés, pas des mesures constructeur prétendues.
- Petits impacts : marques et enfoncements faibles. Impacts moyens : éléments
  écrasés et tenue de route affectée. Gros impacts : déformation importante,
  roue désaxée ou moteur endommagé selon la zone, jusqu'à immobilisation.
  Les défauts suivent les pièces touchées, avec plafonds de gravité réglables.
- Véhicule accidenté : freinage et arrêt si possible, abandon de la trajectoire
  devenue inaccessible, signalement de sa nouvelle emprise comme obstacle.
  Les autres voitures ralentissent, attendent ou contournent par une voie
  autorisée ; elles ne traversent pas l'épave et ne disparaissent pas à l'impact.
  Sons de choc et petits effets de poussière/étincelles sont bornés et réutilisent
  les capacités et assets disponibles.

#### C. Déformation générique des meshes dans Saida

- Ajouter un composant réutilisable, noms proposés `DeformableMeshBehaviour`
  et `DeformationProfile`, pour un mesh rigide ou un assemblage de meshes.
  Un impact manuel et un contact physique empruntent la même API. Une caisse,
  un panneau ou une barrière doivent pouvoir se déformer avec le même système.
- **Structure de contrôle légère** : points et groupes locaux entraînent les
  sommets du modèle détaillé par des poids préparés une fois. Les groupes
  conservent leur forme localement, avec déformation élastique amortie puis
  modification permanente de la forme de repos au-delà du seuil plastique.
  Choix implémenté : cage régulière de 125 points par défaut, interpolation
  trilinéaire, plasticité et contraintes de gradient, sans solveur par triangle.
  Le shape matching par groupes reste une extension ; la version actuelle
  ne simule pas une coque automobile complète. Le coût de la cage dépend de
  sa résolution ; le déplacement du mesh détaillé est étalé par budget de sommets.
- Préparation automatique de base depuis la géométrie ; réglages facultatifs
  par matériau, sous-mesh et zone. Séparer les parties non reliées pour éviter
  qu'une proximité spatiale entraîne une pièce indépendante. Pour les voitures,
  différencier carrosserie, châssis, roues et vitres : la cage ne doit pas
  écraser les pneus ni tirer un pare-chocs à travers l'habitacle.
- Impact localisé et orienté : l'enfoncement et la torsion suivent la zone de
  contact, la résistance et l'énergie attribuée. Limites de compression,
  déplacements et amortissement empêchent les pointes et effondrements
  incontrôlés. Vérifier les dégénérescences et reculs de déformation ; ne pas
  prétendre résoudre l'auto-collision complète de toute carrosserie.
- **Les modèles partagés restent intacts**. Données préparées partagées,
  état déformé propre au véhicule ; géométrie mutable allouée à la demande.
  Libération sûre lors de destruction, annulation de chargement et réutilisation
  des slots. Toutes les copies comptent dans l'arène de 1 048 576 sommets.
- Déplacer réellement les sommets, mettre à jour les normales/tangentes, les
  bounds, le culling et les ombres. Appliquer le même état de déformation aux
  LOD Near et Far via leurs poids respectifs : changer de LOD ne répare pas
  la voiture. UV et séparations de matériaux sont conservés.
- Collisions de forme : utiliser une représentation grossière composée de
  volumes convexes déformables, distincte du mesh détaillé. Actualiser seulement
  les zones dont la forme change suffisamment, à une frontière sûre du pas,
  en conservant vitesses et paramètres du corps. Aucun triangle mesh dynamique
  détaillé, non pris en charge par le contrat actuel. Vérifier l'écart entre
  collision et silhouette et la stabilité des contacts après remplacement.
- Après stabilisation, conserver la forme et dormir : aucune simulation
  continue pour une bosse figée. Paramètres disponibles dans l'éditeur et
  via API ; état compact versionné, réinitialisation explicite et restauration.
  Conserver les dégâts pendant prise de contrôle, entrée/sortie, rebasage,
  changement de LOD et réutilisation contrôlée des ressources. Le trafic ambiant
  évincé peut être retiré hors vue ; pas de registre illimité d'épaves mondiales.
- Première version : meshes rigides avec topologie conservée. Déchirure de
  triangles, fracture générale, pièces arrachées, animation squelettique et
  simulation industrielle complète d'une carrosserie ne sont pas incluses.
  Les meshes non compatibles sont diagnostiqués explicitement.

#### D. Voies, suivi des routes et continuité

- Construire de vraies voies directionnelles à partir du profil routier déjà
  utilisé pour dessiner la route : largeur, voies par sens, sens uniques,
  conduite à gauche/droite, vitesses et mouvements autorisés. Lire les
  restrictions de virage OSM et les indications de voies disponibles ; repli
  explicite et marqué inféré quand la donnée manque.
- Conserver identité des routes/nœuds OSM et niveau routier : ne pas connecter
  un pont à la route située dessous à cause de la proximité en plan.
  Respecter les ponts existants ; la construction de tunnels visitables reste
  hors de cette mise à jour.
- Trajectoires courbes continues dans les virages et connecteurs de carrefour,
  contenues dans la surface roulable. Orientation, pente et roues suivent la
  route ; anticiper la courbure et réduire la vitesse avant le virage.
  Tenir compte de l'empattement et du gabarit des bus/camions.
- Distance de sécurité entre pare-chocs selon longueur réelle, vitesse et temps
  de réaction ; remplacer l'espacement uniforme actuel prévu pour un bus.
  Accélération et freinage progressifs, prédiction de collision et freinage
  d'urgence. Les voitures en sens inverse ne sont pas prises pour un bouchon.
- Stops, cédez-le-passage, priorité applicable au pays, ronds-points et gestion
  des trajectoires en conflit. Ne pas entrer dans un carrefour dont la sortie
  est bouchée. Arbitrage équitable pour éviter les attentes infinies.
- Changements de voie avec contrôles des intervalles avant et arrière ;
  placement pour tourner, dépassement d'un véhicule lent lorsque permis et
  contournement d'obstacle. Une voie unique réellement bloquée peut rester
  bloquée ; aucune traversée du trottoir ni téléportation de secours.
- Raccorder les réseaux des tuiles chargées par identités stables et conserver
  l'identité de l'agent lors du transfert : position, vitesse, modèle, couleur,
  état physique et dégâts. Attendre avant une frontière non chargée avec une
  distance de freinage suffisante plutôt que disparaître au bout du graphe.
- Naissances et retraits validés par le frustum de la caméra, distance et
  visibilité/occlusion ; limiter le coût des requêtes. Si aucun emplacement
  discret n'est disponible, différer. Ni baisse de population par tuile ni
  transfert de propriétaire ne supprime une voiture visible.

#### E. Feux et piétons

- Relier les feux observés à leurs voies et lignes d'arrêt. Les positions
  mesurées restent prioritaires ; affectation incertaine et temps de cycle
  sont inférés et identifiés comme tels. Ne pas inventer une durée réelle OSM
  ni équiper automatiquement tous les carrefours de feux.
- Phases vert/orange/rouge, intervalle de dégagement et groupes de mouvements
  compatibles. Cycle partagé par tout un carrefour et continuité pendant le
  streaming ; configuration des timings et debug de phase.
- Freinage avant la ligne d'arrêt selon vitesse et distance ; à l'orange,
  arrêter si possible sans freinage dangereux, sinon dégager le carrefour.
  Aucun vert simultané pour deux mouvements configurés en conflit.
- Associer les traversées de la foule au contrôleur du carrefour : autorisation
  de départ piéton et temps de dégagement. Les voitures cèdent aux piétons
  engagés, même si leur propre feu vient de passer au vert.

#### F. Rendu et budgets

- Améliorer peinture, rugosité, reflets et séparation vitres/pneus/métal à
  partir des assets existants. Les bosses doivent rester lisibles dans les
  reflets et l'éclairage. Éviter un matériau unique recréé par voiture et frame.
- Cible globale inchangée : **1080p, 30 fps soutenus**, Ryzen 7 5700U/MX450,
  2 Gio VRAM. Garder le plafond actuel de **80 voitures ambiantes** ; budget
  physique et budget graphique distincts, suivi du CPU/GPU/RAM/VRAM.
- Hypothèses initiales à mesurer : physique à **60 Hz**, décisions IA proches
  à **10–20 Hz**, trafic éloigné à **5–10 Hz** avec interpolation ; cage de
  **128–512 points** par voiture et **4–8 itérations** lorsqu'elle se déforme.
  Les dimensions et fréquences sont configurables, pas des performances prouvées.
- Cibles additionnelles sur scènes comparables : CPU du trafic hors solveur
  **≤ 1 ms au p95**, CPU de déformation **≤ 1 ms au p95** pour huit véhicules
  simultanément touchés. Mesurer séparément physique et uploads ; le verdict
  dépend du temps de frame complet et des pics, pas seulement de ces sous-budgets.
- Index par voie pour les files et index spatial pour les obstacles, préparation
  hors thread principal, aucun recalcul de graphe par voiture. Téléversements
  de géométrie limités aux plages modifiées, groupés et asynchrones, compatibles
  avec les frames GPU en vol et la compaction de l'arène.
- Sous charge, étaler préparation/uploads et baisser la fréquence des décisions
  éloignées ou le nombre d'itérations des déformations secondaires. Les contacts
  imminents restent physiques ; un budget ne rend pas une voiture invulnérable.
  Pas d'effacement de dégâts ni de réduction silencieuse des modèles proches.

#### G. Ordre de réalisation et critères de livraison

1. **Référence et prototype de risque** : mesures actuelles reproductibles,
   comparaison des modèles de conduite, scène générique caisse/panneau,
   premier choc entre deux véhicules avec cage, LOD et collider. Vérifier
   les budgets de copies et uploads avant d'étendre à toute la flotte.
2. **Socle moteur** : événements d'impact, géométrie mutable sûre, déformation
   plastique, colliders, API/éditeur et contrat de sauvegarde de l'état.
3. **Véhicules du jeu** : profils des sept modèles, remplacement de l'arrêt
   instantané actuel par contacts physiques, transitions de simulation,
   dégâts mécaniques, prise de contrôle et intégration géodésique.
4. **Réseau et conducteur** : voies et virages, suivi, priorités, index,
   changements de voie, raccord des tuiles et suppression du popping.
5. **Carrefours vivants** : feux visibles et fonctionnels, traversées piétonnes,
   bouchons et réactions aux accidents, puis finition des matériaux et effets.
6. **Qualification** : tests moteur, plugin et générateur, essais intégrés
   hors ligne, captures/vidéos et profils sur la machine de référence.

Essais de livraison : impacts frontaux à 10/30/60/100 km/h, choc latéral,
frottement, impact excentré, voiture contre camion, mur et arbre, retournement,
chocs répétés et carambolage de huit voitures. Vérifier progression des dégâts
pour un même cas, absence d'énergie artificiellement ajoutée et de cumul de
contacts au repos, conduite affectée au bon endroit et épave réellement solide.
Les profils sont des approximations de jeu ; le système ne sera pas présenté
comme une mesure certifiée d'accident réel.

Essais trafic : courbes serrées, pente/pont, sens unique, rond-point, conduite
à gauche, file compact/bus, feu orange/rouge, sortie de carrefour bloquée,
piéton déjà engagé, changement de voie et route coupée par accident. Parcours
avec caméra tournée à 180°, franchissements de tuiles, rebasage, chargement
retardé et prise de contrôle d'une voiture cabossée. Vérifier que les dégâts
ne changent ni les autres instances ni les assets et survivent aux deux LOD.

Comparer à état/scène/densité identiques sur Paris dense, route rurale, route
sinueuse et pont, plus une scène contrôlée de conduite à gauche ; relever
temps de frame p50/p95/p99/max et mémoire pendant streaming et carambolage.
Ces lieux valident les scénarios observés, sans prétendre mesurer toute la
Terre. Les contrats génériques modifiés sont vérifiés dans les builds natif,
authoring WASM et player Web, conformément aux règles moteur ; toute limite
de backend est explicitement rapportée. Mettre à jour `engine/SPEC.md`,
`engine/ROADMAP.md`, le README du plugin et `game/README.md` avec ce qui est
réellement livré et mesuré.

**Périmètre exclu** : combat/armes, ragdolls et conséquences physiques sur les
piétons, nouvelles simulations de bateaux/avions, tunnels, stationnement
urbain automatique et persistance mondiale illimitée des épaves. Le respect
des piétons par le trafic est inclus. Les autres updates restent séparées.

Références techniques pour le prototype :
[shape matching et plasticité, Müller et al., 2005](https://matthias-research.github.io/pages/publications/MeshlessDeformations_SIG05.pdf),
[contrat d'estimation des impacts Jolt](https://jrouwe.github.io/JoltPhysicsDocs/5.0.0/_estimate_collision_response_8h_source.html).
L'assemblage proposé doit être validé par le prototype ; ce plan ne revendique
ni une technique nouvelle publiée, ni un réalisme ou un coût déjà démontré.

### Updates pays par pays — à traiter plus tard

- **Améliorer la génération pays par pays** : sols, végétation, matériaux,
  bâtiments et détails locaux, à partir de comparaisons avec les lieux réels.
  Les règles doivent aussi tenir compte de la région, du climat, de la saison
  et de l'usage du sol à l'intérieur de chaque pays.
- **Burkina Faso — premier cas signalé** : trop d'herbe dans le rendu actuel
  selon le constat de l'utilisateur. Restituer davantage de terre nue là où
  elle est présente dans la réalité, avec des couleurs et une couverture
  végétale adaptées au lieu, plutôt qu'un sol herbeux par défaut.

### Végétation basse — à traiter plus tard

- **Manque général de buissons dans le jeu**, constaté le 7 octobre lors des
  comparaisons photographiques : ajouter arbustes, haies et massifs dans les
  jardins, en bordure des routes et entre les bâtiments. Adapter leur densité
  au climat, au pays et à l'usage du sol ; respecter les limites observées,
  les accès, les centres urbains et les budgets d'instances/LOD. Vérifier aux
  points GPS des photos, à heure et météo comparables, sans déplacer la caméra.

### Raccord brouillard / skybox HDRI — livré le 19 octobre 2026

- [x] Le bas du ciel rejoint la couleur RGB de `fogColor` en HDR linéaire,
  après les nuages et avant le disque solaire et le tonemapping : raccord
  complet sous 1°, retour progressif à la photographie jusqu'à 6°.
  La verticale suit la planète et fonctionne aussi avec les poses des yeux XR.
- [x] Les derniers 10 % de la distance de clipping finie s'estompent vers
  cette même couleur pour masquer l'arrêt artificiel du terrain.
- Preuves : 148 vérifications GPU météo/horizon sur la MX450, 100/100 CTest
  moteur, shaders mono/XR/Web compilés, deux pipelines validés dans Chrome
  WebGPU. Captures avant/après à Lasné, caméra, date et météo identiques.
  Le principe ciel/brouillard partagé est décrit dans le
  [tutoriel Crytek hébergé par NVIDIA, diapositives 18–20](https://developer.download.nvidia.com/presentations/2007/D3DTutorial_Crytek.pdf).

### Animals update
- **Beaucoup d'animaux, adaptés à l'endroit** : l'espèce découle du biome, de
  la région et de l'usage du sol OSM (vaches et moutons dans les prés, pigeons
  en ville, mouettes sur la côte, cerfs en forêt, chameaux au désert...).
- **Comportements réalistes** : errer, paître, fuir le joueur, voler en groupe.
- **Optimisé** : instances partagées et animations simples, avec un budget par
  tuile et rien au-delà du rayon proche.

### Beach update — générateur v49
- Sable sur le littoral, étendue élargie sur les plages/sables OSM, profil de
  rivage, dunes basses et accès. Les ports, quais, bassins et jetées ont
  priorité et restent exclus. Routes et chemins arrêtent la plage : seules
  les composantes reliées au rivage gardent du sable, côté mer.
- Eau côtière courbe avec profondeur signée, ressac et écume fragmentée,
  transition du sable humide au sec et spectre commun avec la mer au large.
- Serviettes, parasols et foule au repos / aller-retour vers l'eau. Présence
  progressive selon chaleur, soleil et heure ; aucune installation ni baignade
  par froid, pluie, neige, nuit ou météo inconnue. Budget partagé avec la foule
  existante, ressources partagées et poses cadencées selon la distance.
- Le manifeste distingue les surfaces et placements inférés. Les transats
  séparés restent à faire. Détails et limites : `game/README.md`.

### Intérieurs, la suite
Le rez-de-chaussée est aménagé (§7). Reste :
- **Étages visitables, escaliers et ascenseurs.**
- **Les intérieurs des monuments** remplacés par leurs modèles propres.

---

## Annexe — Mesures de référence

Vérifiées contre WGS84 le 3 septembre 2026.

- Aller-retour géodésique ↔ ECEF : erreur maximale **0,0000 mm** sur huit points.
- Pas `float32` / chute due à la courbure : 1 km → 0,12 mm / 0,08 m ·
  10 km → 1,2 mm / 7,8 m · 200 km → 2,4 cm / 3 140 m.


### Motos et radios locales — 17–18 octobre 2026

Les cinq chantiers livrés et leurs vérifications sont documentés dans la section
« Motorcycles and local radio » de [game/README.md](../game/README.md) : occupation et orientation des motos,
assets optimisés et 20 % de motardes, conduite, tir à moto et radios géolocalisées.
