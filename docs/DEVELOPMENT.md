# R1World — développement et vérification

Les commandes partent de la racine du dépôt sauf indication. Prérequis :
[README.md](../README.md). Règles de contribution :
[CLAUDE.md](../CLAUDE.md), et [consignes moteur](../engine/AGENTS.md) pour Saida.

## Compiler

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File game\Play.ps1 -BuildOnly
```

Le lanceur met UCRT64 en tête du PATH, configure le moteur dans
`engine/build-rel` (RelWithDebInfo), puis le jeu dans
`game/generated/world-windows/build`. Le jeu consomme le SDK CMake
`engine/build-rel/sdk` via `Saida::Engine` ; aucun flag n'est extrait de Ninja.

Sortie : `game/generated/world-windows/R1World.exe`.
`-Rebuild` nettoie puis reconstruit le jeu. `R1_BUILD_JOBS` choisit le
parallélisme ; le lanceur le borne selon la mémoire disponible par défaut.
Pour utiliser CMake directement :

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
cmake --build game/generated/world-windows/build --target R1World --parallel 2
```

Le jeu génère le monde en C++, sans processus Python au lancement. Les outils
Python servent à la préparation des assets et aux scénarios de vérification.

## Tests console

Après configuration du build :

```powershell
cmake --build game/generated/world-windows/build --target r1_world_headless_tests --parallel 2
ctest --test-dir game/generated/world-windows/build -L headless --output-on-failure
```

Cette cible construit les tests du runtime et du générateur. Les groupes
Service/Sources/Seams sont enregistrés dans CTest. Les tests de règles réseau
peuvent être isolés avec `--target r1_world_online_rules_test`, puis
`ctest ... -R world_online_rules --output-on-failure`.

Pour la suite complète du générateur et les outils Python, depuis `game/` :

```powershell
sh native/build_tools.sh
generated\tools\r1test.exe
python -B -m unittest discover -s tools\r1\tests -t tools
```

`r1test <filtre>` sélectionne un groupe : `Service`, `Sources`, `Seams`,
`BuildingHeights`, `Streets`, `Nature`, `Fauna`, `Birds`, `Combat`,
`Rocket`, `Police`, `Arrival`… Depuis la racine, fournir `--game game`.
`build_tools.sh` produit aussi `r1cook` ; `--fetch` autorise ses téléchargements.

Certaines suites ont besoin d'observations en cache ou de bibliothèques
d'authoring (NumPy, Pillow, ModernGL, Blender). Un échec faute de fixture
reste un échec à expliquer ; ne pas retirer le test.

### Intégrations ciblées

Les exécutables de test sont sous `game/generated/native-tests/`.

| Domaine | Commande ou cible |
|---|---|
| Géographie sans moteur | `R1World.exe --geo-contract` |
| Profil et visibilité photo | CTest `world_player_profile`, `world_photo_detection` |
| Faune physique et poses sauvées | Cible `r1_animal_physics_test` |
| Poses d'oiseaux | Cible `r1_bird_animation_test` |
| Microgestes VR sans casque | Cible `r1_xr_gesture_test` |
| Véhicules | `powershell -File game/tools/verify_vehicle_runtime.ps1` |
| Poses moto | `python game/tools/verify_motorcycle_pose.py` |
| Motos en jeu | `python game/tools/verify_motorcycles.py --skip-build` |
| Radio | `python game/tools/verify_radio.py --skip-build` |
| Fichiers animaux | [Pipeline de préparation](../data/animal-pipeline/README.md) |

Les builds, tests CPU, inspections de fichiers et captures vérifient des aspects
différents. Pour une modification visuelle, inspecter le rendu du jeu.
Pour une performance, mesurer le build optimisé sur le matériel concerné.
Les résultats d'une séance antérieure ne remplacent pas une exécution courante.

## Smoke en jeu et mode hors ligne

Depuis `game/`, après une première visite du lieu :

```powershell
$env:SAIDA_WINDOW_HIDDEN = '1'
$env:HTTP_PROXY = 'http://127.0.0.1:9'
$env:HTTPS_PROXY = 'http://127.0.0.1:9'
python tools\play_world.py --smoke --spawn 2.3522 48.8566
```

Le driver reconstruit d'abord le jeu. Le smoke teste menu, chargement,
marche, saut, conduite, sortie, reprise d'un véhicule et retour à la carte.
Les contrôles propres à chaque mode restent dans le journal.
Le code 0 et le marqueur PASS sont nécessaires ; lire les refus et les erreurs.

Les proxys fermés vérifient le cache de données géographiques. Ils ne bloquent
pas les paquets UDP P2P. Les variables définies ici concernent le terminal ;
les retirer avant de relancer des téléchargements.

| Option | Vérification |
|---|---|
| `--spawn2 <lon> <lat>` | Deuxième destination, éviction, rebasage et reprise |
| `--dive` | Arrivée complète, transition du globe, chute et parachute |
| `--sail` | Navigation ; exemple Vieux-Port `5.3698 43.2951` |
| `--fly` | Jet, obstacle, atterrissage, hélicoptère et sortie ; Roissy `2.5700 49.0060` |
| `R1WORLD_MENU_SMOKE=1` | Menu seul : clics, maintien, bornes et reset |
| `R1WORLD_PHOTO_SMOKE=1` | Captures, profil isolé, portfolio, équipement et viewer |
| `R1WORLD_PHOTO_ANIMAL_SMOKE=1` | Détection d'un animal réel et filtre d'espèce |
| `R1WORLD_RETAIL_SMOKE=1` | Magasin : entrée, sortie, éviction et reconstruction |
| `R1WORLD_INTERIOR_SMOKE=home\|school\|office\|police\|garage` | Recette d'intérieur |
| `R1WORLD_WADE_SMOKE='<lon> <lat>'` | Parcours sur support au-dessus de l'eau sans passer en nage |
| `R1WORLD_IDLE_SMOKE=1` | Arrêt après marche, dérive sous 5 mm et maintien au sol |
| `R1WORLD_TEST_VEHICLE_ESCAPE=1` | Occupant civil fuyant après des tirs sur son véhicule |
| `R1WORLD_SMOKE_VEHICLE_DAMAGE=1` | Conduite d'un véhicule réellement déformé |
| `R1WORLD_POLICE_SMOKE=1` | Patrouille, poursuite, tirs, appui aérien et arrestation |
| `R1WORLD_ROCKET_SMOKE=1` | Paroi mince, tirs, souffle, destruction et éjection |
| `R1WORLD_DEBUG_SMOKE=1` | Interrupteurs de diagnostic et affichage FPS |
| `R1WORLD_PERF_SMOKE=walk\|drive` | Mouvement après préchauffage, frames marquées `World/PerfPhase` |
| `R1WORLD_QUALITY_SWITCH_SMOKE=1` | Basculements Standard/Toaster pendant le parcours perf |
| `R1WORLD_TRAFFIC_VALIDATE=1` | Véhicule physique bloqué plus de 60 s à moins de 60 m |
| `R1WORLD_TRAFFIC_SECONDS=<60..300>` | Durée du contrôle trafic |

Les tests et captures ignorent les options sauvegardées et utilisent des
profils isolés pour les écritures prévues par leurs scénarios.
`R1WORLD_SMOKE_HOUR` fixe l'heure ; `R1WORLD_SMOKE_QUALITY=standard|toaster`
fixe le préréglage et `R1WORLD_GI_QUALITY=off|auto|low|medium|high|ultra`
l'illumination globale.

### Multijoueur

Visiter le lieu avant de lancer deux instances pour éviter deux cuissons
concurrentes sur une machine à faible mémoire.

```powershell
python game/tools/online_smoke.py
python game/tools/online_smoke.py --versions
python game/tools/online_smoke.py --join
python game/tools/online_smoke.py --cars
python game/tools/online_smoke.py --apart 70
```

Ces modes vérifient combat, refus de version, invitation depuis 5 km,
voiture partagée et avatar distant. Les deux instances doivent écrire
`PASS online`. Ils n'établissent pas la traversée de deux NAT réels.
La [qualification à deux PC](QUALIFICATION_MULTIJOUEUR.md) reste distincte.

## Captures et galerie

Depuis `game/` :

```powershell
$env:SAIDA_WINDOW_HIDDEN = '1'
$env:SAIDA_WINDOW_SIZE = '1600x900'
python tools\play_world.py --smoke --spawn 2.3522 48.8566 --screenshot generated/paris.png --camera-pos '70.448,1.745,83.405' --camera-look '45,2,50' --after-frames 12
```

La caméra est exprimée en mètres depuis le joueur : X est, Y haut, Z sud.
`--camera-geo <lon> <lat> <hauteur-sol>` fixe le point indépendamment d'un
déplacement de spawn ; il requiert un mode de capture et une visée.
La fenêtre cachée mesure 640 × 360 sans `SAIDA_WINDOW_SIZE`.

`--at <unix-seconds>` fixe l'instant, un ciel clair et masque l'interface.
`--weather <nuages> <pluie-mm/h> <visibilité-m>` requiert `--at` et
`--screenshot` : valeurs finies, nuages entre 0 et 1, autres valeurs positives
ou nulles. `--temperature <°C>` accepte −100 à 65 °C en inspection.
Le journal `[World inspection]` donne les paramètres appliqués.

Des variables ciblent les captures de scénarios :

- `R1WORLD_DIVE_SHOT`, `R1WORLD_DIVE_SHOT_AT`, `R1WORLD_DIVE_HOUR`.
- `R1WORLD_PHOTO_MENU_SHOT`, `R1WORLD_PHOTO_VIEWER_SHOT`.
- `R1WORLD_RETAIL_SHOT` et `R1WORLD_RETAIL_SHOT_AT=outside|inside|anchor|room`.
- `R1WORLD_FLY_SHOT` et `R1WORLD_FLY_SHOT_AT=stand|climb|stop|heli-stand|heli|jump`.
- `R1WORLD_POLICE_CAPTURE`, `R1WORLD_ROCKET_CAPTURE`, `R1WORLD_DEBUG_SHOT`.
- `R1WORLD_FAUNA_SHOT` et `R1WORLD_FAUNA_SHOT_SPECIES`.
- `R1WORLD_CAPTURE_LAUNCHER=side|front|down` pour inspecter l'arme.

`tools/gallery.py` capture les vues nommées de son catalogue avec cadrage,
date et météo fixes. Depuis `game/` :

```powershell
python tools\gallery.py
python tools\gallery.py kyoto theix
python tools\gallery.py --offline
```

Les résultats et l'index comparatif vont dans `generated/gallery/`.
Examiner les vues touchées par une modification. Les incertitudes de focale,
d'orientation et d'heure des photographies restent consignées dans les
paramètres des outils et dans les [références rurales](VALIDATION_RURAL_REFERENCES.md)
ou [japonaises](JAPAN_UPDATE.md).

Pour les assets humains : `python -m r1.human_views lods` et
`python -m r1.human_impostor`, depuis `game/` avec `PYTHONPATH=tools`.
Les outils d'asset peuvent demander NumPy, Pillow, ModernGL et Blender.

## Journaux, profils et fichiers locaux

Le lanceur conserve stdout/stderr sous `game/cache/sessions/<id>/game.log`.
Les scénarios donnent leur répertoire en fin de lancement.
Le crash reporter écrit un rapport et un minidump ; conserver le binaire
non dépouillé du même commit pour symboliser les adresses d'un paquet distribué.

`--profile <trace.json>` produit une trace lisible par Perfetto ou
`chrome://tracing`. `tools/analyze_profile.py --tail 180 --skip-last 2`
permet d'écarter le chargement et la lecture finale d'une capture.
Noter matériel, résolution, préréglage, conditions et trajet pour comparer
des mesures. Un test court ou une scène synthétique ne qualifie pas un
budget de frame mondial.

| Emplacement | Contenu et durée de vie |
|---|---|
| `game/generated/` | Builds, outils et résultats reproductibles ; garder le build actif utile |
| `engine/build-rel/` | Build moteur consommé par le jeu ; garder SDK, bibliothèques et dépendances |
| `game/cache/world/`, `game/cache/map-tiles/` | Observations coûteuses à récupérer ; utiles hors ligne |
| `game/cache/fauna-remains*` | Progression persistante des morts, à conserver |
| `game/cache/options.json` | Préférences locales |
| `game/saves/` | Profil, photos et identité réseau, à conserver |
| `game/assets/world/relief/` | Couche installée du relief, nécessaire au paquet |
| `data/source-assets/` | Sources de reconstruction d'assets, à conserver |
| `data/animal-pipeline/review/` | Rapports de validation et provenance utilisés par le pipeline |
| `pipeline_cache.bin`, `__pycache__/` | Caches régénérables |

Les journaux, captures intermédiaires, anciens exports de test et sauvegardes
de paquets ne font pas partie des sources. Ne pas appliquer un nettoyage
global du cache : il contient aussi de la progression persistante.

## Distribution

```powershell
game/generated/tools/r1relief.exe --out game/assets/world/relief
python game/tools/package_world.py
```

La couche complète représente 648 blocs, environ 859 Mo ; sa préparation
peut demander plusieurs heures de téléchargement. Le paquet refuse une couche
incomplète. Le générateur reprend les blocs complets après interruption.
`--threads <n>` borne les téléchargements, `--region <sud> <ouest> <nord> <est>`
sélectionne les blocs et `--measure` n'écrit rien. `--partial-relief` du script
de paquet sert aux essais partiels ; il ne produit pas une distribution complète. Le ZIP est écrit sous `game/generated/package/` ; il contient
le jeu, ses dépendances, `Jouer.cmd`, une notice et la journalisation,
sans cache, sauvegarde ni identité du poste de développement.

Tester une extraction sur un autre poste Windows avec Vulkan 1.3 et pilotes
à jour. Les prérequis de compilation du dépôt ne sont pas ceux du paquet.
Voir [QUALIFICATION_MULTIJOUEUR.md](QUALIFICATION_MULTIJOUEUR.md) pour les essais réseau.
