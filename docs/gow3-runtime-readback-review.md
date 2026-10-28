# Revue du runtime avant intégration de la capture

État historique avant implémentation. Pour l'extraction désormais intégrée et
les deux intervalles réels de Gaia, voir [le rapport de pixels](gow3-real-pixels.md).

27 octobre 2026, checkout isolé, base `f58660558a03e74af057f9cdfa556e31346bbba9`.
Revue de sources et tests CPU uniquement. Aucune copie GPU ajoutée au runtime,
aucun jeu lancé, aucune sauvegarde consultée, aucun commit/push.

## Décision proposée

Une première étape **de provenance sans pixels** est réalisable localement,
en ajoutant des résultats POD bornés aux résolutions de bindings existantes.
Elle doit précéder le choix d'une couleur pré-interface. Les points de mutation
sont détaillés ci-dessous; ils ne sont pas modifiés par cette revue.

La copie complète n'est pas encore démontrée intégrable par un simple hook.
Trois contrats restent à établir : rétention de la source face à sa suppression,
association au tick effectivement soumis, et coupure/reprise du rendu sans
rejouer un clear. Une refonte générale des images en `shared_ptr` n'est pas
justifiée à ce stade. Une voie immédiate, bornée et restrictive est proposée,
avec refus explicite si ses préconditions ne sont pas réunies.

## Constats attestés dans les sources

| Point | Source | Conséquence pour la capture |
| --- | --- | --- |
| `SlotId` contient seulement un index; `erase` le remet en liste libre; `Reserve` déplace les objets | `common/slot_vector.h:14,125,232` | Aucun `Image*`, `ImageView*` ou index seul ne doit survivre à une opération susceptible de modifier le cache. |
| `Image::image_uid` est attribué à la construction; tous ses backings et leurs vues lui appartiennent | `texture_cache/image.cpp:126`; `image.h:165,183` | Joindre index + UID non nul. Une vue/handle n'est ni une identité de contenu ni un propriétaire. |
| `TryGetImage(id,uid)` accepte `uid==0` et ne teste pas `Registered` | `texture_cache/texture_cache.h:236` | Ce n'est pas une acquisition de durée de vie. N'utiliser que des IDs internes bornés et une validation stricte dédiée. |
| `SetBackingSamples` réutilise/crée un backing et peut convertir la couleur; `FindView` peut le déclencher | `vk_runtime.cpp:712`; `image.cpp:222` | Figer le backing réellement utilisé après résolution. Ne pas appeler ces chemins pour faciliter une capture. |
| Format et samples Vulkan peuvent différer de la demande invitée | `image.cpp:151-210` | Lire `backing.image.image_ci`, pas seulement `ImageInfo` ou `BackingImage::num_samples`. |
| `PublishAliasWrite` intervient au binding storage/render target; un hit `BeginRendering` peut sauter ce chemin; le depth n'utilise pas ce compteur de la même manière | `texture_cache.cpp:1252,1283,1310`; `vk_rasterizer.cpp:3003` | `alias_generation` n'est pas une version exhaustive de contenu par draw. |
| `DeleteImage` supprime toutes les vues et l'image dans `DeferOperation(CurrentTick())`; un unmap invité peut demander cette suppression | `texture_cache.cpp:671,1791,1840` | Une simple consultation réussie du cache n'empêche pas une suppression différée. |
| Les callbacks de retrait s'exécutent sous `pending_ops_mutex`; un unmap tient le mutex du cache avant d'appeler `DeferOperation` | `vk_scheduler.cpp:401`; `texture_cache.cpp:673,1840` | Ne pas ajouter naïvement une prise du mutex de cache dans le callback : inversion potentielle cache → pending / pending → cache. |
| Stage B possède les caches pendant les packets; A attend `Drain` pour les autres actions | `vk_draw_pipe.h:4`; `vk_rasterizer.cpp:786,802`; `liverpool.cpp:90,184,763` | Le collecteur doit rester sur le consommateur ordonné A/B, pas sur le thread UI/présentation. |
| Le helper textures est joint avant les modifications d'état d'image et à la fin de `BindResources` | `vk_rasterizer.cpp:98,1848,1855`; `vk_runtime.h:104` | Résultats du helper séparés par stage/draw, publication seulement après `Join`, aucun callback qui se rejoint lui-même. |
| `Record` peut différer la commande; `RecordData` et `RecordChunk` ne vivent que jusqu'à l'enregistrement CPU | `vk_scheduler.h:735,781,580` | Capturer les valeurs nécessaires; conserver les ressources GPU dans un propriétaire distinct de la closure. |
| `NextTick()` précède le callback de soumission et `SyncRecording`; `signal_value` est l'ancienne valeur | `vk_scheduler.cpp:448`; `vk_semaphore.h:34` | Lire `CurrentTick()` dans une closure tardive peut désigner le tick suivant. Garder le tick réel de la soumission. |
| `BeginRendering` rejoue les `loadOp=clear` présents dans le `RenderState` à chaque nouvelle portée | `vk_scheduler.cpp:77,159` | Ajouter une coupure peut changer le contenu à la reprise si les clears ne sont pas traités explicitement. |
| `Transit` met à jour l'état CPU puis `FlushBarriers` termine le rendu et enregistre les barrières | `vk_runtime.cpp:195,860`; `image.cpp:242` | Un callback générique dans `EndRendering` serait trop tard pour déduire l'ancien état : des transitions peuvent déjà être en attente. |

Les scénarios d'inversion de verrou, de tick tardif ou de clear rejoué décrivent
les risques **du nouveau chemin proposé**. Ils ne constituent pas une preuve de
bug actuellement observé en jeu dans le runtime existant.

## Identité et provenance minimale

Une identité de ressource proposée comporte : index interne, `image_uid`,
ordinal du backing dans `backing_images` (append-only dans les chemins examinés),
format/extent/samples Vulkan effectifs, aspect, mip/couche et étendue, format et
mapping de la vue. L'ordinal est toujours qualifié par l'UID de l'image; un
changement 2x → 1x → 2x doit retrouver le même backing mais **pas** présumer le
même contenu. Aucun pointeur n'est sérialisé comme garantie de durée de vie.

Ajouter une séquence diagnostique de **commande potentiellement écrivante**,
séparée de `alias_generation`. Ce compteur indique une invalidation possible,
pas qu'un fragment a effectivement écrit un pixel. Il est incrémenté pour les
écritures connues et invalidé lorsque la couverture manque. Ne pas essayer de
déduire les écritures uniquement des changements de layouts.

Le token du résultat doit figer requête/session, intervalle ordonné, draw ou
commande productrice, ressource/version, clés exactes de pipeline, snapshots
caméra existants et état viewport/depth. Le flip accepté ferme ensuite cet
intervalle via `FinishGow3Flip`; l'achèvement GPU est un second état indépendant.
Un résultat ne devient exportable que lorsque ces deux états sont connus.
Un réaffichage ne crée pas un nouveau producteur. Une clôture absente/rejetée,
un snapshot manquant/conflit ou une perte restent explicitement invalides.

### Points de mutation proposés pour la provenance, sans copie

1. `gow3_pair_capture.h/.cpp` : étendre les POD de descripteurs avec identité
   ressource/backing/sous-ressource et état de couverture. Garder les limites
   d'octets, pertes, création exclusive et déclenchement différé existants.
2. `vk_rasterizer.cpp::BindTextures` : relever **après** redirection depth,
   rebind, `FindTexture` et redirections/proxy. Couvrir chaque élément des
   tableaux de mip. Les chemins non pris en charge sont marqués inconnus.
3. `BindTexturesFromSet` : produire la même provenance après ses vérifications
   de génération/backing. Ne pas désactiver le memo pour obtenir artificiellement
   une couverture complète. Le helper produit des POD dans une zone qui lui
   appartient; `BindResources` les rassemble après `Join`.
4. `BeginRenderingFull` et chemin memo : enregistrer les attachments effectifs
   utilisés par le draw, pas les slots restants hors masque. `RecordGow3PairDraw`
   joint les résultats au token explicite après enregistrement du draw.
5. Pour prouver la continuité, observer les commandes draw direct/indirect,
   second draw `square`, clears réels, dispatches, HLE `FilterDraw`/compute copy
   et clear, `UploadImage`, copies/resolve/alias, changement de backing et
   invalidation invitée. Commencer par un ensemble borné de ressources;
   toute voie non instrumentée invalide la preuve au lieu de l'ignorer.

Les invalidations venant de threads invités ne doivent jamais enregistrer de
commande Vulkan. Elles publient une invalidation minimale dans une inbox bornée
à consommer par A/B, ou posent un indicateur de couverture perdue. Les tags de
requête sont transportés explicitement, pas associés au dernier compteur global.

## Durée de vie : voie restrictive avant toute API de pin générale

Option minimale à tester : enregistrer la copie **immédiatement** sur le thread
propriétaire, sans conserver la source entre plusieurs draws. Allouer d'abord
les sorties possédées; joindre le helper; résoudre et vérifier la source sous
le mutex de cache; enregistrer la copie avant de permettre l'unmap concurrent.
La suppression différée existante doit alors être prouvée postérieure à ce
travail. La source doit encore être Registered, même UID, même backing et même
version. Rejeter sinon; ne pas rafraîchir depuis la mémoire invitée.

Cette section critique ne peut appeler ni `Wait`, `Finish`, `PopPendingOperations`,
ni une conversion de backing/proxy, ni une fonction réentrante dans le cache.
`Transit` peut appeler `SceneTargets::NativeAccess` : exiger un chemin natif
non proxy/non redirigé et vérifier l'absence de réentrance avant utilisation.
Il faut démontrer l'ordre de soumission et la protection contre `DeleteImage`
avec des tests contrôlés; la revue seule ne valide pas ce chemin.

Si ces préconditions ne peuvent être garanties, s'arrêter et présenter une API
de lease explicite. Ne pas ajouter un compteur de pin dans un callback avec un
nouvel ordre de verrous improvisé, ni transformer tous les backings en propriétaires
partagés. Le coût d'une telle modification dépasse la simple instrumentation.

Après copie, les callbacks ne doivent plus consulter le cache. Un job possédé
contient ses images, vues, buffers, descripteurs et métadonnées. La closure
`Record` contient seulement les handles/valeurs nécessaires; le propriétaire
du job vit dans une file de retrait indépendante jusqu'au tick GPU exact.
`RecordChunk` ne dispose pas d'un nettoyage explicite de ses commandes restantes
à la destruction : ne pas compter sur sa destruction pour annuler un job.

## Tick, barrières et sortie

Préférer une notification bornée de soumission qui associe les IDs de jobs du
command buffer à `signal_value` et au résultat réel de `queue.submit`. Elle doit
être placée dans `SubmitExecution` sans déplacer `NextTick` ni changer l'ordre
des draws. Les jobs ajoutés pendant le callback `on_submit` appartiennent au
même command buffer. Un tick prélevé sur le thread d'écriture fichier ne convient
pas. Ne pas ajouter une semaphore au `SubmitInfo` sans traiter ses capacités
fixes; ne pas attendre arbitrairement une soumission suivante pour masquer
une association incertaine.

Pour la source, utiliser le tracker `Image::GetBarriers`/`Runtime::Transit` et
`FlushBarriers`, avec la sous-ressource exacte et sans restaurer manuellement un
ancien état dans le dos du tracker. Pour les destinations possédées, suivre
leurs états séparément. Copier vers des images 2x de mêmes formats, puis extraire
les indices 0 et 1; aucune résolution implicite. Vérifier usages, format réel,
extent, samples et flags de vue. Les deux aspects D32S8 doivent être traités
cohéremment même si seule la profondeur est exportée.

Privilégier un point de coupure **naturel et démontré** avant mutation. Si une
coupure artificielle est nécessaire, préparer un mécanisme explicite de reprise
en LOAD, préservant les flags `has_depth/has_stencil` qui partagent une union
avec les clears, et les caches d'état. Ce serait un changement à présenter et
tester avant intégration. Un hook générique dans `EndRendering` est exclu.

Les sorties doivent être possédées jusqu'à achèvement GPU puis lecture hôte.
`StagingBufferPool::Request(..., deferred=true)` retient une allocation, mais
`FreeDeferred` modifie le pool et reprend `CurrentTick`; l'utiliser depuis un
nouveau writer asynchrone exigerait un contrat de thread/retrait explicite.
Pour une capture bornée, des buffers possédés par job évitent de changer ce pool.
Après fence/timeline : invalider si nécessaire, copier les octets vers le writer,
écrire exclusivement. Annulation = abandon du résultat, jamais libération GPU
anticipée. Arrêt et device loss nécessitent un état explicite, sans wait sous
verrou ni boucle de relance.

Conserver le budget proposé de deux intervalles maximum, plafond mémoire
additionnelle vérifié avant allocation et plafond fichier. Les valeurs 256 Mio
et 192 Mio du plan initial restent des **plafonds proposés**, pas une allocation
faite ou une mesure du runtime. Le format exporté doit distinguer codes sRGB
et flottants décodés; le test synthétique ne garantit pas une précision flottante
universelle ni tous les formats.

## Couleur avant interface : conditions encore non réunies

La session groupée établissait couleur `c` avec depth `8`, mais pas l'absence
de HUD dans cette couleur. Les UID sont locaux à la session. Les dessins propres
à la pause indiquent une composition de menu tardive; ils ne classent pas le HUD
en gameplay. `UiComposition::MovieShader` est une liste statique qui ne reconnaît
pas ces captures; `NativeViewport` seul correspond aussi aux post-traitements.
`CameraMotion::OnDisplayPass` reprend la cible du draw précédent, sans preuve de
contenu pré-interface; ce chemin ne doit pas servir de validation.

Conditions avant de qualifier une capture de pré-interface :

- chaîne producteur → consommateurs → sortie de présentation, jointe par
  identité/version/sous-ressource, y compris vues différentes du même backing;
- couverture des écritures et opérations hors draw, sans perte ni intervalle
  tronqué; distinction des compositions vers cible intermédiaire et display;
- classification étayée des shaders/pipelines HUD/menu et des opérations de
  composition, pas seulement viewport, blend ou ordre relatif;
- cohérence temporelle couleur/depth/matrices, en conservant le fait que six VS
  écrivent le depth observé et que seul un bloc caméra est aujourd'hui étudié;
- contrôle local de pixels si nécessaire après résolution des contrats de
  copie; jusqu'alors conserver l'étiquette « candidate de scène ».

## Tests de sûreté préparés

`tests/test_gow3_readback_primitives.cpp` exerce les **vraies** structures
`SlotVector` et `RecordChunk`, sans instance Vulkan. Les six vérifications ont
été compilées et exécutées avec succès : réutilisation index/UID, déplacement
des objets lors de croissance, FIFO et tokens par valeur, destruction de la
closure à la fin de l'enregistrement CPU, propriétaire indépendant, refus par
capacité conservant une commande move-only. Résultats dans
`out/dlaa-readback-review/logs/gpu.stdout.log` (nom historique du runner;
l'exécution est CPU seulement). Le test n'exerce pas le scheduler réel ni une
suppression concurrente de cache.

La matrice suivante est une **spécification de tests à ajouter avec les seams
d'intégration**; elle n'est pas présentée comme déjà exécutée :

| Test | Structure/point réel à exercer | Résultat exigé |
| --- | --- | --- |
| R01 index recyclé, UID nul, unregistered | acquisition TextureCache | rejet, aucune commande/lecture invitée |
| R02 2x → 1x → 2x, deux vues et formats | `SetBackingSamples`, `FindView`, token backing | version distincte; même alias relié sans égalité de vue |
| R03 unmap avant acquisition / pendant record / après submit | cache mutex, `DeleteImage`, file de retrait | refus ou rétention jusqu'au bon tick, pas d'inversion de verrous |
| R04 grow SlotVector pendant une opération autorisée | `FindImage`, acquisition par valeur | aucun pointeur d'objet conservé après mutation |
| R05 helper terminé tard, partiellement accepté, fallback | `BindResources`/`BindTextures` | provenance associée au bon draw, publication après Join, aucun doublon |
| R06 memo normal, texture set hit, mip array, depth redirect | trois chemins de binding effectifs | mêmes identités effectives; élément et sous-ressource exacts |
| R07 A/B avec pipe actif/inactif, tâches EOP/non-EOP | `RunInOrder`, `FinishGow3Flip` | mêmes associations; refus/réaffichage ne crée pas de production |
| R08 NextTick avant exécution de Record, dernier submit sans suivant | `SubmitExecution`, notification réelle | job lié au `signal_value` exécuté, pas au dernier global ni au tick suivant |
| R09 draw 1 clear puis draw 2 partiel, copie entre les deux | scope rendering/memo/clear union | résultat final identique avec/sans capture, aucun clear rejoué |
| R10 barriers pending, état partiel mip/layer, depth+stencil | `GetBarriers`/`FlushBarriers` | copie après writes et avant mutation, tracker cohérent au draw suivant |
| R11 écriture compute/HLE/alias/upload sans draw | hooks Runtime/FilterDraw | version invalidée/avancée; jamais « continu » par absence de draw |
| R12 pertes, seuil octets exact, mémoire insuffisante | collecteur/job queue | résultat incomplet borné; zéro fallback silencieux |
| R13 annulation avant record / après submit / pendant writer | propriétaire Job | pas de fichier succès; GPU resources retenues jusqu'au retrait sûr |
| R14 snapshots manquants/conflit, depth d'un autre producteur | métadonnées existantes | motifs conservés; pas de validation caméra inventée |
| R15 couleur HUD déjà dans la candidate, menu sur intermédiaire | graphe de provenance + preuves shader | candidate rejetée comme pré-interface |
| R16 drapeau absent/0, trigger répété, fichier existant | entrée diagnostic, writer exclusif | aucun coût GPU/IO en mode désactivé; pas d'écrasement ni deuxième capture |
| R17 device loss / arrêt sans soumission / writer lent | notification submit + propriétaires | état terminal explicite, bornes, aucune libération d'objet en vol |

Les tests CPU de protocole doivent utiliser des points de synchronisation
déterministes, sans sleeps pour « espérer » une course. Les tests GPU R09/R10
doivent étendre le banc hors écran avec les mêmes primitives de tracker/reprise
utilisées par l'intégration, puis comparer les rendus avec/sans capture. La couche
de validation Vulkan absente reste une limite; ne rien installer implicitement.

## Prochaine étape minimale

Présenter puis implémenter seulement la provenance POD bornée et ses tests
R01/R02/R05/R06/R11/R12/R14/R16. Ne pas relancer une collecte identique avant
que cette instrumentation distingue les versions et les dépendances manquantes.
La copie doit attendre la revue des seams de durée de vie, tick et reprise du
rendu, puis R03/R07/R08/R09/R10/R13/R17. Aucun branchement à NGX, jitter, debug
motion ou DLAA n'est nécessaire pour cette étape.
