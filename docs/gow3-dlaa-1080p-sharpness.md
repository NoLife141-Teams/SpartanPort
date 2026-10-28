# Essais DLAA et netteté en 1080p natif

Le 28 octobre 2026, sur l'écran 1080p de l'utilisateur. Session conservée :
`out/gow3-sessions/20261028T132831455533Z`. Le rendu, NGX, le plein écran et les
captures F12 sont tous en 1920×1080. Le binaire utilisé porte le SHA-256
`c4946d47aa494e3e59bd663f9c138cb39829eaef05150df65fd90b3620d2a565`.
La copie de la sauvegarde Auto Save Gaia est chargée avec son coupé, santé infinie
et skip intro. Les six fichiers source et leur backup sont inchangés, attestés
par `save-verification.json`.

## Réglage et chemin GPU

- F10 bascule le DLAA.
- Maj+F10 fait défiler les intensités de netteté 0 / 20 / 40 %.
- Le menu Image propose un curseur de session de 0 à 100 %. Le raccourci clavier
  et les valeurs affichées 0, 20 et 40 % ont été vérifiés par pilotage visuel.
- Le badge est placé en haut à droite pour rester visible lorsque la barre des
  tâches recouvre le bord inférieur. Il affiche l'état effectif, la résolution
  et l'intensité de netteté.

Le RCAS existant reçoit une variante de shader pour une entrée RGBA16F. Le shader
NGX brut est lu sans être modifié ; RCAS écrit une image distincte, qui alimente
la composition MSAA du jeu. L'alpha vient toujours de la couleur originale. La
netteté ne s'applique ni à l'historique NGX ni au HUD. À 0 %, la présentation
utilise directement la sortie NGX. Modifier l'intensité ne réinitialise pas
l'historique ; le journal indique `history-retained`.

La netteté démarre à 0 % dans chaque nouvelle session expérimentale ; elle n'est
pas ajoutée aux réglages persistants du lanceur standard. 20 % est le point de
départ recommandé après cet essai, et peut être choisi avec Maj+F10.

## Comparaison équitable des trois niveaux

Le déclencheur `dlss-capture/capture.trigger` exporte désormais quatre images
RGBA16F de **la même scène GPU**, après achèvement du tick : entrée NGX, sortie
neutre, netteté 20 %, netteté 40 %. Ici : scène 4050, évaluation 597, tick 19272.
Chaque fichier fait 16588800 octets ; RGB est fini dans les quatre images.

| Sortie | SHA-256 |
| --- | --- |
| Neutre | `cf62cbb000fe1288107e905d9354ef33ed0ecaa338f27b990d727bf3ff566853` |
| 20 % | `53a107480e67dea77aedf85f20aa30389f7ec792a95fdbb6daca4136efdea200` |
| 40 % | `4f8599f591130d43d35627ed4434b066d7e1cd3be3f9a9b8086b3e0146f97bca` |

`sharpness-comparison.png` présente des zones identiques agrandies ×2 sans
interpolation. Les PNG `output.png`, `low.png`, `medium.png` conservent 1920×1080.
Ce sont des aperçus sRGB du signal linéaire avant les traitements finaux du jeu.

À 20 %, le gain de détail est discret, avec un aspect proche de la sortie neutre.
40 % accentue davantage les textures et les contours. Sur cette image, la fraction
de composantes dépassant de plus de 0,001 la plage locale 3×3 de la sortie neutre
passe de 0,149 % à 1,238 %. Ce dépassement est un indicateur de renforcement des
contours ; il ne constitue pas à lui seul un halo perceptible ou une note de qualité.
Le gradient moyen augmente de 1,63 % à 20 %, contre 4,66 % à 40 %.

## Mouvements

`sequence.trigger` demande une seule séquence bornée de 16 échantillons espacés
de huit scènes. Chacun conserve simultanément les niveaux 0 / 20 / 40 % avec le
même tick, après achèvement du GPU. Ici : scènes 4051 à 4171. Les aperçus GPU
960×540 moyennent les blocs 2×2 en linéaire avant encodage sRGB ; leurs fichiers
RGBA8 font chacun 2073600 octets. Les buffers sont retenus jusqu'à la fin du tick.

`sharpness-motion.webp` juxtapose les trois modes et `motion-contact.png` présente
quatre instants de la séquence. L'animation emploie un intervalle indicatif de
133 ms : ce n'est pas une vidéo avec cadence de présentation mesurée.
Les personnages changent de pose, le décor et les effets de Gaia sont animés.
L'inspection n'a pas montré de traînée supplémentaire évidente due à RCAS dans
cette courte séquence ; les limites temporelles du DLAA lui-même restent à étendre.
Kratos est en partie masqué par les ennemis dans cette séquence.

La différence absolue moyenne entre échantillons successifs, **sans compensation
de mouvement**, augmente de 0,45 % à 20 % et de 1,30 % à 40 %, relativement au
neutre. Cette mesure inclut l'animation réelle ; elle ne mesure pas indépendamment
le scintillement ni les traînées. Le choix de 20 % privilégie une accentuation légère.

## Contrôles et reproduction

Les changements 0→20 % et 20→40 % sont consignés aux scènes 5679 et 10165 sans
reset. F10 désactive ensuite le chemin à la scène 27099.
La réactivation à 32490 produit `reset=1` à 32491. Maj+F10 revient ensuite à 0 %,
puis à 20 %, sans nouvelle remise à zéro ; la session est laissée active à 20 %.
Les PNG F12 de référence
OFF, ON neutre, ON 20 % et ON 40 % ont tous été vérifiés 1920×1080 dans
`runtime/user/screenshots/` et `screenshot-verification.json`. Leurs instants
diffèrent ; utiliser les captures GPU pour comparer les intensités exactement.

Les 62 étapes CPU/Vulkan passent dans
`out/gow3-validation/20261028T133438988634Z/result.json`. Les huit tests Python
d'analyse des pixels passent aussi. La compilation des shaders et du probe réussit.
Le banc TAA historique Windows essayé séparément n'est pas compté dans ces succès :
son adaptation temporaire du stub TLS a été retirée après un arrêt natif. La nouvelle
variante RCAS est validée ici par ses sorties GPU réelles et la comparaison ci-dessus.

Un premier lancement a planté dans le chargement, avant activation du DLAA ; il
reste conservé dans `20261028T132717250889Z`. Le second lancement a permis tous
les essais. Ce travail ne certifie pas une stabilité générale du port.

Lancer le script de session avec `--dlaa --resolution native --fullscreen`, puis
charger la sauvegarde sous contrôle visuel. Créer les deux déclencheurs après
activation et échauffement. Les exports sont bornés et n'écrasent pas les fichiers.
L'analyse NumPy/Pillow se reproduit avec :

```powershell
python tools/analyze_gow3_dlaa.py SESSION/dlss-capture
python tools/analyze_gow3_dlaa_sharpness.py SESSION/dlss-capture
```

La qualité reste à vérifier sur d'autres scènes, cinématiques, transparences et
mouvements de caméra. Ces essais ne justifient pas de modifier le modèle NGX ni
de pousser fortement l'accentuation.
