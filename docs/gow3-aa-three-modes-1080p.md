# Comparaison sans MSAA, MSAA ×2 et DLAA seul en 1080p

Essai réel du 28 octobre 2026 sur RTX 5070 Ti et écran 1080p. Les trois modes
utilisent le même binaire, SHA-256
`10ac4c262f8d6ed78912158f0b0cefc88653756e233aed5efbe59de2278aec40`.
Le rendu et les captures plein écran sont en **1920×1080 natif**. Aucun rendu 4K
ou redimensionnement vers 1080p n'intervient. Netteté NGX et RCAS : zéro.

## Modes et preuves GPU

| Mode | Session sous `out/gow3-sessions/` | Échantillons couleur/profondeur réels | DLAA | Évaluations NGX à la capture |
| --- | --- | --- | --- | --- |
| Sans AA connu | `20261028T141132825538Z` | 1 / 1 | off | 0 |
| MSAA ×2 seul | `20261028T141321091287Z` | 2 / 2 | off | 0 |
| DLAA seul | `20261028T141455607044Z` | 1 / 1 | on | 238 |

Les trois captures se font à la composition numéro 240, après validation des
vues réelles par le cache et achèvement du tick GPU (5106, 5447 et 6179).
Les formats lus sont D32S8 et RGBA8 sRGB. Les vues simples sont vérifiées
`e1`, les vues MSAA `e2` ; un mélange ou un autre nombre est rejeté.
Le signal capturé est RGBA16F linéaire, 16588800 octets par image.

Chaque `dlss-capture/compare.json` contient résolution, nombre d'échantillons,
état DLAA, compteur NGX, jitter et matrice de caméra. Les deux modes off
copient l'entrée préparée à titre de référence et conservent le descripteur
original du jeu. Ils ne créent ni n'évaluent de feature NGX. Le module NGX
est chargé pour partager la même instrumentation entre les trois sessions.

Le mode sans AA désactive le MSAA identifié et tous les AA/upscalers du port.
Il ne constitue pas une démonstration qu'aucun autre shader invité inconnu
n'effectue un filtrage assimilable à un post-AA. Le petit recensement de passes
collecté dans la session sans AA est partiel et ne permet pas de l'établir.

## Changement du chemin DLAA

`SPARTAN_NO_MSAA=1` existait déjà : il transforme les textures invitées MSAA,
les attachements et les shaders correspondants en ressources à un échantillon,
tout en conservant les tailles/adresses invitées d'origine. Le hook DLAA
supposait jusqu'ici deux échantillons et devait être adapté.

Les shaders de préparation et de réinjection possèdent maintenant une variante
`SINGLE_SAMPLE`. Elle lit la couleur et la profondeur une seule fois et écrit
une image de remplacement simple. La préparation ne moyenne donc plus une
image préalablement antialiasée par MSAA. L'attachement de mouvement RGBA32F,
les pipelines de mouvement et la sortie destinée à la composition suivent
le même nombre réel d'échantillons. Le chemin MSAA2 antérieur reste disponible.

La session de repérage `20261028T140952604127Z` a montré la permutation simple :
VS brut `475a41468b1a1fbc`, PS brut `475a4146e3a74097`, clés exactes
`efd04d8023fec408` / `efd0530963dc1d54`. Elle est acceptée uniquement en mode
sans MSAA ; la clé PS `efd0530963dc1d55` reste requise pour MSAA2.
Cette première session n'entre pas dans la comparaison finale.

## Résultat visuel et limites

Artifacts générés par `tools/analyze_gow3_aa_modes.py` :

`out/gow3-aa-comparison/20261028-1080p-final/`

- `comparison.png` : les trois sorties, puis trois rectangles identiques à ×2
  par duplication de pixels, sans interpolation des agrandissements.
- `none-output.png`, `msaa2-output.png`, `dlaa-output.png` : images 1920×1080.
- `dlaa-pair.png` : entrée simple et sortie NGX de **la même frame GPU**.
- `analysis.json` : dimensions, empreintes, contrôles et écarts caméra.
- `save-and-screenshots.json` : sauvegardes et captures plein écran F12.

À l'inspection, les références sans DLAA conservent davantage de petites marches
et de hautes fréquences sur les contours et la végétation. L'écart sans AA/MSAA2
reste discret sur cette scène. Le DLAA seul adoucit davantage les personnages,
les feuilles et certaines textures, malgré l'absence confirmée de MSAA et de
netteté ajoutée. La paire de la même frame montre aussi ce lissage : le cumul
MSAA2+DLAA n'était donc pas la seule cause de l'aspect doux. Retirer le MSAA
est désormais possible, mais cela ne suffit pas à certifier le meilleur équilibre
entre conservation du détail et réduction des marches.

Les sessions distinctes n'ont pas exactement la même caméra ni les mêmes poses.
Les écarts maximum de coefficients VP, relativement au mode sans AA, sont
2,943359375 pour MSAA2 et 2,839515686 pour DLAA (ces nombres ne sont pas des
pixels). Les vues générales sont voisines ; les animations de premier plan
diffèrent. Aucune soustraction entre ces sessions n'est utilisée comme score
de qualité. La paire entrée/sortie DLAA, elle, partage exactement caméra et tick ;
son entrée contient le jitter physique de cette frame (-0,4375 ; +0,388888896 px),
alors que la sortie est reconstruite vers la grille stable.

Les aperçus GPU encodent RGB linéaire vers sRGB et omettent l'alpha ; ils précèdent
la composition finale et le HUD. Les F12 séparés attestent le plein écran final
et le badge, mais leurs instants ne sont pas synchronisés entre modes :

| Mode | F12 sous `runtime/user/screenshots/` |
| --- | --- |
| Sans AA connu | `CUSA01623_20261028_101302_183.png` |
| MSAA2 | `CUSA01623_20261028_101433_122.png` |
| DLAA seul | `CUSA01623_20261028_101616_515.png` |

Pas de benchmark FPS, ni mesure indépendante du scintillement/ghosting dans cet
essai. Le badge actif et la sortie NGX différente prouvent le fonctionnement,
pas une supériorité universelle du DLAA.

## Commandes et vérifications

```powershell
out/pyenv/Scripts/python.exe -B scripts/gow3_diagnostic_session.py `
  --seed-data C:/Users/jerem/Documents/Codex/2026-10-26/task/spartanport-dlaa-msvc/out/gow3-input-collection-f586605 `
  --game-dir C:/Games/God.of.War.III.Remastered.RECOMP.v2.1.5-KaOs/CUSA01623 `
  --aa dlaa --resolution native --fullscreen --launch --wait
```

Remplacer `--aa dlaa` par `--aa none` ou `--aa msaa2` pour les deux références.
Le changement de MSAA se fait au lancement du processus. Le chargement est
piloté visuellement : S vers Load, Espace, puis Entrée dans Auto Save Gaia.
Son coupé, santé infinie, skip intro, sauvegarde God Normal 00:04:35 copiée.
Les six fichiers source, backup et runtime sont inchangés dans les trois modes
au contrôle enregistré dans `aa-verification.json`.

Dans la session DLAA seul, **F10 désactive le DLAA en laissant le MSAA off** ;
un second F10 réactive NGX avec remise à zéro de l'historique. Le badge off puis
actif 1920×1080 / netteté 0 % a été observé, et les transitions sont dans le log
(off scène 2123, on scène 4155, reprise avec `reset=1` à 4156). La session est laissée
active à zéro netteté pour l'essai utilisateur.

L'ancien `--dlaa` conserve son comportement de démarrage off et le choix MSAA
par défaut du GPU ; utiliser `--aa dlaa` pour reproduire explicitement DLAA seul.

Build du probe réussi, 62 vérifications CPU/Vulkan passées dans
`out/gow3-validation/20261028T141035838809Z/result.json`, plus les huit tests
Python d'analyse de pixels. Les trois captures réelles et l'analyse passent les
contrôles de dimensions, nombre d'échantillons, RGB fini, état et compteur NGX,
identité du binaire et taille des fichiers.
