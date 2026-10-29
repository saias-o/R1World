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

Mis à jour le 29 octobre 2026. Ce plan conserve la direction du jeu, ses
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
  lancement du paquet sur une machine propre.
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

### MULTIPLAYER UPDATE — pair à pair par tuile, UDP, rollback et prédiction

**Objectif : rencontrer les autres joueurs présents sur sa tuile, en P2P
direct et sans aucun serveur central.** Cette update utilisera la brique
network UDP générique prévue dans `engine/ROADMAP.md` (§5). Le transport et
les sessions sont fournis par le moteur ; le découpage géographique et les
règles de synchronisation appartiennent au jeu. Ce chantier est à réaliser,
pas une fonctionnalité annoncée comme disponible dans la 0.1.

- À l'arrivée sur une tuile, découvrir ses joueurs et établir une connexion
  UDP pair à pair avec chacun d'eux. Une tuile vide se joue seul et doit
  permettre à un autre joueur de rejoindre ensuite. Aucun serveur central
  de présence, de matchmaking ou de simulation ne doit être nécessaire.
- Concevoir la découverte et l'amorçage décentralisés : comment un premier
  joueur obtient un pair joignable, comment retrouver les joueurs d'une clé
  de tuile et comment expirer les annonces périmées. Définir aussi la
  traversée des NAT et pare-feu et les cas de connectivité impossibles sans
  relais ; ne pas supposer qu'UDP rend tous les pairs directement joignables.
  Une éventuelle entraide entre pairs doit conserver l'absence de serveur central.
- Au changement de tuile, quitter les sessions de l'ancienne et rejoindre
  les joueurs de la nouvelle automatiquement. Préparer la découverte de la
  destination à l'approche de la frontière, puis transférer l'état à un tick
  défini, sans pause du jeu ni apparition en double. Définir la visibilité et
  les interactions aux frontières, y compris pour deux joueurs de part et
  d'autre, sans conserver indéfiniment les connexions de la tuile quittée.
- Conserver une identité de joueur stable entre tuiles, avec identifiants de
  session et générations de transfert pour rejeter les paquets tardifs de
  l'ancienne tuile. Gérer arrivées simultanées, départs brusques, reconnexions,
  changements rapides de tuile et tuiles momentanément injoignables.
- Mettre en place un netcode moderne à pas de simulation fixe : entrées
  horodatées par tick, prédiction immédiate des actions locales et prédiction
  bornée des entrées distantes manquantes. À réception d'une entrée divergente,
  restaurer un état historique puis rejouer les ticks concernés (**rollback**).
  Lisser les corrections visuelles et interpoler les états distants sans
  retarder les commandes locales ; borner historique et travail de rattrapage.
- Définir l'autorité des joueurs et objets partagés, les conflits simultanés
  et le transfert d'autorité quand un pair part ou franchit une frontière,
  sans arbitre central permanent. Prévoir l'état initial d'un nouvel arrivant,
  la détection des désynchronisations et une resynchronisation bornée lorsque
  le retard dépasse la fenêtre de rollback.
- Qualifier le déterminisme de la simulation physique à rejouer : I3 garantit
  la génération du monde, pas le déterminisme de toute la simulation réseau.
  Synchroniser versions du protocole, générateur, observations et atlas utiles ;
  conserver les positions géodésiques et rendre les états restaurés compatibles
  avec les rebasages ENU. Le rejeu ne doit pas doubler sons, effets, dégâts ni
  écritures persistantes.
- Définir le périmètre des états partagés : déplacements à pied et en véhicule,
  collisions, interactions et conséquences persistantes. Séparer les messages
  de contrôle fiables des entrées/états fréquents, et valider les messages reçus
  avec budgets par pair. Mesurer le coût du maillage P2P quand une tuile se remplit
  et définir ses limites de population, débit, CPU et mémoire selon I4.
- Valider à plusieurs joueurs sur LAN puis Internet, sous latence, gigue,
  pertes, duplication et désordre des paquets. Couvrir les frontières à pied
  et en véhicule, le rollback pendant streaming/rebasage, les déconnexions et
  le retour hors ligne. Mesurer fluidité, corrections, bande passante et
  p95/p99 des frames sur la machine de référence, y compris pendant un transfert.

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
