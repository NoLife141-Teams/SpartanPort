# Pilotage par l'agent et collecte GoW3

Validation réelle du 27 octobre 2026, heure de Toronto.

La solution retenue est un lancement isolé suivi d'un pilotage visuel par l'agent :
observer un écran, envoyer une seule action avec Computer Use, puis vérifier
l'écran suivant. Il n'est pas nécessaire de construire un bot de combat ni un
nouveau protocole de commandes pour charger la sauvegarde de test.

## Parcours réellement réussi

Le binaire local a démarré avec le backend audio silencieux, le patch **Skip
Intro** appliqué et `cheat_health=1` dans sa configuration isolée. Le hook santé
est déclaré disponible par le chargeur. Le personnage conserve sa barre pleine
pendant les attaques observées; cela ne constitue pas une preuve concernant
les chutes ou tous les scripts de mort du jeu.

L'agent a sélectionné **Load avec S**, confirmé avec **Espace**, puis chargé
l'unique entrée du dialogue natif avec **Entrée**, conformément à son aide
affichée. Entrée sélectionnée : **Auto Save — Gaia**, difficulté God (Normal),
temps de jeu **00:04:35**, datée du 26 octobre 2026 à 21:45 dans le dialogue.
La scène de jeu a été vérifiée à l'écran; le journal confirme `chose AUTOSAVE`,
le montage en lecture seule et `load job result : 0x0`.

La sauvegarde utilisée est une copie des six fichiers de test de Dot, avec
une seconde copie de sauvegarde et vérification SHA-256 avant lancement.
Les six fichiers source et la sauvegarde de sécurité restent identiques après
l'essai. La configuration normale et les fichiers du jeu ne sont pas édités.
La copie de test a été fermée par Alt+F4 après la collecte.

## Pourquoi les premiers appuis ne suffisaient pas

Le sampler lit l'état courant du clavier. Un appui et un relâchement traités
entre deux lectures peuvent donc disparaître. Les premières tentatives n'ont
pas sélectionné Load. Conserver l'événement pour une seule lecture a encore
échoué, bien que les logs aient confirmé réception et consommation.

Le mode **`SPARTAN_UI_KEY_BUFFER=1`** produit désormais une impulsion de
**100 ms côté sampler**, à partir d'un vrai événement SDL S ou Espace. Elle
couvre plusieurs lectures sans maintenir une touche Windows. Elle n'est pas
réarmée par les événements de répétition et expire au temps fixé par l'événement,
même si le superviseur s'arrête. Un thread de jeu bloqué ne reçoit aucune
garantie d'exécution pendant ce délai; le prochain échantillon rejette l'impulsion
expirée. Les entrées physiques continuent à suivre le comportement existant.

Seules S et Espace sont concernées. Les autres touches, notamment Retour arrière,
conservent le sampler historique. Les fenêtres étrangères, le focus absent,
l'overlay/dialogue qui capture les entrées, une perte de focus ou une régression
d'horloge annulent le tampon. Le thread SDL publie le focus et la capture dans
un contexte atomique avec génération; le lecteur de manette ne consulte aucune
API de fenêtre SDL. Les confirmations du dialogue de sauvegarde restent
gérées par ses événements SDL habituels. Aucun replay de commandes ou injection
Windows supplémentaire n'est utilisé.

Le mode est désactivé par défaut et ne modifie aucun réglage persistant. Son log
est plafonné à 32 relevés d'événement/échantillon. Le test `ui-key-buffer-test`
utilise le vrai sampler avec périphériques et horloge factices : plusieurs
lectures, expiration exacte, neutralisation, répétitions, mauvaises fenêtres,
focus, capture et horloge. Il passe, ainsi que les régressions clavier, replay
de menu et contrat d'automatisation recompilées après la version finale.

## Outils réutilisables

`scripts/gow3_diagnostic_session.py` prépare un nouveau dossier sous
`out/gow3-sessions/`, copie et vérifie la sauvegarde, reprend une copie du cache
de shaders existant, conserve une copie vérifiée du probe dans `bin/` et écrit
sa propre configuration. Le build peut ainsi être reconstruit sans remplacer
le binaire d'une session en cours. Sans `--launch`, il prépare
seulement. Avec `--launch`, il démarre `run.py` avec un environnement de processus
dédié : son coupé, vie infinie, Skip Intro, DLAA/jitter désactivés et collectes
de métadonnées armées. Le chemin `--seed-data` doit contenir `spartanport.ini`
et `user/savedata`; `--game-dir` désigne le dossier contenant `eboot.bin`.

```powershell
out/pyenv/Scripts/python.exe -B scripts/gow3_diagnostic_session.py `
  --seed-data "CHEMIN_DU_RUNTIME_DE_TEST" --game-dir "CHEMIN_DU_JEU" --launch
```

Le script affiche le nouveau dossier de session. La navigation est ensuite
effectuée par l'agent avec des observations visuelles fraîches, et non par une
suite de touches à délais fixes. Les manifestes ne prétendent pas attester
automatiquement l'état du jeu. Les fichiers de déclenchement sont une interface
de diagnostic du processus, pas une voie de contrôle des menus.

Après chargement confirmé : créer `capture.stationary`, puis `capture.motion`
après le bref déplacement. Chaque fichier est créé une fois seulement. Inspecter
la clôture et les pertes avant de continuer. Les phases `pause` et `resume`
existent dans le collecteur mais n'ont pas été collectées dans cet essai :
l'appui bref Retour arrière n'a pas ouvert la pause. Les mêmes garanties de
navigation ne sont donc pas revendiquées pour cette touche.

## Captures et résultat DLAA

Session réussie : `out/gow3-sessions/20261028T011351853690Z/`.
Son `session.json` contient l'environnement, l'empreinte exacte du binaire et les
hashes de sauvegarde. Les essais précédents sont conservés séparément.

| Fenêtre | Draws retenus | Taille | Clôture | Caméra |
| --- | ---: | ---: | --- | --- |
| stationary | 3 988 | 14 308 879 octets | Deux intervalles, aucune perte | Deux statuts camera-candidate, snapshots cohérents |
| motion | 3 204 | 10 771 800 octets | Deux intervalles, aucune perte | Deux statuts camera-candidate, snapshots cohérents |

Les bilans ne rapportent aucune erreur d'écriture, draw perdu, perte de provenance
pendant la fenêtre ou métadonnée incomplète. Les compteurs historiques de pertes
avant chaque fenêtre sont explicitement séparés de leurs baselines.

Le même couple de ressources est observé dans les deux fenêtres : profondeur
**UID 7/backing 1**, D32S8 à deux échantillons, et couleur **UID 10/backing 1**,
vue sRGB à deux échantillons. Le passage VS `475a41468b1a1fbc` /
PS `475a4146e3a74097` lit respectivement leurs bindings 1 et 2; les permutations
retenues sont `efd04d8023fec408` et `efd0530963dc1d55`. Ces UID appartiennent à
cette instance uniquement. Ils identifient des objets, pas une version de contenu.

Chaque intervalle contient une matrice VP unique. La différence maximale d'un
coefficient entre les deux intervalles vaut environ **0,10247** pour stationary
et **0,44846** pour motion. « Stationary » signifie sans commande de déplacement;
les ennemis, animations et caméra restent actifs. Le changement ne peut pas
être attribué uniquement à S.

`tools/analyze_gow3_session.py DOSSIER_SESSION` vérifie indépendamment les
tailles, plafonds, IDs de draws, clôtures, comptabilités de provenance et
associations. Il crée un nouveau `diagnostic-summary.json` et refuse d'écraser
un rapport existant. L'analyse réelle des deux fenêtres passe.

Un rapport de passes/constants supplémentaire existe dans `captures/`, mais ses
trois groupes ne constituent pas une capture exhaustive de toute la frame.
Les captures groupées restent la preuve principale pour les intervalles ordonnés.

### Vérification finale avec propriété du contexte SDL

Après déplacement des lectures de focus sur le seul thread de fenêtre, le parcours
S → Load → Espace → Gaia → Entrée a été rejoué dans la session
`out/gow3-sessions/20261028T012217170806Z/`. Le jeu chargé a de nouveau été observé,
puis la session a été fermée. La copie du binaire reste dans son `bin/`, SHA-256
`9eb9b99f5aa386c57e593461e6d23666a453d7e270cb57465fd449a35141223c`.

- Stationary : **4 552 draws**, **15 051 151 octets**, deux intervalles complets,
  aucune perte ou métadonnée incomplète; analyse valide.
- Motion : **4 730 draws**, **15 498 447 octets**, deux intervalles et snapshots
  caméra cohérents, mais **deux draws sans identité de profondeur résolue**.
  Ils portent `resolution=5` (`CacheBusy`) aux draws 7 465 551 et 7 465 586 :
  la prise non bloquante du verrou du cache a été refusée. Le collecteur signale
  `complete_instrumented_draw_window=0`; l'analyseur rejette correctement cette
  fenêtre comme preuve complète. Aucun draw ni octet n'est perdu, mais les
  associations manquantes ne sont pas inventées. Aucun nouvel essai identique
  n'a été lancé pour masquer ce résultat.

Le passage de composition identifié reste le même pour les ressources connues.
Les deux fenêtres complètes du premier essai sont conservées séparément et ne
sont pas utilisées pour combler les deux observations manquantes de cet essai.
Les six sauvegardes source restent identiques après vérification SHA-256.

Le validateur final, sans répétition des bancs GPU inchangés, passe **59 étapes**,
soit 58 tests Python, 49 exécutions CPU et huit étapes de compilation/édition
de liens. Résultats : `out/gow3-validation/20261028T012257530259Z/result.json`.
Le test de régression `swapchain-focus-test` passe aussi avec les stubs de ces
interfaces d'entrée; son journal est dans `out/verification/`.

**Le DLAA n'est pas validé.** Les captures ne contiennent ni pixels pré-interface,
ni valeurs de profondeur par échantillon, ni preuve de conservation des ressources
jusqu'à achèvement GPU. La prochaine étape utile est la copie bornée au passage
identifié, avec durée de vie, barrières et tick d'achèvement démontrés. La
couleur avant interface, la reprojection sur pixels réels et les mouvements
d'objets doivent ensuite être validés avant le branchement NGX.
