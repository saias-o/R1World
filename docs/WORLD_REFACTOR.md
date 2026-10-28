# Architecture et refactor de World — 28 octobre 2026

## Diagnostic initial

`game/native/world.cpp` contenait 11 187 lignes et une classe `World`
qui possédait presque tout l'état du jeu. Les inclusions de `beach_runtime.hpp`,
`police_runtime.hpp`, `fauna_runtime.hpp` (avec `bird_runtime.hpp`),
`rocket_runtime.hpp` et `rocket_smoke.hpp` ajoutaient encore du code **dans**
la classe. Elles ne constituaient pas des modules indépendants.

Les responsabilités sont entremêlées : lancement, géodésie, préparation CPU
des tuiles, montage de scène, requêtes de sol, streaming, véhicules, population,
combat, environnement, interface, persistance et scénarios de validation.
`update` assure leur ordre d'exécution ; déplacer cet ordre pendant la
migration risquerait de changer la simulation.

Références : `CLAUDE.md`, `docs/PLAN.md`, `game/README.md` et les comptes rendus
des updates. La course de streaming signalée dans PLAN reste un défaut ouvert,
distinct de cette réorganisation.

## Architecture cible

Le point d'entrée `native/world.cpp` configure Saida, construit la session et
branche les callbacks. `native/world/world.hpp` expose la session `World` et
cache son implémentation dans `session.hpp`. Une étape ultérieure pourrait
transférer l'état des acteurs aux systèmes ci-dessous ; cette réécriture
complète n'appartient pas au périmètre de clôture retenu.

| Domaine | Propriété et responsabilité | Dépendances autorisées |
|---|---|---|
| `geography` | Tuilage du runtime, voisinage, coordonnées WGS84, repères ENU | Vocabulaire `gen/common.hpp`, GLM ; aucun moteur actif |
| `tile_preparation` | Conversion des maillages cuits et métadonnées pour le montage | Générateur et types CPU de Saida ; aucune scène |
| `menu_tile_cache` | Cache disque, téléchargements en vol et temporisation | Tuiles de carte, réseau, journal ; aucune session `World` |
| `options_store` | Format des options, validation, lecture/écriture et diagnostics | Bibliothèque standard, JSON, règles heure/météo ; aucun moteur |
| Streaming | Résidence des tuiles, montage progressif, LOD, budgets et éviction | Service de génération, préparation, assets, scène |
| Requêtes du monde | Sol, eau, emprises, surfaces et conversion des positions | Vue en lecture des tuiles, API physique de Saida |
| Joueur et véhicules | Position géodésique du joueur, modes de déplacement et possession | Requêtes du monde, physique moteur, commandes du joueur |
| Population | Trafic, foule, animaux et visiteurs des plages | Tuiles, plugins trafic/faune, assets ; événements de gameplay |
| Combat et police | Armes, dégâts, chute, roquettes, recherche et poursuite | Requêtes physiques, identités des acteurs, commandes de dégâts |
| Environnement | Soleil, météo, mer, relief et monuments distants | Position du joueur, observations, scène |
| Présentation | Menu, arrivée, HUD, minimap, options et audio | Commandes explicites vers la session, états en lecture |
| Diagnostics | Compteurs, inspections, scénarios smoke | Interfaces de session ; aucun scénario lancé implicitement |

Les systèmes doivent posséder leur état et recevoir seulement leurs dépendances
utiles. Ne pas introduire un `WorldContext` contenant tout, des singletons,
un bus d'événements universel ou des classes amies qui restaurent le couplage.
Les changements de véhicule, dégâts et départs de tuile doivent devenir des
opérations explicites. Les pointeurs de nœuds restent des références non
possédantes à la scène Saida ; aucun transfert implicite de propriété.

`Loaded` conserve provisoirement l'état attaché à une tuile. Sa résidence reste
stable dans la map : les graphes de trafic, de marche et d'habitat sont référencés
par les simulations. Ne pas déplacer ces objets ni changer l'ordre de destruction.
Le générateur et les capacités génériques du moteur ne déménagent pas dans le runtime.

## Invariants de migration

- Même grille que le générateur, coordonnées doubles jusqu'au repère local,
  rebasage à 350 m et comportement aux pôles et à l'antiméridien conservés.
- Même ordre de mise à jour, montage, promotion, éviction et post-physique.
- Même durée de vie des services, tâches asynchrones, tuiles et nœuds.
- Même budgets, paramètres, assets, formats persistés, flags et diagnostics.
- Aucun changement de génération : pas de hausse de version du générateur.
- Chaque nouveau module est un vrai `.cpp` compilé séparément, avec un header
  autonome. Aucun `.cpp` ou fragment de méthodes inclus dans une classe.
- Aucun lancement de jeu, aperçu ou test graphique pendant cette session :
  compilation et tests console uniquement, conformément à la demande utilisateur.

## Séquence décidée avant les modifications

1. Extraire `geography`, `tile_preparation` et `menu_tile_cache`, indépendants
   de `World`, avec leurs contrats et tests console pertinents.
2. Séparer la déclaration de `World`, les types de résidence et les constantes.
   Définir les méthodes hors de la classe dans des unités par domaine, y compris
   les anciens fragments `*_runtime.hpp`. Garder les templates nécessaires
   dans le header. Conserver exactement les corps, les valeurs par défaut à la
   déclaration et l'ordre des membres. Ce palier améliore la navigation et les
   recompilations, mais **ne résout pas encore le couplage de la classe**.
3. Vérifier la compilation et l'édition de liens du jeu en RelWithDebInfo avec
   peu de jobs. Vérifier les modules extraits sans initialiser de fenêtre ni de
   GPU. Comparer le contrat géographique avant/après si le binaire initial est
   disponible. Ne pas confondre ces contrôles avec une qualification visuelle.
4. Lors des paliers suivants, transférer état et opérations aux systèmes de la
   table, un domaine à la fois : présentation/options puis environnement,
   streaming/requêtes, véhicules, population et combat. Remplacer les accès
   transversaux par des interfaces étroites et des commandes explicites.
5. Décomposer `update` en phases nommées seulement une fois les propriétaires
   établis. Isoler alors les scénarios smoke derrière les interfaces de session.
   Les vérifications en jeu seront réalisées dans une session qui les autorise.

## Validation et état d'avancement

### Périmètre de clôture demandé

Après le premier palier, l'utilisateur demande de terminer proprement sans
élargir le chantier, puis de committer et pousser l'ensemble sur `main`.
La clôture retient trois finitions : une interface publique courte de session
avec implémentation privée, la persistance des options indépendante du moteur,
et une boucle `update` composée de phases explicites dans l'ordre existant.
Les sorties anticipées doivent interrompre la même frame qu'avant.

Le transfert complet de tous les acteurs vers de nouveaux systèmes n'est pas
un critère de clôture : il multiplierait les changements de propriété sans
validation en jeu. Les étapes 4 et 5 ci-dessus restent une direction possible,
pas une réécriture supplémentaire à lancer dans cette session.

### Premier palier

Plan écrit avant le refactor. Premier palier réalisé le 28 octobre 2026 :

- `world.cpp` : **11 187 → 104 lignes**, limité au démarrage et aux callbacks.
- **481 méthodes** déplacées dans 29 unités par domaine ; leurs corps ont été
  comparés au code initial en ignorant seulement l'indentation, y compris le
  constructeur et `update`. Les sept anciens fragments de classe ont été
  supprimés, avec leurs implémentations conservées dans les unités correspondantes.
- Trois autres unités autonomes : `geography.cpp`, `tile_preparation.cpp`,
  `menu_tile_cache.cpp`. Les sources sont explicitement déclarées dans CMake.
- `world.hpp` garde la déclaration, l'ordre et les valeurs initiales des membres.
  `loaded_tile.hpp` rassemble les données de résidence ; `limits.hpp` conserve
  les budgets existants. Les sept headers passent la compilation séparément.
- Compilation et édition de liens du jeu réussies en **RelWithDebInfo**,
  SDK `engine/build-rel`, deux jobs au maximum.
- **3/3 tests console réussis**, enregistrés sous le label CTest `headless` :
  géographie (pôles, antiméridien, voisinage, repères et rebasage), préparation
  CPU (sommets, LOD, collisions, métadonnées et durée de vie après libération),
  cache de carte (fichiers locaux, absence persistée, décompte et isolation des sources).
- Les **35 cas** de `--geo-contract` donnent exactement les mêmes positions,
  erreurs de conversion et indices de tuiles que le binaire présent avant les
  modifications. Ce binaire était encore en **v62**, alors que les sources
  étaient déjà en **v63** : seul le préfixe de version des clés a été neutralisé
  pour la comparaison. Le refactor n'a pas modifié `gen/common.hpp` ni `kVersion`.
- Aucun jeu, aperçu, smoke graphique ou capture lancé. `--geo-contract`
  retourne avant la construction du moteur. Les tests ne créent aucune fenêtre.

Les premières vérifications ont détecté des déclarations manquantes pour les
pointeurs du header `loaded_tile.hpp`, corrigées par des déclarations anticipées.
Le test de préparation a également été corrigé : le cas de métadonnées absentes
doit être vérifié avant de libérer la géométrie source, que Saida refuse à juste
titre de préparer une seconde fois une fois vide.

À ce premier stade, trois modules étaient autonomes, mais la déclaration de
session et la grande méthode `update` restaient exposées. La clôture ci-dessous
traite cette frontière et la lisibilité de la boucle, sans transférer les acteurs.

### Clôture du refactor

- Le point d'entrée compte **124 lignes**, avec ses dépendances désormais
  explicites. L'interface publique compte **51 lignes** et ne dépend que de
  la bibliothèque standard et de déclarations anticipées des types utilisés.
  L'implémentation privée conserve l'ordre de construction/destruction existant.
  La session n'est ni copiable ni déplaçable ; les callbacks gardent une adresse
  stable, et la session est détruite avant le moteur.
- La méthode `update` passe de **1 033 à 39 lignes**. Elle appelle huit phases :
  interface, session/streaming, scène, joueur, caméra, capture, HUD et smoke.
  Les phases qui pouvaient terminer une frame retournent un booléen explicite ;
  leurs appels propagent immédiatement cet arrêt. Le delta physique borné est
  calculé au même endroit et transmis aux phases suivantes.
- `OptionsStore` gère le format `cache/options.json` hors du moteur. Les valeurs
  chargées sont un instantané de persistance, pas un second état du jeu. La
  session reste responsable de l'application au ciel, à la fenêtre et à l'UI.
- L'audit de déplacement vérifie **477 corps de méthodes inchangés**, les huit
  blocs extraits, leur ordre et le corps de `main`. Les quatre méthodes exclues
  sont `update`, `optionsFile` (supprimée), `loadOptions` et `saveOptions`.
  Le format des options fait l'objet du quatrième test console.
- Validation finale : compilation/édition de liens **RelWithDebInfo réussies**,
  **4/4 tests console réussis**, neuf headers compilés séparément et interface
  publique compilée avec la seule bibliothèque standard. Les 35 cas géographiques
  restent identiques selon la comparaison de version explicitée plus haut.
  `git diff --check` et l'audit de clôture passent. Aucun test graphique lancé.

**Périmètre terminé :** séparation des unités, frontière publique de session,
quatre modules autonomes et orchestration lisible. Les systèmes de gameplay
partagent encore l'état privé ; leur transformation complète en contrôleurs
indépendants est hors périmètre. Aucun gain de performances, validation visuelle
ou correction de la course de streaming n'est revendiqué.

Reproduction depuis la racine, MSYS2 UCRT64 en tête du PATH :

```powershell
cmake --build game/generated/world-windows/build --target R1World r1_world_headless_tests --parallel 2
ctest --test-dir game/generated/world-windows/build -L headless --output-on-failure
game/generated/world-windows/R1World.exe --geo-contract
```

Les journaux de cette session et l'audit de déplacement sont conservés localement
dans `game/generated/world-refactor/` (répertoire généré, non versionné).
