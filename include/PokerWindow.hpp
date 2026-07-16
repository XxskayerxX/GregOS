#ifndef POKER_WINDOW_HPP
#define POKER_WINDOW_HPP

#include "Window.hpp"
#include "GUI/Button.hpp"

/* ── PokerWindow — Jacks-or-Better Video Poker ───────────────────────────
   Ring-0 application using LibGUI widgets.  All rendering goes through
   Graphics::instance() (the WM back-buffer); wm_draw() flips to screen.

   Layout (relative to client area, client_w=656 client_h=396):
     y=  4..26   balance strip + BET display  (text only)
     y= 38..178  5 cards (100×140 each, 8px gap, 62px left margin)
     y=184..208  5 HOLD buttons (LibGUI::Button, auto-drawn by Window)
     y=216..231  status / result message
     y=238..268  DEAL / DRAW button (LibGUI::Button)
     y=276..396  payout table                                               */

class PokerWindow : public Window {
public:
    void draw()            override;
    bool handle_char(int c) override;

    void on_deal();
    void on_hold(int slot);

    /* Pointers set by open_poker_window() for post-click label updates */
    GUI::Button* m_hold_btn[5]  {};
    GUI::Button* m_deal_btn     { nullptr };

    /* Card geometry — needed by open_poker_window() to place HOLD buttons */
    static constexpr int CW  = 100;
    static constexpr int CH  = 140;
    static constexpr int CGP =   8;
    static constexpr int CMX =  62;
    static constexpr int CY  =  38;

private:
    /* ── Deck ─────────────────────────────────────────────────────── */
    int  m_deck[52];
    int  m_deck_pos { 0 };

    void shuffle();
    int  deal_one();

    /* ── Hand ─────────────────────────────────────────────────────── */
    int  m_hand[5] { -1, -1, -1, -1, -1 };  /* -1 = empty slot */
    bool m_held[5] {};

    /* ── FSM ──────────────────────────────────────────────────────── */
    enum class State : int { Betting, Holding };
    State m_state { State::Betting };

    /* ── Bet ──────────────────────────────────────────────────────── */
    int m_bet { 10 };
    static constexpr int BET_LEVELS[] = { 1, 2, 5, 10, 25, 50, 100 };
    static constexpr int N_BET_LEVELS = 7;
    int m_bet_idx { 3 };   /* default = 10 GC */

    /* ── Last result ──────────────────────────────────────────────── */
    char m_msg[72] {};     /* status line */
    int  m_last_win { 0 };

    /* ── Hand evaluation ─────────────────────────────────────────── */
    int         evaluate()              const;
    const char* hand_name(int mult)     const;

    /* ── Drawing helpers ──────────────────────────────────────────── */
    void draw_card(Graphics& g, int x, int y, int card, bool held) const;

    static int pk_slen(const char* s);
    static int pk_itos(int n, char* buf);
    static int pk_cat(char* dst, int pos, const char* s);
    static int pk_cat_int(char* dst, int pos, int n);

    /* Payout multipliers */
    static constexpr int MUL_ROYAL  = 800;
    static constexpr int MUL_SFLUSH =  50;
    static constexpr int MUL_FOUR   =  25;
    static constexpr int MUL_FULL   =   9;
    static constexpr int MUL_FLUSH  =   6;
    static constexpr int MUL_STRT   =   4;
    static constexpr int MUL_THREE  =   3;
    static constexpr int MUL_TPAIR  =   2;
    static constexpr int MUL_JACKS  =   1;
};

#endif /* POKER_WINDOW_HPP */
