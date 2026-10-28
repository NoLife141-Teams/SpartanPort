# Prototype borné de collecte passive de santé

Le cycle autonome est implémenté et testé dans `experiments/gow3_automation/health_collection_trial.h` et `test_health_collection.cpp`. Il utilise le générateur inline et l'adaptateur d'horloge des essais précédents, sans modifier ni inclure ces nouvelles structures dans le runtime de production. Toutes les lectures de santé des tests concernent des tableaux d'acteurs factices.

## Stockage et identité

Un stockage statique `constinit`, trivialement destructible, conserve quatre boîtes pendant toute la durée du processus. Sa taille vérifiée est **832 octets**. Aucune API de libération, remise à zéro, déplacement ou réutilisation n'est fournie. Une collecte échouant après réservation consomme définitivement son emplacement ; après quatre réservations, les suivantes sont refusées. Cette limite volontaire évite un nombre croissant d'allocations conservées.

L'initialisation explicite appartient au contrôleur hôte unique, avant exposition du code aux threads invités. Elle génère un UUID v4 de processus avec `BCryptGenRandom`, puis enregistre le PID et l'heure Windows de création du processus. Chaque collecte reçoit un autre UUID et un numéro local transporté dans le paquet du hook. Les UUID du processus et de la collecte figurent dans l'en-tête du journal. Les doublons connus dans le même processus sont refusés ; l'unicité entre processus repose sur les UUID aléatoires cryptographiques, pas sur le PID seul.

L'en-tête Windows et la bibliothèque d'importation bcrypt étaient déjà disponibles dans le toolchain. Aucun outil ni composant système n'a été installé. Un échec d'identité ne déclenche aucun identifiant de remplacement plus faible. Un échec d'horloge initiale, de création exclusive ou d'écriture de l'en-tête empêche d'obtenir l'entrée de la collecte. Le mode désactivé est le comportement par défaut : pas d'initialisation d'identité, de requête d'horloge, de réservation de boîte ni de journal.

L'horloge et le fichier appartiennent au consommateur ; le stub ne contient pas de pointeur vers ces objets. Après destruction du contrôleur, les producteurs retardés peuvent encore accéder à la boîte statique. Cela ne rouvre pas le fichier et ne réutilise pas l'ancienne session.

## Journal borné

Le fichier est créé par `CreateFileW(..., CREATE_NEW, ...)`, sans écrasement ni append d'un ancien fichier. `WriteFile` écrit les octets ASCII/UTF-8 sans traduction CRLF. La limite maximale du prototype est **16 Kio par collecte**, avec **512 octets réservés au bilan de fermeture**. Le consommateur utilise des tampons de formatage fixes ; le producteur ne fait ni allocation ni I/O. Aucun verrou de boîte n'est détenu pendant l'écriture ou le flush du fichier.

Le journal contient identité, fréquence QPC, génération diagnostique, numéro de publication, bornes L/U, validité temporelle, correspondance de génération, marqueur joueur/ennemi déjà copié, bits exacts de santé/maximum et compteurs de pertes. Il n'enregistre aucun pointeur d'acteur, chaîne invitée, sauvegarde, image ou shader. Les flottants sont conservés en bits hexadécimaux pour éviter une sérialisation JSON invalide de NaN et toute dépendance à la locale.

Les enregistrements dépassant le budget sont comptés puis refusés ; les anciens enregistrements restent intacts. Une écriture partielle invalide le journal et interdit de lui ajouter un bilan prétendument complet. Le bilan normal indique toujours `producer_totals_terminal:false` : les compteurs ne sont que des instantanés, car le producteur peut continuer après l'arrêt de la collecte. Un fichier sans bilan ou avec une dernière ligne partielle ne constitue pas une collecte complète.

L'en-tête porte `health_is_informational:true` et `proves_alive:false`. Un changement de génération indique `proves_respawn:false`. Il n'existe aucune décision Alive/Dead/Playing ni aucune API de commande. Une faute d'horloge reste mémorisée, les anciennes générations ne sont pas renommées, et les pertes connues restent signalées jusqu'à la fermeture.

## Résultats exécutés

Compilation Windows avec Clang 23.1.3 existant, C++23, assertions actives, warnings traités comme erreurs, `-pthread -lbcrypt`, **un job**. Quatre exécutions finales réussies :

1. Mode désactivé : aucune identité initialisée, horloge appelée, entrée exposée, réservation ou fichier créé.
2. Échec d'entropie injecté : échec persistant sans fallback, aucune entrée ni fichier.
3. et 4. Deux processus indépendants réalisant chacun le cycle complet et produisant des UUID de processus distincts.

Chaque cycle complet vérifie :

* publication d'un ancien paquet après changement de génération, conservé avec `generation_matches:false` ;
* marqueurs joueur et ennemi distincts, sans adresse d'acteur dans le journal ;
* pertes de boîte pleine conservées et signalées ;
* deux véritables threads C++ détachés, possédant chacun leurs slots historiques, qui exécutent **2 000 appels après destruction du contrôleur** ;
* **2 006 tentatives = 5 publications producteur + 2 001 pertes** à la fin du test ; quatre paquets ont été journalisés avant l'arrêt, le cinquième reste en attente après l'arrêt ;
* ancienne boîte toujours valide et identique, nouvelle session dans une autre boîte, journal fermé inchangé ;
* régression d'horloge après démarrage, deux paquets explicitement invalides, absence de récupération spontanée ;
* fichier préexistant conservé octet pour octet, capacité de boîtes épuisée sans nouveau fichier ;
* budget de données rempli exactement, enregistrement suivant refusé, place de bilan préservée ;
* erreur d'écriture partielle simulée avec sept octets réellement écrits, sans bilan de succès ajouté.

Une vérification indépendante des JSONL a validé huit fichiers complets, deux UUID de processus et six UUID de collecte distincts. Elle a rejeté les deux fichiers volontairement tronqués, confirmé les compteurs et l'absence de CR ajouté. Les premiers journaux santé mesurent **1 620 octets** pour un plafond de 4 096 ; les fichiers du test de saturation mesurent **1 741 octets** pour un plafond de 2 048, leur budget de données étant entièrement utilisé et le reste réservé au bilan. Les dossiers des modes désactivé et échec d'identité sont vides.

Preuves : `out/dlaa-health-collection/result.json`, `journal-audit.json`, les quatre journaux d'exécution et les sous-dossiers `normal-final-a` / `normal-final-b`. Les résultats intermédiaires sont conservés séparément.

## Limites avant intégration

Il s'agit d'un prototype autonome. Le hook de production, son installation et les commandes du jeu restent inchangés. L'intégration devra choisir explicitement un seul thread propriétaire pour l'initialisation, la réservation, le drain, les changements de génération et la fermeture. Les API du contrôleur et du journal ne sont pas conçues pour des appels concurrents entre plusieurs consommateurs. Seuls les producteurs de paquets sont concurrents.

L'initialisation avant les threads est une condition du point d'appel futur. Le banc la respecte, mais aucun raccordement au démarrage du runtime n'a été effectué. Le code réel des stubs devra, comme la boîte, rester valide jusqu'à la fin du processus. Le banc conserve son code synthétique jusqu'au dernier appel par des références propres aux producteurs ; le stockage de santé, lui, ne dépend d'aucune de ces références.

Un arrêt de collecte ne désactive pas un stub déjà exposé et ne constitue pas une jonction des threads. Aucun hot-unpatch, retrait de code, changement de sécurité ou nouvelle architecture de threads n'a été introduit. Les quatre emplacements suffisent au prototype borné ; un usage nécessitant davantage de collectes devra faire un choix explicite plutôt que recycler une boîte encore référencée.

Un arrêt brutal du processus, une panne disque ou une coupure peuvent empêcher le dernier flush ou bilan. Le fichier n'offre pas une transaction contre les coupures d'alimentation. Enfin, la cohérence d'un paquet ne rend pas les deux champs du moteur atomiques et ne prouve ni santé actuelle, ni vie, ni mort, ni chargement terminé.

Aucun jeu lancé, fichier de jeu ou sauvegarde lu, installation, modification de sécurité, commit ou push pendant cette étape.
