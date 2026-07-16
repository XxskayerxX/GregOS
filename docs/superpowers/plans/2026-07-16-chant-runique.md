# Chant Runique — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Le premier son musical de GregOS : un clavier runique jouable (souris + clavier physique) et 4 mélodies du royaume sur le haut-parleur PC, sans jamais geler l'UI.

**Architecture:** Données musicales dans un **module C pur** `kernel/runechant_data.c` + `include/runechant_data.h` (table de fréquences 2 octaves + 4 mélodies `{hz, dur_j}`) — compilé noyau ET hôte (patron `hnefatafl_core`), testé par `tests/host/runechant_test.c`. Fenêtre `RuneChantWindow` : notes isolées **non-bloquantes** (`timer_speaker_on(hz)` + auto-off cadencé sur `jiffies` dans `draw()`), mélodies dans un **thread ordonnanceur** (`scheduler_spawn`, SANS toucher `is_gui_active` — l'UI continue), annulation par **compteur de génération** (`g_rc_gen`), section `cli/sti` autour de « vérifier gen + speaker_on » pour éviter la note fantôme, `timer_speaker_off()` **garanti** dans `on_removed()`.

**Tech Stack:** C pur (données), C++ freestanding (fenêtre), ponts existants `timer_speaker_on/off` (`include/Kernel/timer_c.h` — **ne pas réimplémenter**), `scheduler_spawn` (`include/Kernel/Scheduler.hpp`).

## Global Constraints

- Freestanding (0 warning, `nm` sans helpers libgcc) ; pas d'alpha ; tokens `Theme.hpp` ;
  texte affiché **sans accents** (CP437) ; jamais de `git add/commit/push` (fournir la
  commande) ; README + mémoire à jour en fin d'incrément.
- ⚠️ Menus : insertion d'une entrée → **remapper les `case`** (StartMenu insère
  « Chant Runique » idx 11, Moniteur→12, Systeme→13, N_ITEMS 13→14 ; ContextMenu
  append idx 11, N_ITEMS 11→12 + `s_icon_color`).
- Patron fenêtre à layout fixe : gate `hit_test` dans `on_event` + garde « Trop petit ».
- **Un seul son à la fois** ; le haut-parleur ne doit JAMAIS rester allumé après
  fermeture de la fenêtre ou fin/annulation de mélodie.

### Task 1: Module de données C pur + test hôte (TDD)
- [ ] `tests/host/runechant_test.c` : A4 == 440 ; table strictement croissante ;
  doublement d'octave (|2·f[i] − f[i+12]| ≤ 2) ; diviseur PIT 1193182/hz < 65536 pour
  toute fréquence jouée ; chaque note de chaque mélodie : hz==0 (silence) ou
  131 ≤ hz ≤ 2093, durée 1..300 j ; 4 mélodies non vides, noms sans accents.
- [ ] Rouge (le module n'existe pas) → écrire `include/runechant_data.h` +
  `kernel/runechant_data.c` : `RC_NOTE_HZ[25]` (Do4..Do6 chromatique, entiers),
  `rc_note{u16 hz, dur_j}`, `RC_SONGS[4]` (« Cor de Guerre », « Fanfare de Victoire »,
  « Marche Funebre », « Chant du Drakkar »), `RC_NWHITE=15/RC_NBLACK=10`.
- [ ] Vert : `cc -std=c11 -O2 -Wall -Wextra -Iinclude -o /tmp/rct tests/host/runechant_test.c kernel/runechant_data.c && /tmp/rct`.

### Task 2: RuneChantWindow + thread mélodie + menus
- [ ] `include/RuneChantWindow.hpp` + `kernel/RuneChantWindow.cpp` :
  - Piano 2 octaves (15 blanches ~30×110 px, 10 noires 18×68), libellés AZERTY sur la
    1ʳᵉ octave (`Q S D F G H J K` blanches, `Z E T Y U` noires), boutons mélodies [1..4],
    bandeau statut (« ♪ <nom> » pendant lecture), pied d'aide.
  - Note isolée (clic ou touche) : `g_rc_gen++` (coupe toute mélodie), `cli` → re-check
    gen → `timer_speaker_on(hz)` → `sti`, `m_note_off_j = jiffies+16`, surbrillance ;
    dans `draw()` : si `jiffies ≥ m_note_off_j` et pas de mélodie → `timer_speaker_off()`.
  - Mélodie (`1..4` ou clic bouton) : `g_rc_gen++`, `g_rc_song=i`,
    `scheduler_spawn(rc_melody_thread)`. Thread : capture `my=g_rc_gen` ; pour chaque
    note : `cli` check-gen `speaker_on/off` `sti`, attente jiffies avec re-check gen à
    chaque itération ; à la sortie : couper le speaker **seulement si toujours
    propriétaire** (sinon le nouveau son serait coupé). Retour simple = fin de thread.
  - `ESC` : mélodie en cours → stop ; sinon → fermer. `on_removed()` : `g_rc_gen++` +
    `timer_speaker_off()`.
- [ ] Câbler StartMenu (idx 11 + remap) et ContextMenu (idx 11 + icône EMBER).
- [ ] Build 0 warning ; `nm kernel/RuneChantWindow.o kernel/runechant_data.o` CLEAN.

### Task 3: Vérification bout-en-bout AUDIO en QEMU (WAV)
- [ ] Lancer QEMU headless avec **enregistrement du speaker** :
  `-audiodev wav,id=snd0,path=/tmp/chant.wav -machine pc,pcspk-audiodev=snd0` (+ QMP).
- [ ] Piloter : login → ouvrir « Chant Runique » → jouer 3-4 notes clavier → lancer la
  « Fanfare de Victoire » → screenshots (surbrillance touche, statut ♪, UI non gelée
  pendant la mélodie : ouvrir le menu Démarrer PENDANT la lecture) → `ESC` stop → fermer.
- [ ] **Analyse du WAV sur l'hôte** (python, zero-crossings par segment) : les fréquences
  mesurées des notes jouées doivent coller à `RC_NOTE_HZ`/à la mélodie (±3 %), et le
  silence doit revenir après stop/fermeture (garantie speaker_off).

### Task 4: Revue adversariale (Workflow)
- [ ] Lentilles : (1) cycle de vie du son (races gen/cli-sti, speaker jamais bloqué ON,
  double mélodie, fermeture pendant lecture, épuisement des 6 slots de threads) ;
  (2) sûreté mémoire/UI (hit-test clavier piano, indices, resize) ; (3) intégration
  (menus remap, freestanding, pas de réimplémentation des ponts). Vérif adversariale
  par finding ; corriger, re-tester.

### Task 5: Docs + remise
- [ ] README (21 apps, ligne « premier son », section détaillée) ; mémoires projet +
  triptyque (3/3 ✅) ; CLAUDE.md (état « prochaine étape » à rafraîchir).
- [ ] Build final + **fournir la commande git**.

## Self-Review
- Spec App 3 couverte : clavier jouable (T2), mélodies thread non-bloquantes (T2),
  extinction garantie (T2), matériel existant réutilisé (contrainte), vérif QEMU + audio
  réel (T3), revue (T4), docs (T5). ✅ Données/fréquences précises fixées en T1 (A440,
  octaves) ; types `rc_note{hz,dur_j}` cohérents T1→T2. Pas de placeholder.
