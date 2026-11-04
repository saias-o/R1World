# R1World

**Explorer la Terre entière à l'échelle 1:1 en photographiant le monde :
monuments, animaux, paysages et événements insolites.**

## À traiter en premier (28 octobre 2026)

- **Le smoke « perf walk » échoue par intermittence.** Depuis le cache des
  contributeurs GI (Saida 7b33d9f : GI/Prepare ~12 → ~1 ms, image ~45 → ~22 ms
  sur la MX450), `R1WORLD_PERF_SMOKE=walk` à Paris finit en
  `FAIL data timeout` 3 fois sur 5. Une fois, deux tuiles n'ont jamais démarré
  le niveau Far de leurs bâtiments (`World::updateBuildingLods`) ; une autre,
  trois tuiles n'ont jamais été servies par le service de cuisson. Le même
  build avec l'ancien parcours GI complet passe, le smoke complet hors ligne
  aussi. C'est une course du streaming que des images plus rapides révèlent,
  et elle se tait : la trouver, et la faire dire (CLAUDE.md §3).
- **Il manque des espèces d'oiseaux.** Paris montre désormais 27 à 37 oiseaux
  autour du joueur, mais seulement pigeons, corneilles, mouettes et
  rouges-gorges : aucun modèle de moineau, merle, étourneau, mésange ni pie.
  Il faut des assets CC0 du même grade que les existants, choisis par
  l'utilisateur (CLAUDE.md §1).

Mis à jour le 3 novembre 2026. Ce plan conserve la direction du jeu, ses
contraintes durables et les travaux restant à faire. Les fonctionnalités
livrées, leur fonctionnement et leurs mesures sont documentés dans
`game/README.md` ; leur historique reste dans Git.

**La version actuelle du jeu est la 0.1. Sa sortie est prévue prochainement.**
Le catalogue de découvertes animalières reste un objectif à terme. La prise de
photo libre, le portfolio persistant et le menu du joueur sont décrits dans
`game/README.md` (29 octobre 2026).

**Le but du jeu est l'exploration par la photographie.** La collection donnera
une raison concrète de voyager : les animaux apparaîtront selon l'endroit du monde où
se trouve le joueur. Compléter la collection imposera de découvrir des pays,
des régions et des milieux différents.

Deux exigences guident les choix :

1. Le monde doit ressembler à la Terre, à l'endroit précis où se trouve le joueur.
2. Le jeu doit tenir 30 fps sur le Ryzen 7 5700U avec NVIDIA MX450, selon I4.

---

## 1. La thèse

> **Le monde n'est pas stocké. Il est décrit, puis régénéré à l'identique chez
> chaque joueur.**

Ce qui transite n'est jamais de la géométrie, mais des observations (OSM,
élévation). La géométrie est fabriquée par le jeu lui-même (`game/native/gen`),
de façon déterministe, à chaque visite. Trois conséquences :

1. **Le volume s'effondre** : le cache garde les observations, jamais la
   géométrie.
2. **Le déterminisme remplace la synchronisation de la géométrie statique** :
   même générateur, mêmes observations et atlas, même tuile, même ville au
   triangle près. Les états dynamiques multijoueurs restent à synchroniser.
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

**On ne reproduit pas :** les intérieurs réels (ils sont générés), les
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
`géométrie = G(version_générateur, profil_génération, tuile, observations, atlas)`.
Le profil versionné contient notamment la cible de sommets ; le manifeste et
`r1cook` la publient. Une clé de tuile identifie les observations, pas à elle
seule une représentation géométrique. Aucune
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
aux arbres (`CLAUDE.md` §1). Les marqueurs de densité sont propres à chaque
tuile et indépendants des observations et de la géométrie déterministe.

Les niveaux de détail doivent conserver le même asset, sa silhouette et son
ancrage. Les transitions sont progressives ; les collisions gardent
l'enveloppe réelle indépendamment du LOD. Les bâtiments Far restent plafonnés
à 12 triangles par bâtiment. Les distances et budgets existants sont décrits
dans `game/README.md` ; ils ne constituent pas de nouvelles tâches à réaliser.

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
7. **Le cache est une promesse.** Un lieu déjà visité se recharge sans requête
   réseau de données du monde (`CLAUDE.md` §7). Les échanges P2P de la future
   MULTIPLAYER UPDATE sont distincts ; le lieu reste jouable hors ligne.

---

## 7. Version 0.1 et sortie prochaine

La **0.1 désigne l'état jouable actuel**, dont la sortie sera préparée
prochainement. La collection animalière à terme ne conditionne pas cette
première sortie.

- Préparer la distribution de la 0.1 et vérifier l'installation et le
  lancement du paquet sur une machine propre. Le paquet existe
  (`game/tools/package_world.py`, l'export du moteur) et démarre hors du
  dépôt, PATH réduit à Windows. **La couche de relief installée doit être
  construite avant** (`generated/tools/r1relief.exe --out assets/world/relief`,
  648 blocs, 859 Mo) : elle manquait sur ce poste, et c'était la cause des
  deux défauts de première visite (tuile en sol plat à 0 m dont le raccord ne
  suivait le voisin qu'aux coins, marches de 18 à 53 m ; anneaux lointains
  refusés). Le jeu le dit désormais (`RELIEF-LAYER-MISSING`, et la raison
  du refus des anneaux), et le paquet refuse de partir sans les 648 blocs.
  Avec le seul bloc de Paris (62 s, 44 Mo lus), un paquet neuf passe son
  smoke dès la première visite, puis 16 raccords fermés à la deuxième.
  Reste : construire la couche entière (plusieurs heures de téléchargement).
- **Textures magenta à Paris (budget des assets décodés).** Depuis que les
  tuiles de Paris ont été recuites (4 novembre, 1 h), les cerisiers urbains
  (5 % des arbres de ville au nord, `gen/scatter.cpp`) s'ajoutent aux arbres
  déjà chargés et le budget de 512 Mo (`kDecodedAssetBudget`) déborde : des
  dizaines de refus « asset memory budget exceeded » par session, et le sol,
  le ciel et des objets dessinés avec la texture manquante, en solo comme en
  ligne. Aucun test n'échoue sur ces refus. Le moteur ne rend la mémoire
  décodée qu'à la libération de l'asset (`AssetLoader`) ; à trancher : ce que
  le jeu garde en prototypes, ce que le moteur garde une fois envoyé au GPU,
  et un smoke qui échoue sur un refus de budget (CLAUDE.md §3).
- Première visite et OpenStreetMap : Overpass répond en 52 s pour un petit
  rectangle de Paris (le miroir en 504), et une requête peut se taire
  jusqu'à environ douze minutes (deux points d'accès, trois essais de
  120 s) avant que l'échec soit dit. Pendant ce temps la ville est
  provisoire, sans bâtiments. À trancher : dire l'attente au joueur, et/ou
  livrer un cache de départ (Paris) dans le paquet, sous ODbL.
- Vérifier les parcours essentiels, les lieux déjà visités hors ligne et
  les limites annoncées sur la machine de référence.
- Documenter les prérequis, commandes et limites du paquet distribué.

---

## 8. Travaux restant à faire

Chaque chantier conserve les règles du projet : ne pas dégrader un asset
existant (`CLAUDE.md` §1), employer des albédos mesurés, distinguer les
inférences dans le manifeste (I5), et mesurer CPU, GPU, RAM, VRAM et arène (I4).
Une implémentation terminée sort du plan ; sa documentation et ses résultats
vont dans `game/README.md`. Une validation non réalisée reste ouverte.

### Animals update — collection photo et exploration du monde

**Objectif principal à terme : prendre en photo tous les animaux du jeu.**
Le catalogue devra dépasser **5 000 animaux différents**. Ce nombre est
une cible de contenu à construire progressivement.

- Étendre la détection des espèces et les catégories du portfolio existantes :
  montrer les animaux qui restent à trouver, avec une progression vers la
  collection complète. Les nouveaux clichés enregistrent déjà les espèces
  cadrées et visibles selon les tests géométriques/collisions du jeu. Affiner
  la visibilité des silhouettes animées et du feuillage sans collision si les
  captures de référence montrent des faux positifs. Étendre ensuite cette
  reconnaissance aux monuments et événements, dont les catégories restent
  manuelles.
- Faire dépendre les apparitions de l'endroit réel du monde : répartition
  géographique, région, biome, climat et habitat/usage du sol. Une espèce
  n'apparaît que dans les lieux qui lui conviennent ; tout le catalogue ne
  peut pas être rencontré au même endroit.
- **La recherche de tous les animaux doit pousser le joueur à explorer la
  planète.** Voyager entre pays, régions et milieux fait partie de la
  progression ; l'exploration reste le but central du jeu.
- Prévoir des animaux terrestres, oiseaux et animaux adaptés aux autres
  milieux, avec comportements cohérents : errer, paître, fuir le joueur,
  voler en groupe. Leur présence doit rendre chaque lieu vivant.
- Construire le catalogue et ses assets progressivement, avec provenance,
  qualité visuelle et répartition géographique explicites.
- Garder des instances partagées, des animations et niveaux de détail adaptés,
  un budget local de population et aucune simulation détaillée hors du rayon
  utile. Les 5 000 animaux du catalogue ne sont pas simultanément chargés.

### MULTIPLAYER UPDATE — pair à pair, sans serveur

**Objectif : rencontrer les autres joueurs là où l'on se trouve, en P2P direct
et sans aucun serveur central** — ni présence, ni matchmaking, ni simulation.
Le transport, la découverte et la réplication sont des briques génériques du
moteur (`engine/plugins/p2p` et `engine/plugins/netcode`, SPEC §11.6–11.7) ;
le découpage géographique et les règles de jeu appartiennent au jeu
(`game/native/world/online*.cpp`). Le fonctionnement livré est décrit dans
`game/README.md`, *Other players*. Ce chantier n'est pas annoncé dans la 0.1.

**Décisions prises (3 novembre 2026)**

- **Pas de rollback pour les personnes.** Le rollback exige que chaque machine
  calcule la même chose ; ici chaque machine a sa propre collision, ses foules
  et sa physique. Chaque joueur fait donc autorité sur son avatar, les autres
  le dessinent un peu dans le passé (`netcode::Replica`), et un coup est jugé
  par sa cible sur son propre historique (compensation de latence sans
  synchronisation d'horloge ni arbitre). Le rollback générique du moteur
  (`netcode::Session`) reste disponible pour un état partagé déterministe à
  venir ; il ne sert pas au monde.
- **Rencontre par proximité, pas par tuile.** Cellules de 0,02° (2,2 km)
  rejointes à moins de 700 m et quittées au-delà de 900 m, chaque joueur lié à
  ses 32 plus proches ; la frontière se traverse sans transfert ni pause.
- **Multijoueur coupé par défaut**, activé en un clic (*Settings > Online*,
  conservé dans `cache/options.json` ; `--online`, `--offline` et `--join`
  le forcent pour une session). Rejoindre un ami passe avant les points chauds.
- **Multijoueur activé, le monde est le même pour tous** : ni trafic, ni
  passants, ni police (chaque machine inventerait les siens) ; des voitures
  garées aux mêmes places pour tout le monde, tirées du graphe routier de
  chaque tuile (`world/parking_rules`). Une voiture prise appartient à son
  conducteur, qui la diffuse comme son avatar ; les autres la dessinent et
  laissent sa place vide. Les animaux et la météo restent propres à chaque
  machine ; les cheats de véhicules sont refusés en le disant.
- **La triche se juge localement** (`Witness`) : chaque machine écarte ce
  qu'elle a vu, rien n'est voté.

**Fait.** Étape 1 (2 novembre) : rollback générique du moteur. Étape 2
(2 novembre) : transport chiffré, perforation des NAT, découverte par la DHT
Mainline et le réseau local. Étape 3 (3 novembre) : les autres joueurs dans
R1World, tirs, coups et roquettes jugés par la cible, smoke à deux instances
réussi. Étape 4 (3 novembre) : la foule (voisinage par proximité, relais par
un pair, DHT IPv6, thread réseau, instantanés de 32 octets par attention,
témoins). Rejoindre un ami (3 novembre) : un code d'invitation par session
(onglet *Online* des réglages, ou `--join CODE`), le sujet `arenaTopic` du
moteur, puis le Go ordinaire jusqu'à quatre mètres de l'ami ; smoke à deux
instances à cinq kilomètres l'une de l'autre réussi. Le monde partagé
(3 novembre) : multijoueur coupé par défaut, interrupteur dans l'onglet
*Online* ; activé, ni trafic ni passants ni police, et des voitures garées
communes, la voiture prise diffusée par son conducteur (protocole
`r1world/online-5`) ; smoke `--cars` réussi (même nombre de places sur les
deux machines, la voiture vue en mouvement, sa place vide). Mesures et
vérifications : `game/README.md` et CHANGELOG du moteur.

**Étape 5 en cours (3 novembre 2026) : les points chauds** (150 000 joueurs à
la tour Eiffel). Fait dans le moteur et vérifié en simulation et par les tests
(SPEC §4.4, §11.7, CHANGELOG) : DHT réduite à la porte même pour une foule
immense (16 clés par cellule, membres silencieux, 8 annonceurs élus par un
résumé circulant, taille de la foule estimée), vue lointaine de la foule
(`sightings`), liens gardés plus longtemps (`keepRatio`, réglé dans le jeu),
AutoLOD sur les maillages animés (et `MSFT_lod` de nouveau écrit), atlas de
billboards animés dont la marche suit la distance parcourue. Reste, dans cet
ordre :

- **Fait (4 novembre) : LOD animés du joueur.** AutoLOD (recompilé : le
  binaire du 16 octobre ignorait les maillages animés) donne au scan du joueur
  trois niveaux, 4 504, 2 363 et 912 triangles, squelette compris
  (`python -m r1.humans --lods`), qui basculent quand il mesure 94, 46 et
  23 pixels de haut (11,6, 23,7 et 47,4 m ; `--coverages`, nouvelle option
  d'AutoLOD). Le moteur gagne `MeshNode::duplicate()`, qui garde la chaîne :
  les six copies du jeu (corps du joueur, autres joueurs, passants,
  motards, animaux, plantes) la perdaient. Les passants gardent leurs deux
  niveaux Blender (2 400 et 500 triangles). Planche de validation :
  `python -m r1.human_views lods` (1,2 %, 2,7 % et 6,7 % des pixels changent
  aux bascules).
- **Imposteurs de personnes cuits (4 novembre), en attente de la validation
  de l'utilisateur, non branchés.** `python -m r1.human_impostor` pose le scan
  du joueur par ses propres clips (`r1/human_views.py`) : 8 azimuts × une
  rangée × 12 poses de `walk` (foulée du clip, 1,214 m) et 8 d'`idle`
  (11,667 s), cellules de 128 px, albédo, normales et visibilité comme
  `tree_impostor.py` ; 2,6 Mo, inscrits dans `assets/THIRD_PARTY_ASSETS.json`
  (`assets/models/humans/impostors/`). Seuls ceux du joueur sont dans le
  dépôt : en ligne, tous les autres joueurs sont son scan ; les 19 passants
  pèseraient environ 50 Mo de plus. Planches à valider dans
  `game/generated/human-impostors/` : chaque cellule ré-éclairée
  (`*_relit.png`) et l'imposteur à côté du maillage qu'il remplace à 23 et
  12 pixels de haut (`*_beside_mesh.png`), pour le joueur et trois passants
  d'échantillon (`--sample`). À valider : l'aspect, puis la distance où ils
  prennent le relais.
- **Dessiner la foule lointaine** : `rc.sightings` (256), `setLook` (cap et
  allure sur 16 bits), un budget fixe d'imposteurs (environ 200, moins en mode
  toaster) placés aux positions vues, prolongées par cap et allure selon leur
  âge, et fondus avec les 48 corps proches. À faire quand une vraie foule
  aura été observée.
- **Couches à la manière des MMO** au-delà de quelques milliers de joueurs au
  même endroit : décision de conception, à prendre avec l'utilisateur ;
  reportée tant qu'aucune foule réelle ne l'exige.

**À qualifier** (une validation non réalisée reste ouverte). La marche à
suivre, le paquet pour le second poste (`game/tools/package_world.py`) et la
lecture des journaux sont dans `docs/QUALIFICATION_MULTIJOUEUR.md`.

- Deux réseaux domestiques, puis un réseau mobile, puis deux joueurs derrière
  un même routeur sans multicast ni hairpinning ; de jour comme de nuit.
- Une session longue (30 min) : liens, relais, débit, pire attente du thread
  réseau (journal `[World online] N linked`), déconnexions et retour hors
  ligne.
- p95/p99 des images sur la MX450 avec 1 à 3 pairs (le résumé par minute les
  donne), puis le coût des imposteurs une fois branchés.
- Une vraie foule, et le coût par pair (débit, CPU, mémoire) selon I4.

**À décider avec l'utilisateur**

- **Un ami gardé quoi qu'il arrive** : le moteur ne garde que les liens vers
  les 48 plus proches ; un ami rejoint est tout près, mais une foule plus
  proche que lui ferait tomber son lien. Un sujet « groupe » dont les membres
  ne sont jamais élagués serait une brique générique du moteur. À faire
  quand une foule réelle le montrera, ou avant si les amis restent ensemble
  en se dispersant.
- **Ce qui reste propre à chaque machine dans le monde partagé** : bateaux,
  avions et hélicoptères des tuiles (un joueur à leur bord n'est pas dessiné
  chez les autres), animaux, météo. Les partager comme les voitures, ou les
  retirer du monde partagé.

### Detail update — couche de détails manuels

- Ajouter une couche de placements et corrections manuels au-dessus des
  modèles prédictifs, sans règle spéciale par lieu dans le générateur.
- Décrire les placements en JSON : coordonnées, modèle, orientation et
  provenance. Les placements manuels priment localement et évitent les doublons.
- Intégrer les monuments au même système en conservant leurs modèles,
  ancrages et orientations mesurés.

### ROCKET UPDATE — qualification

Le lance-roquettes du joueur, les tirs depuis les hélicoptères et les tirs
policiers contre les véhicules sont décrits dans `game/README.md`.
L'explosion SaidaFX, l'éjection et le retrait du véhicule partagent le même
traitement d'impact. Les vérifications sont consignées dans
`docs/ROCKET_UPDATE.md`. Reste à mesurer les poursuites prolongées et le coût
des explosions sur la machine MX450 de référence.
### Véhicules et trafic — extensions et qualifications

- Résistances et dégâts mécaniques par pièce : carrosserie, châssis, roues,
  vitres et moteur, avec conséquences de conduite localisées et bornées.
- Sons et effets dédiés aux impacts, réutilisables et limités par budget.
- Compléter les règles nationales de priorité, les ronds-points et la matrice
  des mouvements en conflit aux carrefours.
- Prendre en charge les restrictions OSM conditionnelles et chaînes via-way,
  le routage vers une destination, le stationnement au bord des rues et les
  contournements autorisés qui manquent encore.
- Réduire le coût des recherches imbriquées de conflits aux carrefours et
  qualifier les bouchons, incidents et franchissements de tuiles prolongés.
- Améliorer les vitrages automobiles, le vernis et les reflets locaux dans
  le moteur générique, à partir des assets existants.
- Qualifier le rendu des déformations sur Web contraint et les performances
  sur casque réel. Conserver les états de dégâts pendant rebasage, LOD et
  prise de contrôle ; ne pas annoncer un backend sur la seule compilation.
- Le shape matching par groupes reste une extension du système générique ;
  la fracture, les pièces arrachées et une simulation industrielle complète
  de carrosserie ne font pas partie du socle à qualifier.

### Updates pays par pays

- Améliorer sols, végétation, matériaux, bâtiments et détails locaux par
  comparaison avec les lieux réels, en tenant compte de la région, du climat,
  de la saison et de l'usage du sol dans chaque pays.
- **Burkina Faso — premier cas signalé :** corriger l'excès d'herbe et
  restituer la terre nue et les couleurs effectivement présentes sur place.
- Poursuivre les vérifications de la végétation basse aux points GPS des
  photos, à heure et météo comparables, en conservant la même caméra.

### Intérieurs, la suite

- Étages visitables, escaliers et ascenseurs.
- Intérieurs des monuments adaptés à leurs modèles propres.

### Fidélité et portabilité — limites à traiter

- Restituer la ville et le couvert détaillé au-delà des tuiles résidentes.
- Affiner les côtes lointaines et les replis côtiers avant réception d'OSM.
- Compléter les données de voirie : polygones municipaux, rampes, îlots,
  raccordements, marquages directionnels et équipements observés manquants.
- Reprendre les palettes du bâti comme albédos mesurés et les finitions des
  monuments qui utilisent encore des feuilles de façade avec fenêtres.
- Pour un port Web ou consoles du monde géographique, intégrer le chargement
  asynchrone des blocs de relief au système d'assets générique du moteur.
