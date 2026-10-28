# Commandes bornées et observation d'état : proposition hors jeu

## Livrable et frontière actuelle

`experiments/gow3_automation/policy.py` est un modèle de référence hors ligne, accompagné de 20 tests synthétiques. Il ne fait aucune entrée/sortie, ne lit pas la mémoire du jeu, ne contrôle pas Windows, ne lance pas de processus et n'est importé par aucun composant du runtime. Les paramètres `verified_source`, `alive`, `selected_save_verified` et les preuves de déplacement sont des **entrées synthétiques représentant un contrat futur**, pas des détecteurs déjà disponibles ni des attestations de sécurité que pourrait fournir librement un fichier de commande.

Les tests vérifient les décisions une fois ces preuves disponibles. Ils ne démontrent pas qu'un menu, un personnage vivant ou une mort sont reconnus dans GoW3. Aucune modification de `runtime_pad.c`, du hook de menu, des scripts de jeu, des sauvegardes ou du probe n'est faite dans cette étape.

## Contrat de commande proposé

Le chemin existant `SPARTAN_PAD_FILE` est réservé aux usages actuels. Il ne faut ni le détourner pour contourner l'outil UI, ni lui prêter une expiration qu'il ne possède pas. Une extension opt-in distincte pourrait réutiliser le point de consommation de `sample()` mais doit être revue avant intégration.

* Identité explicite : nonce de session, PID, date/jeton de création du processus, fenêtre. Un PID ou un HWND seul ne suffit pas après réutilisation.
* Le consommateur, sur sa propre horloge monotone, émet une autorisation à usage unique (`Grant`) liée à une observation fraîche, à une génération d'état et à une action précise. Pas de comparaison entre deux horloges de processus ni de confiance dans l'heure fournie par le script.
* Une seule commande utilise cette autorisation, avec un numéro de séquence croissant. Un doublon, un mauvais PID, une durée invalide ou une autorisation expirée provoque un arrêt mémorisé. Une nouvelle commande ne peut pas prolonger la précédente.
* Durée maximale proposée : **250 ms**, vérifiée à chaque lecture de la manette. L'échéance est le minimum de l'expiration de l'autorisation et de la durée demandée à partir de l'acceptation. Un réveil tardif ne réarme rien. Si le thread ne lit plus la manette, il ne produit plus de nouveaux échantillons ; à sa reprise le premier échantillon doit être neutre.
* Observation sémantique âgée de moins de **100 ms**, sans perte ni réordonnancement. Perte de focus, fichier/canal illisible, perte d'observations, état inconnu, mort ou régression de temps => sortie neutre et arrêt mémorisé. Ces seuils sont des choix conservateurs du modèle, pas des latences mesurées ou déjà garanties en jeu.
* Actions limitées à sélectionner Load depuis New Game observé, confirmer Load, confirmer la sauvegarde vérifiée, mouvement, pause et retour. Aucune commande arbitraire, attaque, sauvegarde, nouvelle partie, réglage, restart ou console.
* Transitions de menu attendues sous 2 s, chargement sous 30 s, essai entier sous 120 s. Ces délais sont également des valeurs de test à valider. Ils arrêtent la logique d'entrée ; ils ne garantissent pas la terminaison du processus.
* Chaque transition confirmée neutralise les touches avant l'état suivant. La sélection New Game ne peut jamais être confirmée par une action de chargement.
* Le chemin désactivé reste un bypass sans suivi d'état ni changement des entrées physiques. En mode d'automatisation armée, la neutralisation doit être appliquée après toute combinaison des entrées : l'addition actuelle des boutons physiques pourrait sinon réintroduire une commande. Cette isolation ne concernerait que notre processus, jamais le périphérique ou Windows. Ce changement d'intégration est proposé, pas appliqué.

Transport envisagé : fichier local à la session, taille fixe ou strictement limitée (par exemple 512 octets), création initiale exclusive, publication atomique et numéro de séquence dans le contenu. Ne pas utiliser la paire mtime/taille comme preuve de changement. Le parseur doit rejeter les troncatures, champs supplémentaires, dépassements et valeurs hors plage. Aucune socket, installation, accès persistant ou privilège système. L'implémentation du parseur et du transport n'est pas incluse dans ce modèle.

Un accusé de consommation doit porter la même identité, le numéro de commande, l'échéance, l'instant d'échantillonnage et le motif de neutralisation. Un journal borné séparé (proposition : 1 Mio / 4096 enregistrements) compte toute perte. Un événement non enregistré ne doit pas être considéré comme prouvé. Aucun log ne doit contenir une sauvegarde, des pixels ou des assets.

## Observateur : sources présentes et preuves manquantes

| Source actuelle | Ce qu'elle établit | Ce qu'elle n'établit pas |
| --- | --- | --- |
| `runtime_savedata.c::mount` | Montage d'un répertoire existant/créé et mode demandé | Sauvegarde entièrement chargée, scène jouable, personnage vivant |
| `runtime_pad.c::note_game_menu` | Heuristique à partir des boutons et du temps de marche | Menu réellement affiché, mort, cinématique, sélection |
| `src/menu_hook.h` / `probe.c::install_menu_exit` | Interception vérifiée d'appels builtin pour nos options et sortie | État courant de la machine de menu ou choix actuellement sélectionné |
| `scripts/pause_menu.py::Script` | Structure et symboles statiques des états SMI_R_UI | État exécuté à un instant donné, vie du personnage, contrôle disponible |
| Diagnostics caméra / flips | Activité de rendu et candidats caméra | Gameplay : le menu principal a effectivement émis camera-candidate |
| CUA, observation de la fenêtre ciblée | Image et identité de fenêtre à un instant | Surveillance continue à moins de 100 ms, ou reconnaissance automatique exhaustive |

Le modèle distingue Unknown, Title, LoadMenu, Loading, Playing, Paused et Dead. Unknown n'est jamais assimilé à Playing. Une mort est mémorisée et interdit toute reprise automatique dans la même session de contrôle. Plusieurs couches d'interface contradictoires, une transition inconnue, une observation périmée ou une perte doivent produire Unknown/arrêt.

Un **adaptateur sémantique fiable n'existe pas encore**. Il devra établir simultanément : état/menu exact, sélection connue, identité du chargement terminé, personnage vivant et contrôle effectivement disponible. L'absence de menu de mort ne suffit pas à établir vivant. Un écran qui ressemble au gameplay ou une variation de matrice ne suffit pas non plus.

## Modification architecturale à décider avant intégration

Pour une exécution autonome robuste, la plus petite voie native plausible est une observation des transitions de la machine de menu **et** d'un signal indépendant de fin de chargement/vie/contrôle. Réutiliser le parseur statique existant aiderait à dresser la table des états, mais le hook builtin actuel ne publie pas leurs entrées/sorties. Il faudrait donc un point d'instrumentation supplémentaire. Aucun emplacement de code, ID de fonction ou offset de mort n'est supposé connu dans cette proposition.

Etapes précises à revoir :

1. Déterminer les transitions pertinentes dans les données locales déjà autorisées, sans modifier le script. Construire une table versionnée liée aux empreintes exactes du binaire et des scripts. Toute version ou signature inconnue refuse l'armement.
2. Identifier et vérifier un site réellement exécuté lors de l'entrée/sortie des états. Vérifier à part la source de vie/contrôle et de fin de chargement ; tant qu'elle manque, Playing doit rester non certifié. Ajouter ces observations natives constitue la modification architecturale nécessitant revue avant réalisation.
3. Publier des événements immuables, avec séquence et génération de scène/menu, dans une file bornée. Le producteur ne lit pas de pointeurs invités arbitraires et ne fait pas d'I/O sous les verrous du runtime. Une file pleine compte les pertes et invalide le contrôle.
4. Un propriétaire unique applique observations et commandes dans l'ordre au point de lecture de la manette ; le thread fenêtre publie séparément une identité/focalisation vérifiées. Aucun timer ne modifie les entrées depuis un second thread. Les courses et l'ordre réel doivent être testés dans l'intégration C/C++, pas déclarés résolus par ce modèle séquentiel Python.
5. Exporter les accusés de consommation avec identifiants explicitement transportés jusqu'aux métadonnées de capture. Ne jamais associer un draw au dernier compteur global. Les intervalles GPU et les échantillons d'entrée ne partagent pas automatiquement une identité de contenu.
6. Garder une fermeture normale ciblée par PID + création + fenêtre, vérifiés à nouveau. En cas d'échec, rapporter l'instance restante ; ne pas fermer par nom de processus global ni forcer un autre jeu. L'arrêt de la politique ne tient pas lieu de fermeture Windows.

Une supervision visuelle par CUA évite d'inventer les états du moteur mais sa latence n'établit pas le délai de 100 ms du modèle. Elle ne fournit donc pas, à elle seule, le pilote autonome demandé face aux ennemis. Aucune API Windows de simulation d'entrée alternative n'est ajoutée pour contourner les appuis maintenus indisponibles.

## Acceptation et essais restants

Les 20 tests hors jeu couvrent : mode désactivé ; faux indices camera/save/menu ; source non vérifiée ; sélection New Game ; trajet de chargement ; perte de focus/identité/ordre ; PID réutilisé ; horloge et délai global ; expiration exacte pour les 250 durées de 1 à 250 ms ; commande répétée ; pause/retour ; réponse de menu tardive ; mort et menu inattendu ; preuve de mouvement absente, étrangère, perdue ou hors durée. La présence de commandes synthétiques dans les tests ne correspond à aucune entrée en jeu.

Avant toute collecte automatique réelle :

* Prouver sur observations contrôlées la table des états, la mort, le chargement échoué, les menus superposés et les cinématiques ; chaque cas non couvert doit rester inconnu. Un passage réussi ne prouve pas une classification exhaustive.
* Mesurer la fraîcheur des événements et le délai de neutralisation ; tester arrêt du superviseur, fichier retiré/tronqué, perte de focus, rechargement et saturation de file.
* Vérifier que la fermeture ne concerne que l'instance possédée et qu'une copie SHA-256 nouvelle de la sauvegarde isolée précède chaque exécution. Ne jamais écraser les copies précédentes.
* Associer deux intervalles complets sans pertes à la commande consommée et à un déplacement constaté. Un coup d'ennemi, un cut ou un mouvement extérieur invalide cette preuve. Le modèle réclame des identités/temps concordants et une preuve visuelle/sémantique indépendante ; il ne la produit pas lui-même.
* Son muet, DLAA/jitter/debug motion désactivés. Aucun test de ces garde-fous ne valide la reprojection ou le DLAA.

Commande hors jeu reproductible : `python -B -m unittest discover -s experiments/gow3_automation -p test_policy.py -v`.
