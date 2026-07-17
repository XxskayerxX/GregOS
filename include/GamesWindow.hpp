#ifndef GAMES_WINDOW_HPP
#define GAMES_WINDOW_HPP

/* ── GamesWindow ─────────────────────────────────────────────────────────────
   Windowed arcade-game launcher. Replaces the blocking start_games_gui() loop
   with a proper WM-hosted window showing the 10 graphical games.

   Navigation:
     KEY_UP / KEY_DOWN      → move highlight
     '1'-'9', '0'           → jump to game and launch immediately
     KEY_ENTER              → launch highlighted game
     Single click           → select row
     Double-click           → launch
     KEY_ESC / 'q'          → close                                          */

#include "Window.hpp"

class GamesWindow : public Window {
public:
    static constexpr int N_GAMES = 11;
    static constexpr int ROW_H   = 28;  /* px per row in the game list */

    GamesWindow() = default;

    void draw() override;
    bool handle_char(int c) override;
    bool on_event(const Event& e) override;

private:
    int           m_selected     { 0 };
    int           m_last_click_r { -1 };
    unsigned long m_last_click_j { 0 };

    void launch();
};

#endif /* GAMES_WINDOW_HPP */
