# Etat du pilote de collecte autonome — 27 octobre 2026

Le cycle demandé (copie de sauvegarde, chargement, mouvement/captures, fermeture de la seule copie de test) **n'est pas prêt pour une exécution autonome**. Aucun script à délais fixes n'est lancé. La collecte compacte reste prête, mais ne reconnaît pas la vie/mort du personnage ni le menu réel.

## Constats observés

Le probe compact PID 38672, fenêtre 7802482, appartient à la session `group-compact-20261027T162002Z`. La capture d'écran de contrôle montre le menu principal avec « New Game » sélectionné. Le journal émet pourtant `camera-candidate` avec 30 à 32 snapshots : ce statut ne prouve donc pas le chargement d'une sauvegarde, une partie active ou l'absence de menu. Aucun bouton de chargement n'a été activé durant cette étude.

Le son de cette instance est désactivé par `SPARTAN_AUDIO=none` ; le journal confirme `Runtime: audio backend timer (silent)`. La manette physique peut rester branchée, mais ses boutons sont combinés à l'injection native. Aucune modification de volume général ou de réglage système.

CUA permet de cibler cette fenêtre et les appuis simples. Sa commande maintenue retourne `Held key presses are unavailable on Windows.` Ce n'est pas un rejet d'autorisation. Aucun SendInput, PostMessage, driver, manette virtuelle système, nouvelle installation ou voie de remplacement des commandes UI n'est utilisé.

## Contrat natif étudié, sans l'activer dans le jeu

`src/runtime_pad.c` propose `SPARTAN_PAD_FILE` et `SPARTAN_PAD_REPLAY`. Ce sont des options du processus lues au démarrage/premier appel, pas un service permettant de prendre arbitrairement le contrôle d'un autre processus. L'instance actuelle n'a pas de chemin de commandes configuré.

Le test `tests/test_pad_automation_contract.c` inclut le vrai fichier du runtime et appelle seulement son parseur et son lecteur de replay. Il n'initialise ni SDL ni manette, n'ouvre aucune fenêtre et n'exécute aucun code du jeu. Les fichiers utilisés se trouvent dans un nouveau dossier de résultats.

Résultats Windows vérifiés :

1. Deux écritures de même longueur pendant la même seconde de mtime peuvent ne pas être distinguées.
2. Un changement de taille rend la seconde commande observable.
3. Si le fichier disparaît, la dernière commande reste active : il n'y a pas de bail ni d'expiration automatique.
4. Un fichier vide effectivement observé relâche les commandes injectées.
5. Un replay de 250 ms avec une dernière ligne neutre atteint bien cette ligne.
6. A la fin du replay, les entrées physiques reprennent : ce n'est pas un contrôleur neutre exclusif.
7. Le replay est chargé une seule fois par processus ; changer son fichier ne change pas une séquence ultérieure.

Autre limite, établie par lecture de `src/probe.c` : `--timeout` est ignoré sous Windows (`no watchdog on Windows`). Il ne fournit pas de garantie de fermeture. `note_game_menu` dans `runtime_pad.c` est explicitement une heuristique déduite des boutons, pas une lecture de l'état du menu.

Le script réutilisable **de validation CPU**, `scripts/test_gow3_automation_contract.ps1`, recompile ces tests avec les outils existants, un seul job, un dossier inédit et un résultat JSON. Il n'est pas présenté comme le pilote de jeu demandé.

## Conditions du pilote complet

Avant une nouvelle exécution : vérifier le binaire, le PID et sa date de création, le dossier user isolé et une nouvelle copie SHA-256 de chaque sauvegarde ; refuser les reparse points, toute destination existante et les instances ambiguës. Conserver son muet et DLAA désactivé.

Le contrôle doit ensuite être supervisé par des observations visuelles fraîches du PID/fenêtre exacts : menu principal, sélection Load, écran de sauvegarde, partie chargée et personnage vivant. Chaque transition inattendue doit arrêter la séquence. Une preuve de shader ou de matrice ne remplace pas ces observations.

Le mouvement doit être bref et neutralisé même si le superviseur s'arrête ; l'API actuelle ne fournit pas cette garantie pour les commandes persistantes. Les ennemis peuvent interrompre le déplacement, déplacer le personnage/la caméra, déclencher une cinématique ou tuer le personnage. Un changement de matrice ne prouve pas à lui seul que le déplacement demandé a eu lieu. Aucun combat, sauvegarde, nouvelle partie ou réglage ne doit être confirmé automatiquement.

Chaque capture doit fermer deux intervalles complets sans perte. Une absence de preuve du mouvement, une mort, un menu ou un changement de fenêtre bloque les phases suivantes. La fermeture doit viser uniquement le PID et la fenêtre dont l'identité a été revérifiée ; aucune fermeture par nom global ni force-kill d'une autre instance.

Il manque donc un observateur fiable du jeu relié au superviseur et un arrêt sûr des commandes. Les ajouter dans le runtime dépasse un simple script de collecte. Ils doivent être présentés avant toute modification de cette architecture. En attendant, une collecte supervisée via CUA/appuis simples et gestes humains reste possible ; aucune nouvelle collecte ou relance n'est faite automatiquement par les livrables de cette étude.
