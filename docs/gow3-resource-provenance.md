# GoW3: provenance des ressources, sans pixels

Évolution du protocole : voir [préfixe réel et correction du budget](gow3-provenance-window-budget.md). Le probe actuel utilise le schéma v2 qui omet les relevés vides redondants ; le schéma v1 ci-dessous décrit la première collecte conservée.

Instrumentation locale sur `f58660558a03e74af057f9cdfa556e31346bbba9`, non commitée. Elle décrit les objets observés et les invalidations connues du cache. Elle ne conserve ni les images GPU ni leur contenu. Elle ne valide pas la couleur avant interface, la reprojection ou le DLAA.

## Activation et bornes

Opt-in supplémentaire strict `SPARTAN_GOW3_PROVENANCE=1`, avec les quatre options existantes `SPARTAN_GOW3_INPUT_DIAGNOSTICS`, `SPARTAN_GOW3_PAIR_METADATA`, `SPARTAN_GOW3_SHADER_METADATA` et `SPARTAN_GOW3_GROUP_METADATA` à `1`. Aucun de ces réglages n'est activé par ce changement. Fichier de sortie et déclencheur absolus existants, quatre suffixes `stationary`, `motion`, `pause`, `resume`, démarrage différé sur flip accepté et deux intervalles par fenêtre. Aucune nouvelle exécution du jeu effectuée.

Les plafonds restent 8192 draws et **16 Mio physiques par fenêtre**, donc au plus 64 Mio pour les quatre fichiers. Réservation de 4096 octets pour le bilan, création exclusive `wbx`, aucun écrasement. Les nouvelles métadonnées augmentent la taille et peuvent tronquer une fenêtre avant ses deux intervalles : vérifier le bilan, sans généraliser au segment perdu. Le plafond n'a pas été augmenté pour masquer ce risque.

File interthreads : 64 événements. Table de bindings : 64 positions de `image_infos`, samplers inclus dans l'indexation mais non relevés comme images. Recherche de vue : 16 backings et 256 vues maximum par observation. Tout dépassement est explicite. Pas d'allocation ni d'attente dans le producteur d'invalidation.

## Identités et chemins

Une vue native est identifiée par le triplet `(image_uid, backing_ordinal, view_ordinal)` : UID non nul du véritable `Image`, ordres à partir de 1 dans les conteneurs append-only de cette image. Le slot de l'image et de la vue ainsi que les handles sont des labels complémentaires. La croissance de `SlotVector` peut déplacer l'objet, et une destruction peut recycler ses slots/handles ; elle ne recycle pas son UID. Changer de backing puis revenir au précédent retrouve la même identité d'objet, **pas une version de contenu**.

Preuves locales de la règle append-only : créations dans `image.cpp` et `Runtime::SetBackingSamples`, création de vues dans `Image::FindView`, suppression seulement dans le callback de `TextureCache::DeleteImage`. Aucun changement à ces règles de durée de vie.

`RegistryGeneration` désigne le registre du cache. `alias_authority_generation` est le compteur existant d'autorité d'alias ; il ne recense pas toutes les écritures. Aucun compteur de contenu nouveau n'est inventé.

Le chemin normal et le helper relèvent la vue **après** la résolution finale, le redirect depth, le rebind et l'éventuel redirect de sampling. Le chemin memo relève la vue réellement réutilisée. Le helper écrit uniquement dans la table auxiliaire, selon la même propriété que `image_infos`; il n'appelle pas le collecteur de fichiers. Le propriétaire initialise le token draw/interval avant Fork et relit après Join, y compris lors d'un repli partiel sur le propriétaire. La conversion en bindings/éléments utilise les écritures de descripteurs effectives et vérifie token, intervalle et handle observé. Pas d'association au dernier compteur global.

Les attachments proviennent du `RenderState` effectif, même lorsque BeginRendering est mémorisé. La recherche porte sur toutes les vues déjà créées de l'image concernée, donc peut identifier un backing non courant. Une vue proxy/externe sans correspondance reste inconnue ; on ne lui attribue pas le contenu de l'image native.

Les formats et aspects de vue sont mémorisés depuis le `ImageViewCreateInfo` effectivement soumis à Vulkan, après les conversions depth/stencil, storage sRGB et format supporté. Cela ajoute huit octets de métadonnées à `ImageView`, même quand le diagnostic est désactivé ; les arguments de création et le rendu ne changent pas. Les ranges et formats d'image sont copiés depuis les structures existantes. Aucune requête Vulkan supplémentaire.

## Synchronisation et invalidations

`DescribeProvenance` s'exécute uniquement sur le propriétaire des bindings ou son helper, avec un ID déjà résolu. Il ne relit jamais un ID provenant d'un journal. Il tente le mutex existant du cache **sans attendre** pour observer flags et autorité d'alias face aux invalidations invitées. Si occupé : `CacheBusy`, association inconnue et fenêtre non certifiée complète. Le parcours borné ne fait aucun appel Vulkan, allocation, callback, Join ou acquisition du verrou des opérations différées. La durée de vie reste celle déjà assurée par les threads du renderer ; ce diagnostic n'ajoute pas de lease.

La file appartient au cache et ne référence pas le collecteur. Les producteurs copient l'UID déjà connu dans les hooks existants : enregistrement, désenregistrement, invalidation CPU réelle ou partielle de page, maybe-dirty, invalidation GPU de même base, notification/application des backing writes, publication d'autorité d'alias, relation entre alias et unmap. Une notification de plage dont l'image n'est pas encore résolue conserve UID zéro et slot inconnu, sans lookup sur le thread étranger. `CpuOverlap` signale le chemin de faute existant ; il ne prouve pas quelles valeurs ont finalement été écrites. `AliasAuthorityPublished` n'est pas une preuve d'exécution d'un draw.

Un `atomic_flag` protège uniquement la petite file. Un producteur en contention perd son événement et incrémente le compteur ; il n'attend jamais. Le consommateur copie les valeurs puis libère ce verrou avant tout accès au cache ou au fichier. Aucun nouvel ordre de verrous croisé, aucun changement de synchronisation GPU.

Les événements portent leur UID d'origine, leur éventuel UID d'alias, le slot, la génération du registre si connue et la plage déjà disponible. `attempt` est un identifiant unique de tentative CPU, pas un ordre total GPU : des tentatives concurrentes peuvent être retenues dans un autre ordre. Le relevé les rattache à un **point d'observation** explicite (`before_bindings`, `after_recording`, `before_accepted_flip`) avec draw/intervalle/flip, et non à une cause supposée. Un producteur en cours peut être relevé au drain suivant. Ce mécanisme ne prouve donc pas de frontière causale entre une faute CPU et un draw GPU.

Les compteurs de tentatives et pertes sont cumulatifs depuis l'activation. Le drain précédant le flip de déclenchement enregistre une baseline ; les pertes de démarrage ne sont pas confondues avec celles de la fenêtre. Le bilan devient incomplet en cas de nouvelle perte, drain occupé, table tronquée, vue inconnue ou sortie saturée. `provenance_event_records_dropped` compte les événements et leur en-tête de lot non écrits (une unité supplémentaire par lot). Les données synthétiques servent à tester ce comptage ; elles ne mesurent pas le taux de perte du jeu.

### Schéma `gow3-provenance-v1`

Les entiers 64 bits du nouveau bloc sont des nombres JSON exacts : utiliser un lecteur conservant `uint64` (Python `int`, par exemple), pas une conversion en flottant JavaScript. Les anciens labels hexadécimaux du journal restent inchangés.

`resolution` : 0 inconnue, 1 vue nulle, 2 vue native résolue, 3 image non enregistrée/UID nul, 4 limite de parcours, 5 cache occupé. `path` : 0 normal, 1 memo, 2 helper. Les attachments ont leurs champs séparés.

Chaque événement est `[attempt, uid, slot, related_uid, registry_generation, reason, address, size]`. `reason` : 0 Registered, 1 Unregistered, 2 CpuOverlap, 3 CpuPageUntrackHead, 4 CpuPageUntrackTail, 5 MaybeCpuDirty, 6 GpuBaseInvalidation, 7 BackingWriteApplied, 8 AliasAuthorityPublished, 9 AliasMember, 10 Unmap, 11 BackingWriteNoted.

## Vérifications et limites

`gow3-resource-provenance-test` utilise le vrai `Common::SlotVector`, le vrai `BindHelper` et le vrai `RecordChunk`, avec les nouvelles structures de production. Seul le renommage OS du thread helper est neutralisé dans le test. Couverture : slot réutilisé avec UID nouveau malgré handles identiques, déplacement, backings et vues distincts, invalidation et alias, transport par valeur, helper et repli normal après Join, memo, refus de token/intervalle/vue incorrect, reset entre draws, opt-in désactivé, bornes, quatre producteurs concurrents, pertes exactes, baseline différée, drain occupé, sérialisation, plafond physique et non-écrasement.

Ces tests CPU ne construisent pas de `TextureCache`/`Image` réel avec device Vulkan et ne prouvent pas l'association de toutes les vues du jeu. Le probe intégrant le code est compilé séparément, sans exécution graphique. Les tests de capture existants vérifient aussi le maintien exact de leur format et de leurs limites lorsque la provenance est désactivée.

Validation finale du 27 octobre 2026 à 15:44:06 UTC : compilation Clang 23.1.3 / Ninja avec `-j2`, puis **5 suites réussies** : `gow3-resource-provenance-test`, `gow3-capture-bytes-test`, `gow3-pair-capture-test`, `gow3-group-capture-test`, `gow3-readback-primitives-test`. Assertions actives (`-UNDEBUG` après `-DNDEBUG`). Lors du stress concurrent synthétique : 16 000 tentatives, 3257 observations, 12 743 pertes explicitement comptées. Ce stress volontaire n'est pas représentatif de la charge du jeu. Un cas ajouté lors de la revue finale confirme aussi qu'un attachment effectif stencil seul, sans observation depth et avec cache occupé, rend le bilan incomplet.

Probe : `out/dlaa-resource-provenance/bin/spartanport-probe.exe`, SHA-256 `E2210B544CDE14AF41AD78336F172818F555F19C052ABDBB7168F80B8C0A3A72`. Résultats et logs finaux : `out/dlaa-resource-provenance/run-verified/`. Les essais antérieurs (`run-final` inclus) sont conservés. Le premier build, arrêté par l'en-tête SlotVector manquant dans le test puis corrigé, reste consigné dans `logs/build.log`. Aucun refus de permission, aucun test restant en échec. Le message de refus d'écrasement dans le test est un résultat attendu de son cas de collision.

Les hashes comparés au début de cette étape confirment que `slot_vector`, `image`, `vk_runtime`, `vk_scheduler` et `vk_staging_buffer_pool` sont inchangés (neuf fichiers). La création de vue ne change que ses deux champs de métadonnées ajoutés ; les modifications de cache et rasterizer sont les points d'observation décrits ci-dessus. Aucun jeu lancé/fermé, aucune sauvegarde lue ou modifiée, aucun upload ou commit/push.

La couverture n'est pas un suivi exhaustif des writes : compute, transferts, HLE, clears non draw et modifications de contenu n'ont pas reçu de nouveaux compteurs. Une invalidation perdue rend une absence non concluante. Les pixels, la rétention GPU, les barrières de copie, le tick de soumission et la couleur pré-interface restent hors de cette étape. Leur intégration demanderait encore les décisions de sûreté décrites dans `gow3-runtime-readback-review.md`.
