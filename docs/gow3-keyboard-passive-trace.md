# Trace clavier passive locale

Cette instrumentation observe K et le chemin de lecture des boutons, sans injecter, retenir, rallonger ou relâcher une entrée. Elle est désactivée par défaut. Le probe se trouve dans `out/dlaa-keyboard-trace/bin/spartanport-probe.exe`. Il a été compilé, mais aucun jeu n'a été lancé pendant cette étape.

## Activation différée

Les trois variables sont limitées au processus de test :

```
SPARTAN_KEY_TRACE=1
SPARTAN_KEY_TRACE_FILE=C:\...\nouvelle-session\logs\keyboard.jsonl
SPARTAN_KEY_TRACE_TRIGGER=C:\...\nouvelle-session\triggers\keyboard.start
```

Seule la valeur exacte `1` active la préparation. Les deux chemins doivent être absolus, sur un lecteur local, différents. Les dossiers doivent déjà exister. Le journal ne doit pas exister : création exclusive Windows `CREATE_NEW`, sans écrasement ni troncature. Le fichier déclencheur appartient uniquement à cette session ; son contenu n'est pas lu. Une fois la fenêtre cible prête, sa création arme la collecte au prochain contrôle sur le thread SDL (contrôle au plus 10 fois par seconde). Un déclencheur présent avant démarrage peut aussi l'activer au premier passage. Aucun fichier n'est supprimé par ce mécanisme.

Une seule collecte par processus. Recréer ou modifier le déclencheur ne redémarre pas une trace expirée. Le diagnostic santé est indépendant ; la trace clavier n'active ni santé, ni DLAA, ni captures d'image, ni autre effet.

## Bornes et propriétaire

La fenêtre de données dure au plus 10 secondes QPC. Chaque producteur vérifie aussi cette échéance, même si le thread SDL tarde à vider la file. La fermeture et le footer restent dépendants de l'exécution du propriétaire ; ce n'est pas une garantie temps réel de fermeture du fichier à 10 secondes. Une horloge invalide ou régressive arrête la capture.

Le fichier entier, en-tête et footer compris, est limité à 65 536 octets. 2 048 octets sont réservés au footer. `WriteFile` compte les octets réels sans conversion CRLF. Saturation du fichier ou de la file : arrêt de la collecte, conservation du préfixe et comptage des pertes. Le remplissage de la file n'est pas un succès complet.

Une file fixe de 128 éléments reste valide pendant tout le processus. Les producteurs tentent une seule acquisition atomique ; en cas de contention, ils comptent une perte et reprennent immédiatement le chemin normal. Ils n'effectuent aucun accès disque, allocation ou appel SDL dans le bridge. Les observations K autour du sampler utilisent seulement le tableau SDL déjà disponible, jamais de mémoire invitée supplémentaire.

Le thread SDL existant est seul propriétaire du fichier. Il vide la file environ toutes les 100 ms, après avoir libéré son verrou. Les lectures successives identiques sont regroupées avec les identifiants premier/dernier, leur nombre, leurs bornes temporelles et l'écart maximal entre lectures. Un événement K ou contexte inséré dans la file sépare les groupes. L'instrumentation et ses écritures peuvent perturber le timing ; leur coût en jeu reste à mesurer.

## Ce qui est enregistré

L'en-tête porte une identité aléatoire de collecte, PID, naissance du processus, ID **SDL** de fenêtre, fréquence QPC et bornes. L'ID SDL n'est pas le HWND utilisé par l'outil Windows. Le premier intervalle est explicitement incomplet : aucune histoire antérieure au déclenchement n'est inventée.

Les seuls événements de touche sont ceux de K, dans la fenêtre SDL cible, avec focus, sans texte ni interface capturant l'entrée. Aucune autre touche, chaîne saisie, adresse d'acteur, sauvegarde, image ou shader n'est écrit. Un événement K contient bas/haut et répétition. Ses temps correspondent au traitement du message par le thread SDL, pas à l'instant physique de la touche Windows.

Les changements de contexte portent focus, capture d'entrée et dialogue texte. Le contexte est publié atomiquement avec une génération. Les tokens de lecture transportent explicitement identifiant, début et génération initiale jusqu'à la fin du même appel. Aucun identifiant « dernier événement global » n'est présenté comme cause de cette lecture. Une génération différente, un focus perdu ou un blocage observé masque les valeurs K/boutons ; seuls les indicateurs de blocage restent.

Les lectures exposent uniquement le bouton Bas, les deux observations de K encadrant le traitement clavier, les blocages et des booléens de présence/activité de manette. Les autres boutons ou touches n'ont pas d'identifiant dans le journal. Les observations K avant/après ne prouvent pas quelle valeur exacte a été lue à chaque instruction entre les deux. Les deux états Bas proviennent du résultat natif avant/après les filtres existants. Aucune modification du résultat n'est faite par le diagnostic.

Types : `header`, `context`, `k`, `read_range`, `stop`. Pour les contextes, les trois bits bas signifient focus=1, capture=2, texte=4 ; les bits hauts sont la génération. Pour `k`, bit 0=bas, bit 1=répétition. Pour `read_range`, les bits sont :

| Bit | Observation |
| --- | --- |
| 1 | Observations K disponibles |
| 2 / 4 | K avant / après le traitement clavier |
| 8 / 16 | Bas dans le résultat hôte / final |
| 32 / 64 | Blocage dans le sampler hôte / porte après ce sampler |
| 128 / 256 | Attente de relâchement avant / après le traitement |
| 512 | Manette présente |
| 1024 | Au moins un bouton de manette actif, sans identité de bouton |
| 2048 | Bas actif côté manette |

Les périodes sans focus ou avec capture sont conservées seulement comme contexte/blocages. Les valeurs masquées ne signifient pas « touche réellement relâchée ». Les enregistrements peuvent être publiés dans un ordre différent de leurs timestamps en cas de concurrence ; joindre par identités et intervalles, pas par seule position dans le fichier.

Le footer donne les lectures commencées, offres, regroupements, pertes par contention/file pleine, rejets de fermeture/expiration, masquages et lignes écrites/perdues. `producer_totals_terminal:false` rappelle que des appels en vol peuvent finir après l'instant du bilan. `queue_drained:false`, footer absent, pertes ou chevauchements ambigus interdisent une conclusion causale complète. Motifs : 1 fermeture, 2 expiration, 3 saturation, 4 horloge, 5 I/O, 6 mauvais propriétaire.

## Validation hors jeu

Les tests du cœur utilisent des horloges synthétiques pour la limite exacte, le relâchement, la perte de focus et les générations. La file concurrente est stressée avec quatre producteurs factices. Le journal Windows vérifie sa limite exacte, la réserve du footer et le refus d'écraser un fichier. Le bridge réel teste désactivation, valeur `true` non admise, fichier existant, activation différée, déclencheur répété après arrêt et expiration QPC réelle.

Les 12 scénarios du sampler réel avec périphériques factices sont exécutés en trois configurations : stubs sans trace, bridge réel désactivé, bridge réel activé. Ils contrôlent les mêmes résultats de boutons/axes et les blocages existants. Ils ne valident pas une livraison clavier réelle Windows → SDL. Aucun test ne crée de fenêtre ou ne commande un périphérique réel.

Compilation du probe Clang/CMake/Ninja existants, deux jobs maximum, téléchargements FetchContent désactivés. Le précédent probe de santé est conservé. Les résultats et journaux synthétiques sont dans `out/dlaa-keyboard-trace/`.

## Prochain protocole, non exécuté

1. Préparer une nouvelle session isolée et la sauvegarde vérifiée selon le protocole existant, son coupé, DLAA désactivé ; lancer uniquement après coordination.
2. Atteindre le menu principal et vérifier fenêtre/focus, New Game sélectionné, Load immédiatement dessous. Manette au repos. Attendre une lecture neutre après toute interface.
3. Créer une seule fois le déclencheur de cette session, vérifier le marqueur de début, puis envoyer **un seul appui ponctuel K pris en charge**. Ne pas essayer de maintenir la touche par une autre voie.
4. Dans la fenêtre de 10 secondes, observer la sélection ; ne pas envoyer Espace si Load n'est pas clairement sélectionné. Ne pas enchaîner avec chargement, mouvement ou combat pour ce test de transport.
5. Examiner le footer et les pertes. Distinguer événement K absent, événement reçu sans état Bas échantillonné, état masqué, ou état exposé au jeu sans changement visible. Si une association ou couverture manque, conclure « indéterminé » ; aucune répétition automatique.

Cette trace rend le prochain test discriminant. Elle ne rend pas l'appui maintenu disponible, ne garantit pas le chargement autonome et ne certifie aucun état vivant à partir d'une santé passée.
