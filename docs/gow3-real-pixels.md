# Pixels réels GoW3, 27 octobre 2026 (Toronto)

Deux intervalles de la sauvegarde **Gaia / God (Normal) / 00:04:35** sont
capturés sur la RTX 5070 Ti. Ce sont les pixels des vues échantillonnées par
la composition retenue, avec les samples 0 et 1 séparés. Le DLAA reste désactivé.

Session : `out/gow3-sessions/20261028T014254344866Z`.
Probe de cette session : SHA-256
`fa2752b082381906e9601bd8126a519b327c96c2bf07b262b85ccaaeb806dfb6`.
Chaque fichier brut contient 82 944 000 octets, soit **165 888 000 octets**
au total (158,2 Mio), en 1920 × 1080.

| Intervalle | Dessin | Tick GPU soumis et terminé | Flip de fermeture | SHA-256 du brut |
| --- | --- | --- | --- | --- |
| 2203 | 2424864 | 4408 | 2203 | `b27a04bae3e00bee2fd10241d2e481b94cf4490dbedd0c24a884bd96f10c6242` |
| 2204 | 2426450 | 4410 | 2204 | `7ace569c802bd90ba037347b91bb45220a78260448cf28038299eb7657149638` |

VS `475a41468b1a1fbc`, PS `475a4146e3a74097`, permutations sélectionnées
`efd04d8023fec408` et `efd0530963dc1d55`. Binding 1 : profondeur D32S8,
UID 7, backing 1, vue 2. Binding 2 : couleur R8G8B8A8 sRGB, UID 10,
backing 1, vue 1. Ces UID sont propres à cette session et ne sont pas codés
en dur dans le filtre.

## Lecture et synchronisation

Le compute diagnostique s'insère après la vidange naturelle des barrières,
avant le dessin de composition, uniquement hors scope de rendu. Aucune
fin de rendu supplémentaire, transition de source, conversion de samples,
résolution MSAA, copie d'image ni attente globale `Finish()` n'est ajoutée.
Deux dépendances mémoire encadrent ses lectures; une barrière de sortie rend
les écritures du buffer visibles à l'hôte.

Sous le verrou du cache, les deux identités, backings courants, vues, formats,
usages, aspects, sous-ressources, dimensions, layouts et générations d'alias
sont revalidés. Le verrou exclut `DeleteImage` pendant l'enregistrement.
La destruction existante est différée au tick courant ou ultérieur;
l'extraction est donc enregistrée avant cette destruction. Aucun pointeur de
cache ne sort du verrou. Le callback d'achèvement manipule uniquement les
sorties possédées et les métadonnées copiées, sans rentrer dans le cache.

Le tick est pris sur le consommateur ordonné lors de l'enregistrement.
L'observateur de soumission reçoit le tick effectivement signalé et le résultat
de `queue.submit`. Le callback différé ne lit la mémoire qu'après achèvement
GPU. L'export attend aussi le flip accepté qui ferme le même intervalle et
une caméra cohérente. Les fichiers sont créés exclusivement et la mémoire
mappée est invalidée avant lecture.

Ce chemin expérimental conserve volontairement ses deux allocations et ses
objets Vulkan jusqu'à la sortie du processus, y compris si la fermeture ne
vide pas les callbacks. Plafond mémoire : 256 Mio; plafond fichiers bruts :
192 Mio. Allocation observée : 165 888 000 octets. Ce n'est pas un collecteur
continu. Le drapeau est absent des configurations ordinaires.

## Format et contrôles

Le tableau brut little-endian float32 a la forme `[1080,1920,2,5]` :
RGBA linéaire puis profondeur, pour chaque sample de chaque pixel. La couleur
est décodée sRGB **par le GPU**, avec la permutation de la vue effectivement
appliquée : **B,G,R,A**. Ce ne sont pas les octets RGBA stockés. Le depth est
lu dans R (vue R,0,0,1); le stencil n'est pas exporté.

`pixels/pixels.jsonl` contient les résultats runtime;
`pixel-summary.json` vérifie tailles, empreintes, identités et permutation du
dessin, flip, matrice et source de profondeur des snapshots caméra.

Les deux intervalles contiennent chacun **213 snapshots caméra**, sans manque
ni conflit et avec une seule matrice VP par intervalle. Les valeurs exportées
sont toutes finies. Profondeur : [0,967230618; 1] puis [0,967259228; 1].
Les samples de profondeur diffèrent sur 1 771 980 puis 1 772 793 pixels;
la couleur diffère sur 57 645 puis 57 227 pixels. Une petite différence depth
peut venir de la pente d'une surface; ces nombres ne sont pas des comptes
de contours ou de disocclusions.

Les PNG `interval-*-color-sample0.png` montrent le sample 0 avec encodage sRGB
d'affichage. Les PNG depth étirent le contraste entre les quantiles 1 % et
99 %; ils ne représentent pas une distance linéaire. Aucun HUD n'est visible
dans l'aperçu couleur examiné; cela ne prouve pas une couleur pré-HUD universelle.

Le journal global de 3 171 dessins est **incomplet en provenance** : dessin
2423386, deux descripteurs `CacheBusy` (résolution 5). Pas de perte de dessin
ni de snapshot caméra. Cette perte concerne un autre dessin que les deux
extractions validées. Elle reste explicitement présente dans
`diagnostic-summary.json` et interdit de certifier toutes les dépendances.

## Diagnostic de reprojection

`reprojection-probe.json` compare des hypothèses sur une grille fixe et un
masque de profondeur lisse. Centres de pixels approximatifs, échantillonnage
au plus proche, objets mobiles présents; positions MSAA non mesurées.

| Hypothèse | Points valides | Erreur depth médiane | Erreur RGB linéaire médiane |
| --- | ---: | ---: | ---: |
| MinusW, viewport négatif | 94 423 | 1,0743e-5 | 0,0023193 |
| ZeroToW, viewport négatif | 94 608 | 2,2125e-6 | 0,0020955 |
| Contrôle sans reprojection | 94 628 | 7,1526e-7 | 0,0010579 |

La reprojection caméra seule n'améliore pas ce contrôle sur cette paire.
Le contrôle transposé ne produit aucun point valide. **La projection et les
vecteurs de mouvement ne sont donc pas validés.** Ne pas choisir ZeroToW
uniquement parce qu'il est meilleur que MinusW dans cette comparaison.
Il reste à expliquer les transformations effectives et les producteurs de
profondeur, traiter les mouvements d'objets et établir le point pré-interface.

## Essais et reproduction

Le premier essai `20261028T013953742285Z` a été refusé avant allocation,
car la couleur avait une permutation BGR exclue par le garde-fou initial.
Les deux résultats d'échec sont conservés. Le banc GPU a ensuite été étendu
à une vue BGR avec alpha constant, comparée bit à bit à la référence;
l'extracteur déclare désormais les composantes réelles de la vue.

Le banc exécute quatre extractions de production sur deux générations,
vérifie les samples contre une référence indépendante après écrasement des
sources, bloque le GPU par sémaphore, refuse les lectures prématurées et
une troisième allocation. Le passage final du validateur réussit : 61 étapes,
58 tests Python, 49 exécutions CPU et deux bancs Vulkan. Preuves :
`out/gow3-validation/20261028T014257759366Z/`. La couche de validation Vulkan
reste indisponible; aucun passage avec cette couche n'est revendiqué.
Six tests Python supplémentaires de l'analyseur passent avec le runtime
NumPy/Pillow : associations valides, flip falsifié, UID recyclé, tick absent,
fichier tronqué et profondeur non finie. Ils sont exécutables séparément avec
`python -B -m unittest discover -s tests -p test_gow3_pixel_analysis.py -v`.

La session utilise le lanceur isolé existant avec `--pixels --launch`.
L'agent observe le menu, presse S, observe Load, presse Espace, vérifie Gaia,
puis confirme avec Entrée. Après chargement et stabilisation caméra, créer
exclusivement `capture.stationary` arme les métadonnées et les pixels des
deux intervalles suivants. Il n'y a pas de rejeu clavier chronométré aveugle.

Analyser avec un Python contenant NumPy et Pillow :

```powershell
python -B tools/analyze_gow3_session.py <session>
python -B tools/analyze_gow3_pixels.py <session>
python -B tools/probe_gow3_pixel_reprojection.py <session>
```

Les sorties d'analyse sont exclusives : ces commandes s'appliquent à une
session non encore analysée. Conserver les rapports et les preuves existantes.
Les deux instances de test ont été fermées. Les six fichiers de sauvegarde
source et leurs backups sont identiques aux empreintes initiales.
Les vérifications sont conservées dans `save-verification.json` pour chacune
des deux sessions. Aucun processus `spartanport-probe` ne reste actif.
