# Hnefatafl — le Jeu du Roi — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ajouter à GregOS le jeu de plateau viking Hnefatafl (variante Tablut 9×9) jouable à la souris contre une IA minimax alpha-bêta, au choix côté Roi (défense) ou Assaillants.

**Architecture:** Le **cœur de jeu est un module C pur** `kernel/hnefatafl_core.c` + `include/hnefatafl_core.h` (zéro dépendance noyau, zéro libc) — compilé automatiquement dans le noyau par le glob `kernel/*.c` du Makefile ET compilable tel quel sur l'hôte pour une suite de tests exhaustive (règles, captures, victoire, légalité de l'IA, self-play). La fenêtre `HnefataflWindow : public Window` (`kernel/HnefataflWindow.cpp`) ne fait que le rendu et l'interaction, et appelle le cœur. Aucune allocation : plateau `u8[81]`, listes de coups en tampons statiques par pli.

**Tech Stack:** C11 pur pour le cœur (freestanding-safe), C++ freestanding pour la fenêtre, primitives `Graphics`, IA negamax alpha-bêta profondeur 3 avec **budget de nœuds** (temps borné, déterministe).

## Global Constraints

- **Freestanding** : aucune libc/libgcc — `nm kernel/hnefatafl_core.o kernel/HnefataflWindow.o | grep -iE ' U (__mul|__udiv|__div|__mod)'` → vide.
- **Pas d'alpha-blending** ; primitives `Graphics::instance()` (`fill_rect/draw_rect/draw_char/draw_str/draw_hline/draw_vline`, `GFX_TRANSPARENT`).
- **Zéro allocation dynamique** dans le jeu ; état POD, tampons statiques (mono-thread).
- **Déterministe** : l'IA ne tire aucun aléa (égalités → premier coup trouvé) → testable.
- UI **française**, ton in-world. Touches : `KEY_ESC=0x1B`, lettres ASCII ; souris `EVT_MOUSE_BUTTON`, `e.mouse.{x,y,buttons}` (0x01 gauche).
- **Git** : NE JAMAIS exécuter `git add/commit/push` — fournir la commande.
- **README** + `memory/project_gregos.md` mis à jour en fin d'incrément.
- Makefile : `kernel/*.c` et `kernel/*.cpp` sont globbés → **aucun edit Makefile**.
- ⚠️ Menus : insérer une entrée décale les `case` du switch — remapper (leçon du Donjon).

## Règles implémentées (Tablut 9×9, verrouillées par la spec)

- Plateau 9×9 ; case centrale (4,4) = **trône** ; les 4 **coins** = sorties du Roi.
- Départ : Roi en (4,4) ; **8 défenseurs** en croix orthogonale (distance 1-2 du trône) ;
  **16 assaillants** en 4 groupes en T au milieu des bords (3 sur le bord + 1 en retrait).
  **Les Assaillants jouent en premier** (standard tablut).
- Tous les pions bougent **comme une tour** (orthogonal, distance libre, sans sauter).
- **Trône et coins** : seuls le Roi peut s'y arrêter ; les soldats ne peuvent ni s'y arrêter
  **ni les traverser** ; le Roi peut re-traverser/re-occuper le trône. Trône (même vide) et
  coins sont **hostiles** : ils comptent comme un « pion ennemi » pour les captures.
- **Capture custodienne** (soldats) : après un coup, tout soldat ennemi orthogonalement
  adjacent à la case d'arrivée est pris s'il est **encadré** (case opposée = pion allié ou
  case hostile). Captures multiples en un coup. **Pas de suicide** : venir se placer entre
  deux ennemis ne capture pas. Le Roi est armé (compte comme défenseur pour capturer).
- **Capture du Roi** : après un coup assaillant, le Roi est pris si ses **4 voisins
  orthogonaux** sont tous assaillants, ou **3 assaillants + le trône**. (Sur un bord → 3
  voisins seulement → imprenable, règle « roi fort » de la spec.)
- **Victoire** : Roi atteint un coin → Défenseurs. Roi capturé → Assaillants.
  Camp au trait **sans coup légal** → il perd.

---

### Task 1: Cœur de jeu C pur, développé en TDD sur l'hôte

**Files:**
- Create: `include/hnefatafl_core.h`
- Create: `kernel/hnefatafl_core.c`
- Test: `scratchpad/hn_test.c` (compile `../kernel/hnefatafl_core.c` directement)

**Interfaces:**
- Produces (API C, tout ce que la fenêtre et les tests consomment) :

```c
enum { HN_EMPTY = 0, HN_ATT = 1, HN_DEF = 2, HN_KING = 3 };
enum { HN_SIDE_ATT = 0, HN_SIDE_DEF = 1 };            /* assaillants trait en 1er */
enum { HN_NONE = 0, HN_WIN_ATT = 1, HN_WIN_DEF = 2 };
typedef struct { unsigned char from, to; } hn_move;    /* index 0..80 = y*9+x     */

void    hn_init(unsigned char b[81]);
int     hn_gen_moves(const unsigned char b[81], int side, hn_move* out, int max);
int     hn_apply(unsigned char b[81], hn_move m);      /* → nb de pions capturés  */
int     hn_winner(const unsigned char b[81], int side_to_move);
int     hn_king_pos(const unsigned char b[81]);        /* index, ou -1 si capturé */
hn_move hn_ai_move(const unsigned char b[81], int side, int depth);
```

- IA : negamax + alpha-bêta, profondeur `depth` (3 en jeu), **budget statique ~200k nœuds**
  (au-delà → évaluation immédiate de la feuille) → temps de réponse borné même sous QEMU TCG.
  Éval entière : matériel (déf. pondérés > ass.), distance du Roi au coin le plus proche,
  **chemins de tour dégagés Roi→coin** (0/1/2+ : 2 chemins = perdu pour l'assaillant),
  encerclement du Roi ; terminal ±10000∓pli (préférer la victoire rapide).

- [ ] **Step 1: Écrire la suite de tests `scratchpad/hn_test.c`** — elle `#include "../kernel/hnefatafl_core.c"`… non : elle compile le .c à côté (`cc hn_test.c hnefatafl_core.c`). Cas :
  1. `hn_init` : 16 ass., 8 déf., Roi en 40 ; trône/coins vides.
  2. Mouvements tour : bloqués par pièces ; soldats ne s'arrêtent/traversent PAS trône+coins ; le Roi si.
  3. Captures : paire custodienne simple ; contre trône vide ; contre coin ; **double capture** ; **pas de suicide** ; le Roi capture (armé).
  4. Roi : pris à 4 assaillants ; pris à 3+trône ; **PAS pris** à 2 en plein champ ; **PAS pris** à 3 sur un bord.
  5. Victoire : Roi dans un coin → HN_WIN_DEF ; camp sans coup → l'autre gagne.
  6. IA : sur des positions à mat-en-1, choisit la victoire (Roi à un coup d'un coin → le joue ; capture du Roi dispo → la joue) ; **jamais de coup illégal** (chaque coup IA ∈ hn_gen_moves).
  7. **Self-play** ×200 parties (profondeur 1 vs 1 avec un tirage xorshift local pour varier les 1ers coups) : la partie se termine < 400 plis, les effectifs ne remontent jamais, le plateau reste valide (1 seul Roi ou 0, pas de soldat sur trône/coin).
- [ ] **Step 2: Lancer les tests → échec de compilation** (le cœur n'existe pas).
- [ ] **Step 3: Implémenter `hnefatafl_core.c`** (init, gen, apply avec captures+roi, winner, éval, negamax alpha-bêta node-capped).
- [ ] **Step 4: `cc -std=c11 -O2 -Wall -Wextra -o /tmp/hnt hn_test.c ../../GregOS/kernel/hnefatafl_core.c && /tmp/hnt`** → tout vert.
- [ ] **Step 5: `make` → le glob compile le cœur en freestanding ; `nm` → CLEAN.**

---

### Task 2: Fenêtre HnefataflWindow + intégration menus

**Files:**
- Create: `include/HnefataflWindow.hpp`
- Create: `kernel/HnefataflWindow.cpp`
- Modify: `include/StartMenuWindow.hpp` (N_ITEMS 12→13), `kernel/StartMenuWindow.cpp` (extern + label « Hnefatafl » idx 10 + remap cases)
- Modify: `include/ContextMenuWindow.hpp` (N_ITEMS 10→11), `kernel/ContextMenuWindow.cpp` (extern + label + icône + case 10)

**Interfaces:**
- Consumes: l'API C de Task 1 (via `extern "C"` du header), `Window`, `Graphics`.
- Produces: `extern "C" void open_hnefatafl_window(void);`

**Disposition** : CELL=40, plateau 360×360 ; client = marge 8 + bandeau statut 24 (haut) + plateau + pied 20 → fenêtre ~382×466. Phases : `CHOOSING` (overlay « Défendre le Roi [D] / Mener l'Assaut [A] », souris ou touche) → `PLAYING` (clic pièce alliée = sélection + surbrillance des coups légaux de CETTE pièce via `hn_gen_moves` filtré par `from` ; clic destination légale = `hn_apply`, puis **réponse IA synchrone** `hn_ai_move(depth=3)` ; re-clic = resélection) → `OVER` (bannière « Le Roi s'est échappé ! » or / « Le Roi est tombé » sang ; `[N] Nouvelle partie`). Pièces : assaillants = disques **rouge sang** (rampe BLOOD), défenseurs = disques **or**, Roi = or + **rune verte** (l'œil de Drakkar) ; trône = rune sombre, coins = marqueurs or ; damier obsidienne bicolore ; dernière case jouée soulignée BRAISE.

- [ ] **Step 1: header + cpp** (états : `unsigned char m_board[81]; int m_phase, m_human, m_turn, m_sel /* -1 */, m_winner; hn_move m_legal[128]; int m_nlegal; unsigned char m_last_from, m_last_to;`).
- [ ] **Step 2: câblage StartMenu/ContextMenu** (⚠️ remap des `case` après insertion idx 10).
- [ ] **Step 3: `make` → 0 warning ; `nm` des 2 objets → CLEAN.**

---

### Task 3: Vérification bout-en-bout QEMU

- [ ] **Step 1:** boot QEMU headless (arrière-plan) + login QMP (AZERTY : qcodes `q d ; i n` !) ; menu Démarrer → « Hnefatafl » ; screenshot plateau initial (16 rouges en T, 9 or en croix, Roi runique au centre, coins marqués).
- [ ] **Step 2:** choisir Défenseurs ([D]) → l'IA assaillante joue son 1er coup seule (screenshot : une pièce rouge a bougé, surlignage BRAISE).
- [ ] **Step 3:** cliquer un défenseur → surbrillance des destinations ; jouer un coup → l'IA répond ; vérifier une **capture** à l'écran (pion disparu) si la position s'y prête.
- [ ] **Step 4:** `N` → nouvelle partie ; `ESC` → fermeture propre.

---

### Task 4: Revue adversariale (Workflow ultracode)

- [ ] Lentilles : (1) **fidélité des règles** vs la section « Règles implémentées » ci-dessus (captures custodiennes, hostilité trône/coins, roi fort, no-suicide, camp sans coup) ; (2) **sûreté mémoire** (indices 0..80, ±9/±1 aux bords via arithmétique x/y, tampons de coups, récursion bornée) ; (3) **IA** (terminaison, légalité, budget de nœuds, débordements d'éval) ; (4) **intégration** (remap des menus, freestanding). Vérification adversariale de chaque finding ; corriger les confirmés, re-tester, re-vérifier.

---

### Task 5: Docs + remise

- [ ] README : « En un coup d'œil » (20 apps, ligne jeux) + section détaillée dans le journal des versions.
- [ ] `memory/project_gregos.md` (liste fenêtres + note menus) ; `memory/project_triptyque_royaume.md` (statut 2/3 ✅).
- [ ] Build final + **fournir la commande git** (ne pas l'exécuter).

## Self-Review

- Couverture spec : règles tablut (T1), IA minimax bornée (T1), UI souris + choix de camp + surbrillance (T2), intégration menus (T2), vérif QEMU (T3), revue (T4), docs (T5). ✅
- Placeholders : aucun ; l'API, les règles précises, l'éval et les cas de test sont fixés ci-dessus.
- Types cohérents : `hn_move{u8 from,to}`, plateau `u8[81]`, `HN_SIDE_*`, `hn_*` partout. ✅
- Architecture testable : cœur C compilé À L'IDENTIQUE hôte/noyau (zéro drift, leçon du Donjon). ✅
