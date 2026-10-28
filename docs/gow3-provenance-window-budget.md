# Provenance : préfixe réel et correction du budget

## Résultat réel conservé

Session `group-provenance-20261027T154816Z`, phase stationary, 27 octobre 2026, 15:50:04.451821 à 15:50:04.534549 UTC. Le fichier original reste intact : SHA-256 `2b803040898b66a1bd293b77af61b674cff5f253598b8117fb8c3ca38c5a271c`, 16 773 833 octets.

L'intervalle **6162** est complet dans le périmètre instrumenté : 2191 draws contigus, 4383 relevés de provenance, marqueur de clôture présent, 8 716 536 octets. Le flip non-EOP de clôture est `0x1812`. Le shader caméra attendu apparaît 375 fois avec 375 snapshots, zéro manquant et zéro conflit. Deux permutations sélectionnées (`e43a5ea709d996aa`:266, `e43a5ea709d996ab`:109) partagent une matrice observée dans cet intervalle.

L'intervalle **6163** est tronqué : 2037 draws conservés et pas de relevé de provenance de clôture. Le bilan global compte **145 draws perdus et 305 éléments de provenance perdus**. Ce dernier nombre v1 additionne des événements et des en-têtes de lots ; ce ne sont pas 305 invalidations. Le compteur de pertes de la file interthreads reste à 100, valeur déjà présente à la baseline ; aucune nouvelle perte de cette file n'est observée. Les deux marqueurs d'intervalle sont présents mais ne rendent pas les données du second intervalle complètes.

La suite de l'analyse utilise l'intervalle 6162 entier ou indique explicitement « préfixe seulement ». Les rapports dérivés `provenance-prefix-analysis-v2.json` et `provenance-association-detail.json`, dans la session, font foi. Le premier rapport v1 utilisait par erreur l'ancien UID depth 8 pour une recherche secondaire ; il est conservé comme historique, remplacé par v2 qui résout l'UID depuis les draws caméra de cette session.

## Associations désormais observables

Les UID sont propres à cette instance, pas des identifiants permanents du rôle d'une image.

* **Profondeur caméra UID 9, backing 1** : D32S8 (`130`), deux samples, range mip 0/layer 0. La vue d'attachment 1 a les aspects depth+stencil (`6`). Le même backing est lu via la vue 2 depth (`2`) et la vue 4 stencil (`4`). Ces associations ne reposent plus sur l'égalité des handles de vue.
* **Couleur UID 12, backing 1** : format sRGB `43`, deux samples, vue d'attachment 2 puis vue de sampling 1. Elle apparaît comme attachment dans 541 draws du premier intervalle.
* Au draw **8277093**, VS `475a41468b1a1fbc`, PS `475a4146e3a74097`, permutations `efd04d8023fec408` / `efd0530963dc1d55`, les bindings 1 et 2 lisent respectivement cette profondeur et cette couleur. Le draw 1920×1080 écrit deux cibles simples samples UID 754 et 11, sans test/écriture depth. C'est un point de composition à étudier ; son opération exacte et sa position vis-à-vis de toute l'interface ne sont pas prouvées.
* Une autre association du même backing UID 24 relie une vue UNORM `37` et une vue sRGB `43`. Un handle différent n'implique donc pas une ressource différente, et le format effectif de vue compte.

Dans le préfixe global, 7701 descripteurs d'image sont résolus : 5213 par chemin normal et 2488 par memo ; aucun cas helper observé ici. Les tests du helper restent synthétiques. Ni la conservation du contenu, ni l'absence de writes non instrumentés, ni les conventions de reprojection ne découlent de ces identités.

## Correction minimale, sans hausse du plafond

Le préfixe contient 8410 relevés vides (1 774 518 octets). Parmi eux, **8409 relevés / 1 774 299 octets** sont redondants selon le prédicat retenu : point `before_bindings` ou `after_recording`, zéro événement, drain non occupé, pas de flip, et compteurs identiques à une baseline déjà sérialisée. Tous les draws restent sérialisés.

Le schéma **`gow3-provenance-v2`** omet seulement ces relevés. Il garde :

* chaque événement réel, même identique à un précédent ;
* chaque changement de compteur, même dans un lot vide ;
* chaque drain occupé, point inconnu ou marqueur de flip accepté ;
* l'identité des draws, snapshots, vues et ressources, sans changement de rendu.

Un échec d'écriture ne met pas à jour la baseline sérialisée. Le relevé suivant contenant un nouveau compteur doit encore être écrit. Les relevés omis sont comptés par intervalle et dans le bilan ; ils ne sont pas comptés comme perdus. Les nouveaux compteurs séparent lots et événements :

`batches_seen = batches_written + empty_batches_elided + batches_dropped`

`events_seen = events_written + events_dropped`

L'ancien `provenance_event_records_dropped` reste égal à `batches_dropped + events_dropped`. Les compteurs de pertes interthreads conservent leur baseline séparée. Le plafond reste **16 777 216 octets**, 8192 draws, deux intervalles, création exclusive et réserve finale de 4096 octets. Aucun tampon croissant ou nouvel accès mémoire invité.

## Mesures et tests

Un rejeu CPU avec le vrai `Capture` répète **deux fois le premier intervalle complet**. Il ne reconstitue pas le second intervalle perdu et n'est pas une nouvelle validation du jeu. Boost.JSON déjà installé sert uniquement au lecteur du test ; aucune dépendance téléchargée.

Résultat : **4382 draws et 126 événements conservés**, zéro perte, **15 593 515 octets**, marge **1 183 701 octets** (environ 7,1 % du plafond). 8662 lots vides redondants omis sur 8766 observés ; 104 lots écrits. Un vérificateur Python indépendant compare chaque payload de draw au JSON original, sans les seuls numéros draw/intervalle remappés pour le rejeu, et chaque événement dans l'ordre. Tous sont identiques ; offsets physiques et compteurs sont vérifiés.

Six suites CPU passent après compilation à **deux jobs maximum** : `gow3-provenance-budget-test`, `gow3-resource-provenance-test`, `gow3-capture-bytes-test`, `gow3-pair-capture-test`, `gow3-group-capture-test`, `gow3-readback-primitives-test`. Elles couvrent notamment changements de compteurs, drains occupés, frontières, saturation, non-écrasement, format historique désactivé, UID réutilisé et propriété helper. Un cas supplémentaire vérifie un gros lot refusé, puis un changement de compteur vide conservé : pertes de lot/événements exactes, baseline non avancée à tort.

Le budget est adéquat pour ce rejeu mesuré. Une autre scène plus complexe peut encore saturer : cette marge n'est pas une garantie universelle. Le bilan reste conservateur et interdit de qualifier une fenêtre perdue de complète.

Probe séparé : `out/dlaa-provenance-compact/bin/spartanport-probe.exe`, SHA-256 `4CDF371264C0D4054DC6D44AA9A1C90A69FED8D0EEF3CFC646DA85C30DE08886`. Résultats : `out/dlaa-provenance-compact/results.json`, `replay-verification.json` et `logs/`. Ancien probe, session et captures conservés ; aucun lancement ou fermeture dans cette étape, aucun accès aux sauvegardes, aucun push.

## Prochain protocole et contrôle clavier

La fenêtre isolée existante (PID 41004, HWND 35326480 lors de la vérification) exécute encore l'ancien probe. **Une relance coordonnée est nécessaire avant une collecte corrigée.** Aucune fermeture, relance ou touche n'a été effectuée.

Après coordination : nouvelle session et vérification de sauvegarde selon le protocole existant, chargement de la partie, puis stationary et motion déclenchées séparément. Examiner clôture, marge physique et pertes après chacune. Pause/retour seulement si les précédentes sont exploitables et les conditions de contrôle permettent de les distinguer. Aucune nouvelle tentative automatique après saturation.

Le code `src/runtime_pad.c` et le log confirment qu'une DualSense est connectée et que le clavier reste traité : boutons combinés, axes remplacés seulement pendant une touche directionnelle. Mapping par défaut sans remappage déclaré : WASD pour le stick gauche, flèches pour le stick droit, Retour arrière pour le menu pause, Entrée pour Options. Aucun besoin démontré de débrancher la manette ; la laisser au repos évite l'addition d'entrées. Les flèches représentent le stick droit : leur effet concret caméra/esquive dépend du jeu, il faut l'observer.

L'outil CUA a identifié exactement le chemin du probe isolé et sa fenêtre. Il expose des touches ciblées par fenêtre et active celle-ci sous Windows. Seule une lecture d'état a été faite ; pas d'essai de mouvement. Cette lecture a pris environ 154 secondes, donc il ne faut pas supposer une synchronisation rapide entre CUA et les fenêtres de capture de quelques dizaines de millisecondes. Le contrôle temporel doit être vérifié avant une séquence automatique ; aucun réglage global ou débranchement n'est requis par les constats actuels.
