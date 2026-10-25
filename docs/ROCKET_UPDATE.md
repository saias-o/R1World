# Rocket Update — 25 octobre 2026

Les règles de travail (`CLAUDE.md`), le plan et les sections combat, police,
vol et compilation de `game/README.md` ont été consultés avant les modifications.
Le contrat SaidaFX est décrit dans `engine/SPEC.md`, section 9.

## Jouer

Lancer `Play.cmd`. À pied : **3** équipe le lance-roquettes, **clic gauche**
tire, **clic droit** vise, **E** recharge. **1** et **2** gardent les poings
et le pistolet. Le lance-roquettes contient une roquette et douze en réserve.

Pour obtenir un hélicoptère : **T**, `buzzoff`, Entrée, puis **F** à proximité.
Le lance-roquettes est sélectionné en prenant les commandes : clic gauche
pour tirer, souris pour orienter la visée. Le code `rocket` continue de faire
apparaître la motocross, conformément au comportement précédent.

En dessous de 100 %, les policiers commencent au pistolet. À partir de 60 %,
les roquettes deviennent possibles après douze secondes de tirs infructueux,
puis leur proportion augmente pendant quatre-vingt-dix secondes. Le niveau
de recherche pondère cette proportion : plafond d'environ 13 % à 60 %,
41 % à 80 %, 66 % à 90 %. Une balle touchant le joueur remet la progression à
zéro ; la perte de contact la réduit. À **100 %, toutes les attaques sont des
roquettes**, à pied comme en véhicule, avec les délais de visée et de recharge.
Un délai partagé de 3 s entre lancements (1,5 s à 100 %) évite les salves simultanées.
L'hélicoptère policier intervient également à 100 %.
L'alerte annonce la visée des roquettes. Les tirs vérifient la visibilité,
anticipent le déplacement et respectent un temps de rechargement.

## Implémentation

- `combat.hpp` : troisième arme, munitions distinctes, rechargement et confiscation.
- `rocket.hpp` : dégâts, budgets, calcul d'interception et règles de visée.
- `rocket_kit.hpp` : géométrie partagée et explosions avec les nœuds GPU SaidaFX.
- `rocket_runtime.hpp` : projectiles, collisions, souffle, retrait des véhicules,
  éjection du joueur, effets et sons.
- `police_runtime.hpp` : tirs depuis les patrouilles, officiers et hélicoptères,
  contrôle de visibilité et délai de remplacement de l'appui aérien détruit.
- `world.cpp` : touches, pose de l'arme, hélicoptères, caméra et origine flottante.

Les modèles et les deux nouveaux sons sont créés par le projet. Les meilleurs
assets existants sont conservés. Aucun code moteur n'a été modifié : les effets
utilisent les capacités documentées de SaidaFX, les collisions et les ragdolls
du moteur. Les roquettes sont non guidées une fois tirées.

## Correction du comportement policier et de la moto

Une balle policière atteignant le châssis compact d'une moto transmet maintenant
12 dégâts au conducteur exposé. Les dégâts ne sont plus ignorés sous prétexte
que le nœud du joueur à pied est désactivé pendant la conduite. Avant une chute,
le personnage est détaché de la moto et reprend sa hiérarchie normale ; le ragdoll
hérite de sa vitesse. Les voitures fermées conservent leur protection et le code
d'invincibilité reste volontairement actif.

Ces corrections ont été effectuées uniquement dans le code et la documentation,
sans lancer de test, de compilation ni de partie, à la demande du joueur.

## Refonte visuelle du lance-roquettes

À la demande suivante du joueur, le modèle a été repris avec des profils
circulaires à 32 côtés : raccords fermés, chanfreins, intérieur du canon et
échappement évasé. Poignées, détente et pontet, coussin d'épaule, colliers,
viseur optique et marquages donnent une silhouette plus détaillée. Cinq
matériaux partagés séparent tube peint, métal, caoutchouc, marquages et verre.
Les normales et l'ordre des faces suivent aussi la paroi intérieure ; aucun
matériau double face ne masque les trous. Le point de départ des roquettes
utilise la même référence que l'embouchure.

La tenue à l'épaule possède ses propres cibles IK, mesurées aux paumes : la
main droite tient la détente, la gauche la poignée avant, doigts repliés.
La position tient compte de la portée du bras gauche en visée basse.

Pour les captures demandées, `R1WORLD_CAPTURE_LAUNCHER=side`, `front` ou
`down` avec `--smoke --screenshot <png> --after-frames 12` impose la direction
de visée et une caméra d'inspection indépendante. `SAIDA_WINDOW_SIZE=1600x900`
fixe la résolution même avec `SAIDA_WINDOW_HIDDEN=1`. Ce mode attend les
ressources du personnage, sans attendre toutes les tuiles lointaines : la
première tentative avait expiré sur des tuiles absentes du cache hors ligne.
La compilation sert ici à produire les vues demandées ; les scénarios de
combat et les suites de tests ne sont pas relancés.

Compilation réussie et trois captures 1600 × 900 produites puis examinées :
`game/generated/launcher-side.png`, `game/generated/launcher-front.png` et
`game/generated/launcher-down.png`. Les raccords et l'intérieur du tube sont
visibles dans ces vues ; la main gauche reste sur la poignée en visée basse.

## Vérifications antérieures à ces corrections

- Compilation optimisée via `game/Play.ps1 -BuildOnly` réussie.
- Tests ciblés : Rocket 5/5, Combat 9/9, Police 2/2, Arrival 11/11.
- Suite native : 456 réussites et un échec de cache Radio avec le répertoire
  temporaire par défaut. Le groupe Radio repasse à 8/8 en utilisant
  `game/generated/tools/tmp` pour TEMP/TMP/TMPDIR, comme le script de compilation.
- Premier scénario en jeu à Paris, fenêtre cachée et proxies fermés : impact
  sur paroi fine, tir du joueur à pied et en hélico, tir réel de l'hélicoptère
  policier, éjection du pilote, destruction unique de la voiture et retrait de
  sa propriété réussis (`game/generated/rocket-smoke.log`).
- Le scénario étendu a aussi passé la protection derrière un mur et la
  conservation de la position de la roquette lors d'un rebasage. Sa vérification
  de destruction de l'hélico policier visait une position fixe alors que celui-ci
  se déplaçait : cette étape a été corrigée pour injecter un impact direct et
  vérifier le retrait ainsi que le délai de remplacement. Elle n'a pas été
  relancée après la demande de laisser les essais au joueur.

Le scénario facultatif reste disponible avec `R1WORLD_ROCKET_SMOKE=1` et
`--smoke`. `R1WORLD_ROCKET_CAPTURE=<png>` demande une capture de l'explosion
finale. Aucune capture finale n'est revendiquée comme validée visuellement.

Les caches hors ligne ne contiennent pas tous les niveaux de relief lointain
de Paris ; leurs avertissements restent dans le journal. Les performances
soutenues sur MX450 et l'appréciation visuelle en jeu restent à vérifier.
