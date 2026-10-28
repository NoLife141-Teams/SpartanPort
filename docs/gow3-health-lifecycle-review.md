# Santé inline : cycle de vie et horloge, hors jeu

Les essais sont terminés sans modifier le hook ni le runtime. L'adaptateur expérimental est dans `experiments/gow3_automation/health_clock_trial.h` ; son banc est `test_health_lifecycle.cpp`. Le générateur inline précédent est réutilisé tel quel sur des acteurs factices. Aucun fichier de sauvegarde ou de jeu n'est consulté.

## Horloge

L'adaptateur appelle `QueryPerformanceFrequency` à l'initialisation, puis `QueryPerformanceCounter` uniquement sur le consommateur hôte. Il conserve les ticks entiers et la fréquence, sans conversion flottante ou multiplication pouvant déborder. Le producteur inline ne fait toujours aucun appel natif. Par défaut l'adaptateur est désactivé : aucune requête QPC, aucune prise du verrou de boîte, aucun compteur diagnostique modifié.

Fréquence invalide, échec de lecture, valeur nulle/négative ou retour en arrière verrouillent l'état invalide pour toute la durée de cet adaptateur. Deux lectures égales restent autorisées. Il n'existe pas de remise à zéro automatique de la faute. Le type ne peut être copié ou déplacé et appartient à un seul consommateur.

Le consommateur acquiert la boîte sans boucle d'attente, copie le paquet puis prend la borne supérieure U. Il publie U comme borne inférieure L pour les prochains événements. Un échec d'horloge libère toujours la boîte et publie L=0, explicitement inconnu, sans réécrire la borne de l'ancien paquet. Si la boîte est occupée, l'horloge n'est pas appelée et la boîte reste au producteur.

La classification exige un instant courant valide issu de l'adaptateur ; une ancienne observation ne redevient donc pas exploitable après une faute d'horloge. Elle vérifie aussi session, génération, état de collecte, pertes connues et ordre des bornes. Elle qualifie **un échantillon**, jamais l'état présent du jeu. Le futur observateur devra conserver une invalidation persistante si des pertes sont découvertes après l'instantané initial des compteurs.

Le test Windows réel a observé une fréquence de **10 000 000 ticks/s**, effectué 1 000 lectures non décroissantes et encadré l'appel du producteur par deux lectures QPC indépendantes : `L <= avant <= après <= U`. Ce résultat ne garantit ni la précision physique de QPC ni un délai maximal entre ces instants. Les cas d'échec et de régression sont injectés par une source synthétique ; aucun réglage de l'horloge système n'est changé.

## Durée de vie démontrée dans le banc

Le stockage est non copiable/non déplaçable. Son adresse et son identifiant de session sont fixés avant la création de toute entrée exécutable. Chaque producteur synthétique possède une référence qui retient ensemble son code, ses slots historiques et le stockage diagnostique. Les références ne sont pas ajoutées dans le hook et n'ajoutent aucun coût partagé au chemin généré.

Cas testés :

* démarrage désactivé ou avec horloge invalide, sans allocation de code ni entrée publiée ;
* thread déjà existant qui attend une publication release/acquire : il ne reçoit l'entrée qu'après initialisation de la session, de la génération et de L ;
* refus d'un second démarrage ou d'une nouvelle liaison après arrêt ;
* identité de session distincte même entre deux types d'adaptateur d'horloge ; attribution refusée à la limite du compteur, sans retour à zéro ;
* ancien paquet conservant sa génération après changement ; rejet par une vue exigeant une autre génération ou session ;
* arrêt alors que deux producteurs ont encore des appels à effectuer : la boîte libre à cet instant n'est pas prise comme preuve de fin des producteurs ;
* nouvelle session créée pendant que l'ancienne reste active, avec adresse et identité distinctes ;
* destruction du stockage ancien seulement après abandon de toutes les références des producteurs, vérifiée après leur jonction.

Dernière exécution de l'arrêt concurrent : **4 002 appels dont 4 000 après l'arrêt logique, 469 paquets drainés et 3 533 pertes**. Les comptes exacts sont réconciliés une fois les appels terminés. Aucun paquet postérieur à l'arrêt n'est qualifié de récent utilisable ; aucun appel ancien ne touche la nouvelle boîte. Ces nombres dépendent de l'ordonnancement et ne constituent pas un débit attendu en jeu.

L'arrêt logique ferme la création de nouvelles liaisons et l'utilisation des observations. **Il n'éteint pas le stub déjà généré** : celui-ci peut encore remplir sa boîte ou compter des pertes. Il ne faut donc pas libérer cette boîte ou la remettre à zéro à l'arrêt de la collecte.

## Contraintes réelles encore à respecter

Le mécanisme de références du banc n'existe pas pour les threads invités réels. Leur absence momentanée dans la boîte, ou un compteur de producteurs actifs égal à zéro avant l'entrée d'un thread retardé, ne prouve pas qu'ils ne rappelleront plus le hook.

La lecture ciblée du runtime confirme :

* `src/runtime_thread.c` autorise des threads détachés ; `thread_join` traite un thread précis, pas une barrière générale de fin ;
* `src/probe.c` possède des sorties `_exit` avec le commentaire indiquant que des threads continuent de tourner ;
* `runtime_restart` sous Windows crée un autre processus puis termine le processus courant ;
* `src/runtime.c::guest_libc_exit` finalise et appelle `exit`, sans preuve ici de jonction de tous les producteurs.

Le plan minimal reste donc un stockage diagnostique trivial de durée de vie du processus, initialisé avant le démarrage invité, sans destruction anticipée, réutilisation, remplacement de boîte ou retrait du hook à chaud. Une véritable récupération avant la fin du processus demanderait une preuve de quiescence supplémentaire ; elle n'a pas été ajoutée.

Les identifiants du banc sont uniques dans son processus seulement. L'intégration devra associer un identifiant externe unique de processus/collecte à la séquence locale, surtout après un redémarrage qui reprend la commande précédente. Une génération diagnostique ne prouve toujours ni chargement réussi ni réapparition. Le journal futur devra être exclusif et borné ; un arrêt brutal peut empêcher un dernier drain ou un pied de journal, qui ne devra pas être interprété comme une attestation de complétude.

Les slots historiques restent inchangés et leurs races ne sont pas résolues. Les deux producteurs du test utilisent des slots historiques distincts pour isoler la vérification de la boîte diagnostique. Le paquet cohérent ne transforme toujours pas les lectures santé/maximum du moteur en transaction atomique.

## Vérification et suites

Compilation Clang 23.1.3 existant : `-std=c++23 -O2 -g -UNDEBUG -Wall -Wextra -Werror -pthread`, un job. Le nouveau test et le test ABI/inline précédent passent. Journaux et empreintes : `out/dlaa-health-lifecycle/result.json`, `tests.log`, `inline-regression.log`.

Avant intégration, il reste à raccorder ce contrat au propriétaire hôte existant, au journal borné et aux identifiants de collecte, puis vérifier le chemin désactivé sans effet. Pas de nouveau thread requis pour le plan minimal. Pas de probe recompilé, jeu lancé, entrée injectée, sauvegarde consultée, téléchargement, commit ou push pendant ces essais.
