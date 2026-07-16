# Le Donjon de Drakkar — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ajouter à GregOS un roguelike au tour par tour fenêtré (« Le Donjon de Drakkar ») : donjon procédural, combat au corps-à-corps, étages, or/potions, trésor final gardé par Drakkar.

**Architecture:** Une app fenêtrée `DungeonWindow : public Window` (modèle `MinesweeperWindow`), état POD de taille fixe (aucune allocation dynamique), logique pure (génération/combat) séparée du rendu. Le cœur de génération est **testable hors-noyau** (harnais C autonome vérifiant la connexité). Rendu en tuiles fonte CP437 8×16. Purement événementiel : l'état avance sur `handle_char`.

**Tech Stack:** C++ freestanding (pas de libc/libgcc), primitives `Graphics`/`gfx_*`, `Window`/`WindowManager`, RNG déterministe entier seedé sur `jiffies`.

## Global Constraints

- **Freestanding** : aucune libc/libgcc. Vérifier `nm kernel/DungeonWindow.o | grep -iE ' U (__mul|__udiv|__div|__mod)'` → vide.
- **Pas d'alpha-blending** : transparence = color-key `GFX_TRANSPARENT` (`0xFFFFFFFF`) uniquement.
- **Framebuffer** 800×600×32, recompose plein écran → garder `draw()` raisonnable.
- **UI en français**, ton in-world (décrets/présages du royaume).
- **RNG déterministe** entier seedé sur `jiffies` (pas de `rand()`, pas de flottants).
- **Git** : NE JAMAIS exécuter `git add/commit/push` — **fournir** la commande à l'utilisateur.
- **README** : `README.md` + `memory/project_gregos.md` mis à jour dans la dernière tâche.
- Constantes moteur : `Graphics::instance()`, `draw_char(x,y,c,fg,bg)`, `draw_str(x,y,s,fg,bg)`, `fill_rect/draw_rect/draw_hline/draw_vline`, `GFX_TRANSPARENT`. Événements : `EVT_MOUSE_BUTTON`, `e.mouse.{x,y,buttons}` (0x01 gauche, 0x02 droite). Touches : `KEY_UP=128, KEY_DOWN=129, KEY_LEFT=130, KEY_RIGHT=131, KEY_ESC=0x1B`. Enregistrement : `extern "C" void open_dungeon_window(void)` → `Greg::make_ref<DungeonWindow>()` + `win->setup(...)` + `WindowManager::instance().add_window(...)`.

---

### Task 1: Squelette de l'app + intégration (fenêtre vide qui s'ouvre)

**Files:**
- Create: `include/DungeonWindow.hpp`
- Create: `kernel/DungeonWindow.cpp`
- Modify: `Makefile` (ajouter l'objet + le header)
- Modify: `kernel/StartMenuWindow.cpp` (entrée de menu + case + extern)
- Modify: `kernel/ContextMenuWindow.cpp` (entrée menu contextuel + extern)

**Interfaces:**
- Produces: `class DungeonWindow : public Window` avec `void draw() override; bool on_event(const Event&) override; bool handle_char(int) override;` et `extern "C" void open_dungeon_window(void);`.

**Constantes de disposition** (grille qui tient dans la fenêtre) :
- `COLS=48, ROWS=26, CELL_W=8, CELL_H=16` (fonte). Carte = 384×416 px.
- `STATUS_H=22` (barre d'état sous la carte).
- client_w = `COLS*CELL_W = 384` → fenêtre `w=388` ; client_h = `ROWS*CELL_H + STATUS_H = 416+22 = 438` → fenêtre `h ≈ 462`.

- [ ] **Step 1: Écrire le header `include/DungeonWindow.hpp`**

```cpp
#ifndef DUNGEON_WINDOW_HPP
#define DUNGEON_WINDOW_HPP

#include "Window.hpp"

/* ── DungeonWindow — « Le Donjon de Drakkar » ────────────────────────────────
   Roguelike au tour par tour. Descendez les étages du donjon, taillez-vous un
   chemin parmi rats, gobelins et spectres, ramassez l'or, et arrachez le trésor
   à Drakkar au dernier étage. Mort permanente ; donjon régénéré à chaque partie.
   Déplacement ZQSD/flèches, se déplacer sur un monstre = l'attaquer.
   Freestanding : RNG entier déterministe seedé sur jiffies, aucune allocation.  */

class DungeonWindow : public Window {
public:
    DungeonWindow() { new_game(); }

    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;

    static constexpr int COLS     = 48;
    static constexpr int ROWS     = 26;
    static constexpr int CELL_W   = 8;
    static constexpr int CELL_H   = 16;
    static constexpr int STATUS_H = 22;
    static constexpr int MAX_MON  = 24;
    static constexpr int MAX_FLOOR= 5;

private:
    enum { WALL = 0, FLOOR = 1, DOOR = 2, STAIRS = 3 };
    enum { PLAYING = 0, DEAD = 1, WON = 2 };

    struct Monster { int x, y, hp, atk; unsigned char glyph; unsigned char alive; };

    unsigned char m_tile[ROWS * COLS];
    unsigned char m_seen[ROWS * COLS];   /* 1 si déjà révélé */
    Monster       m_mon[MAX_MON];
    int           m_nmon;
    int           m_px, m_py;
    int           m_hp, m_maxhp, m_atk, m_gold, m_floor, m_status;
    int           m_pot;                 /* potions en réserve */
    unsigned int  m_seed;

    void         new_game();
    void         gen_level();
    void         step(int dx, int dy);
    void         monsters_turn();
    void         descend();
    unsigned int rnd();
    int          rnd_range(int lo, int hi);   /* inclusif */

    int  tile(int x, int y) const { return m_tile[y * COLS + x]; }
    bool passable(int x, int y) const;
    int  monster_at(int x, int y) const;      /* index ou -1 */

    int  map_x() const { return client_x(); }
    int  map_y() const { return client_y(); }

    void draw_map() const;
    void draw_status() const;
    void draw_gameover() const;
};

extern "C" void open_dungeon_window(void);

#endif /* DUNGEON_WINDOW_HPP */
```

- [ ] **Step 2: Écrire `kernel/DungeonWindow.cpp` — squelette compilable**

Includes + RNG + `new_game()` (stub qui pose héros au centre, aucun monstre), `gen_level()` (stub : tout FLOOR bordé de WALL), `draw()` (fond + `draw_map()` + `draw_status()`), `draw_map()`/`draw_status()` minimaux, `on_event`/`handle_char` (ESC ferme), et la factory. RNG :

```cpp
unsigned int DungeonWindow::rnd() {
    m_seed ^= m_seed << 13; m_seed ^= m_seed >> 17; m_seed ^= m_seed << 5;
    return m_seed;
}
int DungeonWindow::rnd_range(int lo, int hi) {         /* inclusif */
    if (hi <= lo) return lo;
    return lo + (int)(rnd() % (unsigned)(hi - lo + 1));
}
```

Factory (mirroir de `open_minesweeper_window`) :

```cpp
extern "C" void open_dungeon_window(void)
{
    auto win = Greg::make_ref<DungeonWindow>();
    /* client_w = COLS*CELL_W = 384 ; client_h = ROWS*CELL_H + STATUS_H = 438 */
    win->setup(120, 40, 388, 462, "Le Donjon de Drakkar", 0x0A0806u);
    WindowManager::instance().add_window(Greg::move(win));
}
```

- [ ] **Step 3: Câbler le menu Démarrer** — dans `kernel/StartMenuWindow.cpp` : ajouter `extern "C" void open_dungeon_window(void);`, ajouter le libellé `"Donjon de Drakkar"` au tableau des entrées, et le `case` correspondant dans le switch de lancement (`open_dungeon_window(); break;`). Suivre exactement la forme des entrées existantes (`open_minesweeper_window`).

- [ ] **Step 4: Câbler le menu contextuel** — dans `kernel/ContextMenuWindow.cpp` : `extern "C" void open_dungeon_window(void);` + entrée + dispatch, sur le modèle des autres apps.

- [ ] **Step 5: Câbler le Makefile** — ajouter `kernel/DungeonWindow.o` à la liste des objets et `include/DungeonWindow.hpp` à `HEADERS` (mêmes lignes que MinesweeperWindow).

- [ ] **Step 6: Build**

Run: `make -j4 2>&1 | tail -20`
Expected: compile propre, 0 warning, `kernel.bin` + `myos.iso` régénérés.

- [ ] **Step 7: Vérifier freestanding**

Run: `nm kernel/DungeonWindow.o | grep -iE ' U (__mul|__udiv|__div|__mod)' || echo CLEAN`
Expected: `CLEAN`.

- [ ] **Step 8: Fournir la commande git à l'utilisateur** (NE PAS exécuter)

```bash
git add include/DungeonWindow.hpp kernel/DungeonWindow.cpp Makefile \
        kernel/StartMenuWindow.cpp kernel/ContextMenuWindow.cpp
git commit -m "feat(donjon): squelette DungeonWindow + intégration menus"
```

---

### Task 2: Génération procédurale du donjon (salles + couloirs, connexité testée)

**Files:**
- Modify: `kernel/DungeonWindow.cpp` (`gen_level`, `passable`)
- Test: `scratchpad/dungeon_gen_test.c` (portage autonome du générateur + flood-fill)

**Interfaces:**
- Consumes: `m_tile`, `m_seed`, `rnd_range`, constantes `COLS/ROWS`.
- Produces: `void gen_level()` qui remplit `m_tile` (WALL par défaut, salles + couloirs en FLOOR, un `STAIRS`), pose `m_px/m_py` dans la première salle, et garantit qu'un chemin FLOOR relie le héros à l'escalier. `bool passable(int,int)`.

- [ ] **Step 1: Écrire le test de connexité hors-noyau `scratchpad/dungeon_gen_test.c`**

Copier la logique de `gen_level` (mêmes constantes COLS/ROWS, mêmes salles+couloirs, même xorshift) dans un `main()` C autonome ; pour 500 seeds, générer puis flood-fill depuis `(m_px,m_py)` sur les tuiles non-WALL ; asserter que la case `STAIRS` est atteinte à chaque fois. Émettre `OK 500/500` ou lister les seeds en échec.

- [ ] **Step 2: Lancer le test — il échoue (générateur pas encore écrit)**

Run: `cc -std=c11 -o /tmp/dgt scratchpad/dungeon_gen_test.c && /tmp/dgt`
Expected: échec de compilation ou `FAIL` (le corps de génération est vide).

- [ ] **Step 3: Implémenter `gen_level()` (salles + couloirs connexes)**

Algorithme (entier) : tout WALL ; poser jusqu'à `N=8..12` salles rectangulaires (largeur/hauteur 4..8) à positions aléatoires non collées au bord ; pour chaque nouvelle salle, creuser un couloir en L (horizontal puis vertical) reliant son centre au centre de la salle précédente → connexité garantie par construction. Poser le héros au centre de la 1re salle, `STAIRS` au centre de la dernière. Squelette :

```cpp
void DungeonWindow::gen_level() {
    for (int i = 0; i < ROWS * COLS; ++i) { m_tile[i] = WALL; m_seen[i] = 0; }
    int nrooms = rnd_range(8, 12);
    int pcx = 0, pcy = 0, prev_cx = 0, prev_cy = 0;
    int last_cx = 0, last_cy = 0;
    for (int r = 0; r < nrooms; ++r) {
        int rw = rnd_range(4, 8), rh = rnd_range(4, 6);
        int rx = rnd_range(1, COLS - rw - 2), ry = rnd_range(1, ROWS - rh - 2);
        for (int y = ry; y < ry + rh; ++y)
            for (int x = rx; x < rx + rw; ++x) m_tile[y * COLS + x] = FLOOR;
        int cx = rx + rw / 2, cy = ry + rh / 2;
        if (r == 0) { pcx = cx; pcy = cy; }
        else {
            for (int x = (cx < prev_cx ? cx : prev_cx); x <= (cx > prev_cx ? cx : prev_cx); ++x)
                m_tile[prev_cy * COLS + x] = FLOOR;             /* couloir H */
            for (int y = (cy < prev_cy ? cy : prev_cy); y <= (cy > prev_cy ? cy : prev_cy); ++y)
                m_tile[y * COLS + cx] = FLOOR;                  /* couloir V */
        }
        prev_cx = cx; prev_cy = cy; last_cx = cx; last_cy = cy;
    }
    m_px = pcx; m_py = pcy;
    m_tile[last_cy * COLS + last_cx] = STAIRS;
}
bool DungeonWindow::passable(int x, int y) const {
    if (x < 0 || y < 0 || x >= COLS || y >= ROWS) return false;
    return m_tile[y * COLS + x] != WALL;
}
```

- [ ] **Step 4: Répercuter dans le test et relancer — il passe**

Run: `cc -std=c11 -o /tmp/dgt scratchpad/dungeon_gen_test.c && /tmp/dgt`
Expected: `OK 500/500` (l'escalier est toujours atteignable depuis le héros).

- [ ] **Step 5: Build noyau + freestanding**

Run: `make -j4 2>&1 | tail -5 && nm kernel/DungeonWindow.o | grep -iE ' U (__udiv|__umod|__div|__mod)' || echo CLEAN`
Expected: build propre + `CLEAN` (attention : `%` et `/` sur `unsigned` sont OK en 32 bits ; si un helper apparaît, remplacer par masque/soustraction).

- [ ] **Step 6: Fournir la commande git** (NE PAS exécuter)

```bash
git add kernel/DungeonWindow.cpp
git commit -m "feat(donjon): génération procédurale salles+couloirs (connexité testée)"
```

---

### Task 3: Rendu de la carte + barre d'état

**Files:**
- Modify: `kernel/DungeonWindow.cpp` (`draw_map`, `draw_status`, `draw`)

**Interfaces:**
- Consumes: `m_tile`, `m_seen`, `m_mon`, `m_px/m_py`, stats.
- Produces: `draw_map()` (tuiles + héros + monstres + items visibles), `draw_status()` (PV/or/étage/potions).

- [ ] **Step 1: Implémenter le rendu** — champ de vision carré rayon `R=6` autour du héros (tuiles dans le rayon dessinées à pleine teinte + marquées `m_seen`; tuiles `m_seen` hors-vue en sombre ; jamais-vues non dessinées). Glyphes CP437 :

```cpp
void DungeonWindow::draw_map() const {
    Graphics& g = Graphics::instance();
    const int R = 6, ox = map_x(), oy = map_y();
    for (int y = 0; y < ROWS; ++y) for (int x = 0; x < COLS; ++x) {
        int dx = x - m_px, dy = y - m_py;
        bool inview = (dx > -R && dx < R && dy > -R && dy < R);
        int idx = y * COLS + x;
        if (inview) ((DungeonWindow*)this)->m_seen[idx] = 1;   /* mémorise */
        if (!m_seen[idx]) continue;
        unsigned char gl = '.'; unsigned int fg = inview ? 0x6B5A3A : 0x2A2418;
        switch (m_tile[idx]) {
            case WALL:   gl = 0xB1; fg = inview ? 0x5A5048 : 0x241E18; break; /* ▒ */
            case FLOOR:  gl = 0xFA; fg = inview ? 0x4A4030 : 0x1E1A12; break; /* · */
            case DOOR:   gl = '+';  fg = inview ? 0x9A7A3A : 0x3A2E18; break;
            case STAIRS: gl = '>';  fg = inview ? 0xFFCC44 : 0x4A3A14; break;
        }
        int px = ox + x * CELL_W, py = oy + y * CELL_H;
        g.fill_rect(px, py, CELL_W, CELL_H, 0x0A0806u);
        g.draw_char(px, py, gl, fg, GFX_TRANSPARENT);
    }
    /* monstres visibles */
    for (int i = 0; i < m_nmon; ++i) {
        const Monster& m = m_mon[i];
        if (!m.alive) continue;
        int dx = m.x - m_px, dy = m.y - m_py;
        if (!(dx > -R && dx < R && dy > -R && dy < R)) continue;
        unsigned int fg = (m.glyph == 'D') ? 0xE03020 : 0xC08040;
        g.draw_char(ox + m.x * CELL_W, oy + m.y * CELL_H, m.glyph, fg, GFX_TRANSPARENT);
    }
    /* héros */
    g.draw_char(ox + m_px * CELL_W, oy + m_py * CELL_H, '@', 0xFFD24A, GFX_TRANSPARENT);
}
```

`draw_status()` : bandeau sous la carte (`client_y() + ROWS*CELL_H`), afficher `PV x/y  Or:n  Étage:k/5  Potions:p` en fonte, teintes phosphore.

- [ ] **Step 2: `draw()` = `Window::draw()` + fond + `draw_map()` + `draw_status()` (+ `draw_gameover()` si `m_status != PLAYING`).**

- [ ] **Step 3: Build + QEMU (screenshot)** — vérifier visuellement que la carte, le héros doré, l'escalier et la barre d'état s'affichent. (Harnais QMP habituel, cf `scratchpad/qmp.py`.)

Run: `make -j4 2>&1 | tail -5`
Expected: build propre ; QEMU montre le donjon.

- [ ] **Step 4: Fournir la commande git** (NE PAS exécuter)

```bash
git add kernel/DungeonWindow.cpp
git commit -m "feat(donjon): rendu carte tuiles CP437 + champ de vision + barre d'état"
```

---

### Task 4: Déplacement, combat, tour des monstres

**Files:**
- Modify: `kernel/DungeonWindow.cpp` (`new_game`, peuplement des monstres dans `gen_level`, `step`, `monsters_turn`, `monster_at`, `handle_char`)

**Interfaces:**
- Consumes: carte, `passable`, RNG, stats.
- Produces: `void step(int dx,int dy)` (déplace le héros ou attaque un monstre adjacent, puis `monsters_turn()`), `void monsters_turn()` (chaque monstre vivant poursuit et frappe le héros s'il est adjacent), `int monster_at(int,int)`.

- [ ] **Step 1: Peupler les monstres dans `gen_level()`** — après la pose des salles, placer `rnd_range(4, 6 + m_floor)` monstres sur des cases FLOOR aléatoires (jamais sur le héros ni l'escalier). Table par étage : rat (`r`, hp 3, atk 1), gobelin (`g`, hp 6, atk 2), spectre (`S`, hp 9, atk 3) ; au dernier étage, un Drakkar (`D`, hp 30, atk 6) sur l'escalier/trésor.

- [ ] **Step 2: Implémenter `step` + combat + `monster_at`**

```cpp
int DungeonWindow::monster_at(int x, int y) const {
    for (int i = 0; i < m_nmon; ++i)
        if (m_mon[i].alive && m_mon[i].x == x && m_mon[i].y == y) return i;
    return -1;
}
void DungeonWindow::step(int dx, int dy) {
    if (m_status != PLAYING) return;
    int nx = m_px + dx, ny = m_py + dy;
    int mi = monster_at(nx, ny);
    if (mi >= 0) {                                   /* attaque */
        Monster& m = m_mon[mi];
        m.hp -= m_atk + rnd_range(0, 2);
        if (m.hp <= 0) {
            m.alive = 0; m_gold += rnd_range(2, 8);
            if (m.glyph == 'D') { m_status = WON; return; }
        }
    } else if (passable(nx, ny)) {
        m_px = nx; m_py = ny;
        if (tile(nx, ny) == STAIRS && m_floor < MAX_FLOOR) { descend(); return; }
    }
    monsters_turn();
}
void DungeonWindow::monsters_turn() {
    for (int i = 0; i < m_nmon; ++i) {
        Monster& m = m_mon[i];
        if (!m.alive) continue;
        int ddx = (m_px > m.x) - (m_px < m.x);
        int ddy = (m_py > m.y) - (m_py < m.y);
        if (m.x + ddx == m_px && m.y + ddy == m_py) {           /* adjacent → frappe */
            m_hp -= m.atk;
            if (m_hp <= 0) { m_hp = 0; m_status = DEAD; return; }
            continue;
        }
        /* poursuite simple : privilégie l'axe le plus éloigné */
        int ax = m_px - m.x, ay = m_py - m.y;
        if ((ax < 0 ? -ax : ax) > (ay < 0 ? -ay : ay)) ddy = 0; else ddx = 0;
        int tx = m.x + ddx, ty = m.y + ddy;
        if (passable(tx, ty) && monster_at(tx, ty) < 0 && !(tx == m_px && ty == m_py))
            { m.x = tx; m.y = ty; }
    }
}
```

- [ ] **Step 3: `handle_char`** — ZQSD + flèches → `step` ; `p` → boire potion (`if(m_pot>0){m_pot--; m_hp = min(m_maxhp, m_hp+8);}`) ; `n` → `new_game()` ; `KEY_ESC`/`q` → `request_close()`.

- [ ] **Step 4: Build + QEMU** — se déplacer, tuer un rat (disparaît, or ++), se faire toucher (PV baissent), mourir (écran DEAD).

Run: `make -j4 2>&1 | tail -5`
Expected: build propre ; comportements observés en QEMU.

- [ ] **Step 5: Fournir la commande git** (NE PAS exécuter)

```bash
git add kernel/DungeonWindow.cpp include/DungeonWindow.hpp
git commit -m "feat(donjon): déplacement, combat bump, IA de poursuite des monstres"
```

---

### Task 5: Items, étages, fin de partie + finitions

**Files:**
- Modify: `kernel/DungeonWindow.cpp` (`descend`, `new_game`, items or/potion, `draw_gameover`)

**Interfaces:**
- Produces: `void descend()` (incrémente `m_floor`, régénère via `gen_level`, conserve stats), `draw_gameover()`.

- [ ] **Step 1: Items** — dans `gen_level`, semer quelques `$` (or) et `!` (potions) sur des cases FLOOR ; marcher dessus les ramasse (or += , potions ++), la tuile redevient FLOOR. (Stocker les items comme une petite liste POD parallèle, ou un calque `m_item[ROWS*COLS]`.)

- [ ] **Step 2: `descend()` + `new_game()`** — `descend` : `++m_floor; gen_level();` (stats conservées). `new_game` : réinit stats (`m_hp=m_maxhp=20; m_atk=4; m_gold=0; m_floor=1; m_pot=1; m_status=PLAYING; m_seed = (unsigned)jiffies*2654435761u+1;`) puis `gen_level()`.

- [ ] **Step 3: `draw_gameover()`** — superposer un panneau centré : `DEAD` → « Le donjon vous a englouti » (rouge sang), `WON` → « Le trésor de Drakkar est à vous ! » (or) ; indice « [N] Nouvelle expédition ».

- [ ] **Step 4: Build + freestanding + QEMU end-to-end** — partie complète : descendre plusieurs étages, ramasser or/potion, atteindre l'étage 5, vaincre Drakkar → écran WON ; refaire une partie via `N`.

Run: `make -j4 2>&1 | tail -5 && nm kernel/DungeonWindow.o | grep -iE ' U (__mul|__udiv|__div|__mod)' || echo CLEAN`
Expected: build propre + `CLEAN` ; parcours complet validé en QEMU.

- [ ] **Step 5: Fournir la commande git** (NE PAS exécuter)

```bash
git add kernel/DungeonWindow.cpp include/DungeonWindow.hpp
git commit -m "feat(donjon): or/potions, étages, écrans de victoire/défaite"
```

---

### Task 6: Revue adversariale + docs (README / mémoire)

**Files:**
- Modify: `README.md` (section apps/jeux)
- Modify: `memory/project_gregos.md` (feature set)

- [ ] **Step 1: Lancer une revue adversariale (Workflow ultracode)** sur `DungeonWindow.cpp`/`.hpp` : cibler les débordements de tableau (indices `y*COLS+x`, `MAX_MON`), boucles infinies dans la génération (salle qui ne rentre pas → `rnd_range` borné ?), état incohérent (monstre sur héros, escalier sans chemin au dernier étage avec Drakkar dessus), division par zéro dans `rnd_range`. Corriger les findings confirmés, re-vérifier.

- [ ] **Step 2: Mettre à jour `README.md`** — ajouter « Le Donjon de Drakkar » à la liste des applications/jeux, avec une phrase in-world et (optionnel) un screenshot.

- [ ] **Step 3: Mettre à jour `memory/project_gregos.md`** — ajouter l'app au feature set.

- [ ] **Step 4: Build final + QEMU smoke test.**

Run: `make -j4 2>&1 | tail -5`
Expected: `kernel.bin` + `myos.iso` régénérés, 0 warning.

- [ ] **Step 5: Fournir la commande git finale** (NE PAS exécuter)

```bash
git add README.md
git commit -m "docs(donjon): README + notes — Le Donjon de Drakkar"
```

*(Rappel : `memory/` est hors du dépôt projet — ne pas l'inclure dans git.)*

---

## Self-Review

- **Couverture de la spec** : génération procédurale (T2), rendu tuiles + champ de vision + statut (T3), déplacement/combat/tour monstres (T4), or/potions/étages/fin (T5), intégration menus+Makefile (T1), vérif + docs (T6). ✅ Tous les points de la section « App 1 » de la spec sont couverts.
- **Placeholders** : aucun « TBD/TODO » ; le code des algorithmes délicats (génération, combat, poursuite, RNG) est fourni en entier ; le rendu et les libellés restants sont mécaniques.
- **Cohérence des types** : `Monster{int x,y,hp,atk; unsigned char glyph,alive;}`, `m_tile/m_seen[ROWS*COLS]`, `rnd_range(lo,hi)` inclusif, `step(dx,dy)`, `monster_at→index|-1`, `passable(x,y)` — noms identiques entre le header (T1) et les usages (T2–T5). ✅
- **Adaptation codebase** : pas de framework de test unitaire noyau → logique pure testée hors-noyau (T2), reste vérifié en QEMU ; « commit » remplacé par « fournir la commande git » (contrainte permanente).
