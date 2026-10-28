# Intégration locale de santé passive — Windows

Cette étape raccorde le prototype de santé au probe expérimental local. Le nouveau probe compile et les 15 scénarios CPU passent. **Aucun jeu n'a été lancé.** La santé reste informative ; aucune commande automatique, décision de vie/mort, modification d'entrée ou activation DLAA n'est ajoutée.

## Raccordement

`gpu/shim/spartanport_health_passive.cpp` expose une petite interface C déclarée dans `src/health_passive_bridge.h`. Le thread SDL existant appelle l'initialisation après création de sa fenêtre et avant de signaler `g_window_ready`. Le chargeur attend ce signal dans `spartanportgpu_init`, puis installe les hooks avant d'appeler les initialiseurs de modules invités. L'ordre a été vérifié dans les sources ; aucun nouveau thread consommateur n'est créé.

Le même thread SDL effectue les drains dans sa boucle et ferme le journal avant `runtime_report` et `_Exit` lors d'une fermeture normale de fenêtre. Le bridge vérifie l'identité du propriétaire. Un autre thread ne lit ni le consommateur ni le fichier : il dépose seulement un motif atomique d'arrêt, traité par le propriétaire. Les transitions Starting/Active/Stopping/Stopped empêchent une nouvelle activation à chaud.

`src/probe.c::install_actor_watch` conserve la recherche de signature unique, les vérifications de zone, les instructions originales et la portée du saut relatif. Le nouveau constructeur appelle directement le générateur historique si le diagnostic n'est pas prêt ou est désactivé. **Ce stub est identique octet pour octet à l'existant**, pour les mêmes adresses ; l'ensemble du binaire n'est évidemment pas identique. Une erreur de construction revient au stub historique et demande l'arrêt du diagnostic. Une source introuvable ou hors portée est explicitement signalée au propriétaire.

Le chemin opt-in utilise exactement le générateur inline testé : mêmes lectures invitées, temporaires RAX/RDI seulement, aucune utilisation de pile/SIMD et aucun appel natif dans le producteur. Le lecteur ne déréférence jamais la valeur d'acteur mémorisée. Le générateur historique `src/actor_hook.h` lui-même n'est pas modifié.

## Activation explicite, non exécutée dans cette étape

Les deux variables doivent appartenir au processus de test isolé :

* `SPARTAN_HEALTH_PASSIVE=1`, exactement ; absence, `0` et `true` restent désactivés ;
* `SPARTAN_HEALTH_PASSIVE_LOG`, chemin absolu sur un lecteur local vers un **nouveau** fichier.

Les chemins relatifs, UNC et de périphérique ne constituent pas une destination admise. Le journal refuse les fichiers existants et les handles qui ne sont pas des fichiers disque. Il ne crée pas les dossiers parents. Aucune variable persistante ni réglage système n'a été changé ; seules les copies d'environnement des exécutables de test ont reçu ces variables.

Le journal reste plafonné à 16 Kio, dont 512 octets réservés au bilan, sans traduction CRLF. Les identifiants de processus/collecte, la fréquence QPC et les marqueurs de validité accompagnent les observations. Les données santé/maximum sont des bits copiés ; le marqueur joueur/ennemi reprend celui du hook sans certifier une identité vivante. `health_is_informational:true` et `proves_alive:false` sont explicites.

La génération vaut actuellement 1 pour cette collecte de processus : aucun événement de chargement ou de réapparition n'est inventé. Les transitions de génération du composant sont testées synthétiquement, mais ne sont pas branchées à une commande du jeu.

## Durée de vie et arrêts

Les boîtes de 832 octets au total et le consommateur construit dans un stockage statique restent valides jusqu'à la fin du processus. Leur destructeur n'est pas enregistré pour détruire une ressource encore référencée par un producteur. Le code des stubs utilise les pages du chargeur, elles aussi conservées. L'arrêt ferme le fichier et dépublie la boîte pour les constructions suivantes ; il ne libère, ne réinitialise et ne retire aucun hook existant.

Une faute d'horloge ou d'I/O arrête la collecte sur le propriétaire. Motifs diagnostiques en masque : 1 signature, 2 portée relative, 4 génération du stub, 8 mauvais propriétaire, 16 horloge, 32 I/O. Un fichier saturé conserve son préfixe et ses compteurs de pertes. Un fichier incomplet ou un arrêt brutal ne doit jamais être présenté comme une capture complète. Les sorties directes `TerminateProcess`, `_exit`, crash ou redémarrage ne sont pas réécrites pour imposer une fermeture interthreads.

Les producteurs déjà installés peuvent donc continuer après l'arrêt logique, sans écrire dans le journal fermé. Un constructeur déjà en vol lors d'une fermeture peut encore avoir lu l'adresse publiée ; sa sûreté repose sur cette durée de vie du processus, et non sur une réutilisation immédiate de la boîte.

## Validation effectuée

Compilation Clang 23.1.3 / CMake / Ninja existants, `-j2`, dépendances déjà présentes, FetchContent désactivé pour les téléchargements. Sortie distincte : `out/dlaa-health-integration/bin/spartanport-probe.exe`. Le binaire précédent `out/dlaa-provenance-compact/bin/spartanport-probe.exe` conserve son empreinte antérieure.

Les quatre cibles CPU compilées sont `health-passive-bridge-test`, `health-inline-test`, `health-lifecycle-test` et `health-collection-test`. Les 15 scénarios exécutés sont :

* dix scénarios du bridge réel : absent, zéro, autre valeur, chemin relatif, UNC refusé avant I/O, fichier préexistant, activation, mauvais propriétaire, source indisponible, capacité de stub insuffisante ;
* le banc ABI/inline partagé ;
* le banc cycle de vie/horloge ;
* trois scénarios de collecte : désactivation, échec d'identité, cycle complet.

Le bridge actif génère les mêmes octets que le générateur éprouvé. Son code s'exécute sur des acteurs factices en vérifiant les GPR, YMM0–15, MXCSR, DF, RSP et 128 octets de zone rouge SysV. Les chemins plein/occupé, le repli avec canari de capacité et l'appel après fermeture passent. Le passage concurrent intégré produit 2 005 tentatives et 1 986 pertes ; les pertes sont attendues et comptabilisées dans cette boîte à un paquet. Le journal associé mesure 5 319 octets, sous son plafond de 16 384.

Le banc inline vérifie 6 000 tentatives concurrentes (1 776 paquets, 4 224 pertes lors du passage enregistré). Le banc de cycle de vie vérifie 4 002 appels, dont 4 000 après arrêt logique, ainsi que QPC réel à 10 MHz et 1 000 lectures non décroissantes. Le banc de collecte vérifie de véritables threads détachés après destruction du contrôleur, les UUID, les générations, l'erreur d'horloge injectée, la saturation et une écriture partielle.

Audit indépendant : framing JSONL, compteurs, plafond, `proves_alive:false`, `producer_totals_terminal:false`, empreintes des exécutables et ordre d'initialisation dans les sources. Preuves dans `out/dlaa-health-integration/final-test-results.json`, `audit.json`, `result.json` et `logs/`.

## Limites

Les fautes d'horloge et d'écriture sont simulées dans les composants partagés ; aucune panne système réelle n'est provoquée. Le test de bridge utilise le vrai code d'intégration avec un propriétaire simulant le thread de fenêtre, sans SDL ni Vulkan. La compilation complète vérifie leur raccordement au probe, mais la stabilité en jeu et le coût réel du hook ne sont pas mesurés.

Les slots historiques de l'overlay restent inchangés ; le diagnostic ne corrige pas leurs races existantes. Un paquet cohérent ne rend pas les lectures santé/maximum du moteur atomiques. Les bornes temporelles ne sont pas un instant exact de dégât. Aucune ancienne santé ne prouve une vie présente, et la capture n'identifie pas les menus, la mort ou un chargement terminé.

Toute exécution réelle reste une prochaine collecte passive à coordonner. Aucun jeu, sauvegarde, installation normale, configuration système, téléchargement, commit ou push n'a été touché par cette étape.
