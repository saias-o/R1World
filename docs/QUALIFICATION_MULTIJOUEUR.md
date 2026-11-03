# Qualifier le multijoueur en réel (phase B)

Le multijoueur passe ses tests sur une seule machine : deux instances, le réseau
local, la DHT publique (`game/tools/online_smoke.py`, avec ou sans `--versions`
et `--join`). Ce qui n'a jamais été mesuré, c'est **deux machines sur deux
réseaux réels**. C'est le vrai risque : NAT symétrique, réseau mobile, routeur
qui bloque le multicast, pare-feu.

Cette check-list se déroule avec deux PC : **A**, le poste de développement, et
**B**, un second poste. Elle produit des journaux à renvoyer ; le verdict se
lit dedans.

## 1. Préparer le paquet (sur A)

```powershell
cd game
python tools\package_world.py
```

Il produit `game\generated\package\R1World-<commit>.zip` (environ 435 Mo) :
l'export du moteur (`saida_tool export-game`), l'exécutable sans ses
informations de débogage, `Jouer.cmd` qui écrit le journal dans `logs\`, et
`LISEZMOI.txt`. Ni le cache, ni les sauvegardes, ni l'identité en ligne de A n'y
entrent. **Garder `game\generated\world-windows\R1World.exe` du même commit** :
c'est lui qui lit les adresses d'un rapport de plantage venu de B.

Copier le zip sur B (clé USB, cloud), le décompresser dans un dossier où
l'on peut écrire (pas `Program Files`), lancer `Jouer.cmd`.

À savoir avant de commencer :

- Windows peut bloquer un exécutable non signé (« Windows a protégé votre
  ordinateur ») : *Informations complémentaires*, puis *Exécuter quand même*.
- Le pare-feu demande l'accès au réseau : l'autoriser, sinon B ne reçoit rien.
- Un premier passage sur une machine neuve n'a pas de cache : des marches de
  terrain peuvent apparaître le temps que le relief arrive (voir § 5). Ce
  n'est pas du réseau.

## 2. Se retrouver

Sur A, dans *Settings > Online*, lire le code d'invitation (par exemple
`K7QM-2XPA`). Sur B, le taper dans le même onglet et cliquer *Join*, ou lancer
`Jouer.cmd --join K7QM-2XPA`. B est emmené à quatre mètres de A. Le panneau
en ligne, en bas de l'écran, dit chaque étape (recherche, trouvé, en route, à
côté). Un code qui ne répond pas est dit au bout de 30 secondes, et la
recherche continue.

## 3. Les scénarios

À faire dans l'ordre. Chaque scénario est une session d'au moins **dix
minutes**, et le premier dure **trente minutes**.

| # | A | B | Ce qu'il éprouve |
|---|---|---|---|
| 1 | box domestique | une autre box domestique (chez quelqu'un d'autre) | le cas réel : deux NAT, la DHT, le perçage UDP |
| 2 | box domestique | partage de connexion 4G/5G d'un téléphone | NAT d'opérateur, souvent symétrique : le perçage doit passer, car à deux aucun relais n'est possible (un relais est un troisième joueur qui les a présentés) |
| 3 | même box | même box, sur un routeur ou un Wi-Fi qui isole les clients ou coupe le multicast | se trouver sans les balises LAN, par la DHT seule |

Pendant chaque session :

1. Se rejoindre par le code (§ 2), noter le temps affiché jusqu'à « Beside ».
2. Marcher ensemble deux minutes, courir, sauter : l'autre doit glisser sans
   saut ni téléportation.
3. Se tirer dessus au pistolet et se frapper : les touches doivent compter
   chez la victime, sans refus.
4. S'éloigner à plus d'un kilomètre (en voiture), puis revenir : l'autre
   disparaît, puis revient tout seul.
5. Pour le scénario 1, rester ensemble trente minutes en tout.

## 4. Ce qu'il faut renvoyer, et comment le lire

De chaque PC : le fichier `logs\game-<date>.log` de la session (sur A, lancé
par `Play.ps1`, il est dans `game\cache\sessions\<id>\game.log`), et le
dossier `crash\` s'il existe sur B.

Les lignes utiles commencent par `[World online]` :

- `Player X is here, from <adresse>` ou `relayed by Player Y` : le lien, direct
  ou relayé, et son aller-retour.
- `going to Player X`, `joined Player X: N m from him` : la jonction.
- Une fois par minute : liens, liens relayés, joueurs suivis, version
  différente, foule estimée, instantanés envoyés par palier, pire attente du
  thread réseau, et **les images de la minute : médiane, p95, p99 et pire**.
- `refused …` : un refus et sa raison, à lire un par un.

Est **qualifié** un scénario où : la jonction aboutit en moins d'une minute ;
le lien ne tombe pas pendant la session (aucun `left: closed` ou `timed out`
hors départ volontaire) ; aucun refus inattendu ; le p99 des images en ligne
reste dans la même classe que le solo sur la même machine ; le pire retard du
thread réseau reste sous 50 ms.

## 5. Limites connues avant de commencer

- **Première visite sur une machine neuve.** Une tuile peut rester en « sol
  plat provisoire (relief en attente) » à côté d'une tuile qui a son relief,
  une marche de 18 à 53 m : le smoke solo du paquet échoue sur ce point,
  trois fois de suite à Paris. Et le relief lointain (zooms 3 à 6) n'est lu
  que sur le disque, où il n'est pas, sur A non plus : les anneaux lointains
  sont refusés (`ring N refused` dans le journal) sur les deux postes. Ce
  sont des chantiers de la distribution 0.1, pas du multijoueur.
- **Lien conservé par proximité.** Le moteur garde les liens vers les 48
  joueurs les plus proches. Un ami qu'on vient de rejoindre est tout près,
  donc gardé ; dans une foule de plus de 48 personnes plus proches que lui,
  son lien pourrait tomber. Rien ne marque encore « cet ami est gardé quoi
  qu'il arrive ».
- **Véhicules.** Un joueur au volant n'est pas dessiné (`Pose::Vehicle`) : le
  véhicule partagé est un chantier de la phase C.
