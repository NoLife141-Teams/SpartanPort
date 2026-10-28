# Journal exact et plan de lecture GPU, 27 octobre 2026

Travail hors jeu dans le checkout isolé, base
`f58660558a03e74af057f9cdfa556e31346bbba9`. Seul le mode d'ouverture du journal
est corrigé en production. Le plan GPU ci-dessous n'est pas implémenté.

## Correctif du journal et preuves

L'ancien `fopen(...,"wx")` comptait un octet par fin de ligne, mais Windows
écrivait CR+LF. Les écarts observés sont exactement le nombre de lignes avant
le footer : 4 274, 4 245, 4 611 et 4 305 octets dans les quatre fenêtres. Cela
n'a supprimé aucune donnée ; chaque fichier réel reste sous 16 Mio.

Le correctif utilise `wbx` : sortie binaire avec création exclusive inchangée.
Le budget et `bytes_before_end` correspondent maintenant aux octets physiques.
Les captures anciennes ne sont ni réécrites ni normalisées.

Le nouveau test `gow3-capture-bytes-test` échoue avec l'ancien code sous Windows
sur l'écart physique du footer, puis passe avec le correctif. Il vérifie :

- offset réel du footer lu en binaire, absence de CR ajoutés et taille disque ;
- budget minimal, fichier complet, réservation du footer et saturation ;
- limite de 9 000 octets : quatre dessins admis exactement dans la portion
  réservée aux données ; 8 999 octets : seulement trois ;
- réinitialisation du seul budget entre quatre fenêtres, identités conservées ;
- refus d'écraser un fichier binaire préexistant contenant CRLF, NUL et Ctrl-Z.

Les tests pair et group passent aussi. Une lecture Python indépendante vérifie
25 fichiers synthétiques, leurs comptes et limites. Les quatre SHA-256 des
captures réelles et l'ancien probe restent identiques.

Probe corrigé compilé à deux jobs, non lancé :
`out/dlaa-byte-accounting/bin/spartanport-probe.exe`.
SHA-256 : `9858F2FEF877D1C6E2720C7D4D785260DE5F6E85B05A4433A5115275150CC8D5`.
Résultats : `out/dlaa-byte-accounting/results.json`,
`physical-byte-validation.json`, journaux `before-test.log` et `after-test.log`.

## Ce que les quatre fenêtres apportent au choix des ressources

Source locale : session `group-20261027T141607Z`, huit intervalles et
17 423 dessins. Les 3 003 copies caméra sont présentes, sans conflit. Les
associations de permutation, profondeur et matrice sont établies pour ce
shader, pas pour tous les producteurs de l'image de profondeur.

| Observation | Interprétation permise | Limite |
|---|---|---|
| Couleur UID `c`, MSAA x2, toujours testée contre profondeur UID `8`, sans écriture de profondeur | Candidate couleur du bloc de scène | Pas encore la couleur finale avant toute interface |
| 4 400 dessins écrivent/effacent la profondeur `8`, répartis sur six VS ; 3 003 appartiennent au shader suivi | Il existe cinq autres producteurs à examiner | Leur matrice et leur bias ne sont pas tous capturés par le filtre actuel |
| Aucune écriture graphique observée dans `8` après le début du bloc `c` | Un point de copie à la fin de ce bloc est plausible | Compute, transferts, alias et écritures non instrumentées restent inconnus |
| Deux dessins vers le buffer de présentation par intervalle hors pause ; 279 pendant la pause | Le menu ajoute une composition tardive | Le HUD de gameplay peut déjà être inclus avant ces dessins |
| Douze paires/permutations uniquement observées en pause, 278 dessins par intervalle, vers `6` puis `2/3` | Candidats de composition du menu | Corrélation sur deux intervalles de pause, pas classification universelle |
| 32/33 vues d'attachement, 184–194 vues échantillonnées, aucune égalité de handle | La jointure par seul handle ne fonctionne pas | L'absence de jointure ne signifie pas absence de dépendance |
| Aucun `legacy_ui_hint`, aucun match même sur les 32 bits bas | L'ancienne liste MovieShader ne reconnaît pas ces dessins | Ne pas élargir la liste par supposition |

La couleur `18` emploie surtout une autre profondeur, `19`, et apparaît à
plusieurs endroits de l'intervalle : son volume de dessins ne suffit pas à la
choisir comme couleur caméra. Les UID sont propres à cette exécution ; ne pas
les coder en dur pour une nouvelle session.

Preuves détaillées, sans pixels : `readback-plan-evidence.json` et
`depth-order-evidence.json` dans la session. L'heure de frontière est CPU,
les flips sont non-EOP et acceptés ; aucune preuve de présentation GPU/écran
supplémentaire n'est inventée.

## Pourquoi les vues ne peuvent pas servir d'identité de contenu

`Image::FindView` mémorise plusieurs vues par backing selon format, aspect,
mapping, mip/couche et usage. Les constructeurs de `ImageViewInfo` diffèrent
pour une texture et un attachement. Une même image peut donc avoir plusieurs
handles de vue. `Runtime::SetBackingSamples` peut aussi changer le backing
couleur et enregistrer une conversion entre MSAA et non-MSAA.

`BindTextures` possède l'image résolue et la vue après rebind/redirection.
`BindTexturesFromSet` possède `TextureSetEntry` (image ID, backing, vue et
sous-ressources), vérifie registry generation/backing/fraîcheur, puis remplit
les descripteurs. Ce sont les points où la provenance doit être copiée. Une
recherche ultérieure à partir d'un ancien handle pourrait voir un objet recyclé.

## Étape proposée A : provenance bornée, sans pixels

À présenter avant implémentation ; aucune modification de ces chemins ici.

1. Copier, au moment de la résolution effective, un enregistrement POD avec
   token d'intervalle/dessin, stage, slot/élément de descripteur, UID image,
   génération du registre/alias, identité du backing, vue, format, aspect,
   mip/couche, samples et indicateur de proxy/redirection. Aucun pointeur invité
   ou pointeur mutable de cache ne doit sortir de cette étape.
2. Couvrir chemins normaux, hits de mémo, rebind, tableaux de mip et helper.
   Le helper écrit dans son résultat propre ; la jonction publie ces données
   avec l'identité du dessin correspondant, jamais « le dernier compteur ».
3. Pour les ressources candidates seulement, enregistrer également les
   écritures/clear, transferts, conversions de samples, synchronisations d'alias
   et opérations HLE qui peuvent en changer le contenu. Un événement hors
   couverture ou perdu invalide la preuve de continuité ; pas de supposition.
4. Suivre les producteurs de profondeur `8` : clés exactes, état effectif,
   provenance des constantes existantes, sans nouvelle lecture invitée directe.
   Les six VS observés empêchent de certifier tout le depth depuis le seul VS
   caméra actuellement étudié.
5. Plafonner à deux intervalles, 64 ressources candidates et 16 384 événements,
   avec limites d'octets et pertes explicites. Ces nombres sont des plafonds
   proposés ; un dépassement produit « incomplet », pas une capture élargie.

Critère : une chaîne de dépendances explicite du bloc `c` à ses consommateurs,
avec chaque changement de contenu identifié, puis séparation des compositions
HUD/menu. L'hypothèse « fin du bloc c = couleur pré-interface » doit pouvoir
être rejetée par ces données. La pause seule ne prouve pas l'absence de HUD.

## Étape proposée B : copie GPU possédée et synchronisée

Cette étape ajoute un chemin de commande, des ressources temporaires et leur
retrait différé. C'est le changement d'architecture à examiner séparément.
L'ancien dump ne convient pas : `DiagDumpImage` exclut le depth et ouvre en
`wb`; `Runtime::DownloadImage` appelle `SetBackingSamples` puis
`copyImageToBuffer`. Ce n'est pas une extraction des samples D32 x2 demandés.

- **Point d'enregistrement :** après le producteur retenu et avant toute
  réutilisation/modification de son backing. Le début du prochain DrawRecord,
  avant PrepareRenderState/BindResources, est un candidat à étudier, pas un
  hook validé : ces fonctions peuvent rebind/convertir les sources. Les
  dispatches/transferts intermédiaires doivent aussi invalider ou déclencher
  la requête. Ne pas attendre arbitrairement le flip ou le présentateur, qui
  peuvent arriver après composition ou réutilisation.
- **Token :** requête + intervalle ordonné + dessin producteur + UID/backing +
  sous-ressource + version de contenu. Le flip accepté ferme ensuite cet
  intervalle et y associe son ID explicite. Requête sans fermeture ou rejetée :
  résultat incomplet, jamais réattribué au flip suivant. Un réaffichage ne
  crée pas une nouvelle capture de production.
- **GPU :** terminer proprement le rendu au point choisi, utiliser le suivi
  de barrières existant et enregistrer l'extraction avant la commande suivante.
  Vérifier usage/format/samples réels. Pour D32S8 x2, vue depth seule et lecture
  explicite des samples 0 et 1 vers un buffer possédé. Pas de moyenne, resolve
  automatique ou changement de backing de la source pour rendre la copie facile.
- **Durée de vie :** retenir les vues/backings et sorties jusqu'au tick contenant
  l'extraction. `DeleteImage` a déjà un retrait différé, mais son interaction
  avec UnmapMemory et les changements de backing doit être démontrée avant
  réutilisation. Si elle ne suffit pas, présenter une petite API de rétention
  explicite ; ne pas conserver seulement un handle ou une référence brute.
- **Achèvement :** capturer le tick au point d'enregistrement. Le callback
  d'achèvement ne manipule que des sorties possédées et des métadonnées figées,
  sans relire le cache. Attendre le tick, invalider la mémoire hôte si nécessaire,
  écrire exclusivement puis enregistrer succès/hash. Pas d'attente GPU bloquante
  sous un verrou de cache ; pas de `Finish()` global ajouté sans examen.
- **Échec :** allocation, format non supporté, device loss, annulation ou budget
  plein doivent produire un état explicite et libérer au bon moment. Ne pas
  modifier le rendu, les snapshots ou la synchronisation générale pour les masquer.

Pour la couleur sRGB, une lecture shader peut produire une valeur décodée
linéaire, pas les octets stockés. Le format de sortie et cette conversion doivent
être déclarés. Une vue compatible UNORM et une quantification 8 bits ne seraient
acceptées qu'après tests de conservation exacts, jamais présumées.

Le budget initial de 63,3 Mio pour deux frames supposait RGBA8 x2 et D32 x2.
Avec RGBA32F décodé x2, la paire représente environ **158,2 Mio**. Proposition
prudente : au plus deux sorties GPU et un staging hôte, allocation supplémentaire
totale plafonnée à **256 Mio**, fichiers plafonnés à **192 Mio**. Vérifier les
tailles avec arithmétique bornée avant allocation ; refuser si le dispositif
exige davantage. Aucune de ces ressources n'est allouée actuellement.

## Prochain test discriminant, sans Jérémie

Avant toute intégration au jeu, étendre le banc Vulkan synthétique possédant
ses images : deux frames de couleurs/depth distincts par sample, deux vues d'une
même image, destruction/réutilisation d'un ID logique, changement de backing,
exécution GPU retardée, mutation ultérieure et annulation. Chaque résultat doit
conserver son token et les valeurs du producteur au moment enregistré.

Ajouter des tests CPU pour requête rejetée, flip retardé/non-EOP/EOP, réaffichage,
pertes, couverture d'écritures incomplète, dépassement mémoire et fermeture du
processus. La modification diagnostique ne doit pas changer les décisions
caméra/rendu avec le drapeau désactivé. Le banc synthétique ne validera toujours
pas la projection réelle des cinq autres VS ni la composition HUD du jeu.

Pas de nouvelle collecte identique à demander maintenant. Aucune nouvelle
instance lancée, aucun processus fermé, aucune sauvegarde lue/modifiée et aucune
publication pendant ce travail. DLAA et motion restent désactivés.
