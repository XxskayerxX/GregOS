#ifndef DUNGEON_WINDOW_HPP
#define DUNGEON_WINDOW_HPP

#include "Window.hpp"

/* ── DungeonWindow — « Le Donjon de Drakkar » ────────────────────────────────
   Roguelike au tour par tour. Descendez les étages du donjon, taillez-vous un
   chemin parmi rats, gobelins et spectres, ramassez l'or, et arrachez son
   trésor à Drakkar au dernier étage. Mort permanente ; donjon régénéré à chaque
   partie et à chaque étage. Déplacement ZQSD/flèches ; se déplacer sur un
   monstre = l'attaquer (bump-to-attack). Chaque action du héros = un tour,
   puis les monstres visibles jouent (poursuite simple).
   Freestanding : RNG entier déterministe seedé sur jiffies, aucune allocation. */

class DungeonWindow : public Window {
public:
    DungeonWindow() { new_game(); }

    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;

    static constexpr int COLS      = 48;
    static constexpr int ROWS      = 26;
    static constexpr int CELL_W    = 8;
    static constexpr int CELL_H    = 16;
    static constexpr int STATUS_H  = 22;
    static constexpr int MAX_MON   = 24;
    static constexpr int MAX_FLOOR = 5;
    static constexpr int FOV_R     = 6;   /* rayon de vision (carré) */

private:
    enum { WALL = 0, FLOOR = 1, DOOR = 2, STAIRS = 3 };
    enum { ITEM_NONE = 0, ITEM_GOLD = 1, ITEM_POTION = 2 };
    enum { PLAYING = 0, DEAD = 1, WON = 2 };

    struct Monster { int x, y, hp, atk; unsigned char glyph; unsigned char alive; };

    unsigned char m_tile[ROWS * COLS];
    unsigned char m_seen[ROWS * COLS];   /* 1 si déjà révélé          */
    unsigned char m_item[ROWS * COLS];   /* ITEM_* sur la tuile        */
    Monster       m_mon[MAX_MON];
    int           m_nmon;
    int           m_px, m_py;
    int           m_hp, m_maxhp, m_atk, m_gold, m_floor, m_status, m_pot;
    int           m_turns;               /* tours écoulés (affichage)  */
    unsigned int  m_seed;

    void         new_game();
    void         gen_level();
    void         reveal_fov();
    void         step(int dx, int dy);
    void         monsters_turn();
    void         descend();
    void         quaff_potion();
    unsigned int rnd();
    int          rnd_range(int lo, int hi);      /* inclusif */

    int  tile(int x, int y) const { return m_tile[y * COLS + x]; }
    bool passable(int x, int y) const;
    int  monster_at(int x, int y) const;         /* index ou -1 */
    int  free_floor(int* ox, int* oy);           /* case FLOOR libre → 1, sinon 0 */

    int  map_x() const { return client_x(); }
    int  map_y() const { return client_y(); }

    void draw_map() const;
    void draw_status() const;
    void draw_gameover() const;
};

extern "C" void open_dungeon_window(void);

#endif /* DUNGEON_WINDOW_HPP */
