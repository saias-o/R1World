# Références photographiques rurales

Les captures utilisent `--camera-geo` aux coordonnées publiées des photos,
indépendamment du déplacement du joueur lors du chargement. Aucun point n'est
déplacé vers une rue voisine. L'heure EXIF fixe le Soleil ; la météo est une
reconstitution visuelle, pas une mesure historique. Hauteur estimée : 1,65 m,
inclinaison horizontale. Les coordonnées photographiques ont leur propre
incertitude ; le cadrage n'est donc pas un relevé topographique exact.

| Référence | Longitude, latitude | Heure locale photo | Orientation |
|---|---|---|---|
| [Moretown, 1423 Route 100B](https://commons.wikimedia.org/wiki/File:1423_Vermont_Route_100B,_Moretown,_VT.jpg) — AdamFranco, CC BY-SA 4.0 | -72.758531, 44.254078 | 2026-09-20 09:40:56 -04 | 212,34° |
| [Souzdal, Gastev 21](https://commons.wikimedia.org/wiki/File:Suzdal_Gasteva21_192_6046.jpg) — Ludvig14, CC BY-SA 4.0 | 40.440817, 56.428137 | 2018-06-20 10:30:38 +03 | 270° publié |
| [Tafraout, pont sur l'oued](https://commons.wikimedia.org/wiki/File:MA.SS.Tafraout_1149_16x9-R_6K.jpg) — Roy Egloff, CC BY-SA 4.0 | -8.972913, 29.720753 | 2018-08-15 13:07:38 +01 | 340° estimé |
| [Tsumago-juku, Nakasendo](https://commons.wikimedia.org/wiki/File:Looking_up_the_Nakasend%C5%8D_in_the_central_part_of_the_village,_Tsumago-juku,_Nagiso,_2016.jpg) — DimiTalen, CC0 | 137.595610, 35.577542 | 2016-03-29 15:05:01 +09 | 205° publié |

Moretown : direction magnétique EXIF corrigée par WMM2025 et retournée de
180° après identification de la maison OSM 1161789689, dont l'adresse et le
lien Commons désignent cette photo. Tafraout : orientation recalée sur l'oued
et le pont ; panorama assemblé dont la focale/coupe restent incertaines.
Les focales et conditions sont produites dans `captures.json` par le script de capture.

Le générateur ajoute des proportions, matériaux et jardins régionaux US/RU/MA/JP,
des toitures par ailes pour les maisons en L, des annexes basses et des règles
locales pour les rangées historiques de Kiso et les petits commerces de Tafraout.
Les dimensions mesurées gardent la priorité ; les jardins restent exclus des
îlots denses, rues piétonnes et commerces. Les textures de façade contenant des
fenêtres sont retirées du lit de l'oued, des panneaux de clôture et des murs de
limite ; ils utilisent respectivement roche, planches et pierre sans fenêtres.

Restent des écarts visibles : buissons/haies/massifs insuffisants (reportés dans
PLAN pour plus tard), couleurs et détails propres à chaque maison non observés,
position des troncs inférée depuis le couvert, berges et largeur de l'oued,
relief rocheux et nuages. Les captures peuvent attendre des sources absentes :
les avertissements de chargement sont affichés dans la comparaison.

Reproduction : `python game/tools/capture_rural_references.py --variant registered-final --offline`,
puis `python game/tools/rural_comparison.py`. Rapport :
`game/generated/rural-references/registered-final/index.html`.
