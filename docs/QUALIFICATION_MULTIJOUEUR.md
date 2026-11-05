# Qualification du multijoueur sur deux PC

Les smokes automatisés utilisent deux instances sur un seul poste.
La traversée de réseaux réels, les sessions longues et les performances
avec plusieurs pairs restent à qualifier.

## Préparer les postes

Sur A, préparer le relief complet et le paquet selon
[DEVELOPMENT.md](DEVELOPMENT.md#distribution), puis :

```powershell
python game/tools/package_world.py
```

Copier `game/generated/package/R1World-<commit>.zip` sur B et l'extraire
dans un dossier accessible en écriture. Garder sur A l'exécutable non dépouillé
`game/generated/world-windows/R1World.exe` du même commit pour symboliser
les rapports de crash de B.

B doit disposer de Windows et d'un pilote Vulkan compatible. Lancer
`Jouer.cmd` pour conserver un journal. Autoriser le réseau dans le pare-feu.
Un exécutable non signé peut déclencher SmartScreen ; vérifier l'origine
du paquet avant de choisir son exécution.

Activer **Settings > Online** sur les deux postes : le multijoueur est
désactivé par défaut. Sur A, lire le code de session ; sur B, le saisir dans
le même onglet puis **Join**, ou lancer `Jouer.cmd --join CODE`.
Le panneau indique recherche, découverte, voyage et arrivée près de l'ami.
Après 30 s sans réponse, il le signale et poursuit la recherche.

## Scénarios

Le premier dure au moins 30 minutes, les suivants au moins dix minutes.
Noter commit, matériel, résolution, préréglage, heure et topologie réseau.

| Cas | A | B | Objet |
|---|---|---|---|
| 1 | Box domestique | Autre box domestique | Découverte publique et traversée de deux NAT |
| 2 | Box domestique | Partage 4G/5G | NAT opérateur, IPv6 et limites du perçage |
| 3 | Même routeur | Clients isolés ou multicast bloqué | Découverte et connexion sans balises LAN |

Pour chaque cas :

1. Rejoindre par invitation et mesurer le délai jusqu'à l'arrivée.
2. Marcher ensemble, courir et sauter pendant deux minutes ; observer
   saccades, téléportations et disparition de l'autre.
3. Tirer au pistolet et frapper ; vérifier les dégâts sur le poste victime.
4. Prendre une voiture garée, la déplacer, sortir et la reprendre depuis
   l'autre poste. Vérifier sa position et sa place de départ vide.
5. S'éloigner à plus d'un kilomètre, revenir et vérifier la redécouverte.
6. Terminer la durée prévue, puis tester départ et retour hors ligne.
7. Refaire les contrôles visuels de jour et de nuit.

Deux NAT impossibles à percer ne peuvent se relayer sans un troisième pair
joignable par les deux. Dans le cas 2, noter cette topologie si la jonction
échoue ; ne pas la présenter comme une prise en charge validée.
Le cas 3 teste aussi une limite ouverte des adresses privées derrière un
routeur sans multicast ni hairpinning.

## Journaux et verdict

Conserver les journaux des deux postes :
`logs/game-<date>.log` du paquet, ou
`game/cache/sessions/<id>/game.log` du lanceur de développement.
Conserver `crash/` lorsqu'il existe.

Les lignes `[World online]` donnent adresse et lien direct/relais,
départ/arrivée de l'invitation, refus avec raison et bilan par minute :
liens, versions, population estimée, instantanés par palier, pire attente
du thread réseau, médiane/p95/p99/pire temps de frame.

Un scénario est qualifié si :

- La jonction réussit en moins d'une minute.
- Le lien reste actif jusqu'au départ volontaire, sans fermeture ni timeout inattendu.
- Aucun refus inattendu ne survient et les interactions demandées fonctionnent.
- Le p99 des frames reste comparable au solo sur le même poste et les mêmes réglages.
- Le pire retard du thread réseau reste sous 50 ms.

Une session entre deux joueurs ne qualifie pas le coût d'une foule.
Mesurer séparément débit, CPU, mémoire et temps de frame avec davantage de pairs.

## Limites à prendre en compte

- Un ami n'est pas protégé contre l'élagage des liens par proximité dans une foule.
- Bateaux, avions, hélicoptères, animaux et météo restent locaux.
  Un joueur en bateau ou en aéronef n'est pas dessiné chez l'autre.
- Les roquettes ne synchronisent pas la destruction des voitures.
- L'affichage de la foule distante reste à implémenter.
- Une installation sans les 648 blocs de relief n'est pas un paquet complet.
  Les défauts de première visite, les délais Overpass et le budget d'assets
  décodés sont suivis dans [PLAN.md](PLAN.md#7-version-01-et-sortie-prochaine).
