# DLSS 5 NR : guides réels et HUD protégé en 1080p

Le 28 octobre 2026, le chemin expérimental NR a été raccordé à la profondeur
matérielle et aux mouvements caméra + géométrie du jeu. Deux chargements réels
de la sauvegarde Gaia ont produit des évaluations GPU avec historique conservé,
en 1920×1080 natif plein écran. Le HUD est composé par le jeu après NR.
Les instances répétées de végétation/Gaia ont depuis été distinguées par leur
transformation et testées avec de nouvelles captures. Le profil `--nr-scene`
conserve désormais MSAA x2, à la demande de l'utilisateur. Voir
[le correctif et le bilan visuel](gow3-nr-gaia-vegetation-1080p.md).

Sur la branche `codex/gow3-dlss5`, le mode DLAA natif GoW3, F10/Maj+F10,
le curseur de netteté et les anciens profils de lancement ont été retirés.
Le hook dédié `vk_gow3_nr.cpp` conserve les guides, le pré-HUD et F7.
Les rapports DLAA antérieurs sont conservés comme historique.

Le premier lancement après le nettoyage n'avait pas atteint Gaia : Windows
avait refusé la réservation de 8 128 MiB de mémoire invitée (erreur 1450,
session `20261029T004009313974Z`). Les essais d'intégration du 29 octobre
ci-dessous ont depuis atteint Gaia et validé le hook NR renommé.

## Intégration de l'amont du 29 octobre 2026

`NoLife141-Teams/SpartanPort:main` est avancé sans réécriture vers l'amont
`f593377bbaf1934bf606171973d7a58210510991`. La branche
`codex/integrate-upstream-dlss5` fusionne cet amont avec `codex/gow3-dlss5`
au commit `2898c88`. La branche DLSS 5 initiale reste à `d8e6b94`.
Le profil conservé est DLSS 5 NR + MSAA x2, sans réintroduction du mode DLAA natif.

Les trois conflits concernent les membres du rasterizer, la nouvelle séparation
`DrawWindows`/`Render` de l'overlay et la capture clavier. Les résolutions gardent
les diagnostics et F7, ajoutent la règle de flou de mouvement de l'amont et
réinitialisent aussi les requêtes de changement d'arme quand l'overlay capture
les entrées. Les bancs autonomes reçoivent les services runtime manquants ; le
test des hooks d'arme utilise les allocations et l'ABI adéquates sous Windows.

Validation initiale du code fusionné (avant les corrections décrites ci-dessous) :

- Compilation du probe et des bancs : réussie, Clang/MSYS2, deux jobs.
- 62 contrôles GoW3 CPU/GPU, 19 vérifications natives/SPIR-V et 11 contrôles
  du contrat ABI de la passerelle de diagnostic NR : réussis.
- Lanceur Rust : 765 tests de bibliothèque et 407 tests dans 21 suites
  d'intégration réussis ; `cargo fmt --check` et Clippy avec `-D warnings` passent.
  Toolchain locale Rust 1.99.1/MSVC ; pas de paquet de distribution créé.
- Suite Python : 858 cas, 99 ignorés. Après utilisation du probe fraîchement
  compilé avec ses DLL dans le PATH, 753 réussissent ; cinq échecs et une erreur
  restent et sont reproduits sur une copie intacte de `f593377`. Ils concernent
  l'ancien lanceur Python : titre/icône Windows, traductions chinoise et
  indonésienne, et bit exécutable AppImage sous Windows. La table de parité
  est complétée avec les tests Rust existants et sept lacunes explicites.

La session réelle `out/gow3-sessions/20261029T122519893526Z` utilise le probe
fusionné, la copie d'Auto Save Gaia 0:04:35, le silence, la vie infinie et Skip
Intro. La scène reste en 1920×1080 natif, MSAA x2. F7 ON/OFF, le badge après
achèvement GPU et la pause/reprise par les trophées F8 sont observés ; au moins
9 000 évaluations NR se terminent. Huit frames brutes conservent profondeur et
mouvements, sans reset pendant les captures, avec 99,927–99,945 % des surfaces
couvertes par l'historique géométrique. L'erreur médiane sur 4 714 correspondances
indépendantes est de 0,171 pixel ; pour les 338 points d'instances affines,
0,330 pixel. La reprojection caméra CPU/GPU diffère au maximum de 0,000319 pixel.
Les mouvements de fallback ne présentent aucune discordance RG16.

Les 2 400 pixels opaques contrôlés des barres de vie/magie sont identiques sur
les captures ON/OFF de frames différentes. Le jeu sort avec le code 0 et les
six empreintes des sauvegardes source, backup et runtime restent inchangées.
Ces contrôles ne certifient pas tout le jeu, tous les réglages graphiques ou les
zones translucides du HUD. L'ouverture par Insert n'a pas été qualifiée avec
l'injection clavier de l'outil ; un contrôle au clavier réel reste à faire.

Les journaux et `report.json` sont dans
`out/verification/integration-20261029/`. Les contrôles GoW3 sont détaillés dans
`out/gow3-validation/20261029T121526371819Z/result.json` ; les captures et leur
analyse restent dans la session, hors Git. Les changements préexistants du
sous-module FSR sont exclus des commits.

## Lanceur et badge persistants

Le lanceur Rust propose maintenant **Graphismes > DLSS 5 + MSAA x2 (1080p)**.
Cette option, enregistrée sous `dlss5_nr` dans `settings.json`, démarre NR actif.
Elle sélectionne le rendu natif 1920×1080, MSAA x2 et SDR. Choisir ensuite une
autre résolution, HDR ou désactiver MSAA dans le lanceur désélectionne ce profil.
Le runner refuse un profil incompatible ou des DLL absentes avant de lancer le
jeu. Le mode DLAA natif GoW3 reste retiré.

- **F7** active/désactive NR pour la session ; MSAA x2 reste actif.
- **Maj+F7** affiche/masque uniquement le badge. Sa visibilité est enregistrée
  dans `spartanport.ini` (`show_nr_badge=1` par défaut).
- La même préférence existe dans **Graphismes > Afficher le badge DLSS 5** du
  lanceur et dans l'onglet Overlay du menu en jeu.
- Le choix de démarrage DLSS 5 reste indépendant du badge et des changements
  temporaires effectués avec F7.

La passerelle `spartanport_dlssnr.dll` et le runtime NVIDIA `nvngx_dlssnr.dll`
doivent être placés à côté de **spartanport-probe.exe** : `out/` dans ce checkout,
`bin/` dans un paquet Windows. Ils ne sont pas ajoutés à Git. Le lancement normal
ne crée pas de captures brutes NR et ne demande pas le script de diagnostic.

La session `out/gow3-sessions/20261029T131628753487Z` a été lancée depuis le bouton
Jouer du lanceur Rust, avec une copie de la sauvegarde Gaia 0:04:35, sans son,
vie infinie et **Skip Intro enregistré et confirmé dans les patches appliqués**.
Le compteur GPU passe de 2 760 évaluations avant masquage à 3 600 avec le badge
masqué, sans reset provoqué par le masquage. F7 coupe réellement NR alors que le
badge reste caché (`active=0`), puis le réactive : 6 360 évaluations sont atteintes
avant réaffichage du badge actif. Le HUD reste composé après NR. Après sortie,
les six empreintes des sauvegardes source, backup et runtime sont inchangées.
Les preuves de cet essai sont dans `out/verification/nr-launcher-20261029/`.

Les six problèmes Python hérités sont corrigés : isolation du mock de l'icône,
capture GDI avec handles 64 bits et mesure du fond de la barre de titre plutôt
qu'un pixel de texte, traductions chinoises/indonésiennes manquantes, et assertion
du bit exécutable AppImage limitée aux systèmes POSIX. La régénération des tables
Rust conserve aussi les traductions audio/effets déjà présentes. Les références
visuelles de la page Graphismes intègrent les deux nouveaux contrôles.

Validation finale du 29 octobre : **1 175 tests Rust réussis** (766 bibliothèque,
409 intégration), dont les comparaisons visuelles à deux tailles et trois langues ;
`cargo fmt --check` et Clippy `-D warnings` passent. La suite Python termine ses
858 cas sans échec ni erreur (99 ignorés). Les 62 contrôles GoW3 CPU/GPU et le
banc natif de persistance graphique passent également. Après fermeture et
réouverture du lanceur, DLSS 5 reste sélectionné et le badge masqué ; sa visibilité
a ensuite été réactivée et enregistrée pour les prochains essais.

Ces résultats valident ce profil et la sauvegarde testée, pas tous les chapitres,
HDR ou les résolutions supérieures. Aucun paquet de distribution n'est publié.

## Provenance vérifiée

Le [manifeste officiel RTX Remix](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/packman-external.xml)
déclare `rtx-remix-ngx_sdk_dlnr` version 2. Son
[transport public](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/scripts-common/packman/external_config.packman.xml)
fournit cette [archive NVIDIA](https://d1oqksizpb0e3d.cloudfront.net/rtx-remix-ngx_sdk_dlnr@2.7z).
Elle contient les définitions NR, les helpers Vulkan et le runtime x64.
L'absence de NR dans les SDK publics DLSS/Streamline inspectés précédemment
ne signifiait donc pas qu'aucun paquet NVIDIA public n'était disponible.

| Élément | Vérification locale |
| --- | --- |
| Archive `rtx-remix-ngx_sdk_dlnr@2.7z` | 367 851 723 octets |
| SHA-256 archive | `4bb8651b3ac2180f23df1f39422c489810a13d445a94dc836c876c18c93ecefe` |
| `nvngx_dlssnr.dll` x64 | 310.9.16.0, 266 424 944 octets |
| SHA-256 runtime | `2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f` |
| Authenticode | signature valide, NVIDIA Corporation |
| Dossier extrait | `out/dependencies/nvidia-remix-dlssnr-2/` |

La licence et `provenance.json` sont conservés avec les fichiers dans `out/`.
Aucun en-tête NVIDIA, runtime ou modèle n'est ajouté aux sources Git.
Une copie communautaire 310.8.0 signée a également été identifiée, mais le
chemin final utilise le runtime 310.9.16 du paquet NVIDIA officiel.

Le paquet NR complète le SDK DLSS public 310.9.16 déjà utilisé pour sa
bibliothèque statique NGX. Il n'inclut pas lui-même cette bibliothèque.

## Chemin de rendu guidé

`--nr-scene` active le hook de composition déjà identifié pour DLAA. Il exige
les shaders et bindings vérifiés, une profondeur/couleur de même provenance,
une texture de mouvements correspondante et une caméra cohérente. La source
est la scène LDR RGBA8 sRGB avant la composition finale du jeu, son étalonnage
et son HUD. Ce n'est pas le swapchain final, ni une nouvelle passe de tone mapping.

1. Résoudre la couleur et lire la profondeur normale, proche=0, loin/clear=1.
2. Préparer les mouvements courants vers précédents en pixels. Utiliser
   l'historique géométrique lorsque son depth correspond à la surface courante
   (écart maximal 2e-7), sinon la reprojection caméra.
3. Restaurer l'encodage sRGB décodé par la vue source, vers RGBA32F, puis
   transmettre couleur, profondeur R32F et mouvements RG16F au helper NR officiel.
4. Décoder le résultat en RGB linéaire pour la composition originale du jeu ;
   conserver explicitement son alpha d'origine. Le HUD et les textes arrivent après.

NR reçoit une entrée/sortie 1:1 sans jitter. Réglages : intensité 1, tone local 1,
structure locale 1, peau/style 0, auto-mask actif ; GlobalTone réservé reste 0.
L'historique est conservé entre évaluations. Il est réinitialisé à l'activation,
si la caméra/source devient invalide, si une scène manque à un flip ou si le
backing de profondeur change. Une coupure caméra restant formellement valide
n'a pas encore de détecteur spécifique.

Le mode actuel exige 1920×1080 natif et conserve MSAA x2. Les premiers essais
ci-dessous étaient sans MSAA. SR/DLAA restent exclusifs de NR.
Une erreur conserve le rendu original et désactive le traitement. Les ressources
et la feature sont libérées après la fin des commandes GPU ; NGX est arrêté
avant la destruction du device Vulkan.

**F7** active/désactive NR. La session commence OFF. Le badge `NR ACTIF`
n'apparaît qu'après une évaluation terminée sur GPU et précise
`profondeur + mouvements | HUD après NR`. F10 et les réglages DLAA ne sont plus proposés dans le jeu.
Le mode historique `--nr-preview` reste disponible séparément : il traite
l'image finale, HUD compris, sans guides et avec reset à chaque image.
Ce mode historique n'est pas celui utilisé pour les validations ci-dessous.

## Premières captures, avant correction des instances répétées

Sauvegarde : Auto Save Gaia, God (Normal), 00:04:35, 26 octobre 2026 à 21:45.
Sélection observée : S vers Load, Espace, puis Entrée sur la sauvegarde copiée.
Sessions isolées : son coupé, santé infinie, skip intro, 1080p plein écran,
MSAA/SR/DLAA désactivés, runtime NR officiel 310.9.16.0 sur RTX 5070 Ti.

- `out/gow3-sessions/20261028T183222397683Z/` : scènes 3353–3356,
  évaluations 25–28, puis plus de 11 640 évaluations GPU. OFF/ON vérifié :
  réactivation avec reset=1, puis retour à reset=0.
- `out/gow3-sessions/20261028T184409766507Z/` : scènes 318–321,
  évaluations 25–28, pendant une phase plus précoce du mouvement de Gaia ;
  plus de 3 000 évaluations terminées sans reset répété.

Chaque session conserve quatre frames consécutives, chacune avec couleur
avant/après NR, profondeur, mouvements et diagnostic de provenance. Les cinq
readbacks d'une frame sont copiés dans la même commande GPU et écrits après
achèvement. Captures bornées à quatre frames (environ 443 MiB/session).
Les JSON conservent tick, scène, profondeur UID, matrices et reset.
Les PNG sont dérivés de ces pixels, sans génération ni retouche.

`tools/analyze_gow3_nr_scene.py` vérifie taille, finitude, SHA-256, profondeur,
provenance géométrique, continuité des scènes et historique. Il recalcule la
reprojection caméra sur CPU double précision. Le stockage RG16F peut arrondir
vers zéro : le comparateur accepte les représentations adjacentes correspondant
à cet arrondi ou à nearest-even, sans tolérance arbitraire.

Les correspondances de pixels utilisent uniquement la couleur d'entrée,
avec recherche bidirectionnelle indépendante des vecteurs prédits. Le masque
géométrique sert ensuite à établir les cohortes ; il comprend Gaia, le décor
et les personnages, sans constituer un masque d'identité des personnages.
La recherche locale ±20 pixels ne mesure pas tous les déplacements/occlusions.

| Mesure | Première session | Seconde session |
| --- | --- | --- |
| Frames appariées / paires consécutives | 4 / 3 | 4 / 3 |
| Points couleur par paire | 768–830 | 695–731 |
| Erreur médiane des mouvements soumis | 0,168–0,192 px | 0,127–0,146 px |
| Erreur médiane avec caméra seule | 0,320–0,504 px | 8,072–8,555 px |
| Médiane sur géométrie avec historique | 0,157–0,177 px | 0,118–0,135 px |
| Médiane sur fallback caméra | 0,982–1,186 px | 11,409–12,704 px |
| P90 sur fallback caméra | 3,777–4,022 px | 14,816–19,378 px |
| Écart maximal caméra CPU / GPU | 0,000312 px | 0,000337 px |
| Mismatch fallback / représentation RG16F | 0 | 0 |
| Mouvement maximal absolu capturé | 3,385 px | 26,016 px |
| Entrée/sortie/profondeur/mouvements finis | oui | oui |
| Reset pendant les quatre captures | aucun | aucun |

Les témoins signe inversé et mouvement nul figurent aussi dans `analysis.json`.
L'amélioration géométrique de la seconde séquence montre pourquoi le mouvement
caméra seul ne suffit pas sur Gaia. En revanche, les pixels sans historique
restent mal expliqués : ne pas interpréter la bonne médiane globale comme une
validation de la végétation, des transparences ou de toutes les déformations.
Ces différences de flux ne prouvent pas à elles seules un gain de qualité NR.

### Protection du HUD

La couleur soumise à NR ne contient ni barres de vie/magie, ni compteur d'orbes,
ni instruction Magic. La composition originale les dessine ensuite.
Dans la première session, les captures F12 ON et OFF sont séparées de 13 secondes :
ce ne sont pas deux frames de monde identiques. Les zones opaques des barres
vie (1 120 pixels) et magie (1 280 pixels) sont néanmoins **bit à bit identiques**,
écart maximal zéro. Les parties translucides voient naturellement le fond changer.
Cela ne certifie pas l'invariance de chaque pixel du HUD sur toutes les scènes.
Preuve : `nr-hud-verification.json` et captures originales dans
`runtime/user/screenshots/` de la première session.

Les six fichiers de sauvegarde source, backup et runtime sont inchangés dans
les deux sessions (`nr-save-verification.json`). Aucun enregistrement de partie
n'a été demandé. La passerelle SR/DLAA garde le SHA-256
`87b0a2dfd891001865d7298ac670b318e46101edda990fc05305b5cc09142772`.

Compilation jeu + shaders réussie. Les 62 contrôles CPU/GPU passent dans
`out/gow3-validation/20261028T183801129447Z/result.json`.
Les 11 tests du contrat de passerelle NR passent également.
Le badge 60 FPS observé n'est pas un benchmark du coût GPU de NR.

## Reproduire

CMake/MSVC, SDK DLSS de base, headers Vulkan et SDK NR sont sous `out/`.
Utiliser des chemins absolus pour les racines CMake :

```powershell
cmake -S gpu/dlss_bridge -B out/verification/dlssnr-remix2 -G "Visual Studio 17 2022" -A x64 -DDLSS_SDK_ROOT=C:/Users/jerem/SynologyDrive/code/SpartanPort/out/dependencies/nvidia-dlss -DDLSS_NR_SDK_ROOT=C:/Users/jerem/SynologyDrive/code/SpartanPort/out/dependencies/nvidia-remix-dlssnr-2 -DVULKAN_INCLUDE=C:/Users/jerem/SynologyDrive/code/SpartanPort/out/verification/dlss-vkinclude -DSPARTAN_ENABLE_DLSSNR=ON
cmake --build out/verification/dlssnr-remix2 --config Release --target spartanport_dlssnr
ninja -j2 -C out/verification/dot-build spartanport-probe
out/pyenv/Scripts/python.exe -B scripts/gow3_diagnostic_session.py --seed-data C:/Users/jerem/Documents/Codex/2026-10-26/task/spartanport-dlaa-msvc/out/gow3-input-collection-f586605 --game-dir C:/Games/God.of.War.III.Remastered.RECOMP.v2.1.5-KaOs/CUSA01623 --nr-scene --resolution native --fullscreen --launch --wait
```

Charger Gaia puis F7. `--nr-scene` refuse la combinaison avec `--nr-preview`,
`--dlaa`, `--aa`, `--pixels`, `--motion`, `--display-capture` et les résolutions
supérieures. Le lanceur copie module/runtime/licence et les sauvegardes dans une
nouvelle session, sans remplacer les sauvegardes source.

Pour analyser les captures avec Python + NumPy + Pillow :

```powershell
python -B tools/analyze_gow3_nr_scene.py out/gow3-sessions/20261028T184409766507Z/nr-capture
```

Les sorties `input.png`, `output.png`, `diagnostic.png` et `analysis.json`
complètent les données brutes ; seul le rapport dérivé est régénéré.

## Limites et prochaine étape

Le raccordement des guides et la séparation du HUD fonctionnent dans les
séquences testées. Le traitement reste expérimental et désactivé par défaut.
Le correctif des maillages répétés et l'essai MSAA x2 sont détaillés dans
[le rapport de végétation/Gaia](gow3-nr-gaia-vegetation-1080p.md).
Il reste les cas ambigus, les désocclusions, coupures caméra, autres chapitres, HDR/résolutions supérieures,
stabilité temporelle plus longue et coût GPU dédié. Les huit frames capturées
ne certifient pas tout le jeu. La coexistence SR/DLAA + NR demanderait un contexte
NGX partagé et un ordre de passes validé ; elle reste exclue ici.
