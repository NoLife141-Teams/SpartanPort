# DLAA natif GoW3 : essai réel sur Gaia

Le chemin expérimental GoW3 appelle maintenant NVIDIA NGX en mode DLAA natif
1920×1080 vers 1920×1080 ou 2560×1440 vers 2560×1440. Il est indépendant de l'ancien extracteur caméra et
des heuristiques d'upscaling héritées. Il reste explicitement activé par processus.

Le flou excessif observé au premier essai a été relié à un signe de jitter inversé,
puis corrigé et testé : voir [la mesure indépendante et le nouvel essai Gaia](gow3-dlaa-jitter-fix.md).

## Vecteurs mesurés dans le jeu

Session `out/gow3-sessions/20261028T031012398538Z`, sauvegarde Auto Save Gaia,
God (Normal), 00:04:35. Son coupé, skip intro, santé infinie, sauvegarde copiée.
Les intervalles 2054/2055 ont tous deux été soumis, terminés sur le GPU et reliés
à leurs snapshots caméra et vues natives. Deux fichiers de 132710400 octets
conservent RGBA linéaire, profondeur et vecteurs pour les deux échantillons MSAA.

| Mesure indépendante par correspondances d'image | Résultat |
| --- | --- |
| Correspondances retenues sans utiliser les vecteurs prédits | 312 |
| Correspondances avec vecteur GPU et profondeur concordante | 280 |
| Erreur médiane / P90 de ces vecteurs | 0,193 / 1,334 pixel |
| Caméra seule, médiane sur les 312 points | 34,655 pixels |
| Premier plan D < 0,99, vecteurs GPU | 8/8 points, médiane 0,203 pixel |
| Historique chargé au deuxième intervalle | 236 draws |
| Draws ambigus rejetés / capacité épuisée | 148 / 0 |

Ce sont des mesures sur cette paire, pas une preuve d'identité de chaque personnage.
Les surfaces répétées, les transparences et les désocclusions restent des limites.
Le groupe de profondeur lointaine n'est pas un masque certifié de décor statique.

Empreintes SHA-256 des captures :

- `11504b45d34b676407c36dcdbfa7426c88eeff777c090c0fd01c4abd2bf1b731`
- `ee7ca0d77564b2b05c16e85e34b26dbbb7fcdc812a340474fad05076477f358d`

Les positions finales des vertex shaders sont conservées sur GPU, puis appariées
avec les positions précédentes par géométrie, topologie exacte et plage de sommets.
Cela inclut caméra, transformations et déformations. Les constantes d'objet tournent
dans une arène entre frames : leur adresse sert à la provenance, pas à l'identité.
Les indices copiés, scannés et utilisés par le draw proviennent du même snapshot.
Les doublons ne sont pas associés par ordre d'apparition. Le diagnostic seul dispose
d'un tampon de constantes de 128 Mio, contre 32 Mio pour le chemin habituel.

## Raccordement NGX

La permutation de composition observée (VS `475a41468b1a1fbc`,
PS `475a4146e3a74097`, clés `efd04d8023fec408` / `efd0530963dc1d55`)
fournit la couleur sRGB MSAA2 et D32S8 MSAA2. Les vues sont vérifiées à nouveau
dans le cache réel avant lecture. Leurs mappings de composantes sont appliqués
par l'échantillonnage GPU, y compris le swizzle BGR de la couleur.

Un compute produit trois images possédées : couleur RGBA16F linéaire résolue,
profondeur R32F de l'échantillon le plus proche et mouvement RG16F. Le mouvement
d'objet est utilisé seulement si son historique est valide et sa profondeur
concordante ; sinon la reprojection caméra explicite MinusWToW / viewport négatif
fournit un repli. Un jitter Halton de huit phases déplace les viewports 3D ; les
vecteurs restent sans jitter et NGX reçoit le même déplacement signé du viewport
en pixels (origine en haut à gauche). L'ancien signe inverse a été corrigé après
mesure indépendante de la reconstruction ; voir le diagnostic ci-dessous.

NGX écrit une quatrième image RGBA16F. Un compute la développe dans une image
MSAA2 possédée qui remplace uniquement le descripteur couleur de cette composition.
L'alpha provient de la couleur d'origine. Les images invitées ne sont pas réécrites ;
la suite de la composition et du HUD reste exécutée par le jeu. L'historique est
remis à zéro après entrée invalide, disparition de la scène ou changement on/off.
Les epochs du DLAA suivent les compositions produites, indépendamment du nombre
de scènes qu'une clôture VideoOut peut contenir.

Le flag de traitement NGX `IsHDR` est actif parce que les couleurs fournies sont
linéaires, conformément au guide NVIDIA §3.1.2. Cela ne modifie pas le mode SDR
de l'écran. La passerelle utilise son auto-exposition pour cette entrée linéaire.

Le SDK officiel NVIDIA utilisé pour la passerelle MSVC est le commit
`9981c6350dc58c0ce21f024fd65e31e851c31689` de
[NVIDIA/DLSS](https://github.com/NVIDIA/DLSS). SDK, DLL et captures restent sous
`out/` ; ils ne sont pas ajoutés aux sources Git.

## Reproduire l'essai isolé

La passerelle existante `gpu/dlss_bridge` doit être compilée avec MSVC dans
`out/verification/dlss-bridge/Release/`, et le SDK placé dans
`out/dependencies/nvidia-dlss/`. Le binaire GPU se compile avec la cible
`spartanport-probe` dans `out/verification/dot-build/`.

```powershell
out/pyenv/Scripts/python.exe -B scripts/gow3_diagnostic_session.py `
  --seed-data C:/Users/jerem/Documents/Codex/2026-10-26/task/spartanport-dlaa-msvc/out/gow3-input-collection-f586605 `
  --game-dir C:/Games/God.of.War.III.Remastered.RECOMP.v2.1.5-KaOs/CUSA01623 `
  --dlaa --resolution 1440p --fullscreen --launch --wait
```

Le script annonce un nouveau dossier de session. Charger la sauvegarde avec
S, Espace, puis Entrée dans le dialogue. Le DLAA démarre désactivé pour permettre
une référence visuelle. **F10** active/désactive le DLAA en jeu ; le badge permanent
en haut à droite affiche vert « DLAA ACTIF » et la résolution, gris « DÉSACTIVÉ »,
ou ambre « EN ATTENTE », « ERREUR » ou « INDISPONIBLE ». Une demande clavier ne
publie pas à elle seule l'état actif : une évaluation NGX réussie est nécessaire.
Le raccourci ignore les répétitions et ne bascule pas pendant les dialogues du port.
**Maj+F10** fait défiler la netteté 0 / 20 / 40 %. Un curseur de session 0–100 %
est ajouté au menu Image. L'accentuation RCAS utilise une copie séparée après NGX,
avant le HUD, sans accentuer l'historique. Voir [les essais natifs 1080p](gow3-dlaa-1080p-sharpness.md).
Il est disponible dans ce chemin expérimental, lancé avec `--dlaa`.
Écrire `on` / `off` dans `dlaa.switch` reste possible ; F10 met aussi ce fichier
à jour. La transition prend effet à la composition suivante. Sans chemin de commutation,
`SPARTAN_GOW3_DLAA=1`, `SPARTAN_GOW3_INPUT_DIAGNOSTICS=1`,
`SPARTAN_UPSCALER=none` et `SPARTAN_DLSS=1` demandent directement ce chemin.
Les DLL doivent accompagner le probe. La disponibilité effective est attestée
par `DLSS: ready`, la création `quality 0`, puis `GoW3 DLAA: evaluated=...`.

Créer `dlss-capture/capture.trigger` après activation demande une paire bornée de
captures GPU avant/après NGX. Chaque fichier RGBA16F fait 16588800 octets en 1080p
ou 29491200 octets en 1440p, avec
publication après achèvement du tick GPU. Les fichiers existants ne sont pas écrasés.

## Preuve de l'essai final

Session `out/gow3-sessions/20261028T032038120297Z`, avec le binaire final
SHA-256 `0747fca6a5bb343eeb691eeeeaf976d7c1eb5704c01ac113513c91176141c257`.
Le journal atteste `quality 0, HDR input`, puis des évaluations DLAA continues.
L'activation a lieu à la scène 709, la désactivation à 1322 et la réactivation
à 1591. La première évaluation après réactivation porte bien `reset=1`.
L'inspection visuelle en jeu confirme la scène animée et le HUD intact.

La paire GPU de la scène 742, évaluation 33, est exportée après achèvement
du tick 4747. RGB est fini dans les deux fichiers ; 6203820 composantes RGB
diffèrent, avec une différence absolue moyenne de 0,003749 en valeurs linéaires.
Cette différence confirme une sortie calculée, pas une mesure de qualité.

- Entrée : `e99a09a3a8c6dad9f4ed9b042725d7a034bb499dcfcecd3c9dae8e33d6499476`.
- Sortie : `5874c623a65576ae81aad64b2153b6917582f2bc717565017d1fc828d99f0eed`.
- Analyse et aperçus : `dlss-capture/analysis.json`, `input.png`, `output.png`.
- Validation hors jeu : les 62 étapes, dont les deux bancs Vulkan, passent dans
  `out/gow3-validation/20261028T031815469575Z/result.json`.
- Les huit tests Python de l'analyse couleur/profondeur/mouvement passent aussi.

Les captures antérieures rejetées restent conservées : notamment caméra invalide
ou conflictuelle et plantages avant activation. Les premières évaluations avec
le flag NGX LDR ont été remplacées par l'essai final en entrée linéaire HDR.

## Plein écran 1440p, raccourci et comparaison réelle

Session `out/gow3-sessions/20261028T033003124522Z`, binaire SHA-256
`39f2ab39c13ffd0ff5aec66b93f950b89b42339e784234371ac00aac7a01b413`.
Le patch de rendu « Resolution Patch - 1440p » est chargé. Le plein écran SDL
utilise le mode exact 2560×1440 lorsque l'écran le propose ; sinon il conserve
le mode bureau et l'annonce. La taille des ressources NGX reste celle du rendu
natif. Un changement de résolution après création de ces ressources exige un redémarrage.

F10 a activé le DLAA à la scène 796, désactivé à 2040, puis réactivé à 3571.
La scène 3572 reprend avec `reset=1`. Le badge vert et le badge gris ont été
observés en plein écran ; le journal confirme `DLSS 2560x1440 -> 2560x1440,
quality 0, HDR input`. F12 exporte l'image finale du jeu sans les overlays du port.

Captures finales PNG, toutes vérifiées 2560×1440, dans `runtime/user/screenshots/` :

- Sans DLAA : `CUSA01623_20261027_233314_484.png`.
- Avec DLAA : `CUSA01623_20261027_233332_334.png`.
- Nouvelle référence sans DLAA : `CUSA01623_20261027_233356_834.png`.

Ces prises sont à des instants différents : personnages et caméra ont bougé.
Pour isoler le filtrage, la paire GPU de la scène 1250, évaluation 454, tick terminé
23085, conserve l'entrée non filtrée et la sortie NGX de la même image, avec le
jitter de la frame active. Ce n'est donc pas une seconde exécution sans jitter.
`dlss-capture/comparison.png` présente deux crops identiques agrandis ×2 sans
interpolation ; `input.png` et `output.png` restent à la résolution native.
Ce sont des aperçus sRGB du signal linéaire avant la composition finale du jeu.

Dans cet essai antérieur au correctif de jitter, le DLAA adoucit aussi les textures de
roche, les fougères et les détails de Kratos. Ce réglage mérite encore d'être
affiné ; une seule paire ne mesure ni le scintillement ni les traînées temporelles.
RGB est fini, 11029281 composantes diffèrent, différence absolue moyenne linéaire
0,002729 : cela prouve une sortie calculée, pas une amélioration de qualité.

- Entrée brute : `d5ccc528113bb44009c20733fac5b7f28285ebbf4745daf5aab9ddd295dbfb47`.
- Sortie brute : `382d1b3fc7ed32a97f3fffb098baf4acac40e6316612fbdeaa9d4a597d872f36`.
- Les 62 étapes CPU/Vulkan passent : `out/gow3-validation/20261028T033339464069Z/result.json`.
- Les huit tests Python d'analyse passent. Source et backup des six fichiers de
  sauvegarde inchangés, attestés dans `save-verification.json`.

## Limites conservées

Les tests visent Gaia à 1080p et 1440p sur la RTX locale. Ils ne certifient pas tout le jeu,
toutes les résolutions, les cinématiques, les effets transparents ou les coupures
caméra. Le repli caméra sur des objets non appariés peut laisser des traînées.
Des plantages avant activation ont aussi été conservés dans les sessions rejetées ;
ce travail n'établit pas une stabilité générale du port. Le chemin standard sans
les flags GoW3 reste inchangé.
