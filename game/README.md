# R1World — jouer

R1World est un jeu d'exploration de la Terre à l'échelle 1:1, générée à partir
de données géographiques. La version actuelle est la **0.1**, en préparation
de sortie. Le catalogue de découvertes animalières reste à développer.

Les [instructions d'installation](../README.md) sont à la racine du dépôt.
La [référence technique](../docs/WORLD.md), le [guide de développement](../docs/DEVELOPMENT.md)
et le [plan](../docs/PLAN.md) couvrent respectivement le fonctionnement,
les vérifications et les travaux ouverts.

## Lancer et choisir un lieu

Sous Windows, lancer `Play.cmd` à la racine, ou :

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File game\Play.ps1
```

Le lanceur compile le moteur et le jeu. Une connexion Internet est nécessaire
pour obtenir les données détaillées d'un lieu nouveau ; les observations déjà
téléchargées restent en cache.

Dans le menu, rechercher une ville, choisir un point sur la carte ou saisir
des coordonnées, puis cliquer **Go**. Le globe descend vers la destination,
puis laisse place à une arrivée en parachute. Le chargement peut prolonger
le survol ; l'écran indique les données attendues. Le joueur peut diriger
sa chute et son parachute. Sans intervention, il rejoint le point choisi.

**Échap** ouvre la carte ; **Reprendre** revient au jeu. **F11** change le
mode plein écran. Les réglages contiennent la destination, les options
graphiques, le multijoueur et les crédits.

## Commandes

Les deux dispositions ZQSD et WASD sont prises en charge.

| Touche | À pied | Voiture / moto |
|---|---|---|
| ZQSD / WASD | Marcher | Accélérer, freiner/reculer, tourner |
| Maj | Courir | |
| Espace | Sauter | Frein à main / frein arrière |
| Souris | Orienter la caméra | Regarder autour |
| F | Prendre le véhicule le plus proche | Sortir |
| M | Portfolio, argent, inventaire et munitions | Même menu |
| Échap | Fermer le menu actif ou ouvrir la carte | Même action |
| P | Sortir/ranger l'appareil photo | |
| 1 / 2 / 3 | Poings / pistolet / lance-roquettes | Pistolet utilisable à moto |
| Clic gauche | Frapper, tirer ou photographier | Tir à moto pendant la visée |
| Clic droit | Viser | Viser à moto |
| E | Recharger | Recharger à moto |
| T | Saisir un code | |
| B | | Activer/couper la radio |
| N / Maj+N | | Station suivante / précédente |
| Page haut / bas | | Volume de la radio |

La radio sélectionne des stations géolocalisées via Radio Browser. Le cache
conserve leurs métadonnées ; écouter un programme en direct nécessite Internet.
La radio s'arrête à la sortie du véhicule et reprend à la prochaine entrée.

### Bateaux et aéronefs

**F** prend le véhicule accessible le plus proche, y compris depuis l'eau.
Pour quitter un bateau, ralentir sous 1,5 m/s. Une voiture peut être quittée
en mouvement ; le personnage conserve les conséquences de sa chute.

| Commande | Avion | Hélicoptère |
|---|---|---|
| Z / W, S | Augmenter/réduire les gaz ; à l'arrêt, freiner puis reculer | Avancer/reculer |
| Q / A, D | Roue avant au sol, inclinaison en vol | Tourner |
| Espace / ↑ | Cabreur | Monter |
| Maj / Ctrl / ↓ | Piquer | Descendre |
| F | Quitter, même en vol | Quitter, même en vol |
| Clic gauche / E | | Tirer une roquette / recharger |

La conduite et le vol sont de type arcade. Un appareil heurtant un bâtiment
est arrêté ; une sortie en altitude fait tomber le personnage.

## Photographie et progression

À pied sur le sol, **P** équipe l'appareil. Orienter la vue avec la souris,
zoomer avec la molette de **1× à 8×**, puis prendre la photo avec le clic
gauche ou **Entrée**. **Tab** change la catégorie manuelle : monde, animaux,
monuments ou insolite. **P** ou **Échap** range l'appareil.

Les animaux visibles sont identifiés automatiquement par espèce ; un cliché
peut figurer dans plusieurs catégories d'espèces sans dupliquer son image.
La détection utilise le cadrage et les collisions : feuillage sans collision
et silhouettes animées restent approximatifs. Les monuments et événements
gardent une catégorie manuelle. La photographie n'est disponible qu'en mode
bureau, à pied, hors chute et baignade.

**M** ouvre le menu du joueur : argent, portfolio, objets, armes et munitions.
Le portfolio propose des filtres et huit miniatures par page. Cliquer une
photo l'agrandit ; la molette et les boutons +/− zooment, le glisser déplace
l'image, **100 %** rétablit le cadrage et **× / Échap** ferme la vue.

Le joueur commence avec 0 €, l'appareil et les armes disponibles. Les photos
ne rapportent pas encore d'argent ; il n'y a pas de boutique ni de collection
complète à remplir.

Les originaux sont dans `saves/photos/` et le profil dans `saves/player.json`.
Ils survivent à la suppression du cache géographique. Un profil corrompu ou
d'une version future est signalé et protégé contre l'écrasement. Conserver
aussi `saves/online-identity.key`, l'identité multijoueur.

## Combat et police

Le personnage dispose de 100 points de vie. Les coups, balles, collisions et
chutes peuvent le renverser ou le tuer. Le pistolet contient 17 balles ;
le lance-roquettes contient une roquette et douze en réserve. **E** recharge ;
un chargeur vide se recharge automatiquement. Le souffle des explosions
respecte les obstacles et peut détruire les véhicules.

En solo, les actes observés par des civils ou policiers augmentent la barre
**Recherche 0–100 %**. Les infractions mineures peuvent mener à une arrestation
avec confiscation des munitions. Les violences ou une alerte d'au moins 40 %
autorisent les tirs. Les roquettes deviennent possibles à partir de 60 %
après des tirs infructueux ; à 100 %, toutes les attaques utilisent des
roquettes et un hélicoptère intervient. Perdre le contact permet à l'alerte
de redescendre. Après la mort, le joueur revient près d'un établissement
de soins résident, sinon à l'endroit de sa chute.

### Codes

**T**, saisir le code, puis **Entrée**. **Échap** annule.

| Code | Effet |
|---|---|
| `buzzoff` | Hélicoptère |
| `plane` | Jet |
| `comet` | Voiture de sport |
| `moto` | Moto de ville |
| `rocket` | Motocross |
| `painkiller` | Invincibilité pendant cinq minutes |

Le véhicule apparaît sur un emplacement disponible ; **F** permet de le prendre.
Un refus explique le manque de place ou une situation incompatible. Les codes
de véhicules sont désactivés en multijoueur.

## Multijoueur

Il est **désactivé par défaut**. L'activer dans **Settings > Online** ; le
choix est conservé. Le panneau affiche les joueurs proches et les problèmes
de connexion ou de version.

Pour rejoindre un ami, lire son code d'invitation dans le même onglet, le saisir
et cliquer **Join**, ou lancer le jeu avec `--join CODE`. Le jeu recherche
l'ami puis utilise l'arrivée habituelle pour rejoindre un point près de lui.
Une recherche sans réponse est signalée après 30 secondes et continue.

Le réseau est pair à pair, sans serveur de jeu central. La découverte utilise
la DHT publique Mainline et les balises LAN. Deux joueurs derrière des NAT
infranchissables ont besoin d'un troisième joueur joignable pour relayer ;
ils peuvent rester séparés s'ils sont seuls.

Les avatars, combats et voitures prises sont partagés. En ligne, trafic,
passants, visiteurs des plages et police sont retirés ; des voitures sont
garées aux mêmes emplacements sur les postes utilisant les mêmes tuiles.
Bateaux, avions, animaux et météo restent locaux. Un joueur en bateau ou en
avion n'est pas affiché chez les autres ; les dégâts d'une roquette sur les
voitures ne sont pas synchronisés.

Les essais entre deux PC sur des réseaux différents, les longues sessions et
les grandes foules restent à qualifier. Voir la [procédure à deux postes](../docs/QUALIFICATION_MULTIJOUEUR.md).

## Graphismes et diagnostic

Les préréglages **Standard** et **Toaster** règlent la qualité. L'illumination
globale dispose d'un sélecteur distinct : Off, Auto, Low, Medium, High et
Ultra / Max. Toaster la coupe temporairement et Standard restaure le choix.
High et Ultra sont plus coûteux ; Ultra sert à comparer le plafond de qualité.

**R** affiche les FPS et active les raccourcis de diagnostic : **V** trafic,
**Y** herbe, **M** eau et **T** arbres. Dans ce mode, M et T remplacent leurs
fonctions habituelles. Appuyer à nouveau sur R restaure ces éléments.

## VR

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File game\Play.ps1 --vr --spawn 2.3522 48.8566
```

Un runtime OpenXR actif est nécessaire. Le mode VR passe directement à la
destination et affiche une vue à la première personne avec suivi des mains.

La marche par microgestes utilise les extensions natives Meta : un contact
pouce/index active une main, ses balayages avant/arrière déplacent de 0,5 m,
gauche/droite tournent de 15°. Un nouveau contact arrête la marche ; l'autre
main peut prendre le contrôle. Menus, perte de focus et changements de mode
de déplacement annulent l'activation. Une perte temporaire de suivi suspend
les gestes. Les extensions absentes sont signalées.

La reprise après perte de suivi et l'alignement visuel doivent encore être
vérifiés sur casque après les derniers correctifs.

## Limites

La précision dépend des observations disponibles : les dimensions non relevées,
une partie de la végétation et des aménagements sont inférés. Seul le
rez-de-chaussée des bâtiments est meublé. Au-delà des tuiles proches, le jeu
affiche le relief, sans la ville détaillée. Des animaux n'ont pas encore de
locomotion utilisable.

La cible de 30 fps à 1920 × 1080 sur Ryzen 7 5700U / MX450 n'est pas qualifiée
partout. Les défauts de chargement, de mémoire et de distribution ouverts sont
suivis dans le [plan](../docs/PLAN.md#7-version-01-et-sortie-prochaine).

## Attribution

Données cartographiques : © contributeurs OpenStreetMap, ODbL. Les sources
d'élévation, d'imagerie, de végétation, de sons et de modèles sont détaillées
dans la [référence technique](../docs/WORLD.md#attribution) et dans
`assets/THIRD_PARTY_ASSETS.json`. Voir aussi les [attributions des animaux](assets/animals/ATTRIBUTION.md).
