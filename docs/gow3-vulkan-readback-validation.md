# Validation synthétique des copies Vulkan — 27 octobre 2026

Le test autonome `gow3-vulkan-readback-test` passe sur la RTX 5070 Ti. Il ne lie
que Vulkan et la bibliothèque C++, ne crée ni fenêtre ni surface et ne charge
aucun code ou fichier du jeu. Aucune intégration au runtime n'est effectuée.
Base du checkout : `f58660558a03e74af057f9cdfa556e31346bbba9`, avec les travaux
locaux antérieurs conservés. Aucun commit ou push.

## Plan exécuté et critères

Images synthétiques de 64 × 48 pixels, deux échantillons : couleur
`R8G8B8A8_SRGB` et profondeur/stencil `D32_SFLOAT_S8_UINT`. Chaque échantillon
reçoit une profondeur distincte et un motif couleur qui encode la frame,
l'échantillon et les coordonnées. Le canal alpha contient un code exact de
producteur, indépendant du token fourni à l'extraction.

Pour chacune de deux générations d'une même identité logique (42) :

1. Produire A dans les images source; extraire une référence; copier les images
   multisample dans des allocations possédées par la capture A.
2. Réutiliser les **mêmes images source** pour B; extraire sa référence; copier B
   dans d'autres allocations possédées par sa capture.
3. Écraser encore la source avec C, puis seulement extraire A et B depuis leurs
   copies. Extraire également C pour prouver que la source a changé.
4. Extraire explicitement les indices 0 et 1 avec `texelFetch(texture2DMS, …)`.
   Aucune résolution ni moyenne. Lire la couleur à travers deux vues du même
   backing : sRGB décodée et UNORM contenant les codes encodés normalisés.
5. Transférer les buffers GPU vers des buffers hôte; attendre la fence, invalider
   la plage mappée, puis lire. Les descripteurs et tokens sont propres à chaque
   extraction, jamais réécrits pendant une soumission.

Les barrières explicites relient écritures d'attachments → lecture shader de
référence → lecture de transfert → copie → lecture shader, puis écriture
storage → lecture de transfert → écriture staging → lecture hôte. Les layouts
et aspects profondeur/stencil sont transportés explicitement. Une seule queue
graphics+compute est utilisée; il n'y a pas de transfert entre familles.

Une timeline semaphore initialement non signalée retarde chaque soumission.
La fence doit rester non signalée pendant une attente de 50 ms; une lecture
anticipée doit être refusée sans mapping. Le propriétaire du registre est
retiré pendant cette attente : la soumission conserve ses images jusqu'à la
fence. Après lecture et destruction du batch, les références faibles expirent
et le compteur d'allocations doit revenir à zéro. En cas de timeout GPU après
libération, le programme quitte sans détruire volontairement des objets en vol;
il ne boucle pas et ne lance pas de nouvelle instance.

Critères numériques et d'identité :

- Profondeur CPU indépendante construite avec des fractions binaires : erreur
  ≤ `1e-7`; coordonnées et identités exactes.
- Référence GPU avant copie versus extraction après écrasement : comparaison
  **bit à bit** de tous les résultats pour A et B, profondeur et couleurs incluses.
- Encodage RGB : écart ≤ 1,01 code 8 bits par rapport à la fonction sRGB idéale;
  alpha : code exact. La valeur UNORM doit correspondre à un entier 8 bits.
- Décodage sRGB : le réencodage CPU de la valeur retournée doit retrouver le même
  code 8 bits (distance ≤ 0,50001 code). C'est un critère de conservation choisi
  pour cette capture, **pas** une déclaration de conformité générale Vulkan.
- Contrôles négatifs : mauvaise frame malgré métadonnée falsifiée, échantillons
  inversés, génération incorrecte, profondeur moyennée et codes sRGB pris pour
  des valeurs linéaires doivent être détectés. Le contenu C présenté sous le
  token de A doit aussi être rejeté.

La distinction entre RGB encodé sRGB et alpha normalisé est décrite dans
[la définition officielle de VkFormat](https://docs.vulkan.org/refpages/latest/refpages/source/VkFormat.html).
Ce test n'assimile pas les valeurs flottantes décodées aux octets encodés.

## Résultats réels

Exécution finale : `out/dlaa-vulkan-readback/run-03/summary.json`, terminée à
`2026-10-27T14:58:13.7286379Z`, code de sortie **0**.

| Contrôle | Résultat |
| --- | --- |
| GPU | NVIDIA GeForce RTX 5070 Ti, Vulkan 1.4.357, pilote brut 2604679168 |
| Capacités | sampleRateShading et timeline semaphore présentes; combinaisons de formats/usages à deux échantillons disponibles |
| Compilation | Clang existant, Ninja `-j2`, réussie; 3 shaders validés par `spirv-val --target-env vulkan1.2` |
| Contenus | 4 frames retenues et 2 sources écrasées; 36 864 échantillons vérifiés contre la référence CPU |
| Copie exacte | 4 comparaisons complètes bit à bit avant/après copie réussies, soit 24 576 échantillons supplémentaires de référence |
| Deux échantillons | 18 432 pixels avec profondeurs différentes entre indices 0 et 1 |
| Retard GPU | 49,438 et 50,442 ms observés; fences non signalées, 2 lectures prématurées refusées |
| Durée de vie | 2 retraits de propriétaire pendant attente, ressources retenues puis libérées; zéro octet explicite restant |
| Contrôles négatifs | 36 864 détections pour chacune des 5 catégories, plus 2 sources écrasées détectées |
| Profondeur | erreur maximale **0** sur les motifs synthétiques |
| Encodage sRGB | écart maximal **0,559254454 code** |
| Décodage sRGB | écart flottant maximal **0,00187984599**; aller-retour maximal **0,303763462 code** |
| Allocations explicites | pic **4 153 344 octets** (3,96 Mio), plafond vérifié avant allocation 32 Mio; 32 allocations au total |
| Vecteurs hôte lus | pic **1 966 080 octets** (1,875 Mio), hors coût du pilote et du processus |
| Fichiers de pixels | **0**; les données synthétiques restent en mémoire |

La couche `VK_LAYER_KHRONOS_validation` est **absente**. Debug utils est présent;
aucun message d'erreur/avertissement n'a été reçu, ce qui ne remplace pas une
validation des synchronisations par cette couche. Aucune installation effectuée.
La mémoire hôte sélectionnée est cohérente; la branche d'invalidation explicite
a été appelée, mais un type de mémoire non cohérent n'a pas été exercé.

Les deux premières exécutions sont conservées (`logs/` et `run-02/`). Elles
échouaient sur le seuil initial de décodage flottant `5e-4`, pas sur une identité
ou profondeur erronée au point examiné. La dernière exécution mesure **41 088
composantes RGB** au-dessus de ce seuil. Le test final ajoute les références
avant copie et exige leur conservation bit à bit; il ne dissimule pas cet écart
du décodage matériel. Il vérifie séparément la conservation des codes 8 bits.

## Livrables et reproduction

- Source : `tests/test_gow3_vulkan_readback.cpp`.
- Shaders synthétiques : `tests/shaders/gow3_readback_fixture.{vert,frag,comp}`.
- Cible `EXCLUDE_FROM_ALL` dans `gpu/CMakeLists.txt`, liée à `Vulkan::Vulkan`
  uniquement; aucune source du runtime dans la commande de lien.
- Préparation : `out/validate-vulkan-readback.ps1` refuse un répertoire de sortie
  existant. Exécutions complémentaires : `out/run-vulkan-readback.ps1 -Run NN`
  refuse de remplacer un sous-dossier de résultat existant.
- Journaux finaux : `out/dlaa-vulkan-readback/run-03/{build.log,gpu.stdout.log,gpu.stderr.log,summary.json}`.
- Exécutable : `out/dlaa-vulkan-readback/bin/gow3-vulkan-readback-test.exe`.
  SHA-256 : `A15D568F629ADDCA889976D64DC1A3DA073A7E97CF4C1D56045D1D2D9A90EE93`.

La cible se compile par `ninja -j2 -C out/dlaa-native/gpu gow3-vulkan-readback-test`.
Elle reçoit uniquement le chemin `out/dlaa-native/gpu/gow3-readback-shaders`.
L'environnement du toolchain existant reste limité au processus PowerShell.
Les journaux et l'exécutable du dossier dédié occupent environ 0,92 Mio avant
l'ajout du manifeste; objets/SPIR-V de compilation sont dans le build partagé.

## Limites avant intégration

Ce succès établit le mécanisme de copie synthétique sur ce GPU, avec une queue,
des objets possédés et des barrières connues. Il ne prouve ni l'identification
pré-interface de la couleur du jeu, ni la rétention des objets de son cache,
ni ses alias, changements de backing, commandes helper, ordres interthreads,
présentations, annulations ou pertes. Une génération logique distincte et une
nouvelle allocation sont testées; aucune réutilisation spécifique de la valeur
numérique d'un handle Vulkan n'est supposée ou imposée. Le stencil est copié
avec la profondeur, mais ses valeurs ne sont pas relues ni vérifiées.

Le retard injecté précède la soumission complète; il ne reproduit pas toutes
les courses possibles du runtime. Aucun timeout matériel, device loss ou cas
multi-queue n'est provoqué. La copie exacte est vérifiée pour ces motifs, pas
pour tous les octets et tous les formats possibles.

L'intégration éventuelle doit d'abord résoudre les propriétaires de backing et
de vues, la version du contenu et le point de copie avant mutation, décrits dans
`docs/gow3-readback-plan.md`. Elle devra conserver le token de production jusqu'à
la clôture ordonnée et à l'achèvement GPU, et rejeter toute provenance ambiguë.
Cette architecture n'a pas été modifiée ici. Le test ne valide aucune convention
de projection GoW3, reprojection visuelle, entrée objet ou fonctionnalité DLAA.
