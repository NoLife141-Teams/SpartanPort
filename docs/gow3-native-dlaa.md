# DLAA natif GoW3 : essai réel sur Gaia

Le chemin expérimental GoW3 appelle maintenant NVIDIA NGX en mode DLAA natif
1920×1080 vers 1920×1080. Il est indépendant de l'ancien extracteur caméra et
des heuristiques d'upscaling héritées. Il reste explicitement activé par processus.

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
vecteurs restent sans jitter et NGX reçoit l'offset inverse en pixels.

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
  --dlaa --launch --wait
```

Le script annonce un nouveau dossier de session. Charger la sauvegarde avec
S, Espace, puis Entrée dans le dialogue. Le DLAA démarre désactivé pour permettre
une référence visuelle. Écrire `on` dans `dlaa.switch` l'active à la composition
suivante ; écrire `off` rétablit la couleur native. Sans chemin de commutation,
`SPARTAN_GOW3_DLAA=1`, `SPARTAN_GOW3_INPUT_DIAGNOSTICS=1`,
`SPARTAN_UPSCALER=none` et `SPARTAN_DLSS=1` demandent directement ce chemin.
Les DLL doivent accompagner le probe. La disponibilité effective est attestée
par `DLSS: ready`, la création `quality 0`, puis `GoW3 DLAA: evaluated=...`.

Créer `dlss-capture/capture.trigger` après activation demande une paire bornée de
captures GPU avant/après NGX. Chaque fichier RGBA16F fait 16588800 octets, avec
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

## Limites conservées

Le test vise Gaia à 1080p sur la RTX locale. Il ne certifie pas tout le jeu,
toutes les résolutions, les cinématiques, les effets transparents ou les coupures
caméra. Le repli caméra sur des objets non appariés peut laisser des traînées.
Des plantages avant activation ont aussi été conservés dans les sessions rejetées ;
ce travail n'établit pas une stabilité générale du port. Le chemin standard sans
les flags GoW3 reste inchangé.
