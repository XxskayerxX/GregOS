# Le Triptyque du Royaume — trois nouvelles apps GregOS

**Date :** 2026-07-15
**Statut :** design validé (concepts approuvés par l'utilisateur), en attente de revue de spec
**Auteur :** session de brainstorming GregOS

## Résumé

Trois nouvelles applications natives pour GregOS, ancrées dans le lore Greg & Drakkar,
chacune livrée comme un incrément séparé et vérifié :

1. **« Le Donjon de Drakkar »** — roguelike au tour par tour (le fleuron).
2. **« Hnefatafl — le Jeu du Roi »** — jeu de plateau viking contre une IA minimax.
3. **« Chant Runique »** — clavier/séquenceur qui fait sonner le haut-parleur PC
   (premier usage du son dans GregOS).

Chaque app suit le modèle éprouvé de `MinesweeperWindow` : un header `include/XxxWindow.hpp`
+ une source `kernel/XxxWindow.cpp`, une fonction d'entrée `extern "C" void open_xxx_window(void)`,
câblée dans le menu Démarrer, le menu contextuel du bureau et le `Makefile`. Contraintes moteur
respectées : framebuffer logiciel 800×600×32, **pas d'alpha** (color-key `0xFFFFFFFF` uniquement),
primitives `Graphics`/`gfx_*`, fonte bitmap 8×16 CP437, timing sur `jiffies` (100 Hz),
RNG déterministe seedé sur `jiffies`, **freestanding** (aucune libc/libgcc — vérifier l'absence
de symboles helper via `nm`), UI en **français** au ton in-world.

## Contexte et contraintes partagées

### Modèle d'application fenêtrée

Les apps dérivent de `Window` (`include/Window.hpp`) et surchargent :

- `void draw()` — appelée à chaque compose ; redessine tout le client area.
- `bool on_event(const Event& e)` — événements souris (position, boutons).
- `bool handle_char(int c)` — touches déjà décodées (constantes `KEY_UP`, `KEY_DOWN`,
  `KEY_LEFT`, `KEY_RIGHT`, `KEY_ESC`, caractères ASCII, `'\n'`).

Géométrie utile : `client_x()`, `client_y()`, `client_w()`, `client_h()`.
Les apps tour-par-tour sont **purement événementielles** (l'état avance sur une entrée) ;
elles lisent `jiffies` pour les animations légères et le seed RNG.

### Enregistrement d'une app (checklist d'intégration, identique pour les 3)

1. `include/XxxWindow.hpp` : la classe + `extern "C" void open_xxx_window(void);`.
2. `kernel/XxxWindow.cpp` : implémentation + définition de `open_xxx_window()`
   (créer la fenêtre, `setup(x,y,w,h,"Titre")`, l'enregistrer auprès du WindowManager —
   sur le modèle exact de `open_minesweeper_window()`).
3. `kernel/StartMenuWindow.cpp` : ajouter le libellé dans le tableau des entrées +
   un `case` dans le switch de lancement ; déclarer `extern "C" void open_xxx_window(void);`.
4. `kernel/ContextMenuWindow.cpp` : ajouter l'entrée au menu contextuel du bureau (optionnel
   mais cohérent avec les autres apps).
5. `Makefile` : ajouter `kernel/XxxWindow.o` aux objets, `include/XxxWindow.hpp` aux headers.
6. `README.md` + `memory/project_gregos.md` : documenter la nouvelle app (contrainte permanente).

### Son (nouveau) — haut-parleur PC via PIT canal 2

Déjà disponible mais jamais utilisé :

- `Kernel::Timer::beep(hz, ms)` — **bloquant** (`speaker_set_freq` + `speaker_on` +
  `delay_ms` + `speaker_off`). Pont C : `timer_beep(hz, ms)` (`include/Kernel/timer_c.h`).
- Primitives sous-jacentes : `speaker_set_freq(hz)`, `speaker_on()`, `speaker_off()`.

Pour du son **non-bloquant** (mélodies), on ajoute des ponts C fins
(`timer_speaker_freq/on/off`) et on joue la séquence dans un **thread ordonnanceur**
(même mécanisme que `launch_arcade_game_async`), en cadençant les notes sur `jiffies`.
Un appui-touche isolé peut rester bloquant (~60–90 ms, imperceptible).

> Note QEMU : le son ne sera audible que si l'audio hôte est branché ; le chemin matériel
> (registres 0x42/0x43/0x61) est réel et testé pour sa **logique** (fréquence, timing, gate).

---

## App 1 — « Le Donjon de Drakkar » (roguelike)

### But

Un roguelike au tour par tour : descendre les étages du donjon de Drakkar, tuer des monstres,
ramasser de l'or, et atteindre le trésor du dragon. Mort permanente, donjon régénéré à chaque
partie et à chaque étage.

### Mécaniques

- **Carte** : grille de tuiles (proposé 40×20) rendue avec la fonte CP437 8×16 (cellule =
  1 glyphe). Tuiles : `#` mur, `.` sol, `+` porte, `>` escalier descendant, `@` héros,
  lettres = monstres (`r` rat, `g` gobelin, `S` spectre, `D` Drakkar au dernier étage),
  `$` or, `!` potion.
- **Génération procédurale** : salles rectangulaires posées aléatoirement + couloirs les
  reliant (algorithme « rooms + corridors » classique, entièrement entier, seedé sur `jiffies`
  + compteur d'étage). Garantir la connexité (relier chaque nouvelle salle à la précédente).
- **Déplacement/combat** : ZQSD + flèches ; se déplacer vers un monstre = l'attaquer
  (bump-to-attack). Dégâts déterministes avec petite variance RNG. Chaque action du héros =
  un tour ; ensuite les monstres visibles jouent (poursuite simple : se rapprocher du héros).
- **Champ de vision** : rendu simple « tout visible dans le rayon » ou carte entière révélée
  au fur et à mesure (option la plus simple : révéler les tuiles vues, mémoriser le reste en
  sombre). MVP : rayon de vision carré autour du héros, hors-vue non dessiné.
- **Stats** : PV, attaque, or, étage. Barre d'état en bas. Potion = soin. Mort = écran
  « Le donjon vous a englouti » ; victoire (trésor de Drakkar) = « Le hoard est à vous ».
- **Étages** : `>` descend, régénère un niveau plus dur (plus de monstres / plus forts).
  Dernier étage (p. ex. 5) = salle du trésor gardée par Drakkar.

### Architecture

- `include/DungeonWindow.hpp` : classe `DungeonWindow : public Window`.
- `kernel/DungeonWindow.cpp` : implémentation + `open_dungeon_window()`.

État (tous POD, taille fixe, zéro allocation dynamique) :

```
enum Tile { WALL, FLOOR, DOOR, STAIRS };
struct Monster { int x, y, hp, atk; unsigned char glyph; bool alive; };
unsigned char m_tile[H*W];       // terrain
unsigned char m_seen[H*W];       // 0 jamais vu, 1 vu
Monster       m_mon[MAX_MON];    // monstres de l'étage
int m_nmon;
int m_px, m_py, m_hp, m_maxhp, m_atk, m_gold, m_floor, m_status;
unsigned int m_seed;             // RNG déterministe
```

Méthodes clés : `new_game()`, `gen_level(floor)`, `carve_room/corridor`, `step(dx,dy)`
(déplacement+combat+tour des monstres), `monsters_turn()`, `rnd()`, `draw_map()`,
`draw_status()`, `tile_glyph(x,y)`.

### Rendu

Cellule = 8×16 px. `gfx_draw_char(x, y, glyph, fg, bg)` par tuile visible ; palette
thématique (sol ambre sombre, murs pierre, monstres teintés, `@` doré, `$` or, Drakkar rouge
sang). Barre d'état sous la carte. Aucune animation lourde (redraw complet à chaque compose OK,
grille modeste).

### Entrée

`handle_char` : ZQSD/flèches = déplacement, `>`/Entrée sur escalier = descendre, `p` = potion,
`ESC`/`q` = fermer. `on_event` : clic sur croix de fermeture (géré par `Window`).

### Vérification

- Compilation propre (0 warning), `nm kernel/DungeonWindow.o` sans helper libgcc.
- Test QEMU (QMP) : ouvrir l'app, se déplacer, tuer un monstre (PV monstre → 0, disparaît),
  ramasser de l'or (compteur ++), descendre un étage (nouvelle carte), mourir/gagner.
- Invariants testables hors-noyau si utile : connexité de la carte générée (portage du
  générateur en test C autonome, flood-fill depuis `@` atteint `>`).

---

## App 2 — « Hnefatafl — le Jeu du Roi » (plateau viking + IA)

### But

Le jeu de plateau nordique (variante **Tablut 9×9**). Le joueur peut prendre le camp du Roi
(défense) ou des Assaillants ; l'autre camp est joué par une **IA minimax**. Fidèle au lore
norrois — bien plus que des échecs occidentaux.

### Règles (Tablut 9×9)

- Plateau 9×9. Case centrale = le **trône** (château). Les 4 **coins** = refuges du Roi.
- Pièces de départ : **Roi** au centre ; **8 Défenseurs** en croix autour ; **16 Assaillants**
  en 4 groupes sur les bords (disposition standard tablut).
- **Déplacement** : toutes les pièces bougent comme une tour (orthogonal, distance libre,
  sans sauter). Seul le Roi peut occuper le trône et les coins ; les autres pièces les
  traversent selon la variante (MVP : trône/coins interdits aux soldats, hostiles pour la
  capture).
- **Capture** : une pièce ennemie est prise si elle est **encadrée** par deux pièces adverses
  (ou une pièce + trône/coin hostile) sur deux côtés opposés (mouvement actif — on ne se
  « suicide » pas en se plaçant entre deux ennemis). Captures multiples possibles en un coup.
- **Victoire** : le **Roi atteint un coin** → les Défenseurs gagnent. Le **Roi est capturé**
  (encerclé sur ses 4 côtés orthogonaux, ou 3 côtés + trône) → les Assaillants gagnent.

### IA

- **Minimax** avec élagage alpha-bêta, profondeur 2–3 (à caler pour rester < ~200 ms).
- **Génération de coups** : pour chaque pièce du camp au trait, énumérer les cases atteignables
  (rook moves) ; appliquer capture ; évaluer.
- **Évaluation** (heuristique entière) : matériel (Défenseurs vs Assaillants pondérés),
  distance du Roi au coin le plus proche (fuite), nombre de cases de fuite libres pour le Roi,
  encerclement du Roi. Signe selon le camp de l'IA.
- **Perf** : plateau 9×9, ~ quelques dizaines de coups/niveau ; profondeur 2 est instantanée,
  3 acceptable avec alpha-bêta. Cap de profondeur si nécessaire ; l'IA joue de façon
  **synchrone** juste après le coup du joueur (freeze imperceptible visé). Fallback si trop
  lent : profondeur adaptative ou coup « glouton » (profondeur 1 + capture-aware).

### Architecture

- `include/HnefataflWindow.hpp` : `class HnefataflWindow : public Window`.
- `kernel/HnefataflWindow.cpp` : implémentation + `open_hnefatafl_window()`.

État :

```
enum Cell { EMPTY, DEF, KING, ATT };   // occupant d'une case
unsigned char m_board[9*9];
int  m_turn;            // camp au trait
int  m_human_side;      // DEF ou ATT (choix au démarrage)
int  m_status;          // en cours / roi échappé / roi capturé
int  m_sel;             // case sélectionnée (-1 sinon)
```

Cœur logique **pur** (testable hors-noyau) : `gen_moves(board, side, out)`,
`apply_move(board, from, to)` (retourne les captures), `is_win(board)`,
`evaluate(board, side)`, `minimax(board, side, depth, alpha, beta)`,
`ai_pick_move(board, side)`.

### Rendu & entrée

Damier 9×9 dessiné en cases ~40 px (plateau ~360 px + marges), pièces stylisées
(Roi = couronne/rune dorée, Défenseurs = boucliers, Assaillants = haches) via glyphes CP437
ou petits dessins `fill_rect`. Trône et coins marqués. Souris : clic sur une pièce alliée =
sélection (surbrillance des coups légaux) ; clic sur une case cible légale = jouer, puis l'IA
répond. Clavier : `n` nouvelle partie, `ESC` fermer. Écran de fin selon `m_status`.

### Vérification

- Test C autonome du **cœur logique** (scratchpad) : positions connues → captures correctes,
  détection de victoire (Roi au coin ; Roi encerclé), l'IA ne joue jamais de coup illégal,
  et préfère un coup gagnant immédiat si disponible.
- Compilation propre, `nm` sans helpers.
- Test QEMU (QMP) : partie complète jouée contre l'IA (le Roi s'échappe ou est capturé),
  coups illégaux refusés, surbrillance correcte.

---

## App 3 — « Chant Runique » (clavier/séquenceur, son PC speaker)

### But

Faire **sonner** GregOS pour la première fois : un clavier musical jouable + quelques mélodies
préréglées du royaume (cor de guerre, fanfare de victoire, marche funèbre). Jouet ludique et
première démonstration audio de l'OS.

### Mécaniques

- **Clavier jouable** : une rangée de touches du clavier physique mappée sur une gamme
  (p. ex. `Q S D F G H J K` → do ré mi fa sol la si do, une octave ; rangée du dessus =
  altérations/octave sup.). Appui → note jouée (bloquant court ~70 ms, imperceptible) via
  `timer_beep(freq, ms)`. Table de fréquences entières (Hz) pour ~2 octaves.
- **Touches à l'écran** : dessin d'un clavier de piano runique ; la touche pressée s'illumine
  (ambre → braise). Clic souris sur une touche = joue la note aussi.
- **Mélodies préréglées** : 3–4 morceaux (tableaux `{freq, durée_j}`), joués **dans un thread**
  (`launch_runechant_melody_async(id)`) cadencés sur `jiffies` pour ne pas geler l'UI ;
  bouton/`1..4` pour lancer, `ESC` pour couper le son (`speaker_off`).
- **Anti-conflit** : un seul son à la fois ; couper le thread/gate avant d'en lancer un autre ;
  toujours `speaker_off()` en quittant l'app (`on_removed`).

### Architecture

- `include/RuneChantWindow.hpp` : `class RuneChantWindow : public Window`.
- `kernel/RuneChantWindow.cpp` : implémentation + `open_runechant_window()` +
  la routine de thread mélodie.
- `kernel/Timer.cpp` / `include/Kernel/timer_c.h` : ajouter les ponts C non-bloquants
  `timer_speaker_freq(hz)`, `timer_speaker_on()`, `timer_speaker_off()`.

État :

```
static const unsigned int NOTE_HZ[N];      // table de fréquences (Do..Si sur 2 octaves)
struct Song { const char* name; const unsigned short* notes; int len; };
int m_active_key;        // touche illuminée (-1)
unsigned long m_key_lit_until;  // jiffies
volatile int m_playing;  // id mélodie en cours (0 = aucune)
```

### Rendu & entrée

Clavier de piano dessiné en `fill_rect` (touches blanches/noires re-thémées phosphore),
barre de titres des mélodies, indicateur « ♪ en cours ». `handle_char` mappe les touches
musicales + `1..4` mélodies + `ESC` (stop/fermer). `on_event` : clic sur une touche dessinée.

### Vérification

- Compilation propre, `nm` sans helpers, ponts C exposés correctement.
- Logique : table de fréquences correcte (La4 = 440 Hz, rapports d'octave ×2), durées de
  mélodie cohérentes, `speaker_off` garanti en sortie (pas de son qui reste bloqué).
- Test QEMU (QMP) : ouvrir l'app, appuyer des touches (surbrillance visible), lancer une
  mélodie (indicateur actif, UI non gelée), fermer (son coupé). L'audio hôte QEMU est optionnel ;
  on valide la logique et le non-blocage même sans écouter.

---

## Ordre de livraison

Trois incréments séquentiels, chacun **entièrement terminé et vérifié** avant le suivant :

1. **Donjon de Drakkar** (le plus riche, pose le ton) — build + QEMU + README/mémoire, puis
   fournir la commande git à l'utilisateur.
2. **Hnefatafl** — cœur logique testé hors-noyau d'abord (règles + IA), puis intégration +
   QEMU + README/mémoire + commande git.
3. **Chant Runique** — ponts son non-bloquants + app + QEMU + README/mémoire + commande git.

Rappel contrainte permanente : **ne jamais exécuter `git add/commit/push`** — seulement
**fournir** la commande à l'utilisateur. README mis à jour à **chaque** incrément.

## Hors périmètre (YAGNI)

- Sauvegarde/persistance des scores ou parties (pas de FS d'écriture fiable ciblé ici).
- Graphismes sprite complexes / alpha-blending (interdit par le moteur).
- Multi-joueur, réseau, sons polyphoniques (le PC speaker est monophonique).
- IA d'échecs occidentale (remplacée par Hnefatafl, plus thématique et plus léger).
- Musique lue depuis fichier (mélodies codées en dur).
