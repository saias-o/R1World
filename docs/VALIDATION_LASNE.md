# Croix de Lasné — générateur v31, 7 octobre 2026

Référence : capture Street View fournie par l'utilisateur, avril 2026,
route de Saint-Colombier. Caméra de contrôle approximative, située à
(-2.71559, 47.56272), hauteur 1,7 m, regard (-25, 1.7, 150) dans le repère
du joueur. Le jour et l'heure de la photo ne sont pas connus : le contrôle
fixe le 20 avril 2026 à 10:30 UTC, ciel clair, 1334 × 646 pixels.

## Changements

- Trottoirs inférés selon le bâti voisin, par tronçons de 40 m. Les tags OSM,
  y compris `sidewalk:both=separate`, restent prioritaires.
- Accotements étroits en granulat et bandes herbeuses à largeur variable,
  drapés sur le terrain, découpés autour des chaussées, bâtiments et eaux.
- Foule liée à la part des foyers effectivement dehors et à la demande
  locale, sans concentrer toute la population d'une tuile près du joueur.
  La part locale est recalculée après 8 m de déplacement, pas à chaque image.
- Canopée basse rendue avec le modèle de buisson existant, un LOD distant,
  et des petits groupes. Chaque cellule basse relevée a priorité sur le
  remplissage inféré. Petites plantations de jardin inférées sous 1 m,
  hors des cellules de végétation relevée, avec une ouverture d'accès.
- Plafond inchangé : 640 arbres et buissons par tuile avec canopée.

## Vérifications

`r1test Streets`, `Crowd`, `Nature`, `Canopy`, `Roads` : **46 tests réussis**,
y compris l'absence de bandes vertes en climat aride.
Test Python de la galerie : **1 réussi**. La suite C++ complète, avant
l'ajout du dernier test de jardin, avait 259 réussites et un échec :
`Bridges.two_carriageways_mapped_side_by_side_are_one_deck`.

Cuisson hors réseau, trois répétitions, tuile `v31_27512_23926` :
14 389 sommets, environ 80 ms de cuisson, 469 arbres et 171 buissons,
dont 36 plantations basses de jardin. Le plafond de tuile est 120 000 sommets.
Le manifeste compte 11 personnes à l'échelle de la tuile, avant la réduction
à la zone proche de la caméra et l'heure locale.

Captures dans `game/generated/lasne-before.png` et `lasne-after.png`.
Dernière session de capture :
`game/cache/sessions/1de51e57bc994126970072121bf12251/game.log`.
Le profil contient 600 images : 16,35 ms/image en moyenne, dont 8,13 ms
d'attente du limiteur. Pic de 372,69 ms pendant le chargement.
Mesure sur la machine de développement, pas sur la GTX 1060 cible.

Test de fumée complet hors réseau : **PASS**, marche de 102,8 m, saut,
conduite, sortie du véhicule, prise d'une voiture du trafic et reprise du jeu.
Journal : `game/cache/sessions/164b271f6cce4d8cad3f706f8cb06c98/game.log`.

La tuile centrale utilise les observations en cache ; deux tuiles périphériques
restent provisoires faute de leurs sources OSM hors réseau. Les différences
de trafic entre les captures viennent de la simulation.

## Limites visuelles

La fréquentation et les profils de bord de route progressent, mais la capture
reste loin de la photographie : feuillage trop pâle, bâtiments non tagués
aux gabarits parfois excessifs, clôtures et mobilier incomplets. Les plantations
de jardin sont une inférence, pas le relevé des propriétés réelles.
La vue `lasne` fait désormais partie de `game/tools/gallery.py` pour permettre
de suivre ces écarts avec une caméra et un éclairage fixes.

## Correctifs du moteur — 7 octobre 2026

La vue `game/generated/lasne-engine-final.png` utilise le même générateur v31,
les mêmes matériaux, la même caméra et le même instant que `lasne-after.png`.
Le moteur corrige la double conversion sRGB de la sortie native, le format
sRGB erroné de sa normal map neutre et l'IBL qui ignorait le second ciel et sa
rotation. L'éclairage gagne en contraste ; les reflets clairs du feuillage,
les gabarits des bâtiments et les clôtures restent à améliorer.

Validation du moteur : 91/91 CTest, contrôles numériques HDR, mires d'IBL
diffus/spéculaire et de rotations, contrôle des faces des matériaux et build
du player Web. Dans Chrome/WebGPU, une mire de gris donne RGB 142 contre
141,60 attendu, sans erreur de shader. Le démarrage Web et les lectures GI
à LOD explicite ont aussi été corrigés pendant ce contrôle. Pas de vérification
sur casque XR ni de comparaison Lavapipe, dont le pilote n'est pas installé.

La capture finale a réussi après un premier essai interrompu par le délai de
chargement des tuiles. Deux tuiles périphériques attendent toujours OSM hors
réseau. Journal final :
`game/cache/sessions/48ee3373bb374d1ca29771659e9c689d/game.log`.
Le profil final couvre 600 images : 16,40 ms/image, dont 8,75 ms d'attente du
limiteur ; portée CPU `Renderer/DrawFrame` 2,06 ms, pic de chargement 385,48 ms.
Ce relevé sur RTX 4070 ne qualifie pas la GTX 1060 cible.

L'IBL conserve les projections du ciel en cache et ne prélève le second ciel
dans le shader que pendant le fondu. Le préfiltrage GGX des reflets reste dans
le backlog du moteur ; ses mipmaps ordinaires ne constituent pas ce filtrage.
