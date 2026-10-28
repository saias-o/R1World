# Audit de R1World — 29 octobre 2026

Référence du dépôt principal : `69846f7`. Générateur des sources : `v63`.

## Conclusion

**Le problème principal n'est pas le nombre de constantes : ce sont des contrats importants qui ne sont pas garantis de bout en bout.** Le projet sait expliquer ses intentions, sépare déjà correctement plusieurs briques du moteur et dispose de tests utiles. Mais le streaming peut perdre une invalidation, les échecs ne remontent pas tous jusqu'à la décision de chargement, et certains états persistants échappent aux budgets du monde.

Cet audit retient **huit constats substantiels**. Il ne demande ni réécriture générale, ni remplacement des assets, ni nettoyage cosmétique. Les compromis explicitement documentés sont distingués des défauts de code.

| ID | Priorité | Constat | Nature de la preuve |
|---|---|---|---|
| A1 | P1 | Une modification de relief peut ne jamais invalider une première cuisson en cours | Reproduction isolée du service |
| A2 | P1 | Un cache de relief corrompu bloque durablement sa tuile ; l'écriture peut publier un fichier incomplet | Reproduction de lecture + analyse de l'écriture |
| A3 | P1 | L'échec d'un upload de terrain peut rester traité comme un chargement en cours | Parcours moteur → jeu vérifié dans les sources |
| A4 | P1 | La persistance des animaux morts implique une résidence et des sauvegardes sans borne | Comportement explicite du code et de la documentation |
| A5 | P2 | Les limites 12/25 du service peuvent écarter des tuiles locales au profit de la prélecture | Reproduction avec la géographie réelle du projet |
| A6 | P2 | Le joueur, la foule, la police et le trafic n'avancent pas à la même vitesse temporelle lors des ralentissements | Calcul direct à partir des chemins de mise à jour |
| A7 | P2 | Le contrat de génération omet un paramètre qui change effectivement la géométrie | Code + test existant exécuté |
| A8 | P2 | Le découpage de World ne centralise pas encore les transitions de possession et de déplacement | Analyse architecturale, dette déjà reconnue |

**P1** : à traiter avant de considérer la robustesse ou les budgets comme acquis. **P2** : correction ciblée à programmer ; le scénario ou l'impact est plus limité. Aucun de ces classements ne prétend remplacer une qualification en jeu.

## Périmètre et méthode

Lecture des règles racines, de `PLAN`, du compte rendu `WORLD_REFACTOR`, des comptes rendus Japon, Rocket et références rurales ; lecture des sections techniques pertinentes du README du jeu et des documents du pipeline animalier. Consultation des contrats Saida, de ses règles de contribution et des plugins trafic/faune pour contrôler la frontière jeu/moteur.

Le code examiné en profondeur couvre le service de génération, les observations et raccords de relief, le montage et les LOD, les budgets, la session, les déplacements, le trafic, les animaux morts et les tests associés. Ce n'est pas un audit exhaustif des dépendances vendoriées, du rendu Vulkan ou de chaque asset.

Les reproductions sont **console, sans réseau et sans GPU**. Aucun gameplay n'a été modifié. Les résultats graphiques, les pics de mémoire et les performances MX450 ne sont pas mesurés dans cet audit. Les sources et journaux des sondes sont conservés dans [le dossier local de vérification](C:/Users/sandr/OneDrive/Documents/R1world/game/generated/audit-2026-10-29), qui est un dossier généré non versionné.

## A1 — P1 : une invalidation de relief peut être perdue

**Références :** [service.cpp:413](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:413), [observe:648](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:648), [publication:702](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:702), [ordonnancement:213](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:213).

`groundChanged()` ne signale un changement qu'aux tuiles déjà présentes dans `cooked`. Or une tuile en première cuisson peut avoir lu le relief de ses voisines sans être encore publiée dans cette map.

Scénario :

1. A prend son instantané d'observations, avec l'ancien relief disponible autour de B.
2. La première cuisson de A continue ; A appartient à `busy`, mais pas à `cooked`.
3. Le relief détaillé de B arrive. `groundChanged(B)` ignore A à cause de `!cooked.count(A)`.
4. A publie sa géométrie construite sur l'ancien instantané.
5. Si ses propres sources sont complètes, aucun état restant ne demande sa recuisson.

La sonde place directement le service dans cet entrelacement et obtient :

```text
SEAM_NOTIFICATION initial_cook_notified=0 next_scheduled=0
```

**Conséquence :** le terrain peut conserver un raccord calculé sur un état dépassé, jusqu'à une autre invalidation ou une éviction. Deux mêmes visites peuvent converger différemment selon l'ordre des téléchargements. Le problème est une perte de notification logique, même si les accès aux conteneurs sont protégés par mutex.

La même chaîne possède une autre lacune : [sharesSeams:405](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:405) compare les bornes de longitude sans bouclage. À l'équateur, les colonnes 71999 et 0 sont voisines ; la fonction répond pourtant faux. La notification d'un meilleur relief ne traverse donc pas cette couture, alors que [le calcul du raccord](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/seams.cpp:156) sait boucler les colonnes.

**Amélioration :** conserver une révision des dépendances de relief, incluant les cuissons en cours. À la publication, comparer les révisions lues aux révisions courantes et reprogrammer la tuile si nécessaire. Faire calculer le voisinage d'invalidation par la même géographie que le raccord.

**Validation attendue :** bloquer A après lecture de ses observations, publier le nouveau relief de B, libérer A et vérifier qu'une nouvelle version de A paraît ; comparer ensuite les deux côtés de la couture. Ajouter la frontière ±180°.

Ce défaut **n'est pas identifié comme la cause** du `perf walk` intermittent signalé dans PLAN. Cette relation reste à établir.

## A2 — P1 : le cache n'a pas de récupération fiable après corruption

**Références :** [sources.cpp:43](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/sources.cpp:43), [recherche des versions:105](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/sources.cpp:105), [lecture du relief:243](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/sources.cpp:243), [service.cpp:590](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:590), [traitement d'erreur:695](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:695).

`find()` prend le fichier de la version la plus récente qui existe. `ground()` le parse sans récupération si son JSON ou son contenu est invalide. L'exception sort de `observe()` **avant** le téléchargement du relief et les solutions de repli. Le service attend ensuite 60 secondes et recommence la même lecture.

Une sonde avec un fichier de relief tronqué donne deux fois :

```text
CORRUPT_CACHE attempt=1 throws=1 file_retained=1
CORRUPT_CACHE attempt=2 throws=1 file_retained=1
```

Un fichier plus ancien encore valide n'est pas essayé. Le retour du réseau ne suffit pas à réparer ce chemin : le fichier présent continue d'être choisi.

L'écriture fragilise également cette promesse. `writeJson()` utilise un temporaire, mais ne vérifie ni le succès de l'écriture ni celui de la fermeture avant de le renommer. Une erreur de disque peut ainsi laisser un temporaire partiel qui sera publié. Si le premier renommage échoue, le code supprime la destination avant une seconde tentative : l'ancienne bonne observation n'est plus protégée jusqu'au succès du remplacement. Le code de sauvegarde des animaux vérifie déjà l'écriture et utilise une primitive de remplacement Windows ; les deux politiques de persistance ne sont pas cohérentes.

**Conséquence :** une donnée régénérable devient un obstacle permanent à l'accès d'un lieu, en contradiction avec la promesse de cache et de dégradation explicite.

**Amélioration :** vérifier écriture/flush/fermeture, conserver l'ancienne version jusqu'au remplacement réussi, et distinguer « absent », « valide » et « corrompu ». Mettre un fichier invalide en quarantaine avec diagnostic, essayer les versions valides antérieures, puis le relief installé ou le téléchargement si autorisé. Ne pas appliquer cette récupération destructive aux sauvegardes du joueur.

**Validation attendue :** JSON tronqué, bornes invalides, échec d'écriture, remplacement refusé, puis redémarrage hors ligne avec une ancienne observation valide.

## A3 — P1 : un upload définitivement échoué reste une attente pour le terrain

**Références :** [uploadParts:85](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/tile_streaming.cpp:85), [MeshCache.cpp:174](C:/Users/sandr/OneDrive/Documents/R1world/engine/src/graphics/MeshCache.cpp:174), [condition d'arrivée:428](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/streaming.cpp:428), [timeout:61](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/update.cpp:61).

Le moteur crée d'abord un mesh non chargé. L'upload est asynchrone. S'il échoue, `pumpUploads()` journalise l'erreur, annule l'upload et retire le travail de sa file. Le mesh reste non chargé.

Côté terrain, `uploadParts()` a déjà avancé `nextPart` et conservé ce mesh dans `partMeshes`. La condition d'arrivée se contente ensuite d'attendre que chaque `mesh->loaded()` devienne vrai. Ce chemin ne distingue pas un travail encore actif d'un travail abandonné après erreur.

**Conséquence :** un échec GPU peut conduire à une attente sans issue puis à `FAIL data timeout`, alors que le moteur connaît déjà l'erreur. Il existe un log moteur ; le problème est son absence de traduction en état terminal du chargement et en réponse exploitable par le jeu. C'est précisément le type de défaut interdit par CLAUDE §3.

La différence interne est parlante : [les LOD de bâtiments](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/tile_streaming.cpp:183) détectent certains échecs lorsque les chargements sont terminés et font échouer le smoke. Les morceaux de terrain n'ont pas le traitement équivalent. Pour les remplacements, [promoteIncoming](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/streaming.cpp:262) peut même afficher une version non prête après expiration du délai.

**Amélioration :** exposer un résultat d'upload par ressource — en attente, prêt, échoué et raison — dans le moteur générique. Le runtime doit alors soit conserver l'ancienne tuile, soit retenter après libération de ressources, soit refuser explicitement l'arrivée. Un timeout ne doit pas décider qu'une géométrie incomplète est devenue acceptable.

**Validation attendue :** injection d'un échec sur un morceau de terrain, sans GPU réel si le contrat est injectable ; absence d'attente infinie et conservation de l'ancienne représentation. Le chemin d'erreur est établi par lecture ; aucun défaut GPU n'a été injecté pendant cet audit.

## A4 — P1 : la persistance des cadavres est confondue avec leur résidence

**Références :** [animal_remains.cpp:141](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/animal_remains.cpp:141), [pause des corps:207](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/animal_remains.cpp:207), [ajout:250](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/animal_remains.cpp:250), [sauvegarde:70](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/animal_remains.cpp:70), [budget physique:142](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/limits.hpp:142).

Les corps restaurés sont montés à l'approche, puis restent montés sans éviction par distance ou par nombre. Quitter leur terrain suspend une chute ; cela ne libère pas leur représentation. Les corps stabilisés conservent notamment un collider statique. La documentation le dit explicitement dans [Animal deaths and persistent remains](C:/Users/sandr/OneDrive/Documents/R1world/game/README.md:2758).

Ce choix laisse croître les nœuds, poses et colliders avec **l'historique de la partie**, alors que les budgets du monde sont calculés sur un voisinage borné. `kLiveRagdolls=8` ne borne pas ces cadavres d'animaux. Le budget physique n'exprime aucune réserve capable de couvrir une population de corps conservée indéfiniment.

À cela s'ajoute une sauvegarde globale sur le thread du jeu : chaque sauvegarde reconstruit le JSON de tous les corps, puis effectue un remplacement avec `MOVEFILE_WRITE_THROUGH`. Elle est appelée lors des tirs, explosions, stabilisations et départs de terrain. Le travail par sauvegarde augmente avec le nombre de corps et d'os ; une série de morts peut provoquer des réécritures cumulées quadratiques du volume historique.

**Conséquence :** le coût d'une scène dépend de toutes les zones parcourues et de tous les animaux tués. Aucune valeur finie du budget physique ne peut garantir cette politique pour une partie arbitrairement longue. Les durées exactes des pauses disque restent à mesurer.

**Amélioration :** conserver intégralement les morts et leur pose dans une persistance indexée géographiquement, mais monter seulement les corps utiles autour du joueur. Démonter les corps éloignés après instantané, sans les effacer du monde. Borner les simulations actives et les écritures ; sérialiser un instantané immuable en arrière-plan, avec regroupement des changements et vidage contrôlé à la fermeture.

**Validation attendue :** longue partie synthétique avec plusieurs milliers de morts répartis sur des tuiles éloignées : résidence et nombre de corps physiques plafonnés, pose et mort préservées après retour/redémarrage, temps de frame pendant sauvegarde mesuré.

Il s'agit d'une **limite déjà reconnue**, pas d'une fuite cachée découverte ici. Elle reste incompatible avec I4 tant que persistance et résidence ne sont pas séparées.

## A5 — P2 : des constantes de capacité contredisent la priorité locale

**Références :** [construction des priorités:37](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/streaming.cpp:37), [limite runtime:110](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/lifecycle.cpp:110), [troncature:779](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/service.cpp:779), [voisinage polaire](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/geography.hpp:38), [limite 25](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/limits.hpp:47).

Le runtime construit jusqu'à 25 demandes. Il place trois tuiles de prélecture après les trois premières tuiles locales, puis ajoute le reste du voisinage. Le service tronque cette liste à `options.keep=12`, qui sert à la fois de plafond de cache et de plafond de demandes.

Avec neuf tuiles locales, neuf plus trois tiennent. Près des pôles, le voisinage peut en contenir davantage. La sonde utilise les vraies fonctions géographiques, puis reproduit l'ordre de `request()` à longitude 0°, latitude 89,99°, vitesse 20 m/s vers le sud :

```text
POLAR local=10 requested=16 admitted=12 local_admitted=9
```

Une tuile du voisinage à afficher est donc exclue, tandis que trois tuiles spéculatives sont admises. Si elle n'était pas déjà servie, `stream()` peut continuer à la demander sans qu'elle appartienne aux demandes réellement acceptées. Le problème subsiste tant que cette combinaison de priorités est reconduite.

**Amélioration :** séparer la taille du cache, les demandes en attente et la résidence nécessaire. Faire passer toutes les tuiles locales avant la prélecture, ou réserver explicitement leur capacité. Le service doit retourner ce qu'il a accepté/refusé ; le runtime ne doit pas attendre une demande tronquée silencieusement.

**Validation attendue :** 9, 10 et 12 tuiles locales, avec véhicule et changement de direction ; aucune demande locale perdue, mémoire toujours bornée. C'est un véritable problème de « magic numbers » : **12, 25 et la règle 9+3 ne partagent pas leur invariant**, même lorsque certaines valeurs portent un nom.

## A6 — P2 : les ralentissements changent les règles temporelles du jeu

**Références :** [joueur:75](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/player.cpp:75), [foule:264](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/people.cpp:264), [faune:284](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/fauna.cpp:284), [police:426](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/police.cpp:426), [trafic:506](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/traffic.cpp:506).

Le joueur, la foule et la faune prennent au maximum 0,05 s par image ; la police prend 0,1 s ; le trafic peut prendre 0,25 s et son plugin subdivise le temps. La partie du delta rejetée par les premiers chemins n'est pas rattrapée par un accumulateur.

À 10 images/s, en une seconde réelle, ces chemins consomment respectivement **0,5 s, 1 s et 1 s**. Le déplacement à pied et les temporisations du combat du joueur passent par le delta limité à 0,05 s, alors que la police avance sur une autre horloge. À 15 images/s, le premier groupe ne consomme encore que 0,75 s par seconde réelle.

**Conséquence :** une baisse de performances change les vitesses relatives et les temps de réaction, au moment même où le projet doit rester jouable sur une petite machine. Ce n'est pas uniquement une animation moins fluide.

**Amélioration :** définir une politique commune de temps de simulation, avec sous-pas bornés et limite explicite de rattrapage. Garder le temps réel pour l'heure du monde et les interfaces ; ne pas faire de chaque système l'arbitre indépendant du temps qu'il accepte.

**Validation attendue :** mêmes commandes sur dix secondes à 60, 30, 15 et 10 images/s, puis avec une pause ponctuelle de 200 ms : distance à pied, poursuite, cadence et rechargement comparables dans les tolérances choisies. Ce constat est arithmétique et structurel ; aucun benchmark de framerate n'est revendiqué.

## A7 — P2 : la géométrie dépend d'un budget absent du contrat annoncé

**Références :** [PLAN I3](C:/Users/sandr/OneDrive/Documents/R1world/docs/PLAN.md:116), [Observations:41](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/cook.hpp:41), [budget runtime:114](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/lifecycle.cpp:114), [choix de géométrie:483](C:/Users/sandr/OneDrive/Documents/R1world/game/native/gen/cook.cpp:483), [outil de cuisson:27](C:/Users/sandr/OneDrive/Documents/R1world/game/native/tools/r1cook.cpp:27).

Le contrat décrit la géométrie comme fonction de la version, de la tuile, des observations et de l'Atlas. En pratique, `targetVertices` change les niveaux de géométrie générés. Le runtime calcule ce paramètre à partir de sa part d'arène ; `r1cook` part de 120 000 par défaut, avec une option permettant de le changer.

Le [test existant](C:/Users/sandr/OneDrive/Documents/R1world/game/native/tests/test_world.cpp:921) démontre volontairement que les mêmes bâtiments et observations produisent un autre `buildingGeometryLod` quand le budget baisse. Ce test passe. Le problème n'est donc pas un aléa caché : **l'identité annoncée du résultat est incomplète**. Le fait que le champ soit rangé dans `Observations` ne transforme pas une décision de budget graphique en observation du monde.

**Conséquence :** les sorties de l'outil de référence et celles du jeu ne sont pas comparables sans reporter ce paramètre. Une clé `v63_rangée_colonne` ne suffit pas à identifier la géométrie obtenue. Cette distinction deviendra également nécessaire pour les échanges multijoueurs envisagés, sans présumer ici que la simulation réseau existe déjà.

**Amélioration :** séparer un résultat canonique du monde et une politique de représentation/budget. Si le choix actuel reste dans la génération, lui donner une identité explicite, enregistrée avec les entrées et résultats de validation ; les outils doivent utiliser le même profil que le runtime quand ils prétendent le reproduire.

**Validation attendue :** comparaison outil/runtime avec un profil identique ; empreinte de génération différente lorsque les paramètres qui influencent réellement les sorties diffèrent. Vérifier séparément quels éléments physiques doivent rester invariants.

## A8 — P2 : World a une façade courte, mais les transitions restent dispersées

**Références :** [session.hpp:127](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/session.hpp:127), [sortie de voiture:386](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/vehicles.cpp:386), [téléportation:474](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/streaming.cpp:474), [arrivée:109](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/arrival.cpp:109), [éjection d'avion:137](C:/Users/sandr/OneDrive/Documents/R1world/game/native/world/rocket.cpp:137).

La réduction de la taille du fichier d'entrée et l'interface publique courte sont utiles. Cependant, la déclaration privée compte encore 1 907 lignes et les unités de gameplay restent des méthodes d'un même objet qui peut tout modifier.

Le point concret à traiter est la possession : `driving`, `sailing`, `piloting`, `swimming`, le véhicule courant, le parent du personnage, son activation, sa position et sa caméra sont mis à jour depuis plusieurs domaines. Une éjection par roquette réécrit directement une partie de ces états ; une arrivée et une téléportation font leur propre remise en place. La cohérence repose sur le souvenir des effets secondaires nécessaires dans chaque chemin.

**Conséquence :** ajouter un véhicule ou un nouveau motif d'éjection oblige à raisonner sur plusieurs fichiers sans contrat unique de transition. Le correctif récent du personnage attaché à la moto avant ragdoll, décrit dans [ROCKET_UPDATE](C:/Users/sandr/OneDrive/Documents/R1world/docs/ROCKET_UPDATE.md:50), illustre le type de fragilité concerné ; cet audit ne le présente pas comme encore non corrigé.

**Amélioration :** commencer par un propriétaire des transitions de déplacement/possession, avec des opérations explicites comme entrer, sortir, éjecter et téléporter. Ces opérations portent les règles de reparentage, d'activation physique, de position et de caméra. Extraire ensuite un domaine à la fois selon les besoins réels. Une nouvelle couche de fichiers autour du même état global ne suffirait pas.

**Validation attendue :** tester la matrice sortie normale/éjection/téléportation/chute pour les différents modes, avec les mêmes invariants de parentage et de possession. Aucun bug supplémentaire d'état impossible n'est prétendu reproduit ici.

Cette dette est reconnue honnêtement dans [WORLD_REFACTOR](C:/Users/sandr/OneDrive/Documents/R1world/docs/WORLD_REFACTOR.md:171). Ce n'est pas un motif pour annuler le refactor terminé ou imposer immédiatement une refonte générale.

## Ce que les vérifications établissent, et ce qu'elles ne prouvent pas

| Vérification exécutée | Résultat |
|---|---|
| Sonde blanche compilant directement l'implémentation actuelle de `WorldService` | Notification perdue et voisinage ±180° non reconnu reproduits |
| Rejeu de la construction des priorités avec `geography.cpp` | 9 tuiles locales admises sur 10 dans le scénario polaire décrit |
| Deux lectures d'un relief volontairement tronqué dans un dossier isolé | Deux exceptions ; fichier invalide toujours sélectionnable |
| `r1test.exe --game game Seams` | 7 réussites, 0 échec |
| `r1test.exe --game game Cook.a_resident_target` | 1 réussite, 0 échec |
| `r1test.exe --game game Service` | 11 réussites, 0 échec |

Les 19 tests existants ont été lancés avec le binaire déjà présent, daté du 28 octobre ; aucun rebuild complet du jeu n'a été effectué. La sonde est compilée séparément, avec accès de test aux membres privés, en incluant le `service.cpp` actuel et en liant les objets locaux du générateur. Elle ne remplace pas un scénario complet de streaming avec rendu. Son [source](C:/Users/sandr/OneDrive/Documents/R1world/game/generated/audit-2026-10-29/probe.cpp) et son [résultat](C:/Users/sandr/OneDrive/Documents/R1world/game/generated/audit-2026-10-29/probe.log) sont disponibles localement.

Deux angles morts méritent d'être corrigés avec les constats, sans créer une campagne de tests abstraite :

- Les quatre tests `headless` déclarés dans [CMake](C:/Users/sandr/OneDrive/Documents/R1world/game/CMakeLists.txt:131) concernent géographie, préparation, cache de menu et options. Leur réussite ne couvre pas le service, les entrelacements de cuisson ou les refus d'upload. Les tests du générateur passent par une autre chaîne.
- Le test [« a visited place never touches the network »](C:/Users/sandr/OneDrive/Documents/R1world/game/native/tests/test_world.cpp:1059) ferme les proxies, vérifie la tuile et cherche certains mots dans les logs. Il ne compte pas les tentatives réseau ; ses options n'activent pas l'enrichissement retail et la canopée comme le runtime. Il démontre une disponibilité hors ligne dans ce scénario, pas l'absence générale de requêtes. Cette dernière promesse réclame un transport injectable avec compteur d'appels, et des cas de migration explicites.

## Lecture de la philosophie de code

Les orientations restent bonnes : génération native séparée de la scène, coordonnées géographiques doubles, modèles partagés, priorité aux observations, réutilisation des capacités moteur et refus de sacrifier discrètement les assets. Le défaut récurrent apparaît dans leur traduction en contrats exécutables :

- **« Budget borné »** devient parfois une collection bornée localement, mais pas une borne sur tous les propriétaires de ressources. Les cadavres et les demandes polaires en sont deux manifestations différentes.
- **« Même monde »** demande une identité complète des entrées et une convergence de toutes les dépendances. Une version de générateur et des RNG seedés ne suffisent pas à garantir l'orchestration asynchrone.
- **« Un refus doit se dire »** exige un état terminal transmis à l'appelant. Un log isolé dans le moteur ne suffit pas si le jeu continue d'attendre.
- **« Un système possède son état »** n'est pas obtenu par le seul déplacement de méthodes dans plusieurs fichiers. La possession doit être visible dans les opérations autorisées.

Les nombres intrinsèques de WGS84, les coefficients d'un algorithme documenté, les distances de gameplay et les réglages d'assets ne sont pas des défauts simplement parce qu'ils sont numériques. Les nombres à reprendre en priorité sont ceux qui **se contredisent entre systèmes** : capacités 12/25/9+3 et pas temporels 0,05/0,1/0,25. Les déplacer dans un fichier de constantes commun sans corriger leur signification ne résoudrait rien.

## Ordre de traitement conseillé

1. Corriger la perte d'invalidation et la récupération du cache avec les reproductions ciblées A1/A2.
2. Faire remonter les résultats d'upload jusqu'au runtime et aux smokes (A3).
3. Séparer persistance et résidence des corps, puis mesurer une longue session et les sauvegardes (A4).
4. Unifier l'admission des tuiles locales et la politique temporelle (A5/A6).
5. Rendre le profil de génération explicite (A7), puis centraliser les transitions de possession au prochain chantier qui les touche (A8).

La course `perf walk` du PLAN et la qualification des 30 fps partout restent ouvertes. Les constats ci-dessus fournissent des corrections et des expériences précises ; ils ne justifient pas de déclarer ces deux objectifs résolus.
