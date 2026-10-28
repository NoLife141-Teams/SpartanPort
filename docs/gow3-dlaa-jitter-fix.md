# Correction du flou DLAA : signe du jitter

Validation du 28 octobre 2026 sur la RTX locale. Le défaut principal était le signe
du jitter transmis à NGX. Le rasterizer ajoute `(jx,jy)` à l'origine du viewport,
en pixels avec origine en haut à gauche. Le chemin DLAA envoyait `(-jx,-jy)`.
NGX reconstruisait donc avec une information de décalage incompatible avec les
images produites. Le correctif transmet `(jx,jy)`. Les vecteurs restent sans jitter.

## Comment la cause a été isolée

`tools/gow3_dlaa_replay.cpp` est un banc opt-in qui utilise la véritable passerelle
NGX et les DLL locales. Il ne démarre pas le jeu. Il compare les deux signes,
les signes mixtes X/Y, l'absence de jitter déclaré, une remise à zéro à chaque
image et les presets par défaut/K/L. Chaque cas crée son propre historique,
utilise 48 images et mesure les huit dernières phases Halton.

Deux sources sont utilisées :

- Un motif analytique avec une onde X et une onde Y, dont la phase et l'amplitude
  sont connues indépendamment du résultat NGX. Cela vérifie l'alignement, pas
  seulement la quantité de hautes fréquences conservées.
- Un crop 768×512 de l'entrée GPU réelle de la session
  `20261028T033003124522Z`, figé puis rééchantillonné bilinéairement avec les
  décalages connus. Un second cas le déplace de 0,75 pixel horizontal par image
  avec le vecteur correspondant. Ce replay est distinct de nouvelles frames Gaia.

Résultats : `out/gow3-dlaa-replay-axis/` pour les ondes et
`out/gow3-dlaa-replay-real-final/` pour le crop réel. L'analyse publiée est
`jitter-analysis.json`, avec quatre contrôles réussis. La comparaison visuelle
`jitter-comparison.png` est un crop identique agrandi ×2 sans interpolation.

| Mesure | Ancien signe inverse | Signe corrigé |
| --- | --- | --- |
| Erreur de phase maximale X/Y sur les huit dernières images | 0,1394 px | 0,0065 px |
| Amplitude moyenne conservée sur les ondes X/Y | 87,5 % | 94,9 % |
| RMSE linéaire du crop réel figé | 0,009481 | 0,007992 (−15,7 %) |
| RMSE linéaire du crop translaté | 0,008686 | 0,006189 (−28,7 %) |

Déclarer zéro jitter conserve davantage d'amplitude mais laisse une erreur de phase
jusqu'à 0,0752 pixel : cette image plus piquée ne représente pas un alignement correct.
Le preset K donne quasiment le même résultat que le défaut ; changer de preset
ne corrige donc pas cette erreur. Le preset L n'améliore pas ce banc.

Aucune accentuation n'est ajoutée : le défaut mesuré vient de l'alignement.
Le champ `InSharpness` de l'ancienne API est déprécié dans le guide NVIDIA livré
avec le SDK (§3.11). Une éventuelle préférence de netteté serait un post-traitement
séparé, après reconstruction ; elle n'est pas nécessaire à ce correctif.

Empreintes SHA-256 des sorties du replay réel comparées :

- Ancien signe : `d94a5311520e402549e125a1cb8cabc2fa9025ddbb6f7c7eb68a85d1b3c34667`.
- Signe corrigé : `aa4f01fe1e5bdda2e0607958f091f598dd0ecb0eb6529a4d345f951637293d57`.

## Confirmation dans Gaia

Session `out/gow3-sessions/20261028T130110620766Z`, binaire
`4a209d492e410dd2a54153bc01377a968cd3a32ea36d5d56f4dda24fef986907`.
La copie de la sauvegarde Auto Save Gaia a été chargée sous pilotage visuel,
avec santé infinie, son coupé et skip intro. F10 a activé le DLAA à la scène 1546,
désactivé à 5714, puis réactivé à 12361. La scène 12362 repart avec `reset=1`.
Le journal confirme NGX en 2560×1440 vers 2560×1440 et les nouveaux signes de jitter.

La paire GPU réelle de la scène 1590, évaluation 44, est publiée après achèvement
du tick 18578. Les deux images RGBA16F font chacune 29491200 octets et leur RGB
est fini. Les aperçus et l'analyse sont dans `dlss-capture/` :

- Entrée : `68ae5c1dfe7fa471a69be3499fa7d923259840aeffc3088a2527612f1704568f`.
- Sortie : `4bcf7b86ee8d552cf237e85663a7105ec1d6e57bc99390c729e3f666389b1911`.
- `input.png`, `output.png`, `detail.png`, `analysis.json`.

La texture de roche reste plus proche de l'entrée après filtrage qu'au premier
essai au signe inverse. Le HUD et la scène animée ont été inspectés. Le mode
d'affichage 2560×1440 n'était pas proposé par l'écran au moment de cet essai :
SDL l'annonce et présente à la taille bureau 1920×1080. Les captures F12 finales
sont donc 1920×1080 ; les captures avant/après NGX sont bien 2560×1440.
La barre des tâches visible masque le badge inférieur dans cette capture écran.

Les six fichiers de sauvegarde source et leur backup sont inchangés, attestés par
`save-verification.json`. Les 62 étapes CPU/Vulkan passent dans
`out/gow3-validation/20261028T130505930740Z/result.json`.
L'échec préalable de réservation de l'espace PS4 dans le bac à sable est conservé
dans `20261028T130018697776Z`; le lancement local suivant a réussi.

## Reproduire le banc

Compiler la cible `gow3-dlaa-replay` du build GPU Windows. Fournir un dossier qui
contient les deux DLL et une capture RGBA16F 2560×1440 complète. Les dossiers de
sortie doivent être nouveaux. L'analyse Python nécessite NumPy et Pillow.

```powershell
out/verification/dot-build/gow3-dlaa-replay.exe DLL_DIRECTORY CAPTURE_DIRECTORY out/replay-analytic analytic
out/verification/dot-build/gow3-dlaa-replay.exe DLL_DIRECTORY CAPTURE_DIRECTORY out/replay-real
python tools/analyze_gow3_dlaa_replay.py out/replay-analytic out/replay-real
```

Ce banc démontre le défaut de signe et son effet dans ces conditions. Il ne
certifie pas tous les mouvements, les transparences, les désocclusions ou les
coupures caméra du jeu. Les limites d'appariement des objets restent présentes.
