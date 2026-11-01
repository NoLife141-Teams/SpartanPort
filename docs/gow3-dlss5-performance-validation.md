# DLSS 5 : validation des douze étapes, 31 octobre 2026

Configuration testée : RTX 5070 Ti, rendu interne 3840×2160, MSAA x2,
écran 2560×1440, SDR, VSync, limite 60 FPS et Skip Intro. Les essais utilisent
des copies de Gaia et The Caverns. Le paquet personnel conserve aussi Daedalus.

## Changements et contrôles

| Étape | Résultat et preuve |
| --- | --- |
| 1. Préserver la référence | Les 40 fichiers initialement modifiés, leur patch et l'ancien exécutable sont archivés. Les 12 fichiers de sauvegarde du paquet sont contrôlés par SHA-256. |
| 2. Mesurer la référence | Trois relevés ETW distincts : NR ON, F7 OFF et NR absent au démarrage, avec la même copie de The Caverns. Les mesures d'affichage utilisent `msBetweenDisplayChange`. |
| 3. Indépendance du cache | Reconnaissance par identité originale des shaders et ressources vérifiées, sans numéro de cache. Jeu réel validé avec cache vide, cache Gaia recompilé (1 865 fichiers copiés et vérifiés) et cache importé (93 811 fichiers vérifiés). |
| 4. Historique caméra | Tests des mouvements continus, rotation 180°, rupture de projection, changement de profondeur et pause >250 ms, dont 96 fixtures de reprojection. Chargement, pause/reprise et caméra mobile Gaia vérifiés en jeu. |
| 5. Passes GPU regroupées | Préparation + encodage, puis décodage + expansion regroupés. Deux intermédiaires supprimés en jeu normal : 132 710 400 octets, soit 126,56 Mio en 4K. Comparaison GPU sur une même image détaillée ci-dessous. |
| 6. Travail CPU réduit | Provenance détaillée demandée seulement pour les deux entrées de la composition reconnue. Les UID de ressource sont revérifiés. Tests de provenance et captures de guides réels passent. Aucun gain CPU isolé en millisecondes n'est revendiqué. |
| 7. NR désactivé | Les guides de mouvement réservés à NR sont suspendus. Les diagnostics explicites restent disponibles. Tests de sélection des shaders OFF/ON et bascules répétées F7 en jeu passent ; la réactivation reconstruit l'historique. |
| 8. Intensité 0 % | État « NR NEUTRE », image MSAA originale et aucune évaluation NVIDIA. Arrêt du compteur GPU puis reprise avec intensité non nulle observés. Les tests empêchent une ancienne completion de réafficher « actif ». |
| 9. Démarrage et présentation | Pipelines hôtes préparés pendant le démarrage. Effacement noir supprimé seulement si le blit couvre toute l'image. Fenêtre avec bandes noires, redimensionnement, Alt-Tab et retour plein écran validés visuellement. Les attentes de rendu/copie de la correction de cadence sont conservées. |
| 10. Graphique et badge | Graphique facultatif de 240 intervalles CPU de présentation, désactivé par défaut ; persistance testée. Badge masqué avec NR toujours actif, y compris dans le paquet Windows final. |
| 11. Jeu réel | Sessions prolongées The Caverns et Gaia, ennemis attaquant, réactions du personnage, saut, végétation, caméra et menus observés. Quatre captures GPU Gaia consécutives avec historique conservé, profondeur et mouvements finis. Aucun crash constaté dans ces essais. |
| 12. Livraison | Compilation native, validation CPU complète, fixtures Vulkan, suite Rust complète (1 175 tests), 17 tests ciblés i18n après correction et Clippy avec avertissements interdits passent. Lanceur Rust release reconstruit. Paquet personnel distinct lancé avec PATH limité à Windows, vraie sauvegarde The Caverns chargée et NR actif. Revue indépendante et publication restent suivies dans la PR. |

## Mesure du coût GPU

Sessions séparées du profilage ETW, sans captures brutes. Chaque valeur est la
médiane des blocs du profiler exécutant exactement une évaluation NR par image
(11 blocs référence, 2 blocs optimisés), dans The Caverns au même point de sauvegarde.

| Passe | Référence | Optimisé |
| --- | ---: | ---: |
| Préparation couleur/profondeur/mouvements | 0,298 ms | 0,4195 ms, encodage inclus |
| Encodage RGB | 0,217 ms | inclus |
| Décodage RGB | 0,230 ms | inclus |
| Expansion MSAA | 0,215 ms | 0,2955 ms, décodage inclus |
| Total guides et conversions | **0,960 ms** | **0,715 ms** |
| Évaluation NVIDIA | 10,044 ms | 9,9185 ms |

Le gain mesuré sur les guides/conversions est de 0,245 ms, soit environ 25,5 %.
Le modèle NVIDIA n'a pas été modifié ; sa petite variation de temps ne prouve
pas une accélération du modèle. Ces mesures locales ne promettent pas 25 % de
FPS supplémentaires : le jeu reste limité à 60 et le traitement NVIDIA domine.

## Pixels, profondeur et mouvements réels

Le mode de capture conserve les anciens intermédiaires pour exécuter aussi
l'ancienne préparation/encodage sur la **même** source MSAA. Les fichiers
`reference-input.rgba32f` peuvent ainsi être comparés à `input.rgba32f` sans
confondre mouvement du jeu et différence de conversion. Ce surcoût est absent
du jeu normal. `SPARTAN_GOW3_NR_FUSED=0` conserve le chemin ancien de diagnostic.

Sur quatre images Gaia 4K après correction du writer : 709–800 composantes
différentes sur 33 177 600 par image, toutes finies, écart absolu maximal
0,00048828125 et écart moyen inférieur à 3,3e-9. Les résultats ne sont pas
identiques bit pour bit ; ce sont des différences rares de quantification
half-float. Aucune dégradation visuelle significative n'a été démontrée.
Cette comparaison porte sur l'entrée couleur, pas sur une identité bit à bit
de toute la sortie temporelle NVIDIA.

Les quatre captures sont GPU-complètes, avant HUD, MSAA x2, sans jitter et
sans reset entre images. Les mouvements caméra calculés indépendamment sur
CPU diffèrent de moins de 0,00066 pixel ; aucun écart de soumission du fallback
caméra hors arrondi half-float n'est trouvé. Une paire donne 2 560 correspondances
indépendantes à pleine résolution (médiane 0,188 pixel, p90 0,622 pixel).

Les déplacements d'environ 40 pixels dépassent la recherche ±20 pixels de
l'analyseur initial sur les deux autres paires. Une analyse complémentaire
des couleurs moyennées à un quart de la taille, sans utiliser les vecteurs
pour choisir les correspondances, porte la recherche à ±80 pixels natifs :

| Paire | Correspondances | Médiane soumise, pixels 4K | p90 | Médiane caméra seule |
| --- | ---: | ---: | ---: | ---: |
| 0 → 1 | 1 072 | 0,579 | 1,531 | 1,065 |
| 1 → 2 | 1 152 | 0,476 | 1,213 | 0,985 |
| 2 → 3 | 1 083 | 0,557 | 1,368 | 0,835 |

Ce sont des correspondances clairsemées ; le moyennage peut mélanger des
contours. Elles complètent les contrôles de profondeur/caméra à pleine
résolution et ne certifient pas chaque pixel de végétation ou chaque chapitre.

## Cadence d'affichage et limites

Relevés de 30 s ; 120 premières et 60 dernières images exclues, puis aucun
retrait sélectif des valeurs extrêmes. Un seul relevé par condition : ce n'est
pas une étude statistique de toutes les scènes.

| Condition | Moyenne | p99 | Intervalles >25 ms |
| --- | ---: | ---: | ---: |
| Référence ON | 17,06 ms | 36,39 ms | 3,60 % |
| Référence F7 OFF | 16,79 ms | 24,23 ms | 0,13 % |
| Référence sans NR au démarrage | 17,60 ms | 20,48 ms | 0,59 % |
| Optimisé ON | 17,14 ms | 19,34 ms | 0,76 % |
| Optimisé F7 OFF | 16,98 ms | 20,49 ms | 0,63 % |
| Optimisé ON, badge caché après OFF/ON | 16,92 ms | 26,58 ms | 1,63 % |

Des pauses isolées de plusieurs centaines de millisecondes subsistent, même
sans NR au démarrage. Leur cause n'est pas établie par ces relevés ; toutes
les saccades ne sont donc pas déclarées corrigées. Le graphique en jeu mesure
les intervalles CPU de présentation, pas le balayage physique de l'écran.

La création des images et de la feature NGX reste différée jusqu'à la première
scène vérifiée. Le préchauffage ne supprime donc pas toute première utilisation.
La détection des ruptures caméra est conservatrice ; les tests ne couvrent
pas tous les changements de caméra, HDR, ni un parcours complet du jeu.

## Traçabilité locale

Les preuves volumineuses restent hors Git sous `out/verification/performance-12/` :
`progress.json`, mesures CSV/JSON, journaux des tests, captures d'écran et scripts
de préparation/analyse. Les captures GPU sont dans
`out/gow3-sessions/20261101T002731175715Z/nr-capture/` (`analysis.json`,
`fusion-comparison.json`, `pyramid-motion.json`). L'analyse initiale reste
reproductible avec `tools/analyze_gow3_nr_scene.py` et l'environnement Python
local `out/pyenv/Scripts/python.exe`.

Les commits séparent l'état initial conservé (`bbcbd44`), les optimisations
(`9e96994`) et les corrections de revue (`8997b69`). La revue a corrigé les
écritures synchrones de captures qui perturbaient leur propre historique,
ainsi que le libellé chinois limité à 1080p. Le worker de captures est optionnel,
borné et vidé avant la destruction Vulkan ; il n'existe pas en jeu normal.
