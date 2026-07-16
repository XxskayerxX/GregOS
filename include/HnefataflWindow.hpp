#ifndef HNEFATAFL_WINDOW_HPP
#define HNEFATAFL_WINDOW_HPP

#include "Window.hpp"
#include "hnefatafl_core.h"

/* ── HnefataflWindow — « Hnefatafl, le Jeu du Roi » ──────────────────────────
   Le jeu de plateau des rois du Nord (variante Tablut 9×9), joué à la souris
   contre l'IA du royaume (negamax alpha-bêta, profondeur 3, temps borné).
   Choisissez votre camp : défendre le Roi jusqu'à un coin du plateau, ou mener
   l'assaut et l'encercler. Règles et moteur dans kernel/hnefatafl_core.c
   (C pur, testé exhaustivement hors-noyau).                                  */

class HnefataflWindow : public Window {
public:
    HnefataflWindow() { to_choice(); }

    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;

    static constexpr int CELL     = 40;
    static constexpr int MARGIN   = 8;
    static constexpr int HDR_H    = 24;   /* status band above the board  */
    static constexpr int FOOT_H   = 20;   /* hint line below the board    */
    static constexpr int AI_DEPTH = 3;

private:
    enum { CHOOSE = 0, PLAY = 1, OVER = 2 };

    unsigned char m_board[81];
    int           m_phase;
    int           m_human;                /* HN_SIDE_ATT / HN_SIDE_DEF     */
    int           m_turn;
    int           m_winner;               /* HN_WIN_*                      */
    int           m_sel;                  /* selected square, -1           */
    hn_move       m_legal[HN_MAX_MOVES];  /* legal moves for side to move  */
    int           m_nlegal;
    int           m_last_from, m_last_to; /* last move played (-1 = none)  */
    int           m_lost_att, m_lost_def; /* fallen soldiers per camp      */

    void to_choice();
    void start_game(int human_side);
    void refresh_legal();
    void play_move(hn_move m);            /* apply + tallies + win check   */
    void ai_turn();

    int  board_x() const { return client_x() + MARGIN; }
    int  board_y() const { return client_y() + 4 + HDR_H; }
    int  cell_at(int px, int py) const;   /* screen px → square idx or -1  */

    void draw_board() const;
    void draw_piece(int idx) const;
    void draw_status() const;
    void draw_choice() const;
    void draw_over() const;
};

extern "C" void open_hnefatafl_window(void);

#endif /* HNEFATAFL_WINDOW_HPP */
