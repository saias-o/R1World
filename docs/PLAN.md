# R1World

**Explorer la Terre entiÃ¨re Ã  l'Ã©chelle 1:1 et, Ã  terme, photographier tous
les animaux du jeu.**

Mis Ã  jour le 23 octobre 2026. Ce plan conserve la direction du jeu, ses
contraintes durables et les travaux restant Ã  faire. Les fonctionnalitÃ©s
livrÃ©es, leur fonctionnement et leurs mesures sont documentÃ©s dans
`game/README.md` ; leur historique reste dans Git.

**La version actuelle du jeu est la 0.1. Sa sortie est prÃ©vue prochainement.**
Le catalogue animalier et la collection photo sont un objectif Ã  terme,
pas une fonctionnalitÃ© annoncÃ©e comme disponible dans cette version.

**Le but du jeu est l'exploration.** La collection photo donnera une raison
concrÃ¨te de voyager : les animaux apparaÃ®tront selon l'endroit du monde oÃ¹
se trouve le joueur. ComplÃ©ter la collection imposera de dÃ©couvrir des pays,
des rÃ©gions et des milieux diffÃ©rents.

Deux exigences guident les choix :

1. Le monde doit ressembler Ã  la Terre, Ã  l'endroit prÃ©cis oÃ¹ se trouve le joueur.
2. Le jeu doit tenir 30 fps sur le Ryzen 7 5700U avec NVIDIA MX450, selon I4.

---

## 1. La thÃ¨se

> **Le monde n'est pas stockÃ©. Il est dÃ©crit, puis rÃ©gÃ©nÃ©rÃ© Ã  l'identique chez
> chaque joueur.**

Ce qui transite n'est jamais de la gÃ©omÃ©trie, mais des observations (OSM,
Ã©lÃ©vation). La gÃ©omÃ©trie est fabriquÃ©e par le jeu lui-mÃªme (`game/native/gen`),
de faÃ§on dÃ©terministe, Ã  chaque visite. Trois consÃ©quences :

1. **Le volume s'effondre** : le cache garde les observations, jamais la
   gÃ©omÃ©trie.
2. **Le dÃ©terminisme remplace la synchronisation de la gÃ©omÃ©trie statique** :
   mÃªme gÃ©nÃ©rateur, mÃªmes observations et atlas, mÃªme tuile, mÃªme ville au
   triangle prÃ¨s. Les Ã©tats dynamiques multijoueurs restent Ã  synchroniser.
3. **Le monde s'amÃ©liore sans Ãªtre re-livrÃ©** : amÃ©liorer un gÃ©nÃ©rateur repeint
   la planÃ¨te.

Ce qui distingue R1World d'une extrusion OSM de plus :

- **L'Atlas de styles rÃ©gionaux.** Un bÃ¢timent sans attributs hÃ©rite de la
  distribution de *sa* rÃ©gion, pas d'un dÃ©faut mondial. C'est le premier facteur
  de ressemblance, loin devant la qualitÃ© des modÃ¨les 3D.
- **L'infÃ©rence comble le vide sans le masquer**, et elle est marquÃ©e comme
  telle.

---

## 2. Ce que Â« ressembler Ã  la Terre Â» veut dire

Classement par contribution rÃ©elle Ã  Â« je reconnais cet endroit Â». Il gouverne
l'allocation de l'effort.

| Rang | Facteur | Origine | PrÃ©cision |
|---|---|---|---|
| 1 | Silhouette du relief et horizon | IGN RGE ALTI, Copernicus DEM | Exacte |
| 2 | Trame urbaine : routes, Ã®lots, orientation | OSM | Exacte |
| 3 | Emprises des bÃ¢timents | OSM | Exacte |
| 4 | LumiÃ¨re : soleil rÃ©el, atmosphÃ¨re | Ã‰phÃ©mÃ©rides + heure rÃ©elle | Exacte |
| 5 | Hauteurs et gabarits | OSM partiel â†’ Atlas | MesurÃ©e ou infÃ©rÃ©e |
| 6 | MatÃ©riaux et couleurs de faÃ§ade | Atlas | InfÃ©rÃ©e â€” *le point critique* |
| 7 | Toitures | OSM partiel â†’ Atlas | InfÃ©rÃ©e |
| 8 | VÃ©gÃ©tation | OSM + Atlas | InfÃ©rÃ©e par biome |
| 9 | Sols et revÃªtements | OSM partiel â†’ rÃ¨gles | InfÃ©rÃ©e |
| 10 | Mobilier urbain | OSM + kits | MesurÃ© ou infÃ©rÃ© |

Les quatre premiers rangs sont **mesurÃ©s**, donc exacts et quasi gratuits. Le
travail crÃ©atif commence au rang 5 et il est portÃ© par l'Atlas
(`assets/world/atlas.json`).

**On ne reproduit pas :** les intÃ©rieurs rÃ©els (ils sont gÃ©nÃ©rÃ©s), les
personnes rÃ©elles et les plaques, les logos exacts des marques, la
photogrammÃ©trie (elle ne se corrige pas), ni l'exactitude au-delÃ  de la donnÃ©e
disponible.

---

## 3. Les invariants

### I0 â€” La fidÃ©litÃ© arbitre
En cas de conflit, on rÃ©duit la portÃ©e (zone, densitÃ©), jamais la justesse. Seule
exception : les plafonds de I4.

### I1 â€” Aucune coordonnÃ©e ECEF n'atteint le moteur
Le moteur ne voit que de l'ENU local, ancrÃ© prÃ¨s du joueur. Le `float32` n'est
pas ce qui limite la vue (2,4 cm de pas Ã  200 km) ; ce qui limite, c'est le
jitter camÃ©ra/physique, d'oÃ¹ une **origine flottante rebasÃ©e** tous les 350 m
(`native/world.cpp`).

### I2 â€” La courbure est du contenu, pas une erreur
L'ENU est rigide. Aucune Â« correction de courbure Â» nulle part. Horizons :
1,7 m â†’ 4,7 km Â· 50 m â†’ 25 km Â· 4 000 m â†’ 226 km Â· 10 000 m â†’ 357 km.

### I3 â€” Le dÃ©terminisme est un contrat versionnÃ©
`gÃ©omÃ©trie = G(version_gÃ©nÃ©rateur, tuile, observations, atlas)`. Aucune
horloge, aucun alÃ©a non seedÃ© : les tirages d'un bÃ¢timent dÃ©rivent de son
identifiant OSM. Le gÃ©nÃ©rateur est compilÃ© sans contraction flottante
(`-ffp-contract=off`) pour donner la mÃªme ville sur toute machine. Changer un
gÃ©nÃ©rateur incrÃ©mente sa version (`kVersion`), jamais en silence.

### I4 â€” Le budget est un contrat
Machine de rÃ©fÃ©rence : **Ryzen 7 5700U, NVIDIA MX450 2 Gio, environ 7,4 Gio de
RAM utilisable, alimentation secteur, 1920 Ã— 1080 / 30 fps.** Objectif fixÃ© par
l'utilisateur le 9 octobre 2026 : **30 fps soutenus dans le jeu, partout dans
le monde**, soit un budget de **33,33 ms par image**. Il s'applique aux villes
denses, campagnes, forÃªts, cÃ´tes, montagnes et pÃ´les, aux intÃ©rieurs et aux
dÃ©placements Ã  pied ou en vÃ©hicule, y compris pendant le streaming. Une mesure
locale ou une moyenne qui masque des ralentissements ne qualifie pas cet
objectif global ; les distributions de temps de frame et les pics sont relevÃ©s.
CPU, GPU, RAM et VRAM sont mesurÃ©s sur ce portable : aucune ressource n'est
prÃ©sumÃ©e Ãªtre le seul facteur limitant. Les anciens relevÃ©s sur d'autres
machines restent des observations, pas une qualification de cette cible.
L'arÃ¨ne de gÃ©omÃ©trie du moteur est fixÃ©e Ã  **1 048 576 sommets**
(`GeometryRegistry::kDefaultMaxVertices`)
et tous les budgets de sommets en dÃ©coulent (`CLAUDE.md` Â§5). **120 000 sommets
est un seuil de dÃ©gradation, pas de refus** : une tuile dont la gÃ©omÃ©trie propre
le dÃ©passe voit sa foule divisÃ©e par quatre et ses nouveaux intÃ©rieurs bloquÃ©s.
La dÃ©cision est conservÃ©e par rangÃ©e et colonne entre visites, redÃ©marrages et
versions du gÃ©nÃ©rateur. Une tuile de 140 000 sommets reste visitable si l'arÃ¨ne
rÃ©sidente le permet ; seul le plafond global peut encore refuser de la
gÃ©omÃ©trie, en disant pourquoi.

Seule la gÃ©omÃ©trie de la tuile compte, au montage, telle que le manifeste la
mesure. Les props et les arbres sont des nÅ“uds sur des modÃ¨les partagÃ©s
(`CLAUDE.md` Â§5) et n'entrent pas dans ce compte, et la rÃ¨gle ne touche jamais
aux arbres (`CLAUDE.md` Â§1). Les marqueurs de densitÃ© sont propres Ã  chaque
tuile et indÃ©pendants des observations et de la gÃ©omÃ©trie dÃ©terministe.

Les niveaux de dÃ©tail doivent conserver le mÃªme asset, sa silhouette et son
ancrage. Les transitions sont progressives ; les collisions gardent
l'enveloppe rÃ©elle indÃ©pendamment du LOD. Les bÃ¢timents Far restent plafonnÃ©s
Ã  12 triangles par bÃ¢timent. Les distances et budgets existants sont dÃ©crits
dans `game/README.md` ; ils ne constituent pas de nouvelles tÃ¢ches Ã  rÃ©aliser.

### I5 â€” Le jeu sait toujours ce qu'il ignore
Chaque Ã©lÃ©ment porte sa provenance : `mesurÃ©`, `infÃ©rÃ©` ou `synthÃ©tisÃ©`. Ce qui
est mesurÃ© n'est jamais Ã©crasÃ© par ce qui est infÃ©rÃ©, et le manifeste de chaque
tuile dit lequel a rÃ©pondu.

---

## 4. Les donnÃ©es sources

| Couche | Source | Licence |
|---|---|---|
| BÃ¢ti, voirie, eau, usage du sol, aÃ©roports | OpenStreetMap (Overpass) | ODbL |
| Ã‰lÃ©vation, France | IGN RGE ALTI (GÃ©oplateforme) | Licence Ouverte |
| Ã‰lÃ©vation, ailleurs | Terrain Tiles (Mapzen, AWS) : SRTM, 3DEP, EU-DEM, relevÃ©s nationaux | libre, attribution par source (`assets/licenses/`) |
| Ã‰lÃ©vation, en dernier recours | Copernicus DEM GLO-90 (Open-Meteo) | libre, attribution |
| Sommets (altitude relevÃ©e) | OpenStreetMap `natural=peak` avec `ele` | ODbL |
| MÃ©tÃ©o, visibilitÃ©, vent, neige au sol, courants | Open-Meteo | CC BY 4.0 |
| Banquise (glace de mer, son Ã¢ge) | NOAA CoastWatch/PolarWatch, ASCAT Metop-C | libre, sans garantie |
| Trait de cÃ´te hors ligne | Natural Earth | domaine public |
| Trafic maritime moyen | Global Shipping Traffic Density (Banque mondiale) | CC BY 4.0 |
| CanopÃ©e (arbres, haies, hauteur) | High Resolution Canopy Height Maps (Meta & WRI), 1 m, images 2009-2020 | CC BY 4.0 |

Les donnÃ©es dÃ©rivÃ©es d'OSM sont soumises Ã  l'ODbL, avec attribution visible en
jeu. Aucune source `CC BY-NC`. Les assets sont **CC0** (Poly Haven, ambientCG,
Kenney) ou dessinÃ©s par le projet, avec deux exceptions : les personnages sont
Microsoft Rocketbox, **MIT** ; les sept voitures de Ruff et Scopia / Space
Mushrooms sont **CC BY 3.0** ; les deux motos originales de R1World sont
**CC0**. Les notices sont conservÃ©es
(`assets/licenses/`) et les auteurs crÃ©ditÃ©s en jeu. Tous sont normalisÃ©s avant
d'entrer : Ã©chelle mÃ©trique, albÃ©dos mesurÃ©s, un asset non conforme n'entre pas.

---

## 5. Le tuilage

Anneaux de latitude mÃ©triques, sans coupure polaire (`native/gen/common.hpp`) :
36 000 rangÃ©es de 0,005Â° et, par rangÃ©e, `72 000 Â· cos(lat)` colonnes. Une
tuile fait donc environ 556 m de cÃ´tÃ© Ã  toute latitude, et sa clÃ©
(`v<version>_<rangÃ©e>_<colonne>`) ne dÃ©pend ni de la session ni du point
d'arrivÃ©e.

Ã€ moins de 18 km d'un pÃ´le, un anneau n'est plus que quelques quartiers :
le voisinage y est fait des tuiles les plus proches en mÃ¨tres, douze au pÃ´le.

Une requÃªte Overpass sert un voisinage de neuf tuiles. Le jeu cuit d'abord la
tuile du joueur, puis ses voisines par distance, puis un couloir devant le
vÃ©hicule. La gÃ©omÃ©trie rÃ©sidente reste bornÃ©e par l'arÃ¨ne.

**Le dÃ©tail ne dÃ©pend pas de la vitesse.** Le mobilier continue Ã  se charger et
la portÃ©e de la vÃ©gÃ©tation reste celle du mode graphique choisi, mÃªme en vÃ©hicule.

---

## 6. DÃ©cisions prises

1. **Licence et distribution.** Jeu gratuit. DonnÃ©es OSM dÃ©rivÃ©es sous ODbL avec
   attribution visible.
2. **Moteur : dÃ©pÃ´t Saida direct, sans fork.** Une modification moteur n'est
   admise que si elle amÃ©liore le moteur en gÃ©nÃ©ral ; aucune exception
   spÃ©cifique Ã  R1World. Ce qui est propre au jeu se compile dans le binaire du
   jeu, jamais dans la bibliothÃ¨que du moteur.
3. **Le monde est gÃ©nÃ©rÃ© dans le jeu, en C++.** Pas de processus Ã  cÃ´tÃ©, pas de
   Python au lancement ; `game/Play.ps1` compile et lance.
4. **L'heure est rÃ©elle.** Arriver de nuit Ã  Tokyo est l'heure qu'il y est.
5. **Le ciel suit l'heure.** Une journÃ©e de ciels photographiÃ©s (sÃ©rie Qwantani,
   Poly Haven, CC0), fondus deux Ã  deux selon la vraie hauteur du soleil, calÃ©s
   sur la couleur d'horizon du modÃ¨le. Le soleil des photos est retirÃ© ; le
   moteur dessine un seul disque, lÃ  d'oÃ¹ vient la lumiÃ¨re (`r1/skies.py`).
6. **Le joueur est une position gÃ©odÃ©sique.** Ã€ pied, en voiture, en bateau ou
   en vol, il est une longitude, une latitude et une altitude ; une seule
   autoritÃ© sur la position, dont dÃ©pendent le rebasage, le streamer et le
   soleil.
7. **Le cache est une promesse.** Un lieu dÃ©jÃ  visitÃ© se recharge sans requÃªte
   rÃ©seau de donnÃ©es du monde (`CLAUDE.md` Â§7). Les Ã©changes P2P de la future
   MULTIPLAYER UPDATE sont distincts ; le lieu reste jouable hors ligne.

---

## 7. Version 0.1 et sortie prochaine

La **0.1 dÃ©signe l'Ã©tat jouable actuel**, dont la sortie sera prÃ©parÃ©e
prochainement. La collection animaliÃ¨re Ã  terme ne conditionne pas cette
premiÃ¨re sortie.

- PrÃ©parer la distribution de la 0.1 et vÃ©rifier l'installation et le
  lancement du paquet sur une machine propre.
- VÃ©rifier les parcours essentiels, les lieux dÃ©jÃ  visitÃ©s hors ligne et
  les limites annoncÃ©es sur la machine de rÃ©fÃ©rence.
- Documenter les prÃ©requis, commandes et limites du paquet distribuÃ©.

---

## 8. Travaux restant Ã  faire

Chaque chantier conserve les rÃ¨gles du projet : ne pas dÃ©grader un asset
existant (`CLAUDE.md` Â§1), employer des albÃ©dos mesurÃ©s, distinguer les
infÃ©rences dans le manifeste (I5), et mesurer CPU, GPU, RAM, VRAM et arÃ¨ne (I4).
Une implÃ©mentation terminÃ©e sort du plan ; sa documentation et ses rÃ©sultats
vont dans `game/README.md`. Une validation non rÃ©alisÃ©e reste ouverte.

### Animals update â€” collection photo et exploration du monde

**Objectif principal Ã  terme : prendre en photo tous les animaux du jeu.**
Le catalogue devra dÃ©passer **5 000 animaux diffÃ©rents**. Ce nombre est
une cible de contenu Ã  construire progressivement.

- Concevoir la boucle de jeu : rechercher un animal, l'observer, le
  photographier et enregistrer la dÃ©couverte dans une collection persistante.
  Le joueur doit pouvoir consulter ses photos et les animaux qui lui restent
  Ã  trouver, avec une progression vers la collection complÃ¨te.
- Faire dÃ©pendre les apparitions de l'endroit rÃ©el du monde : rÃ©partition
  gÃ©ographique, rÃ©gion, biome, climat et habitat/usage du sol. Une espÃ¨ce
  n'apparaÃ®t que dans les lieux qui lui conviennent ; tout le catalogue ne
  peut pas Ãªtre rencontrÃ© au mÃªme endroit.
- **La recherche de tous les animaux doit pousser le joueur Ã  explorer la
  planÃ¨te.** Voyager entre pays, rÃ©gions et milieux fait partie de la
  progression ; l'exploration reste le but central du jeu.
- PrÃ©voir des animaux terrestres, oiseaux et animaux adaptÃ©s aux autres
  milieux, avec comportements cohÃ©rents : errer, paÃ®tre, fuir le joueur,
  voler en groupe. Leur prÃ©sence doit rendre chaque lieu vivant.
- Construire le catalogue et ses assets progressivement, avec provenance,
  qualitÃ© visuelle et rÃ©partition gÃ©ographique explicites.
- Garder des instances partagÃ©es, des animations et niveaux de dÃ©tail adaptÃ©s,
  un budget local de population et aucune simulation dÃ©taillÃ©e hors du rayon
  utile. Les 5 000 animaux du catalogue ne sont pas simultanÃ©ment chargÃ©s.

### MULTIPLAYER UPDATE â€” pair Ã  pair par tuile, UDP, rollback et prÃ©diction

**Objectif : rencontrer les autres joueurs prÃ©sents sur sa tuile, en P2P
direct et sans aucun serveur central.** Cette update utilisera la brique
network UDP gÃ©nÃ©rique prÃ©vue dans `engine/ROADMAP.md` (Â§5). Le transport et
les sessions sont fournis par le moteur ; le dÃ©coupage gÃ©ographique et les
rÃ¨gles de synchronisation appartiennent au jeu. Ce chantier est Ã  rÃ©aliser,
pas une fonctionnalitÃ© annoncÃ©e comme disponible dans la 0.1.

- Ã€ l'arrivÃ©e sur une tuile, dÃ©couvrir ses joueurs et Ã©tablir une connexion
  UDP pair Ã  pair avec chacun d'eux. Une tuile vide se joue seul et doit
  permettre Ã  un autre joueur de rejoindre ensuite. Aucun serveur central
  de prÃ©sence, de matchmaking ou de simulation ne doit Ãªtre nÃ©cessaire.
- Concevoir la dÃ©couverte et l'amorÃ§age dÃ©centralisÃ©s : comment un premier
  joueur obtient un pair joignable, comment retrouver les joueurs d'une clÃ©
  de tuile et comment expirer les annonces pÃ©rimÃ©es. DÃ©finir aussi la
  traversÃ©e des NAT et pare-feu et les cas de connectivitÃ© impossibles sans
  relais ; ne pas supposer qu'UDP rend tous les pairs directement joignables.
  Une Ã©ventuelle entraide entre pairs doit conserver l'absence de serveur central.
- Au changement de tuile, quitter les sessions de l'ancienne et rejoindre
  les joueurs de la nouvelle automatiquement. PrÃ©parer la dÃ©couverte de la
  destination Ã  l'approche de la frontiÃ¨re, puis transfÃ©rer l'Ã©tat Ã  un tick
  dÃ©fini, sans pause du jeu ni apparition en double. DÃ©finir la visibilitÃ© et
  les interactions aux frontiÃ¨res, y compris pour deux joueurs de part et
  d'autre, sans conserver indÃ©finiment les connexions de la tuile quittÃ©e.
- Conserver une identitÃ© de joueur stable entre tuiles, avec identifiants de
  session et gÃ©nÃ©rations de transfert pour rejeter les paquets tardifs de
  l'ancienne tuile. GÃ©rer arrivÃ©es simultanÃ©es, dÃ©parts brusques, reconnexions,
  changements rapides de tuile et tuiles momentanÃ©ment injoignables.
- Mettre en place un netcode moderne Ã  pas de simulation fixe : entrÃ©es
  horodatÃ©es par tick, prÃ©diction immÃ©diate des actions locales et prÃ©diction
  bornÃ©e des entrÃ©es distantes manquantes. Ã€ rÃ©ception d'une entrÃ©e divergente,
  restaurer un Ã©tat historique puis rejouer les ticks concernÃ©s (**rollback**).
  Lisser les corrections visuelles et interpoler les Ã©tats distants sans
  retarder les commandes locales ; borner historique et travail de rattrapage.
- DÃ©finir l'autoritÃ© des joueurs et objets partagÃ©s, les conflits simultanÃ©s
  et le transfert d'autoritÃ© quand un pair part ou franchit une frontiÃ¨re,
  sans arbitre central permanent. PrÃ©voir l'Ã©tat initial d'un nouvel arrivant,
  la dÃ©tection des dÃ©synchronisations et une resynchronisation bornÃ©e lorsque
  le retard dÃ©passe la fenÃªtre de rollback.
- Qualifier le dÃ©terminisme de la simulation physique Ã  rejouer : I3 garantit
  la gÃ©nÃ©ration du monde, pas le dÃ©terminisme de toute la simulation rÃ©seau.
  Synchroniser versions du protocole, gÃ©nÃ©rateur, observations et atlas utiles ;
  conserver les positions gÃ©odÃ©siques et rendre les Ã©tats restaurÃ©s compatibles
  avec les rebasages ENU. Le rejeu ne doit pas doubler sons, effets, dÃ©gÃ¢ts ni
  Ã©critures persistantes.
- DÃ©finir le pÃ©rimÃ¨tre des Ã©tats partagÃ©s : dÃ©placements Ã  pied et en vÃ©hicule,
  collisions, interactions et consÃ©quences persistantes. SÃ©parer les messages
  de contrÃ´le fiables des entrÃ©es/Ã©tats frÃ©quents, et valider les messages reÃ§us
  avec budgets par pair. Mesurer le coÃ»t du maillage P2P quand une tuile se remplit
  et dÃ©finir ses limites de population, dÃ©bit, CPU et mÃ©moire selon I4.
- Valider Ã  plusieurs joueurs sur LAN puis Internet, sous latence, gigue,
  pertes, duplication et dÃ©sordre des paquets. Couvrir les frontiÃ¨res Ã  pied
  et en vÃ©hicule, le rollback pendant streaming/rebasage, les dÃ©connexions et
  le retour hors ligne. Mesurer fluiditÃ©, corrections, bande passante et
  p95/p99 des frames sur la machine de rÃ©fÃ©rence, y compris pendant un transfert.

### Detail update â€” couche de dÃ©tails manuels

- Ajouter une couche de placements et corrections manuels au-dessus des
  modÃ¨les prÃ©dictifs, sans rÃ¨gle spÃ©ciale par lieu dans le gÃ©nÃ©rateur.
- DÃ©crire les placements en JSON : coordonnÃ©es, modÃ¨le, orientation et
  provenance. Les placements manuels priment localement et Ã©vitent les doublons.
- IntÃ©grer les monuments au mÃªme systÃ¨me en conservant leurs modÃ¨les,
  ancrages et orientations mesurÃ©s.

### ROCKET UPDATE — poursuite policière en véhicule

- Quand le joueur conduit une voiture, permettre à la police de tirer des roquettes sur son véhicule.
- À l’impact, provoquer une explosion, éjecter le joueur de la voiture et faire disparaître (despawn) le véhicule détruit.
- Coordonner l’impact, l’éjection, l’explosion et la suppression du véhicule pour éviter les effets en double ou un véhicule encore utilisable après sa destruction.
### VÃ©hicules et trafic â€” extensions et qualifications

- RÃ©sistances et dÃ©gÃ¢ts mÃ©caniques par piÃ¨ce : carrosserie, chÃ¢ssis, roues,
  vitres et moteur, avec consÃ©quences de conduite localisÃ©es et bornÃ©es.
- Sons et effets dÃ©diÃ©s aux impacts, rÃ©utilisables et limitÃ©s par budget.
- ComplÃ©ter les rÃ¨gles nationales de prioritÃ©, les ronds-points et la matrice
  des mouvements en conflit aux carrefours.
- Prendre en charge les restrictions OSM conditionnelles et chaÃ®nes via-way,
  le routage vers une destination, le stationnement au bord des rues et les
  contournements autorisÃ©s qui manquent encore.
- RÃ©duire le coÃ»t des recherches imbriquÃ©es de conflits aux carrefours et
  qualifier les bouchons, incidents et franchissements de tuiles prolongÃ©s.
- AmÃ©liorer les vitrages automobiles, le vernis et les reflets locaux dans
  le moteur gÃ©nÃ©rique, Ã  partir des assets existants.
- Qualifier le rendu des dÃ©formations sur Web contraint et les performances
  sur casque rÃ©el. Conserver les Ã©tats de dÃ©gÃ¢ts pendant rebasage, LOD et
  prise de contrÃ´le ; ne pas annoncer un backend sur la seule compilation.
- Le shape matching par groupes reste une extension du systÃ¨me gÃ©nÃ©rique ;
  la fracture, les piÃ¨ces arrachÃ©es et une simulation industrielle complÃ¨te
  de carrosserie ne font pas partie du socle Ã  qualifier.

### Updates pays par pays

- AmÃ©liorer sols, vÃ©gÃ©tation, matÃ©riaux, bÃ¢timents et dÃ©tails locaux par
  comparaison avec les lieux rÃ©els, en tenant compte de la rÃ©gion, du climat,
  de la saison et de l'usage du sol dans chaque pays.
- **Burkina Faso â€” premier cas signalÃ© :** corriger l'excÃ¨s d'herbe et
  restituer la terre nue et les couleurs effectivement prÃ©sentes sur place.
- Poursuivre les vÃ©rifications de la vÃ©gÃ©tation basse aux points GPS des
  photos, Ã  heure et mÃ©tÃ©o comparables, en conservant la mÃªme camÃ©ra.

### IntÃ©rieurs, la suite

- Ã‰tages visitables, escaliers et ascenseurs.
- IntÃ©rieurs des monuments adaptÃ©s Ã  leurs modÃ¨les propres.

### FidÃ©litÃ© et portabilitÃ© â€” limites Ã  traiter

- Restituer la ville et le couvert dÃ©taillÃ© au-delÃ  des tuiles rÃ©sidentes.
- Affiner les cÃ´tes lointaines et les replis cÃ´tiers avant rÃ©ception d'OSM.
- ComplÃ©ter les donnÃ©es de voirie : polygones municipaux, rampes, Ã®lots,
  raccordements, marquages directionnels et Ã©quipements observÃ©s manquants.
- Reprendre les palettes du bÃ¢ti comme albÃ©dos mesurÃ©s et les finitions des
  monuments qui utilisent encore des feuilles de faÃ§ade avec fenÃªtres.
- Pour un port Web ou consoles du monde gÃ©ographique, intÃ©grer le chargement
  asynchrone des blocs de relief au systÃ¨me d'assets gÃ©nÃ©rique du moteur.
