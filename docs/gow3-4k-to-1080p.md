# Scène 4K vers écran 1080p : DLAA OFF et ON

Essai réel du 28 octobre 2026 sur RTX 5070 Ti. Même binaire dans les trois
sessions, SHA-256 `adedcda5a4f1ee87da6fa84e06b407aeec9250e648be90d2d2a10f972f5210d4`.
MSAA désactivé, un échantillon réel en couleur et profondeur, netteté NGX/RCAS
à zéro. Son coupé, skip intro, santé infinie, copie Auto Save Gaia, God Normal,
00:04:35 du 26 octobre à 21:45. Les six fichiers source, backup et runtime sont
inchangés aux contrôles conservés dans `supersampling-save-verification.json`.

## Sessions et preuve du rendu

| Mode | Session sous `out/gow3-sessions/` | Source scène GPU | NGX à la composition 240 | Tick du readback scène |
| --- | --- | --- | --- | --- |
| Natif, OFF | `20261028T144227687357Z` | 1920×1080, 1 sample | 0 | 11266 |
| 4K, OFF | `20261028T143219416873Z` | 3840×2160, 1 sample | 0 | 11308 |
| 4K, DLAA seul | `20261028T143934697588Z` | 3840×2160, 1 sample | 239 | 10934 |

`Resolution Patch - 4K` est appliqué par le chemin normal de `run.py` pour les
deux essais 4K. Le hook vérifie les vues natives D32S8 et RGBA8 sRGB, leur backing,
leur disposition et le sample count réel avant d'appeler NGX. Sa taille acceptée
comprend maintenant 3840×2160 ; les shaders simples, la reprojection, le jitter
en pixels et les vecteurs d'objet utilisent déjà les dimensions réelles.
NGX reçoit 3840×2160 et produit 3840×2160. Le badge actif a été observé dans le
jeu, à netteté zéro. Le mode 4K OFF ne crée/évalue pas de feature NGX.

Les readbacks `dlss-capture/compare-{input,output}.rgba16f` sont finis en RGB,
après achèvement GPU, de la même composition. Chaque fichier 4K fait 66355200
octets, contre 16588800 pour le natif. Entrée et sortie sont identiques en OFF ;
NGX modifie la sortie en ON. L'alpha de la sortie NGX n'est pas un critère.

## Où se fait la réduction

La **scène** est bien en 4K. Dans cette configuration, la chaîne de composition
produit néanmoins déjà une image VideoOut/présentateur en **1920×1080**. Le
présentateur blitte ensuite cette image vers la swapchain 1920×1080, donc à 1:1.
Il serait incorrect d'affirmer que le dernier blit est ici une réduction 4K→1080p.
Les traitements entre la source 4K capturée et la sortie finale effectuent déjà
la conversion. Le filtre exact de cette réduction invitée n'a pas été caractérisé.

Les F12 du port lisent la source du présentateur ; une capture diagnostique
distincte lit maintenant la **swapchain réellement affichée**, après le blit
normal et avant le badge/menu/compteur du port. Le HUD et les effets du jeu
restent inclus. Ses métadonnées attestent 1920×1080 en source de présentation
et en destination, pour les trois modes. La mention `internal` dans son message
console désigne cette source de présentation, pas la scène 3D.

Le diagnostic est explicitement activé par `SPARTAN_GOW3_DISPLAY_CAPTURE` :
répertoire absolu vide, SDR 1080p, RGBA8/BGRA8 seulement. Il attend qu'un readback
de scène valide soit en cours, puis capture 16 présentations uniques, espacées
de quatre frames. Les images réutilisées sont exclues. Ce signal d'ouverture
n'établit pas une association exacte entre composition 240 et première image
présentée : le présentateur a sa propre file et son propre scheduler.

Chaque readback fait 8294400 octets. Les commandes passent la swapchain de
TransferDst à TransferSrc pour la copie, puis restaurent TransferDst avant la
suite habituelle. Les buffers restent possédés jusqu'à la fin du tick GPU ;
les fichiers bruts et leur JSON ne sont écrits qu'après cette fin. Le PNG est
encodé hors jeu, sans correction gamma, netteté ou réduction supplémentaire.
Le swizzle BGRA/RGBA est explicite. Aucun traitement du pipeline normal n'est
activé par ce diagnostic lorsqu'il n'a pas de répertoire de capture.

## Résultat à voir

`out/gow3-aa-comparison/20261028-4k-to-1080p-final/` contient :

- `overview.png` : vue générale, 1080p OFF / 4K OFF / 4K DLAA ON.
- `comparison.png` : la vue et trois rectangles de pixels affichés agrandis ×2
  sans interpolation : personnages, végétation et racines/rochers.
- `motion.webp` : 16 triptyques en mouvement, environ une seconde, intervalle
  de lecture indicatif 67 ms. `motion-contact.png` montre quatre instants.
- `native-off-00.png`, `4k-off-00.png`, `4k-dlaa-00.png` et les 15 suivants de
  chaque mode : vrais pixels 1920×1080, sans le redimensionnement des triptyques.
- `analysis.json` : dimensions, ticks, état DLAA observé à l'enregistrement,
  empreintes des sources, preuve NGX, intervalles et limites.
- `save-verification.json` : vérification des trois sauvegardes.

À l'inspection, le passage 1080p→4K rend les petits contours de végétation moins
crénelés et conserve une image détaillée. Le 4K OFF constitue donc une référence
utile pour privilégier le détail. Le DLAA en plus lisse encore certaines hautes
fréquences et adoucit légèrement l'image ; son apport au plein écran 1080p est
plus discret que celui du passage à la 4K. Ce constat ne démontre pas un
classement universel de la stabilité temporelle. Du flou apparaît aussi pendant
des secousses dans les sorties OFF ; tout l'aspect flou des mouvements ne peut
donc pas être imputé au DLAA.

Les trois sessions suivent la même sauvegarde, mais leurs caméras, poses et
effets ne sont pas synchronisés exactement. Une différence entre colonnes
pendant une secousse n'est donc pas une mesure de ghosting ou de motion blur.
La séquence courte ne valide pas tous les personnages, transparences ou
désocclusions. Le rendu final est inspecté à netteté zéro dans tous les modes.

Les médianes entre enregistrements espacés de quatre présentations sont
66,4937 / 66,5837 / 66,5130 ms, cohérentes avec environ 60 présentations par seconde
sur cette scène. Ce ne sont pas des coûts GPU individuels. Le profiler existant
est activé et ses fenêtres conservées, mais ses plages de timestamps peuvent
inclure attente et trous entre soumissions. En particulier, ses totaux ON/OFF
ne permettent pas de conclure à un gain de performance du DLAA. Aucun benchmark
déplafonné ni mesure dédiée du temps NGX n'a été réalisé ici.

## Reproduire

```powershell
out/pyenv/Scripts/python.exe -B scripts/gow3_diagnostic_session.py `
  --seed-data C:/Users/jerem/Documents/Codex/2026-10-26/task/spartanport-dlaa-msvc/out/gow3-input-collection-f586605 `
  --game-dir C:/Games/God.of.War.III.Remastered.RECOMP.v2.1.5-KaOs/CUSA01623 `
  --aa dlaa --resolution 4K --fullscreen --display-capture --launch --wait
```

`--aa none --resolution 4K` donne 4K OFF ; `--aa none --resolution native` donne
1080p OFF. Attendre la fin complète du processus précédent avant de lancer
le suivant. Une tentative `20261028T143447203243Z` a échoué à réserver les
12224 Mio du pool invité (erreur 1450), avant toute frame et tout appel NGX.
Elle a été arrêtée ; la relance ci-dessus a réussi. Aucun réglage Windows ni
aucune autre application n'a été modifié/fermé.

Charger par S vers Load, Espace, puis Entrée dans Auto Save Gaia. Les captures
sont bornées et automatiques une fois le jeu chargé. F10 reste le raccourci
DLAA ; sa désactivation conserve le MSAA OFF. Le réglage 4K et les options des
essais restent dans leurs copies de configuration isolées.

Build du probe réussi ; 62 vérifications CPU/Vulkan dans
`out/gow3-validation/20261028T143242894286Z/result.json` et huit tests Python
d'analyse de pixels réussis. L'outil `tools/analyze_gow3_supersampling.py` vérifie
les 48 readbacks finaux, les trois couples de scène, l'identité du binaire,
les états NGX, les dimensions, les tailles et les valeurs RGB finies.
