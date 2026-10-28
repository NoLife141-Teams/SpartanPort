# État du travail GoW3 de Dot et intégration locale

Revue du 27 octobre 2026, heure de Toronto.

Le travail de Dot est rapatrié dans ce dépôt sur la branche
`codex/integrate-dot-gow3-diagnostics`. Il constitue une base de recherche et
d'instrumentation expérimentale. **Le DLAA natif GoW3 n'est pas fonctionnel et
le pilote autonome sans supervision n'est pas opérationnel.** Le chargement
et une collecte sous pilotage visuel de l'agent sont désormais validés; voir
[le protocole et les résultats réels](gow3-supervised-control.md).
La capture de **pixels réels couleur/profondeur MSAA x2** est maintenant
intégrée et exécutée sur Gaia : voir [les résultats et limites](gow3-real-pixels.md).

## Provenance et périmètre

- Source : `C:\Users\jerem\Documents\Codex\2026-10-26\task\spartanport-dlaa-msvc`.
- Base source : `f58660558a03e74af057f9cdfa556e31346bbba9`, HEAD détachée,
  historique superficiel. Le prototype caméra initial est déjà dans ce commit.
- Travail supplémentaire sur disque : 19 fichiers suivis modifiés et 84 nouveaux
  fichiers de sources, tests, outils et documentation. Il n'était pas commité.
- Destination : base `9dd2d120245f18a007d42b27fa4fcf41358dfc27`, auparavant `main`,
  sans changement local de fichier suivi avant cette intégration.
- 103 fichiers du travail expérimental ont été importés. Les changements de base
  concernant le lanceur Rust, la distribution et `runtime_memory.c` restent hors
  périmètre. CMake conserve la base locale et reçoit les cibles expérimentales.
- Malgré le suffixe `msvc`, les outils utilisés dans les preuves de Dot et pour
  cette validation sont **Clang/MSYS2 CLANG64, CMake et Ninja**. MSVC n'a pas été validé.

Le dossier source est conservé. Son répertoire `out/` contient environ **2,93 Gio**
de builds, sessions, captures et preuves locales. Il n'a pas été recopié dans les
sources ni ajouté à Git. Les chemins, empreintes de binaires et PID des notes de
Dot sont des observations historiques de ce dossier, pas l'identité du nouveau
build ni l'état actuel d'une instance du jeu.

## Ce qui existe réellement

| Sujet | État et limite |
| --- | --- |
| Caméra | Reconnaissance ciblée du shader, snapshots du constant ring et contrôles de cohérence. `camera-candidate` signifie seulement deux échantillons cohérents; ce statut apparaît aussi dans le menu. |
| Frontières de frame | Fermeture caméra au flip accepté et ordonné, plutôt qu'à chaque groupe de draws d'affichage; identité transmise jusqu'à la présentation. |
| Constant ring | Correction du retrait des allocations lorsque le curseur cesse d'avancer, avec test de saturation et de retard GPU simulé. Cette correction s'applique aussi sans les diagnostics. |
| Collecte | Traces bornées et extraction opt-in de deux intervalles de pixels réels couleur/profondeur MSAA x2, avec tick GPU et fermeture ordonnée. La provenance globale peut rester incomplète; voir le rapport de pixels. |
| Projection/profondeur | Contrats CPU et bancs Vulkan hors écran, y compris profondeur multisample et durée de vie des copies. Les images synthétiques appartiennent au banc, pas au cache réel du jeu. |
| Santé | Hook passif opt-in et journal borné; santé informative, aucune preuve de vie/mort ni décision de contrôle. |
| Clavier | Trace passive de S et de l'axe vertical du stick gauche, désactivée par défaut; les tests caractérisent aussi les limites du replay existant. |
| Automatisation | Modèles et tests synthétiques. Le script de menu est attaché à une ancienne session et à une empreinte précise; ce n'est pas un pilote générique prêt à lancer. |
| DLAA/NGX | Aucun dispatch DLAA GoW3 ajouté, aucune option activée dans le lanceur. |

Les principaux diagnostics exigent des variables de processus explicites
(`SPARTAN_GOW3_INPUT_DIAGNOSTICS`, `SPARTAN_HEALTH_PASSIVE`, `SPARTAN_KEY_TRACE`)
et, pour les collectes, des destinations et déclencheurs dédiés. Cette intégration
ne les active pas dans les réglages persistants.

## Vérification initiale de l'intégration (historique)

La compilation complète de `spartanport-probe` et des 29 cibles de validation
réussit depuis un nouveau dossier de build, avec deux jobs et les dépendances
déjà présentes. À cette étape initiale, le probe construit n'avait pas été exécuté.

Le passage final du validateur réussit entièrement : **58 tests Python,
48 exécutions de bancs CPU et deux bancs Vulkan hors écran**. Les huit étapes de
compilation/édition de liens supplémentaires du validateur passent aussi :
`result.json` contient 60 étapes réussies, aucune échouée ou non prise en charge.

- GPU utilisé : NVIDIA GeForce RTX 5070 Ti, Vulkan 1.4.357.
- Profondeur/clip : 72 cas Vulkan réussis, un/deux samples, conversion et chemin
  natif MinusW, signes de viewport et plages de profondeur.
- Readback : 36 864 samples vérifiés, quatre copies bit à bit égales à leur
  référence, refus des lectures prématurées et retrait des ressources confirmé.
- La couche de validation Vulkan est indisponible. Les compteurs debug-utils à
  zéro ne constituent pas un passage avec cette couche.
- Rejeu supplémentaire du préfixe de provenance de Dot : réussi, 4 382 draws,
  126 événements, 15 593 515 octets, 8 662 lots vides omis. Il répète le premier
  intervalle complet deux fois; il ne récupère pas un second intervalle du jeu.
  Le SHA-256 du préfixe correspond à la preuve de Dot :
  `2b803040898b66a1bd293b77af61b674cff5f253598b8117fb8c3ca38c5a271c`.

Preuves de cette revue :

- `out/verification/dot-build-final.log` : compilation et édition de liens.
- `out/gow3-validation/20261028T010211453436Z/result.json` et ses journaux voisins.
- `out/verification/dot-provenance-replay.log` : rejeu du budget.
- `out/verification/dot-import-manifest.json` : empreintes des 103 imports
  initiaux, avant les adaptations documentées dans Git.
- Nouveau binaire : `out/verification/dot-build/spartanport-probe.exe`, SHA-256
  `7fd94290afd798c198e59ee67f6b33bc4e9f31007160ffc0fdc7ae4a4261684f`.

Les compilations et tests restent dans `out/verification/` et
`out/gow3-validation/`; le paquet déjà installé et les sauvegardes ne sont pas
remplacés. Aucun jeu n'est lancé par le validateur.

Quatre corrections accompagnent l'import :
création des parents manquants dans `test_native_menu_script.py`, portage Windows
du test de régression existant `test_upscaler_support.cpp` (environnement et
fichier temporaire), correction du commentaire K devenu S dans l'instrumentation
de fenêtre, puis mise à jour de
l'introduction du prototype qui décrivait encore l'ancienne clôture par groupe
d'affichage. Un validateur
réutilisable est ajouté dans `tools/validate_gow3_diagnostics.py`, ainsi que la
cible de compilation agrégée `gow3-diagnostics-tests`.

Reproduire depuis la racine du dépôt avec la toolchain déjà disponible :

```powershell
$taskTools = Join-Path $PWD 'out/toolchain/msys64/clang64'
$env:PATH = "$taskTools/bin;$env:PATH"
cmake -S gpu -B out/verification/dot-build -G Ninja `
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSPARTAN_PGO=off -DSPARTAN_LTO=OFF `
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  "-DCMAKE_PREFIX_PATH=$taskTools"
cmake --build out/verification/dot-build --parallel 2 `
  --target spartanport-probe gow3-diagnostics-tests
python -B tools/validate_gow3_diagnostics.py --build-dir out/verification/dot-build --gpu
```

Chaque passage crée de nouveaux résultats sous `out/gow3-validation/`.
Omettre `--gpu` conserve les seuls tests CPU; les fixtures Vulkan sont quand
même compilées par la cible agrégée. Les notes de Dot et les journaux des
premiers essais sont conservés; les erreurs corrigées ne sont pas comptées
comme des passages réussis dans le bilan final.

## Ce qui reste avant un DLAA utilisable

1. Valider sur une vraie scène les constantes, la permutation du shader, les
   conventions clip/viewport et la profondeur par échantillon correspondant au
   même producteur et à la même frame.
2. La capture bornée au cache réel est désormais exécutée sur Gaia. Étendre sa
   validation au-delà des deux intervalles capturés et résoudre les vues encore
   non identifiées dans la provenance globale.
3. Établir la couleur avant toute interface, les mouvements d'objets, les
   disocclusions, les coupures caméra et la gestion du jitter/historique.
4. Implémenter et valider le dispatch NGX et la recomposition de l'interface.

Le pilote autonome est un chantier distinct : il lui manque une identité de
processus attestée par un adaptateur Windows pris en charge, l'état fiable du
dialogue de chargement, une expiration des commandes possédée par le consommateur
et une fermeture de la seule instance possédée avec confirmation. Les 26 tests
du modèle d'intégration ne fournissent pas ces interfaces réelles.

Les études détaillées restent dans les documents `gow3-*`, notamment
[le prototype DLAA](gow3-dlaa-experiment.md),
[la revue de capture runtime](gow3-runtime-readback-review.md),
[le budget de provenance](gow3-provenance-window-budget.md) et
[l'essai synthétique d'intégration de session](gow3-session-integration-trial.md).
