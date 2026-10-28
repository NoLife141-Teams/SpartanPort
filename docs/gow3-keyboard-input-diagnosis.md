# Diagnostic hors jeu des appuis clavier

Le trajet clavier actuel peut perdre un appui bref. Ce mécanisme est reproduit par les tests, mais la cause des deux appuis K sans effet dans la session `health-passive-20261027T181558Z` reste indéterminée : aucun événement SDL ni échantillon de manette correspondant n'a été enregistré. Cette étape ne lance aucun jeu, ne lit aucune sauvegarde et ne modifie pas le runtime.

## Faits établis dans les sources et données existantes

`gpu/shim/spartanportgpu.cpp` fait tourner `WindowSDL::PollEvents()` sur le thread de fenêtre, puis le drain passif et `SDL_Delay(2)`. Ce délai est une demande d'attente, pas une garantie de période de 2 ms. `gpu/shim/window.cpp` vide la file par `SDL_PollEvent`, traite le texte et l'overlay, mais ne conserve pas les fronts K/Espace pour la manette invitée.

`src/runtime_pad.c::sample_host` lit `SDL_GetKeyboardState` si le sous-système vidéo existe. `scePadReadState` et `scePadRead` appellent le sampler à la demande du jeu. La seconde API renvoie une seule entrée actuelle, même si plusieurs sont demandées. Il n'y a ni file historique des touches ordinaires ni prolongation minimale de K.

Les en-têtes SDL installés (3.4.18) documentent que `SDL_PollEvent` pompe les événements et qu'un appui puis relâchement entièrement traités avant une lecture de `SDL_GetKeyboardState` peuvent ne jamais apparaître dans l'état lu. Un autre cas existe même avec deux pompages distincts : l'état peut redevenir relâché avant le prochain `scePadReadState`. Le succès de `pressKey` prouve seulement l'achèvement de l'appel de l'outil, pas la réception SDL ou la consommation par le jeu.

Mappings par défaut : K = croix directionnelle bas (`0x40`), I = haut, Espace = Croix (`0x4000`), Entrée = Options (`0x08`). La flèche bas incline le stick droit ; ce n'est pas un remplacement équivalent de K. L'environnement enregistré ne définit ni remappage clavier/manette, ni fichier de commandes, ni replay. Le journal confirme l'ouverture de la manette logique, mais ne contient aucune ligne de connexion d'une manette SDL : cela ne prouve pas que le périphérique physique était débranché.

Le sampler n'interroge pas explicitement `SDL_GetKeyboardFocus`. Il fait confiance au tableau SDL. Le focus annoncé par l'outil Windows n'est donc pas la preuve enregistrée de l'identité de fenêtre et de l'état de focus vus par SDL. Le focus utilisé dans `window.cpp` pour le verrouillage de souris ne constitue pas un filtre équivalent du clavier dans `sample_host`.

Une manette présente ne désactive pas le clavier : les boutons sont combinés par OU. Des directions opposées peuvent donc être exposées simultanément ; le jeu décide de leur traitement. Une dérive de stick reste présente sur les axes que le clavier ne commande pas. Aucun besoin de débrancher la manette n'est démontré par les données actuelles.

L'overlay, le dialogue de texte, le sélecteur de sauvegarde hôte ou les trophées peuvent neutraliser les entrées du jeu via `CapturesInput`. Après leur fermeture, `hold_after_capture` masque les boutons jusqu'à une lecture sans bouton tenu. Un bouton de manette tenu peut prolonger ce masque. Le menu principal du jeu n'est pas à lui seul cet overlay ; les captures visuelles disponibles ne montrent pas ces variables internes.

`game_menu.up` est une heuristique de navigation de molette, initialisée vraie et modifiée par les boutons/mouvements. Ce n'est pas un détecteur fiable de menu, de chargement, de vie ou de mort ; elle ne change pas le mapping direct de K.

## Tests exécutables

`tests/test_keyboard_sampling_contract.c` inclut le vrai `runtime_pad.c` et remplace seulement les lectures de périphériques par des états factices. Il ne crée aucune fenêtre SDL et n'énumère aucun périphérique réel. Douze scénarios couvrent les mappings, le mélange clavier/manette, les axes, le blocage de l'overlay, la libération après capture, une impulsion perdue entre lectures, une impulsion visible, la lecture dite tamponnée, l'absence de vidéo et l'absence d'interrogation explicite du focus.

Ces tests vérifient les transformations du runtime. Ils ne simulent pas le transport réel Windows → SDL, la fréquence de lecture réelle du menu, l'état des ennemis ou la reconnaissance visuelle d'un chargement. Ils ne fixent aucune durée d'appui prétendument garantie.

## Restriction de l'outil et prochain essai discriminant

L'outil Windows a précédemment indiqué : `Held key presses are unavailable on Windows.` Il s'agit d'une capacité indisponible, pas d'un refus d'approbation. Les deux appels ponctuels K ont ensuite réussi au niveau de l'outil, mais sans sélection visible. Ni la durée de leur état bas, ni les événements SDL correspondants ne sont connus. Remplacer cet outil par SendInput, une injection, un fichier de commandes ou un replay pour maintenir une touche contournerait la restriction ; aucune de ces voies n'est utilisée ou proposée comme solution opérationnelle.

Il n'existe donc pas encore de méthode autonome démontrée fiable avec les seuls appuis ponctuels disponibles. Le prochain test doit mesurer le transport, pas répéter aveuglément Load. Proposition à préparer avant une nouvelle session :

1. Journal passif opt-in, local, limité à 10 secondes et 64 Kio ; arrêt sur saturation avec compteurs de pertes. Uniquement K, Espace et Entrée, sans texte saisi, touches générales, mémoire invitée ou données de sauvegarde.
2. Sur le thread SDL : événements bas/haut, scancode, répétition, ID de fenêtre, focus SDL et changements de capture d'entrée, horodatés par une même horloge monotone. Aucun enregistrement d'une touche dans une autre fenêtre ni en saisie de texte.
3. Sur le chemin réel de lecture manette : compteur de lecture, masque pertinent avant/après blocage, `hold_after_capture` et présence de bouton de manette concurrent. Séquences portées explicitement, compteurs de pertes ; absence d'association certaine ou chevauchement concurrent signifie « indéterminé ». Publication bornée sans I/O dans ce chemin ; consommation sur le propriétaire SDL. Ne pas associer une lecture au dernier événement global sans preuve d'ordre.
4. Menu principal confirmé visuellement, focus confirmé, aucune autre entrée tenue : une seule pression K, puis attente de la lecture des traces et du changement visuel. Avec l'outil actuel, sa capture reste un test de transport qui peut échouer ; une courte pression humaine est l'alternative prise en charge pour établir la référence. Une durée telle que 100 ms ne serait qu'un point de départ, jamais une garantie sous contention.
5. N'accepter l'action que si le journal complet montre la pression, au moins une lecture exposant exclusivement Bas, puis le relâchement et une lecture neutre, et si l'écran montre exactement Load sélectionné. Sinon arrêt, sans Espace ni répétition. Une fois Load visible, valider séparément, puis contrôler le véritable écran de sauvegarde avant toute nouvelle action. Aucun mouvement ou combat.

Les catégories recherchées sont : événement absent de la fenêtre attendue ; événements reçus mais impulsion absente de toutes les lectures ; entrée masquée par capture ; entrée visible mais choix inchangé. Avec perte, intervalle incomplet ou ordre ambigu, aucune catégorie causale n'est affirmée. Cette instrumentation est un plan, pas une intégration réalisée. Elle doit être revue et testée avant utilisation ; elle ne répare ni n'allonge les appuis.

## Limites de cette étape

Pas de modification de `runtime_pad.c`, de `window.cpp`, des commandes de jeu ou des paramètres système. Pas de nouveau lancement du jeu, accès aux sauvegardes, installation, commit ou push. Les résultats de compilation et d'exécution sont conservés dans `out/dlaa-keyboard-diagnosis/`.
