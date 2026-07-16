#ifndef MINESWEEPER_WINDOW_HPP
#define MINESWEEPER_WINDOW_HPP

#include "Window.hpp"

/* ── MinesweeperWindow — « Démineur du Donjon » ──────────────────────────────
   Classic minesweeper, reskinned for the realm: the mines are DRAKKAR'S EGGS
   buried in the lair. Left-click uncovers a tile (flood-fills empty areas),
   right-click plants a banner (flag). Clear every safe tile to win; disturb an
   egg and the dragon wakes. Click the crest to start a new hunt.
   Freestanding: no libc, deterministic RNG seeded from jiffies.            */

class MinesweeperWindow : public Window {
public:
    MinesweeperWindow() { new_game(); }

    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;

    static constexpr int COLS   = 10;
    static constexpr int ROWS   = 10;
    static constexpr int MINES  = 15;
    static constexpr int CELL   = 28;   /* px per tile               */
    static constexpr int MARGIN = 8;
    static constexpr int HDR_H  = 46;   /* header: counter/crest/timer */

private:
    enum { HIDDEN = 0, REVEALED = 1, FLAGGED = 2 };
    enum { PLAYING = 0, WON = 1, LOST = 2 };

    unsigned char m_mine[ROWS * COLS];
    unsigned char m_state[ROWS * COLS];
    unsigned char m_adj[ROWS * COLS];
    int           m_status;
    bool          m_placed;      /* mines laid (deferred to first click) */
    int           m_flags;
    int           m_lost_idx;    /* the egg that woke the dragon (-1)    */
    unsigned long m_start_j;
    unsigned long m_end_j;
    unsigned int  m_seed;

    void         new_game();
    void         place_mines(int safe_idx);
    void         reveal(int idx);
    void         toggle_flag(int idx);
    void         check_win();
    unsigned int rnd();

    int  grid_x() const { return client_x() + MARGIN; }
    int  grid_y() const { return client_y() + HDR_H; }
    int  cell_at(int px, int py) const;    /* screen px → idx, or -1 */
    bool crest_hit(int px, int py) const;

    void draw_header() const;
    void draw_cell(int idx) const;
};

extern "C" void open_minesweeper_window(void);

#endif /* MINESWEEPER_WINDOW_HPP */
