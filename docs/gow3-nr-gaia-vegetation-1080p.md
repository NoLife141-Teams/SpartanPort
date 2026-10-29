# NR, végétation et Gaia en 1080p avec MSAA x2

28 octobre 2026. Le suivi des maillages répétés a été corrigé et testé sur la
copie Auto Save Gaia, God (Normal), 00:04:35, 26 octobre à 21:45. Le profil
`--nr-scene` garde maintenant **MSAA x2 + DLSS 5 NR**, conformément à la demande
de l'utilisateur. F7 ne commute que NR ; MSAA x2 reste actif ON comme OFF.
NR et SR/DLAA restent exclusifs dans ce laboratoire.

## Cause et correction

Dans la référence `out/gow3-sessions/20261028T185130834200Z/`, le journal borné
retient 1 432 dessins avec attachement de mouvement : 1 154 avec historique,
278 sans. Des groupes de 2 à 8 dessins partagent la géométrie mais ont des
transformations d'objet différentes. L'ancien History strict les rejetait
comme doublons. Dans les premières captures, les pixels en repli possèdent
bien une profondeur de producteur concordante : le problème principal est
l'identité d'instance, pas un shader de profondeur absent.

`Motion::AffineInstances` associe les instances répétées à partir des douze
composantes de transformation du bloc de constantes de 144 octets déjà copié
pour le dessin. Cette lecture est limitée au VS `aed40011eb5259f3`, dont le
contrat est établi dans `gow3-position-contract.md`. L'adresse tournante du
bloc et l'ordre d'apparition des dessins ne servent pas d'identité.

L'association exige la même géométrie, topologie, plage, nombre d'instances et
viewport, dans la scène immédiatement précédente. La translation doit changer
de moins de 1 unité et la norme de différence des neuf coefficients linéaires
de moins de 0,75. Le second candidat doit avoir un score supérieur à
`max(4 * meilleur_score, 0,0625)`. Les poses identiques répétées gardent la même
clé afin que History strict les rejette. Un saut, une pose non finie, un trou
de scène ou une ambiguïté provoque un réchauffement de l'historique.
Les tables sont bornées à 8 192 entrées par scène.

Cette transformation sert à distinguer les instances ; le mouvement transmis
provient toujours des **positions finales des sommets sur GPU**, avec la
déformation et les palettes du dessin. Les maillages uniques gardent leur
chemin existant. Le diagnostic marque les instances répétées suivies par 2,
les autres historiques valides par 1 et le repli caméra par 0. Ce marquage ne
constitue pas un masque sémantique de végétation.

Avec MSAA x2, la couleur est résolue en moyennant les deux samples. La
profondeur la plus proche et le mouvement du même sample sont sélectionnés,
puis NR reçoit des images résolues à 1920×1080. L'alpha original est conservé
et le HUD reste dessiné ensuite par le jeu. Aucun jitter n'est ajouté.

Le cache NR utilise une version distincte pour le nouveau marquage des
mouvements. Une collecte avec cache neuf a aussi révélé que les clés de
permutation contiennent un ordinal de compilation : la composition native
1080p a été observée aux deux clés PS finissant par `1d54` et `1d55`.
Le hook NR accepte ces deux observations en single-sample ; les gardes
de shaders bruts, backing, formats et samples restent appliquées. La garde
DLAA existante reste identique. Le chemin MSAA x2 utilise la variante `1d55`.

## Validation réelle

Deux sessions corrigées ont été chargées visuellement par S → Load → Espace
→ Gaia → Entrée, sans son, santé infinie et skip intro :

| Session | AA | Scènes capturées | Historique géométrique valide |
| --- | --- | --- | --- |
| `20261028T190232846974Z` | sans MSAA | 693–696, 909–912 | 99,760–99,988 % |
| `20261028T190936396733Z` | MSAA x2 | 991–994, 1207–1210 | 98,199–99,815 % |

Chaque groupe contient quatre scènes consécutives avec entrée/sortie NR,
profondeur, mouvement et diagnostic de provenance de la même commande GPU.
Les écritures sont publiées après achèvement GPU. Les huit captures sont
bornées à environ 886 MiB par session. Les JSON MSAA attestent `scene_samples=2`.
Tous les readbacks sont finis, le backing de profondeur reste cohérent et
l'historique n'est pas réinitialisé dans les groupes capturés.

Les correspondances sont obtenues uniquement à partir des couleurs d'entrée,
avant consultation des vecteurs. Les deux hypothèses sont comparées aux mêmes
points. La recherche ±20 pixels et les contrôles aller-retour ne couvrent pas
toutes les grandes vitesses ni les désocclusions.

| Mesure sur les instances répétées suivies | Sans MSAA | MSAA x2 |
| --- | --- | --- |
| Correspondances indépendantes, six paires | 145 | 139 |
| Erreur médiane des vecteurs corrigés | 0,288 px | 0,334 px |
| P90 des vecteurs corrigés | 1,141 px | 0,849 px |
| Médiane du repli caméra, mêmes points | 5,286 px | 17,206 px |
| P90 du repli caméra, mêmes points | 11,751 px | 24,827 px |
| Écart maximal caméra CPU double / GPU float | 0,000303 px | 0,000336 px |
| Mismatch du repli / conversion RG16F | 0 | 0 |

En MSAA x2, l'erreur médiane globale par paire est de 0,138–0,204 pixel,
sur 574–672 points par paire. Le groupe des instances suivies reste une
petite cohorte mesurée, pas une validation exhaustive de chaque feuille.
Environ 0,2–1,8 % des pixels des captures MSAA restent en repli. Aucun point
de ce petit masque n'a passé les filtres de correspondance indépendants :
sa précision n'est donc pas certifiée par ces métriques.

La session MSAA termine plus de 11 880 évaluations GPU, puis F7 OFF et une
fermeture normale (code 0). Les 2 400 pixels opaques des barres vie/magie sont
bit à bit identiques dans les captures finales ON/OFF (images de jeu à des
instants différents ; les parties translucides ne sont pas ainsi certifiées).
Les six fichiers source, backup et runtime de sauvegarde sont inchangés,
vérifiés après fermeture dans `nr-save-verification.json` ; le HUD est consigné
dans `nr-hud-verification.json` et l'achèvement dans `nr-session-result.json`.

Les 62 contrôles CPU/GPU passent dans
`out/gow3-validation/20261028T190311499797Z/result.json`. Le test History inclut
changement d'ordre, mouvement commun, ambiguïté, doublon, frame absente,
téléportation et séparation des identités. Les quatre variantes motion/plain
VS/FS passent `spirv-val --target-env vulkan1.3`. Le nouveau compute de
préparation NR MSAA x2 passe aussi cette validation puis l'évaluation réelle.

Un lancement intermédiaire a planté avant le chargement de Gaia dans libc++.
Sa cause n'est pas établie ; il n'est pas présenté comme résolu par le
correctif. Les relancements suivants ont atteint Gaia, les captures et des
milliers d'évaluations. Les logs de cet incident restent dans la session
`20261028T185851959537Z`.

## Ce que NR change visuellement ici

Les montages `nr-capture/visual-crops.png` comparent **la même frame**, avant
et après NR, avec zoom nearest 2x, sans ajout de netteté externe. Dans la
session MSAA x2, la colonne gauche contient déjà MSAA x2, la droite ajoute NR.
Il s'agit de la scène avant composition finale/HUD, pas d'une paire de captures
de monde prises à des instants différents.

- Les microtextures de roche ressortent davantage ; les creux sont plus
  contrastés et les petites marques claires plus tranchées.
- Le feuillage et les bords d'armures sont davantage découpés. Certaines
  textures prennent un aspect plus accentué et granuleux.
- L'éclairage général et la disposition de la scène changent peu dans ces
  captures. On n'observe pas une transformation visuelle majeure du chapitre.

En MSAA x2, la différence RGB absolue moyenne vaut 0,0135–0,0140 sur 0–1,
soit environ 3,4–3,6 niveaux sur 255. Le changement moyen de luminance reste
entre −0,00020 et +0,00026. L'énergie des gradients augmente de **32–35 %** :
cela mesure une accentuation des variations spatiales, pas « 35 % de qualité ».

Le résidu temporel médian sur patches, recalés par les correspondances
indépendantes, augmente de **1,49 à 1,66 fois** avec NR dans la séquence MSAA.
Cette mesure inclut variation d'éclairage, interpolation et occultation ; elle
ne constitue pas un test de scintillement complet. Elle ne justifie toutefois
pas de revendiquer une meilleure stabilité temporelle. Le contraste plus fort
peut accentuer le fourmillement. Les mouvements corrigés aident le modèle,
mais ne prouvent pas à eux seuls que sa sortie est plus fidèle.

Le bénéfice observé est donc surtout un rendu des matières plus contrasté,
avec un gain subjectif modeste. Le prochain réglage utile est de comparer une
structure locale NR plus douce, plutôt que d'ajouter encore de la netteté.
MSAA x2 est conservé ; NR reste commutable et expérimental.

## Reproduire

La provenance du SDK/runtime officiel NR 310.9.16.0 et la compilation de la
passerelle restent dans [le rapport principal](gow3-dlss5-nr-experiment.md).

```powershell
out/pyenv/Scripts/python.exe -B scripts/gow3_diagnostic_session.py --seed-data C:/Users/jerem/Documents/Codex/2026-10-26/task/spartanport-dlaa-msvc/out/gow3-input-collection-f586605 --game-dir C:/Games/God.of.War.III.Remastered.RECOMP.v2.1.5-KaOs/CUSA01623 --nr-scene --resolution native --fullscreen --launch --wait
```

Ce profil force `SPARTAN_NO_MSAA=0` et conserve les sauvegardes source.
Charger Gaia, F7 pour NR, F12 pour une capture du rendu final avec HUD.
Les données, SHA-256 et rapports restent dans la nouvelle session sous `out/`.

```powershell
python -B tools/analyze_gow3_nr_scene.py out/gow3-sessions/20261028T190936396733Z/nr-capture
python -B tools/report_gow3_nr_visual.py out/gow3-sessions/20261028T190936396733Z/nr-capture --crop-spec out/gow3-sessions/20261028T190936396733Z/nr-capture/crops.json
```

Les outils d'analyse requièrent NumPy et Pillow. Les crops ont des coordonnées
consignées dans `crops.json` et `visual-report.json`. Aucun pixel n'est généré.
Les autres chapitres, HDR, résolutions supérieures, fortes désocclusions,
associations ambiguës et coût GPU dédié restent à valider.
