# Revue du producteur de santé — essai hors jeu

La publication cohérente paraît faisable par une modification locale du générateur de hook, **si un intervalle temporel conservateur suffit**. Un horodatage exact du dégât et une génération de réapparition restent indisponibles. Aucun runtime, hook installé, commande, rendu ou probe n'est modifié par cette étude.

## Preuves dans les sources

* `src/runtime.h` définit l'ABI invitée `sysv_abi`. Un appel natif Windows inséré au milieu de la routine devrait traiter la convention d'appel, tous les registres vivants et la pile ; préserver seulement les registres non volatils serait insuffisant.
* `src/actor_hook.h` remplace exactement sept octets : `mov rax,[r13]` puis `mov rdi,r13`. La signature de 17 octets inclut ensuite `call [rax+0xf8]` et `test eax,eax`. RAX et RDI peuvent servir de temporaires avant le rejeu. Les flags arithmétiques ne constituent pas des arguments SysV ; la branche après l'appel utilise le nouveau `test`. Cette analyse reprend le contrat du hook existant, pas une preuve de toutes les routines de toutes les versions du jeu.
* Les lectures existantes sont le marqueur à `r13+0x298`, la santé à `+0x1cc`, le maximum à `+0x1d0`, puis la lecture de la table virtuelle par l'instruction rejouée. La variante testée conserve exactement ces lectures sur chaque chemin. Le lecteur hôte ne déréférence jamais la valeur d'acteur enregistrée.
* `src/probe.c::install_actor_watch` exige une signature unique, la zone mappée, les sept octets originaux et un saut relatif dans la plage admise. `src/signature.h` rejette zéro ou plusieurs occurrences. Le hook historique est installé quand cette validation réussit, indépendamment de l'affichage de la barre de santé. Le diagnostic proposé doit avoir son propre choix opt-in au démarrage.
* `gpu/shim/spartanport_actors.cpp` contient les deux slots historiques ; `spartanport_overlay.cpp:718` les copie sans protocole producteur/lecteur. L'incrément final de `seq` ne protège pas les champs précédents. Un nouveau diagnostic ne doit pas réutiliser cette copie comme source cohérente.
* Le site santé est juste après la mise à jour qu'intercepte aussi `src/cheat_hook.h`. Si une option de santé infinie ou de multiplicateur est active, les valeurs observées peuvent déjà être transformées. Cette étude n'a pas consulté ni changé les réglages du jeu.

## Essai indépendant exécuté

Fichiers : `experiments/gow3_automation/health_inline_trial.h` et `test_health_inline_trial.cpp`. Ils ne sont inclus dans aucune cible runtime. Le code généré s'exécute uniquement sur de petits acteurs factices alloués par le test. Le générateur historique réel sert de référence.

Une boîte alignée de 128 octets contient un verrou atomique sans boucle de reprise, un paquet et des compteurs. Le producteur prend la boîte par `xchg`, duplique les scalaires à partir des registres utilisés par les lectures existantes, puis publie le paquet et libère la boîte. Boîte pleine ou occupée : perte comptée, exécution complète du hook historique. Une boîte pleine conserve son ancien paquet sans écrasement. Aucune attente logicielle, allocation, I/O, instruction SIMD, utilisation de pile ou appel natif dans le stub. Les opérations atomiques peuvent néanmoins subir la contention matérielle ; aucune garantie de latence temps réel n'est revendiquée.

Les atomes 32/64 bits sont vérifiés lock-free et leurs tailles/offsets contrôlés. L'interopérabilité entre code machine généré et atomiques C++ dépend du compilateur et de x86-64 ; ce n'est pas une preuve de portabilité C++ universelle ni un résultat ThreadSanitizer.

Compilation Windows : Clang 23.1.3 existant, cible `x86_64-w64-windows-gnu`, C++23, `-O2 -UNDEBUG -Wall -Wextra -Werror -pthread`, un seul job. Résultats dans `out/dlaa-health-inline-trial/` : `tests.log`, `result.json`, `enabled-stub.asm` et exécutable autonome.

Tests réussis lors du dernier passage :

* chemin désactivé octet pour octet identique au générateur historique, aucun compteur diagnostique touché ;
* publication joueur et ennemi, boîte pleine sans écrasement, boîte occupée sans changement de propriétaire ni omission du hook historique ;
* registres généraux au point de reprise, YMM0–15, MXCSR, DF, RSP et 128 octets de zone rouge SysV ; la capture de MXCSR est prise immédiatement avant le stub, non avant les appels de préparation du banc ;
* paquet en attente conservant son ancienne génération et ses bornes ; changement de génération croissant seulement, refus si occupé ;
* borne future, inversée ou non initialisée => inconnu ; intervalle large ne rajeunissant pas un ancien événement ;
* deux producteurs concurrents : **6 000 tentatives, 1 753 paquets retenus, 4 247 pertes**, sans paquet mélangé et avec numéros de publication continus. Les deux producteurs ont des slots historiques distincts : ce test démontre la boîte diagnostique, pas la correction des races des slots historiques partagés. Les comptes exacts sont réconciliés après la fin des producteurs.

Le désassemblage vérifié couvre les branches joueur, ennemi, plein et occupé. Le stub fait 392 octets. Les deux zones `health_resume_pointer_*` sont des données de saut, pas du code exécuté. Les états x87/AVX-512 ne sont pas testés dynamiquement ; aucune instruction du stub ne les touche. Les flags arithmétiques ne sont volontairement pas comparés.

## Temps, génération et limites sémantiques

L'hôte publie sous le même verrou un instant monotone **L**. Le hook copie ce dernier L avec son paquet. À la consommation, après acquisition du verrou, l'hôte prend **U** : l'observation du producteur est dans **[L,U]**. Aucun appel d'horloge n'est fait dans le hook. La consommation peut publier un nouveau L pour les événements suivants, sans changer le L d'un paquet ancien.

À l'instant N, avec durée de validité T : `N-L <= T` autorise seulement « échantillon dans une fenêtre récente » ; `N-U > T` établit un échantillon périmé ; entre les deux, résultat inconnu. Une horloge absente, en régression, une borne initiale inconnue, une perte ou un changement de session doit invalider l'interprétation. Les tests utilisent une horloge synthétique ; l'adaptateur monotone du runtime n'est pas intégré.

La génération est celle du collecteur, copiée sous verrou. Elle peut nommer une tentative de chargement, jamais affirmer sa réussite ou une réapparition. Un paquet déjà en attente conserve son ancienne génération et doit être rejeté par une vue exigeant la nouvelle. Ni l'adresse d'acteur ni sa réutilisation ne prouvent une identité de vie.

La boîte empêche le mélange entre publications diagnostiques. Elle ne rend pas atomiques les deux lectures de champs que le moteur pourrait modifier concurremment. Le hook n'est pas un heartbeat ; des morts, chutes ou transitions peuvent ne pas passer par lui. Aucune valeur positive ancienne ne prouve que le personnage vit actuellement, et zéro ne constitue pas un signal universel de mort.

## Plan d'intégration borné, non appliqué

1. Initialiser une boîte stable avant de démarrer les threads invités. Drapeau diagnostique absent : appeler exactement le générateur historique, sans boîte ni journal nouvellement utilisés. Pas d'activation à chaud.
2. Ajouter une variante du générateur dans `src/actor_hook.h`, ou un générateur C++ appelé uniquement à l'installation, reproduisant les instructions testées ; préserver tous les contrôles actuels de `install_actor_watch`. Ne pas exporter Xbyak comme nouvelle dépendance runtime : il est seulement utilisé dans ce banc.
3. Faire vivre le stockage hôte et ses adresses jusqu'à la fin de tous les threads invités. Un lecteur propriétaire existant draine sans attente et émet dans la file passive bornée déjà prototypée. Aucun nouveau thread nécessaire ; un lecteur retardé donne des données inconnues ou périmées, pas des données fraîches inventées.
4. Garder un journal exclusif à limite exacte, des compteurs de pertes et une invalidation persistante des conclusions après perte. Transporter des identités de session/génération explicites. Un total lu pendant la concurrence n'est qu'un instantané.
5. Ne pas remettre à zéro une boîte possiblement possédée. Si une lecture invitée déjà existante échoue après acquisition, le verrou peut rester pris : les événements suivants sont alors perdus, sans attente. Traiter la santé comme indisponible ; ne pas « réparer » ce cas par une lecture invitée ou une libération concurrente hasardeuse.
6. Avant tout jeu : tester l'initialisation et l'arrêt, l'adaptateur de temps monotone et ses échecs, session/génération étrangères, non-écrasement et limites en octets ; compiler un probe séparé, au plus deux jobs. La validation réelle devra rester passive et coordonnée.

L'option inline évite une refonte de l'ABI ou de l'architecture, mais n'est pas encore une intégration validée. Si un instant exact au producteur ou une preuve sémantique vivant/mort est exigé, ce plan ne satisfait pas cette exigence : il faut un signal supplémentaire démontré, sans élargir les lectures invitées par supposition.

Aucun jeu lancé, sauvegarde consultée, téléchargement, commit ou push. Les seuls ajouts de cette étape sont le banc d'essai, cette revue et ses sorties locales.
