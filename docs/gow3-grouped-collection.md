# Collecte groupée préparée, 27 octobre 2026

La partie **métadonnées** est implémentée, désactivée par défaut et compilée dans
`out/dlaa-grouped-evidence/bin/spartanport-probe.exe`. Aucun lancement du jeu
n'a été effectué pendant cette préparation. La capture de pixels synchronisés
reste un plan technique, pas une fonctionnalité prête.

## Contenu et utilité

| Données | Couverture préparée | Question traitée |
|---|---|---|
| Shaders | Hash VS/PS et clés de permutation sélectionnées pour les dessins graphiques instrumentés | Associer le bon module au dessin ; ne plus deviner depuis le cache |
| Caméra | 36 mots exacts de la copie existante, motifs de rejet, observations, conflits | Distinguer transformation, copie absente et conflit sans masquer un rejet |
| Sommets du shader cible | Formats, locations, bindings, stride, divisor, offsets et labels des buffers | Vérifier les entrées de la permutation et la transformation d'objet |
| Profondeur | Identités image/vue, UID, génération alias, format, tailles, échantillons, mip/couche, test/écriture/comparaison, bias/bounds exacts, clip/clamp | Relier les conventions au producteur et à ses états |
| Vue | Bits exacts des viewports et scissors | Séparer profondeur, inversion Y et étendue |
| Couleur | Cibles, masques d'écriture, constantes de blend et clés de blend/logique/primitive | Repérer les passes candidates scène/interface |
| Descripteurs d'images | Binding, élément, type, layout et label de vue réellement soumis | Rechercher des liens entre passes, sans prétendre prouver lecture ou contenu |
| Frontières | IDs de flip accepté EOP/non-EOP, intervalle, dessin, statut caméra et heure CPU de frontière | Associer les données sans compteur global « dernier événement » |

Les écritures de descripteurs sont copiées après la jonction du helper et avant
ResetBindings, sur le consommateur ordonné des dessins. Aucun pointeur vers ces
descripteurs n'est conservé. Les métadonnées de sommet sont prises au point
d'émission des bindings. Aucun nouvel accès aux octets invités, aucune copie de
palette ou de vertex buffer, aucun changement des décisions du rendu/historique.

Le blend enregistré est la clé d'entrée du pipeline. Le runtime peut la réécrire
et effectuer un second dessin SquarePass ; son indicateur reste présent. Ce
n'est pas une déclaration d'état Vulkan final de cette seconde passe.

## Quatre fenêtres dans une session

Il faut exactement `1` pour les quatre options :
`SPARTAN_GOW3_INPUT_DIAGNOSTICS`, `SPARTAN_GOW3_PAIR_METADATA`,
`SPARTAN_GOW3_SHADER_METADATA`, `SPARTAN_GOW3_GROUP_METADATA`.
Les chemins absolus de base restent `SPARTAN_GOW3_PAIR_FILE` et
`SPARTAN_GOW3_PAIR_TRIGGER`. L'option groupée seule ne suffit pas.

Chaque phase a son propre déclencheur : `stationary`, `motion`, `pause`, `resume`.
Le logger commence après le prochain flip accepté ordonné ; l'intervalle déjà
commencé est exclu. Il ferme deux intervalles complets, puis attend le
déclencheur suivant. Les identifiants de dessin, intervalle et flip ne sont pas
réinitialisés. Le label décrit l'intention de l'opérateur, pas une preuve de
mouvement ou de menu. L'heure enregistrée est celle du CPU, pas l'achèvement GPU.

Le protocole n'utilise pas F11. Il faut commencer le mouvement **avant** de
déclencher sa fenêtre et le maintenir jusqu'à confirmation de la fin ; envoyer
« bouge maintenant » après le déclenchement ferait à nouveau manquer le geste.

Consignes simples pour Jérémie, après coordination du lancement :

1. Charger la sauvegarde de test et rester immobile. Confirmer « en jeu ».
2. Après confirmation de la fenêtre immobile, commencer un déplacement continu
   et confirmer « je bouge ». Continuer jusqu'au signal d'arrêt.
3. Ouvrir la pause et confirmer « en pause ». Ne pas changer d'écran pendant la
   fenêtre correspondante.
4. Revenir en jeu et confirmer « revenu ». Rester immobile jusqu'à confirmation.

Une pause qui arrête les flips ne donnera pas deux intervalles ; après dix
secondes, l'opérateur le signale sans inventer une capture réussie. La phase
reste en attente et peut couvrir la reprise ; son label ne doit alors pas être
interprété comme preuve d'un menu. Ne pas relancer automatiquement.

## Commandes opérateur préparées, non exécutées sur un jeu

Depuis la racine du checkout, créer une nouvelle session, jamais réutiliser
`group-protocol-synthetic-v1` (réservée au test factice) :

```powershell
& .\out\prepare-gow3-group.ps1 -SessionName 'group-YYYYMMDDTHHMMSSZ'
```

Ce script crée seulement le plan et le dossier de logs. Il n'ouvre pas les
sauvegardes, ne change pas l'environnement et ne lance rien. Le plan donne les
variables à appliquer au **seul processus test**, après les vérifications
d'instances et une nouvelle sauvegarde de sécurité coordonnée. Réutiliser
l'isolation déjà vérifiée ; préserver strictement l'instance et l'installation
normales. Les upscalers, DLAA, jitter, motion et intégrations restent désactivés.

Après chaque confirmation de Jérémie :

```powershell
& .\out\trigger-gow3-group.ps1 -SessionName 'group-YYYYMMDDTHHMMSSZ' -Phase stationary
& .\out\trigger-gow3-group.ps1 -SessionName 'group-YYYYMMDDTHHMMSSZ' -Phase motion
& .\out\trigger-gow3-group.ps1 -SessionName 'group-YYYYMMDDTHHMMSSZ' -Phase pause
& .\out\trigger-gow3-group.ps1 -SessionName 'group-YYYYMMDDTHHMMSSZ' -Phase resume
```

Exécuter ces commandes séparément au bon moment. Chaque nouvelle phase exige
le footer à deux intervalles de la précédente. Un déclencheur ou résultat déjà
existant est conservé et refusé. Le script ne touche ni au processus ni aux
fichiers du jeu. Vérifier le `start`, les deux `interval`, le `end`, les pertes,
les motifs et l'heure CPU avant de qualifier une fenêtre d'exploitable.

## Limites de stockage et de preuve

- Quatre fichiers de 16 Mio maximum : **64 Mio de métadonnées**, avec au plus
  8 192 dessins par fenêtre, 64 descripteurs d'image par dessin, 32 attributs,
  32 streams et 32 bindings de constantes par dessin cible.
- Une réserve de footer garde les compteurs après saturation. Le premier
  dépassement du budget d'octets arrête la rétention de dessins de cette fenêtre.
  Les pertes sont comptées ; aucune fenêtre avec perte n'est déclarée complète.
- Ouverture exclusive, aucun écrasement ni répétition infinie ; au plus quatre
  fenêtres. Les anciens journaux et probes sont conservés.
- Le plafond de 64 Mio ne couvre **pas** les logs généraux du runtime ni son
  cache normal. Garder la session courte et surveiller ces fichiers séparément.
  Ne pas activer de dump F11/raw général pour cette collecte.
- Le journal de trace v4 reste désactivé dans le plan groupé : les associations
  présentateur/queue ne sont donc pas récoltées pour ces quatre fenêtres.
  Les frontières enregistrées sont les flips acceptés ordonnés, pas la preuve
  qu'une image a été présentée à l'écran.
- Les opérations compute/transfert/HLE filtrées restent hors du périmètre de
  ce logger. Une vue liée n'établit ni une lecture effective ni une identité
  persistante après recyclage/alias. Les pixels, positions MSAA, géométrie et
  palette ne sont pas copiés. La couleur avant interface reste non prouvée.

Après la session, utiliser les clés sélectionnées pour récupérer **seulement**
les modules nécessaires dans le cache isolé, avec contrôle du nom/hash, de la
version et de la permutation, comme dans `gow3-shader-evidence.md`. Ne jamais
désérialiser/exécuter le SRT walker du cache. Garder modules, désassemblages et
données localement ; aucun archivage global de cache ou upload.

## Pixels : extension arrêtée avant modification architecturale

`TextureCache::DiagDumpImage` exclut la profondeur, utilise un transfert couleur
et `scheduler.Finish()`, puis ouvre en `wb`. Il ne fournit pas la capture D32S8
à deux échantillons demandée. Les dumps présentateur sont après traitement ;
ceux de l'upscaler dépendraient d'un chemin qui doit rester désactivé.

Les images utilisent un backing brut et des vues détenues par le cache.
`DeleteImage` possède déjà une destruction différée au tick du scheduler ;
cela peut aider, mais ne prouve pas qu'une copie ajoutée plus tard lit le bon
alias, le bon contenu ou même le bon tick. `DeferPriorityOperation` attend le
tick capturé puis exécute son callback sur un autre thread. Le callback ne doit
pas dereférencer un pointeur mutable vers une image du cache.

Plan minimal à examiner avant intégration runtime :

1. Identifier un point producteur précis pour profondeur et couleur, avant leur
   réutilisation ; transporter ses IDs et la génération d'alias dans une requête.
2. Enregistrer la copie/extraction dans le même ordre de commandes, avec les
   barrières et sous-ressources prises en charge par le suivi existant. Retenir
   explicitement les ressources nécessaires jusqu'au tick d'achèvement.
3. Extraire séparément les deux valeurs de profondeur par pixel via accès
   multisample, sans moyenne ni résolution implicite. Rejeter un format/usage
   non pris en charge. Copier la couleur candidate au même point identifié.
4. Après achèvement GPU et invalidation mémoire hôte, écrire un fichier exclusif
   avec format, pitch, nombre/positions d'échantillons, IDs et hash. Distinguer
   « enregistré », « GPU terminé », « écrit », « annulé/échec ».
5. Bornage proposé pour le futur prototype : deux frames, une profondeur D32 x2
   et une couleur RGBA8 x2 à 1920x1080, soit environ **63,3 Mio** bruts ; staging
   plafonné à 128 Mio, pas plus de deux requêtes. Ce n'est pas alloué aujourd'hui.
6. Tester hors écran réutilisation d'image/alias, présentation retardée,
   destruction différée, saturation, annulation et device loss avant une session.

Le test Vulkan synthétique précédent établit des calculs MSAA sur des images
qu'il possède ; il ne remplace pas ces garanties de durée de vie du runtime.
Il faut encore cet examen d'intégration avant une collecte de pixels. La
collecte préparée peut résoudre les associations de métadonnées en une session,
mais ne promet pas de supprimer toute session ultérieure ni de valider le DLAA.

## Vérification locale

Quatorze tests CPU passent, dont les nouveaux contrats de sommets et capture
groupée ; les tests pair/group concernés sont rejoués après les derniers ajouts.
Le contrat du shader est vérifié par 32 assertions statiques et 216 cas CPU
indépendants. Dix-sept fichiers JSONL synthétiques sont relus indépendamment :
IDs continus, plafonds respectés, pertes et incomplet conservés. Le script de
protocole est testé dans une session factice sans processus de jeu.

Résultats et hash final : `out/dlaa-grouped-evidence/final-results.json`.
Sources figées dans `source-manifest.json`, journaux dans `logs/`.
Aucun commit, push, téléchargement, installation ou accès aux sauvegardes.
