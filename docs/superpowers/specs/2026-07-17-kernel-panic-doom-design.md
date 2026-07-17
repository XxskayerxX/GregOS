# KERNEL PANIC — Doom-64-like bare-metal FPS pour GregOS

*Spec de conception — 2026-07-17*

## Vision

Un FPS 3D « à l'ancienne » façon **Doom 64**, sur le lore **Greg & Drakkar**. Tu es
**L'Intrus**, entré par effraction dans le cœur de GregOS pour **voler le kernel**. Boss
final : **Greg 1ᵉʳ + son dragon Drakkar**, en deux phases. Jeu arcade plein écran,
complet et jouable, esthétique phosphore CRT × norrois — grand, sombre, menaçant.

## Contraintes (héritées de GregOS)

- Bare-metal x86 32 bits freestanding, **pas de libc/libgcc**, **pas de flottants**.
- Rendu framebuffer 800×600×32 double-buffer ; primitives via `gfx`/`Graphics`.
- Boucle arcade bloquante sur un thread `scheduler_spawn`, cadencée `jiffies` (100 Hz).
- Texte affiché en **CP437 sans accents** (fonte 8×16). Couleurs via `Theme.hpp`.
- Build 0 warning ; `nm` sans helper libgcc (`__mul/__udiv/__div/__mod`) ; logique pure
  testée sur l'hôte ; vérif bout-en-bout QEMU/QMP ; revue adversariale finale.

## Moteur (approche A + filet B)

- **Raycaster sur grille** (DDA), murs texturés, **fixed-point 16.16**, tables
  sin/cos/tan entières précalculées. **z-buffer par colonne** (1 profondeur/colonne
  écran) pour l'occlusion des sprites.
- **Floor/ceiling casting** texturé par pixel. Rendu dans un **buffer basse-résolution
  interne (~400×240)** puis **upscalé ×2** dans le back buffer 800×600 → le look low-res
  Doom 64 authentique ET le budget perf sous QEMU TCG.
- **Fog / diminished lighting** : LUT d'assombrissement par paliers de distance (entier).
- **Hauteurs variables (heightmap sur grille)** : chaque cellule porte hauteur sol +
  hauteur plafond + ids de textures + flags. Marches, plateformes, fosses, estrade du
  boss. Pas de room-over-room (assumé).
- **Filet B (dégradation gracieuse)** : flag runtime coupant le floor-casting
  (sol/plafond en aplat + fog) si une frame dépasse le budget → garantit « ça marche ».

## Ajouts pilotes (petits, non-invasifs)

1. `PS2Keyboard` : bitmap down/up par scancode, `kb_poll_all()` (draine tous les
   scancodes en attente par frame), `kb_scan_down(sc)`. Débloque le multi-touches
   (avancer + strafe + tourner simultanés) — impossible via les events char.
2. `PS2Mouse` : accumulateur de delta relatif brut capté dans `handle_irq` **avant le
   clamp**, `ps2mouse_take_rel(&dx,&dy)` → mouselook sans blocage aux bords.
3. `gfx`/`Graphics` : `gfx_get_backbuffer()` (pointeur XRGB + largeur/hauteur) +
   `gfx_present()` → le jeu upscale son buffer basse-réso dans le back buffer, puis blit.

## Contrôles

Clavier AZERTY + souris. ZQSD avance/recule/strafe, flèches ← → tournent (backup clavier),
**souris = viser (mouselook)**, **clic gauche / Ctrl / Espace = tir**, **1-5 = arme**,
**E = interagir/voler**, **Shift = courir**, **ESC = quitter**.

## Mission — une carte, 3 zones enchaînées

- **Zone 1 — Le Pare-feu** : couloirs, murs circuiterie phosphore, Sentinelles. Tutoriel
  implicite (bouger, tirer, ramasser).
- **Zone 2 — La Chambre du Kernel** : piédestal du kernel. **E → vol** : alarme
  « KERNEL PANIC », éclairage rouge sang, klaxon, vague d'ennemis, ouverture de l'arène.
- **Zone 3 — L'Arène de Greg** : boss 2 phases. Victoire → écran de fuite.

## Bestiaire (5 types + boss) — FSM idle→alerte→chasse/attaque→touché→mort

1. **Sentinelle** — mêlée rapide, peu de PV, griffe au contact.
2. **Runeur** — distance, éclairs runiques (projectiles), reste à distance.
3. **Golem de granit** — tank lent, gros PV, frappe de sol.
4. **Spectre** — rapide, erratique, mêlée, difficile à toucher (semi-transparent).
5. **Corbeau de Hugin** — volant, plonge sur toi, harcèle à distance.

Ligne de vue par raycast grille ; sprites billboard triés en profondeur, occludés par le
z-buffer ; projectiles ennemis en entités.

## Arsenal (5 armes) — switch 1-5

1. **Lame SIGKILL** — mêlée, munitions infinies (secours).
2. **Décompileur** — pistolet hitscan, munitions généreuses.
3. **Fork Bomb** — gerbe type fusil à pompe (hitscan multiple), munitions moyennes.
4. **Rafale kill -9** — hitscan rapide auto (chaingun), grosse conso.
5. **rm -rf /** — déflagration d'énergie AoE (BFG-like), munitions rares.

## Boss — Greg 1ᵉʳ + Drakkar (2 phases)

- **Phase 1 — Drakkar** (dragon, EMBER/BLOOD) vole dans la bande haute, **souffle de feu**
  (cône/projectiles), fond en piqué, invoque des adds. **Greg invulnérable** (bouclier du
  dragon). Tuer Drakkar.
- **Phase 2 — Greg enragé** (roi, AMBER/GOLD) : salves runiques, **frappe de sol à onde
  de choc**, téléportation. Devient vulnérable. Tuer Greg → **WIN**.
- Sprites larges multi-frames (idle/attaque/touché/mort), **screen shake**, barres de PV
  boss en haut. Grand, sombre, menaçant (règle de lore).

## HUD (phosphore, CP437 sans accents)

Barre basse : **Vie %**, **armure**, **munitions**, **icône arme**, **objectif**
(« VOLE LE KERNEL » → « KERNEL VOLE - FUIS »), **visage-témoin** (l'œil de Greg). Arme au
centre-bas avec **weapon bob**. Flash rouge aux dégâts, flash aux ramassages. Barres boss
en haut en zone 3.

## Pickups

Vie (« RAM », éclats de données), munitions (paquets de signaux, par type d'arme), armure
légère, clés/accès si besoin de portes. Répartis dans les zones.

## Son (PC speaker)

Ponts `timer_speaker_on/off`, mélodies/SFX en threads `scheduler_spawn`, **protocole
ticket de génération** (annulation, cli/sti, `speaker_off` garanti) du Chant Runique. File
courte à priorité (un seul haut-parleur) : tir, impact, râle ennemi, douleur joueur, porte,
**klaxon d'alarme** (vol kernel), **rugissement Drakkar**, jingle victoire, sting de mort.

## Art

- **Textures procédurales** murs/sol/plafond 64×64 (briques, circuiterie phosphore, runes,
  marbre-sang) générées une fois, palette `Theme.hpp`.
- **Sprites hand-authored** indexés color-keyés (`GFX_TRANSPARENT`), blit scalé + test de
  profondeur par colonne. Boss = grands multi-parts, le plus détaillé.

## Fin de partie

- **Mort** (Vie 0) → « KERNEL PANIC — tu as ete purge », touche pour rejouer/quitter.
- **Victoire** (Greg mort) → « Le kernel est a toi. Le royaume s'effondre. » → bureau.
- **ESC** quitte à tout moment.

## Architecture code

- `kernel/doom.c` (+ `include/doom.h`) — module C pur ramassé par le glob. **Logique non
  graphique** (maths fixed-point, collision, IA/FSM, hitscan, machine à états, dégâts)
  factorisée en fonctions pures → `tests/host/doom_test.c` (patron hnefatafl).
- Rendu (framebuffer/back buffer) en noyau, vérifié QEMU/QMP par screenshots + comparaison
  de pixels entre frames (animation prouvée).
- Intégration : `case` dans `launch_arcade_game`/`arcade_game_thread`, entrée menu
  `GamesWindow` (+ bump compteur), README/CLAUDE/mémoire.

## Plan d'implémentation (phases vérifiables)

0. **Hooks pilotes** (keystate, mouse rel, gfx backbuffer/present) + squelette plein écran
   (gradient de test, input, ESC). QEMU : on entre/sort proprement.
1. **Murs** raycaster texturés + fog + mouvement/collision + mouselook. Screenshot.
2. **Sol/plafond** castés + hauteurs (heightmap) + toggle fallback. Perf check.
3. **Sprites** billboard depth-testés + pickups.
4. **Ennemis** (5, FSM, projectiles) + **armes** (5, hitscan/AoE) + dégâts.
5. **HUD** + **son** + **carte 3 zones** + triggers (vol kernel → alarme).
6. **Boss** Greg + Drakkar (2 phases).
7. **Fin de partie**, polish, passe son, **revue adversariale**, docs.

Chaque phase : build 0 warning, freestanding clean, tests hôte logique, screenshot QEMU.
