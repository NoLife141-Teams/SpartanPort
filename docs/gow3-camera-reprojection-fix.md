# Reprojection caméra : correction du contrat et du diagnostic

27 octobre 2026, Toronto. Le fond lointain de deux paires réelles suit la
caméra avec une erreur médiane inférieure à 0,21 pixel. Le premier plan de
Gaia ne peut pas être reprojeté par la caméra seule. **Le DLAA reste désactivé.**

## Problème identifié et correctif

Le diagnostic initial comparait toute une zone de profondeur lisse à une
hypothèse de monde immobile. Une profondeur lisse ne signifie pas un objet
immobile : le sol porté par Gaia, la végétation et les personnages peuvent
bouger, parfois en compensation du déplacement de la caméra. Ce mélange
masquait la bonne correspondance du fond.

Le nouvel analyseur obtient d'abord des correspondances dans les images, sans
consulter les vecteurs prédits. Il emploie une grille fixe, des patches de
luminance de 15 × 15 pixels, une recherche de ±20 pixels, un contrôle
d'ambiguïté et une vérification aller-retour. Il compare ensuite les hypothèses
sur ces mêmes points. Les seuils ont été explorés sur la première paire, puis
conservés pour une nouvelle session indépendante de Gaia.

Le contrat corrigé est explicite : matrices VP brutes column-major,
`MinusWToW`, viewport capturé `[0,1080,1920,-1080,0,1]`, et coordonnées de
samples en framebuffer. Il faut reconstruire **z_clip/w = 2D−1**, appliquer
`VP_previous × inverse(VP_current)`, puis le viewport précédent. Sa hauteur
négative fournit déjà le sens de Y. Une transformation d'objet nécessite en
plus `M_previous × inverse(M_current)`; les palettes animées demandent leur
propre historique.

Implémentations : `gow3_reprojection.h` et `tools/gow3_reprojection.py`.
Le pont CPU `gow3-reproject-points` exécute le contrat C++ sur les points des
captures réelles. L'ancien helper `Reproject` reste une référence synthétique
ZeroToW; il ne doit pas recevoir directement les VP brutes de ces captures.
Le contrat corrigé est utilisé par les outils CPU. Son raccordement au compute
GPU GoW3 reste à réaliser avant un dispatch temporel dans le jeu.

## Résultats réels

| Session / intervalles | Correspondances lointaines | Caméra MinusW : médiane / P90 | Sans reprojection : médiane | Y incorrect : médiane | Temps inversé : médiane |
| --- | ---: | ---: | ---: | ---: | ---: |
| `20261028T014254344866Z` / 2203–2204 | 116 | 0,200 / 0,499 px | 2,413 px | 1,984 px | 4,897 px |
| `20261028T015621472365Z` / 4502–4503 | 106 | 0,172 / 0,557 px | 1,779 px | 3,460 px | 3,541 px |

Le contrôle ZeroToW donne des médianes de 0,257 et 0,344 pixel sur le fond.
Le mode MinusW est établi par l'état et les permutations inspectés; il n'est
pas choisi seulement parce qu'il minimise une métrique. Le P90 du contrôle
ZeroToW sur la seconde paire (0,551) est légèrement meilleur que MinusW :
cette métrique isolée ne constitue pas un test de convention.

Sur les points proches (`D < 0,99`), la caméra seule donne **9,048 puis
9,569 pixels** d'erreur médiane; l'absence de reprojection donne 0,142 puis
0,252 pixel. C'est compatible avec un premier plan dont le mouvement propre
compense largement la caméra. Les constantes d'objet changent effectivement
entre les frames. La profondeur seule ne fournit cependant pas une identité
de producteur par pixel : cette explication n'est pas une classification
exhaustive de tous les objets et effets.

Le C++ et Python concordent sur **665 correspondances** : écart maximal
inférieur à 5,3e-10 pixel et à 1,6e-15 pour la profondeur. Cette concordance
vérifie les implémentations; les appariements d'images apportent la comparaison
indépendante aux pixels.

Les positions MSAA ne sont pas mesurées : les outils utilisent explicitement
une approximation au centre du pixel pour le sample 0. Le groupe lointain
`0,995 ≤ D < 0,9999` est un candidat de fond fixe, pas un masque d'objet prouvé.
Deux paires de Gaia ne valident pas toutes les caméras du jeu.

## Nouvelle collecte et intégrité

Même probe que la première capture, SHA-256
`fa2752b082381906e9601bd8126a519b327c96c2bf07b262b85ccaaeb806dfb6`.
La sauvegarde Gaia est chargée par S, Espace et Entrée après observation de
chaque écran, sans son, vie infinie et intro sautée. L'instance est fermée
après les deux extractions; les six fichiers source et les backups sont intacts.

| Intervalle | Tick terminé | UID depth / couleur | SHA-256 du fichier brut |
| --- | ---: | --- | --- |
| 4502 | 8821 | 21 / 7 | `b2e643c80d2294045da17aaaa16a6a86afc9c6975b005916ef8a6d5825ce79a9` |
| 4503 | 8823 | 21 / 7 | `852e6ec128642edeccf0bea6f86fe7d604f250a3351c1c55c9f5c2d6622ea7b2` |

Les UID ont changé; le filtre ne les code pas en dur. Les deux fichiers font
82 944 000 octets chacun. Le nouvel intervalle contient 252 puis 253 snapshots
caméra, tous présents et cohérents. Le journal des **3 243 dessins** est complet
dans son périmètre graphique instrumenté. La première paire conserve son
dessin à provenance incomplète; elle n'a pas été réécrite.

Le premier lancement restreint de la nouvelle session a échoué à réserver
l'espace mémoire PS4 (erreur 487). Le lancement natif autorisé a réussi;
les logs des essais restent dans la session. Aucune modification système.

## Ce qui reste avant d'activer le DLAA

| Entrée / étape | État constaté et travail requis |
| --- | --- |
| Caméra et viewport | Contrat CPU explicite testé et cohérence du fond confirmée sur deux paires; étendre aux autres caméras, coupures et changements de taille. |
| Mouvements d'objets | Bloquant sur Gaia. Capturer les identités stables et historiques des modèles/palettes; 351 observations de la seconde paire utilisent la branche palette. |
| Producteurs depth | Cinq VS écrivent/effacent la profondeur retenue dans la seconde paire; seul le shader caméra suivi fournit ses matrices. Valider les autres producteurs, contours et disocclusions. |
| Compute de mouvement GoW3 | À raccorder au nouveau contrat. `CameraMotion::OnConstants` attend encore l'ancien bloc view/projection avec offsets 8, 52 et 180; il ne décrit pas le bloc GoW3 de 36 mots. |
| Couleur avant HUD | Les aperçus n'affichent pas de HUD, mais la chaîne complète pré-HUD reste à établir. Conserver le décodage sRGB et la permutation BGRA de la vue, puis recomposer l'interface. |
| MSAA, jitter et historique | Mesurer les positions des samples, choisir une profondeur et une couleur de même surface, vérifier le jitter, les resets et les coupures caméra sur des séquences. |
| NGX/DLAA GoW3 | Intégration spécifique, synchronisation et évaluation temporelle à réaliser une fois les entrées précédentes validées. Aucun DLAA GoW3 exécuté ici. |

La prochaine étape utile est l'historique des transformations/palettes et leur
association aux pixels du premier plan, avec un test GPU des vecteurs obtenus.
Changer le signe Y ou forcer ZeroToW pour améliorer une moyenne globale ne
corrigerait pas ce mouvement propre.

## Tests et reproduction

- Nouveau banc C++ : **32 cas analytiques**, MinusW/ZeroToW, signes de hauteur,
  viewport décalé et plage depth non triviale; contre-exemple d'objet mobile et
  correction par son historique connu.
- Cinq tests Python : projection analytique proche, historique d'objet, entrées
  invalides, translation d'image indépendante de la caméra et patch ambigu.
- Validateur CPU complet : **60 étapes réussies**, dont le nouveau banc,
  `out/gow3-validation/20261028T020154568140Z/result.json`. Les shaders GPU et
  l'extracteur runtime n'ont pas changé dans cette étape.

```powershell
cmake --build out/verification/dot-build --parallel 2 --target gow3-calibrated-reprojection-test gow3-reproject-points
python -B -m unittest discover -s tests -p test_gow3_calibrated_reprojection.py -v
python -B tools/validate_gow3_pixel_reprojection.py <session>
```

Le Python des deux dernières commandes nécessite NumPy. Les résultats sont
créés exclusivement dans `reprojection-correspondences.json`; le rapport v1
reste conservé. La synthèse de disponibilité est
`out/gow3-reprojection-investigation/readiness.json`, avec `ready_to_enable_dlaa=false`.
