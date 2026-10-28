# Instrumentation passive : prototype et point d'arrêt

## Ce qui est prêt

`experiments/gow3_automation/passive_events.h` et `test_passive_events.cpp` constituent un prototype natif autonome. Ils ne sont inclus dans aucun fichier du runtime, aucun hook, ni aucune cible de probe. Ils ne collectent rien dans le jeu.

Le prototype contient une file fixe de 64 événements copiés par valeur, désactivée par défaut, avec réservation sans attente (`atomic_flag`), publication synchronisée et compteurs de tentatives/pertes. Deux producteurs peuvent publier sans mélanger leurs champs. Le numéro de publication ordonne les événements retenus ; le numéro de tentative ne doit pas être interprété comme leur ordre chronologique entre threads. Les compteurs lus pendant la concurrence sont des instantanés, pas une attestation de complétude ; l'égalité exacte retenus + pertes = tentatives est vérifiée après l'arrêt des producteurs.

Chaque événement peut transporter une session, une génération diagnostique, un producteur, une séquence, un horodatage et des valeurs de dialogue/blocage/focus/QTE/santé. Il ne contient aucun pointeur d'acteur à déréférencer. La file ne fait ni allocation, ni I/O, ni appel au jeu. Un préfixe fixe de 4096 enregistrements démontre la conservation des premiers événements sans écrasement ; le sérialiseur fichier et sa limite en octets ne sont pas encore intégrés.

La vue consommateur distingue échantillon positif récent, échantillon nul récent, périmé, invalide, inconnu et pertes. **Elle ne possède aucun état Alive ou Playing ni aucune fonction de commande.** Une santé ancienne ne prouve rien sur la vie actuelle. Une génération diagnostique n'est pas une preuve de scène ou de réapparition.

Compilation : Clang existant, C++23, un job, assertions actives. Résultats : `out/dlaa-passive-prototype/result.json` et `tests.log`.

Cas vérifiés :

* mode désactivé sans publication ni compteur ;
* saturation de file et préfixe sans écrasement ;
* deux producteurs, 8000 tentatives : 2747 retenues, 5253 pertes explicites lors de l'exécution enregistrée, chaque payload corrélé et sans mélange ;
* vieillissement, horodatage futur, santé nulle et valeurs invalides ;
* invalidation à un chargement/réapparition **synthétique**, réutilisation du même token et rejet des anciennes générations ;
* source incohérente, session étrangère, pertes persistantes et état inconnu ;
* contre-exemple déterministe du compteur final actuel : deux lectures égales du compteur peuvent entourer un mélange de champs anciens et nouveaux. Le test utilise des atomiques pour ne pas introduire lui-même de data race C++.

Ces tests vérifient la file avec des événements synthétiques cohérents. Ils ne prouvent pas la cohérence de l'actuel hook de santé, l'absence de pertes en conditions réelles ou la reconnaissance des états du jeu.

## Pourquoi l'intégration est arrêtée

`src/actor_hook.h` écrit successivement acteur, santé et maximum dans le slot partagé, puis effectue `lock inc seq`. Le lecteur existant copie le slot. Un lecteur peut observer la nouvelle santé avant le nouveau maximum, avant que seq change. Ajouter une lecture finale de seq ne ferme pas cette fenêtre. Un compteur impair/pair ne suffit pas non plus sans établir l'exclusion entre producteurs ; le code ne démontre pas qu'un seul thread peut exécuter le hook de dégâts à la fois.

Corriger cela exige de changer **le producteur dans le stub généré**. Il ne faut ni déréférencer plus tard `slot.actor`, ni appeler un helper natif depuis ce point arbitraire sans préserver tous les registres et états vivants. Cette dernière option implique notamment convention d'appel, alignement de pile, états SIMD et absence d'allocation/blocage/exception dans le helper. L'horodatage doit être produit au moment de l'événement, pas lorsque l'overlay découvre tardivement le compteur.

Le signal de réapparition n'existe pas actuellement. Une modification du pointeur d'acteur ou une nouvelle santé ne prouve pas une réapparition ; une même adresse peut être réutilisée. Il serait incorrect de fabriquer une génération de scène à partir de ces seules valeurs.

Conformément au point d'arrêt demandé, **aucun hook, point d'appel runtime, rendu ou commande n'a été modifié**, et le probe n'a pas été recompilé avec une instrumentation prétendument complète.

## Plan d'intégration à revoir avant réalisation

1. **Session et collecte.** Un drapeau strict désactivé par défaut, un identifiant de session unique au processus et un nouveau fichier exclusif. File fixe, journal borné proposé à 1 Mio/4096 événements, motifs de pertes et état incomplete. Aucun fichier créé lorsque le drapeau est absent. Aucun secret, nom de sauvegarde, image, shader ou donnée de jeu dans les enregistrements.
2. **Dialogues.** Dans `runtime_services.c`, publier les scalaires déjà copiés au moment de `save_open`, du résultat de sélection/annulation, et de `save_close/term`. Ne pas conserver les pointeurs `items`, `dirs`, `user_data`, icônes ou métadonnées. Une génération peut nommer une tentative de chargement, explicitement sans affirmer sa réussite ni une réapparition.
3. **Blocage et focus.** Dans le thread propriétaire de l'overlay et de la fenêtre, publier les changements à partir des états déjà connus. Ne pas relire les booléens non atomiques depuis un nouveau thread. Distinguer overlay natif, liste/saisie, focus et minimisation ; aucun n'est à lui seul le menu pause du jeu.
4. **QTE.** Observer uniquement le compteur hôte déjà publié et l'état d'installation du hook. Ne pas ajouter de lecture de `block+offset`. Nommer l'événement activité de ce hook QTE, sans le transformer en signal de contrôle universel.
5. **Santé — revue du producteur obligatoire.** Préparer une variante opt-in du stub conservant à l'identique le chemin désactivé. Réserver un emplacement de diagnostic avec une opération sans attente ; si occupé, compter la perte puis poursuivre les instructions originales. Dupliquer dans l'emplacement réservé les scalaires au moment où le hook les lit déjà, publier seulement après remplissage complet. Une publication cohérente du paquet ne signifie pas que le moteur a fourni une transaction atomique de santé maximale/courante.
6. **Horodatage et ABI.** Choisir explicitement entre un helper natif borné dont l'état vivant est intégralement préservé et un mécanisme inline vérifiable. Tester sur acteurs factices la conservation de tous les registres/flags nécessaires, la pile, les chemins joueur/ennemi, les producteurs simultanés et les échecs de réservation. Ne pas masquer un temps de lecture consommateur comme un temps producteur exact.
7. **Interprétation.** Pertes, busy drains, trous de séquence, source inconnue ou changement de génération invalident les conclusions. Aucun état de vie ou de mort certifié à partir d'une seule valeur. Réapparition reste unavailable tant qu'un événement sémantique indépendant n'est pas identifié et validé.
8. **Validation avant jeu.** Intégrer d'abord les producteurs sur doubles synthétiques, tester publication et arrêt simultanés, absence de fichier/accès invité en mode désactivé, limites en octets et non-écrasement. Puis compiler le probe séparément à deux jobs maximum. La prochaine exécution réelle devra rester passive et être coordonnée ; elle comparera les événements aux états visuellement confirmés.

Cette étape ne lance aucun jeu, ne consulte ni ne modifie les sauvegardes et ne publie aucun fichier. Le diagnostic de cohérence est un blocage technique d'intégration, pas un refus d'autorisation.
