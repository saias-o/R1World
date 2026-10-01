# R1World

**Reproduire la Terre entière à l'échelle 1:1, jouable, sur SaidaEngine.**

Ce document garde ce qui ne change pas : la thèse, les contraintes, les
invariants et les décisions prises, puis la liste des prochaines updates (§8).
Le détail de ce qui est fait et mesuré est dans `game/README.md`.

**Deux critères priment sur tous les autres, et ils sont en tension :**

1. **Le monde doit ressembler à la Terre.** Pas à une terre plausible : à *la*
   Terre, à l'endroit précis où le joueur se tient.
2. **Cela doit tourner sur un Core i5 avec une GTX 1060.** C'est la machine de
   référence, celle qui contraint le design.

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

**On ne reproduit pas :** les intérieurs réels (ils sont générés, §8), les
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
Machine de référence : **Core i5 4 cœurs, GTX 1060 6 Go, 1080p / 60 fps.** Sur
i5, **c'est le CPU la ressource rare**, pas le GPU. L'arène de géométrie du
moteur est fixée à **1 048 576 sommets** (`GeometryRegistry::kDefaultMaxVertices`)
et tous les budgets de sommets en découlent (`CLAUDE.md` §5). Une tuile qui
dépasse son budget est refusée, jamais tronquée, et le refus se dit.

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
| Élévation, ailleurs | Copernicus DEM GLO-90 (Open-Meteo) | libre, attribution |
| Météo, visibilité, vent, neige au sol, courants | Open-Meteo | CC BY 4.0 |
| Banquise (glace de mer, son âge) | NOAA CoastWatch/PolarWatch, ASCAT Metop-C | libre, sans garantie |
| Trait de côte hors ligne | Natural Earth | domaine public |
| Trafic maritime moyen | Global Shipping Traffic Density (Banque mondiale) | CC BY 4.0 |
| Canopée (arbres, haies, hauteur) | High Resolution Canopy Height Maps (Meta & WRI), 1 m, images 2009-2020 | CC BY 4.0 |

Les données dérivées d'OSM sont soumises à l'ODbL, avec attribution visible en
jeu. Aucune source `CC BY-NC`. Les assets sont **CC0** (Poly Haven, ambientCG,
Kenney) ou dessinés par le projet, avec une exception : les personnages sont
Microsoft Rocketbox, **MIT**, notice conservée (`assets/licenses/`) et crédit en
jeu, faute d'humains scannés et riggés en CC0. Tous sont normalisés avant
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

**Le détail suit la vitesse.** On ne voit pas les poignées de porte à 130 km/h :
au-delà de 15 km/h le mobilier n'est plus posé, tout revient quand on ralentit.

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
  c'est une `region`, une `band` ou `none`.
- **Sols et matières** : terrain partitionné par classe (`native/gen/terrain.cpp`),
  habillé de textures photographiées CC0 à leur taille réelle
  (`r1/surfaces.py` les prépare) ; la texture apporte le détail, la palette
  garde l'albédo mesuré. L'eau bloque la marche.
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
- **Ponts** : tabliers au gabarit, rampes et remblais à 5 %, piles, culées ; ce
  que le tablier ne peut pas donner est creusé dans le relief
  (`native/gen/bridges.cpp`). On y marche et on y roule, dessus comme dessous.
- **Voiture et circulation** : `F` pour monter/descendre, n'importe quelle voiture
  se prend ; trafic sur le graphe OSM, `maxspeed` tagué d'abord. Véhicules
  dessinés à 0,8 de leur taille, comme les personnes.
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
- **Cache hors ligne** : un lieu déjà visité ne touche plus le réseau ; une
  réponse à une ancienne question Overpass est cuite telle quelle, et ce qui
  lui manque arrive en couche séparée (`kOsmQueryVersion`, `kOsmBaseVersion`).

---

## 8. Prochaines updates

Chacune garde les règles du projet : rien ne dégrade un asset existant
(`CLAUDE.md` §1), les couleurs sont des albédos, tout ce qui est inféré le dit
dans le manifeste (I5), et le coût se compte contre l'arène et le CPU de la
machine de référence (I4).

### Animals update
- **Beaucoup d'animaux, adaptés à l'endroit** : l'espèce découle du biome, de
  la région et de l'usage du sol OSM (vaches et moutons dans les prés, pigeons
  en ville, mouettes sur la côte, cerfs en forêt, chameaux au désert...).
- **Comportements réalistes** : errer, paître, fuir le joueur, voler en groupe.
- **Optimisé** : instances partagées et animations simples, avec un budget par
  tuile et rien au-delà du rayon proche.

### Mall / magasins — générateur v23
- **Des magasins qui ressemblent à des magasins** : les bâtiments tagués
  `shop=*` (ou dont le rez-de-chaussée porte un commerce) reçoivent une
  devanture, une marquise, des vitrines encadrées et le **nom OSM de
  l'enseigne** (`name`, puis `brand`). Un nom générique reste le secours
  lorsque la donnée manque. Décision du 30 septembre 2026 : les noms réels
  sont désormais permis ; les logos et l'architecture exacte ne sont pas inventés.
- **On peut entrer** : double porte coulissante automatique, ouverte des deux
  côtés, seuil raccordé au terrain, plancher horizontal, collisions des murs
  et des meubles. Le rez-de-chaussée occupe l'emprise entière ; les étages
  supplémentaires, escaliers et ascenseurs restent à développer.
- **Des aménagements par usage** : rayonnages et caisses, présentoirs de
  boulangerie, portants de vêtements, kiosques de galerie. Une circulation
  centrale reste dégagée. L'emprise est observée, l'aménagement synthétisé.
- **Trois tailles de supermarché** : la supérette (Carrefour City, Vannes), la
  moyenne surface (Carrefour Market, Theix) et l'hypermarché dans sa galerie
  (Carrefour dans Le Fourchêne, Vannes). Un nœud `shop=supermarket` cartographié
  dans une galerie en devient l'ancre : sa position est mesurée, la surface de
  vente autour d'elle reçoit des gondoles de 7,5 m (trois instances du même
  rayon) et une ligne de caisses face à la galerie.
- **La porte s'ouvre sur une vraie entrée** : les entrées publiques
  cartographiées seulement (pas `access=private`, ni secours ni sortie) ; parmi
  elles, celle dont l'allée est la plus profonde. Sinon la porte est inférée
  vers le parking, sur une allée d'au moins 6 m.
- **Le parking appartient au magasin** : emprises `amenity=parking`, asphalte,
  places et cheminement piéton. Rattachement au commerce voisin indiqué comme
  inféré. À défaut, un parvis peut être inféré dans une parcelle commerciale
  adaptée ; une simple surface de béton ne suffit pas. Routes, eau et
  bâtiments sont exclus ; les tracés suivent le terrain et traversent les tuiles.
- **Généré seulement quand on s'approche** : l'intérieur n'existe qu'à portée
  de la porte (65 m), il est libéré au-delà de 85 m ; deux intérieurs au plus,
  avec contrôle de l'arène et priorité à celui où se trouve le joueur.
  Les meubles sont des instances de prototypes partagés (128 présentoirs et
  deux caisses au plus ; un hypermarché ancré a des gondoles de trois modules
  et douze caisses), les petits emballages ne projettent pas d'ombres.
- **Socle des prochains intérieurs** : `native/gen/interiors.*` expose un plan
  sérialisable (emprise, sol, plafond, portail, recette), puis l'aménagement et
  la géométrie. La donnée OSM et les parkings restent dans `retail.*`. Les
  futures prisons, gendarmeries et églises ajouteront leurs recettes et leurs
  règles d'accès sans recopier la logique des portes ou du streaming.
- **Observations anciennes** : une couche `.retail.json` complète les anciens
  caches sans remplacer l'OSM existant ni bloquer l'arrivée. Une fois présente,
  elle n'est jamais revalidée ; son absence est indiquée dans le manifeste.

### Végétation update
- **La végétation est mesurée, pas seulement inférée** : la carte mondiale de
  hauteur de canopée de Meta et WRI (1 m) dit où sont les arbres et les haies,
  y compris ceux qu'OSM n'a jamais tracés (rangées le long d'un parking,
  arbres isolés, bocage). C'est une couche d'observation générique
  (`native/gen/canopy.*`), que tout système peut interroger : végétation,
  animaux, ombre…
- **Compacte, pour tenir le monde entier hors ligne** : la carte brute (des
  dizaines de téraoctets) n'est jamais gardée. Chaque tuile devient une grille
  de 48 × 48 cellules (~12 m) : arbre (≥ 3 m), végétation basse (1-3 m) ou
  rien, plus la hauteur médiane par bloc de ~100 m, codée par un modèle de
  contexte (comme JBIG). Mesuré : ~200 octets par tuile en bocage breton,
  2 octets en mer ; ~7 Go estimés pour toutes les terres. Un fichier par degré
  carré, jamais un fichier par tuile.
- **Téléchargée une fois, par bandes** : une bande de la carte source couvre
  78 km ; la convertir remplit une centaine de tuiles d'un coup. Le jeu la lit
  ensuite hors ligne.
- **Le mesuré gagne** : un arbre inféré (bord de route, remplissage d'un parc
  ou d'une forêt) ne reste que là où la canopée voit un arbre ; chaque cellule
  arborée vide reçoit un arbre photoscanné à la hauteur mesurée, chaque cellule
  basse un arbuste. Les arbres et rangées cartographiés dans OSM restent à leur
  place. Budget : 640 arbres par tuile mesurée (320 sinon).

### Station-service update
- **Une station n'est jamais un magasin** : `amenity=fuel` (souvent tagué
  `shop=gas`) et un toit sans murs (`building=roof`) n'ont ni rayons ni portes
  coulissantes.
- **L'auvent est un auvent** : l'emprise cartographiée (`building=roof`, ou le
  `wall=no` du cadastre quand une station s'y trouve) devient une dalle sur
  colonnes, 4,7 m de dégagement et 0,9 m de bandeau, qu'on traverse à pied ou en
  voiture. Un `building=roof` quelconque devient un toit sur poteaux. Les autres
  constructions légères du cadastre (`wall=no`) gardent leurs murs.
- **Sous l'auvent** : îlots, pompes, écrans, bornes et colonnes, synthétisés
  sur le rectangle de l'auvent (rien dans OSM ne dit où sont les pompes). Les
  îlots et le totem sont des obstacles ; l'auvent n'en est pas un.
- **Le bandeau dit « station-service » dans la langue du pays** (STATION-SERVICE,
  TANKSTELLE, ESTACIÓN DE SERVICIO, PETROL STATION…), sur la couleur de
  l'enseigne. Le pays vient de la réponse OSM, sinon des frontières embarquées.
  Quand la police ne sait pas écrire la langue (autre alphabet), le bandeau
  porte la marque, et le manifeste le dit. Le totem, au bord de la route, porte
  la marque et aucun prix : un prix n'est pas observé.
- **Sans auvent cartographié**, un auvent est inféré (18 × 9 m sur un nœud,
  dans l'emprise d'une zone), le long de la route et à l'écart des bâtiments et
  des chaussées ; s'il ne tient pas, le manifeste en donne la raison.
- **Observations** : la requête 9 demande les stations ; la couche
  `.retail.json` passe en version 2 pour les apporter aux anciens caches.
  L'ancienne couche reste lue hors ligne en attendant.

### Beach update
- Sur les plages OSM (`natural=beach`) quand l'heure, la saison et la météo s'y
  prêtent : parasols, serviettes, transats et baigneurs dans l'eau et sur le
  sable. La densité suit la chaleur et l'heure, et le manifeste dit que c'est
  inféré.

### Pays froids update, la suite
Le pôle, la banquise et la neige sont faits (§7). Reste :
- **Montagnes** : un relief qui se voit de loin, avec des sommets qui ne soient
  plus rabotés par la grille d'élévation grossière, et un horizon qui ne
  s'arrête plus à la brume de 5 km sur la terre ferme.

### Interior update — générateur v24
- **Le socle des magasins s'étend au bâti ordinaire** : logements privés,
  écoles, commissariats et gendarmeries, bureaux, garages, centres de soins,
  lieux de culte, restaurants, entrepôts et prisons ont des recettes distinctes.
  Les usages tagués gagnent ; les nœuds de service et les campus cartographiés
  complètent les bâtiments anonymes. À défaut, le logement reste une inférence.
- **Le logement contient des pièces** : salon avec canapé et table basse,
  cuisine équipée et coin repas, chambre avec lit et rangement, salle de bain
  avec douche, lavabo et toilettes, quand l'emprise permet ces pièces. Les
  cloisons ont de vrais passages sur un couloir dégagé ; murs et meubles
  arrêtent le joueur. Les écoles ont des tables, chaises et tableaux ; les
  commissariats un accueil, une attente, des bureaux, archives et auditions.
- **Les entreprises ont des bureaux**, écrans, rangements et salles de réunion.
  Les garages ont des établis, des outils et des voitures issues de la flotte
  existante, dedans et devant lorsque l'espace le permet. Le stationnement
  synthétisé exclut bâtiments, routes, eau et terrain trop pentu.
- **Les portes sont animées** : battants dans le bâti ordinaire, portes
  coulissantes conservées pour les magasins, avec capteur des deux côtés.
  Le portail seul est percé dans la façade régionale ; la toiture et les
  hauteurs observées sont conservées. Un seuil raccorde le plancher au terrain.
- **Le mobilier dérive du seed OSM et de la région**. Seul le plan léger
  accompagne la tuile ; les pièces, meubles, portes et lumières sont construits
  à moins de 65 m, libérés au-delà de 85 m, deux intérieurs au plus. Celui du
  joueur reste prioritaire ; les prototypes de meubles et voitures sont partagés.
- **Le budget reste 24 000 sommets par intérieur** et 120 000 par tuile.
  Chaque façade percée est triangulée autour de sa porte avec huit coins
  soudés, au lieu de quatre bandes superposées ; emprises, portes, hauteurs,
  silhouettes et textures régionales sont conservées dans les LOD existants.
  Un refus de capacité se dit et peut être retenté lors d'une visite ultérieure.
- **Observations** : question Overpass 10, couche `.retail.json` version 3
  pour les usages civiques et professionnels manquant aux anciens caches.
  Le manifeste sépare usage observé, rattachement au campus inféré et
  aménagement synthétisé ; il dit aussi pourquoi un bâtiment est indisponible.
- **À poursuivre** : étages visitables, escaliers et ascenseurs. Cette version
  aménage le rez-de-chaussée ; les emprises trop étroites pour une porte, les
  volumes surélevés sans accès et les bâtiments sans hauteur libre restent
  signalés comme indisponibles. Les monuments remplacés par leurs assets
  particuliers ne passent pas encore par ce générateur d'intérieur.

### Residential refinement — générateur v25

Le générateur v25 améliore le logement : séjour/cuisine ouverts, chambres et
pièces d'eau séparées, mobilier rembourré arrondi, literie, frigo, hotte,
chevets, lampes, tapis et plantes lorsque l'espace le permet. Les prototypes
restent partagés, les distances 65/85 m et les budgets restent inchangés.
Les collisions passent par les corps et requêtes du moteur ; les anciennes
collisions de polygones, rectangles et cercles du runtime sont supprimées.
Le winding des boîtes est corrigé et le moteur respecte les matériaux double face.
Le bug de réactivation du corps du personnage est corrigé dans SaidaEngine,
avec un test de régression et le parcours Paris → Tunis après conduite validés.
Le moteur construit désormais les colliders en espace local : le test de
régression vérifie qu'un mur créé à plusieurs millions de mètres de l'origine
reste raccordé au mesh après recentrage du monde.

---

## Annexe — Mesures de référence

Vérifiées contre WGS84 le 3 septembre 2026.

- Aller-retour géodésique ↔ ECEF : erreur maximale **0,0000 mm** sur huit points.
- Pas `float32` / chute due à la courbure : 1 km → 0,12 mm / 0,08 m ·
  10 km → 1,2 mm / 7,8 m · 200 km → 2,4 cm / 3 140 m.
