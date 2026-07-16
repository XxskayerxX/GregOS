/* PokerWindow — Jacks-or-Better Video Poker for GregOS.
   Ring-0 application; uses LibGUI widgets for buttons and Graphics for cards.
   Freestanding: no libc, no exceptions.                                    */

#include "../include/PokerWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/tty.h"

extern "C" volatile unsigned long jiffies;

/* ── Felt color palette ───────────────────────────────────────────────── */
static constexpr unsigned int FELT_DK = 0x0A3A10u;
static constexpr unsigned int FELT_LT = 0x1A6030u;
static constexpr unsigned int GOLD    = 0xFFD700u;
static constexpr unsigned int WHITE   = 0xFFFFFFu;
static constexpr unsigned int RED_C   = 0xCC1111u;
static constexpr unsigned int BLK_C   = 0x111111u;
static constexpr unsigned int HELD_HL = 0xFFEE00u;

/* ── constexpr array definitions (needed outside class in C++11) ─────── */
constexpr int PokerWindow::BET_LEVELS[];

/* ── String helpers (freestanding) ─────────────────────────────────── */
int PokerWindow::pk_slen(const char* s) { int n=0; while(s[n]) ++n; return n; }

int PokerWindow::pk_itos(int n, char* buf) {
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    bool neg = (n < 0);
    if (neg) n = -n;
    char t[12]; int ti = 0;
    while (n > 0) { t[ti++] = '0' + n % 10; n /= 10; }
    int start = 0;
    if (neg) buf[start++] = '-';
    for (int k = 0; k < ti; ++k) buf[start+k] = t[ti-1-k];
    buf[start+ti] = '\0';
    return start + ti;
}

int PokerWindow::pk_cat(char* dst, int pos, const char* s) {
    while (*s) dst[pos++] = *s++;
    dst[pos] = '\0';
    return pos;
}

int PokerWindow::pk_cat_int(char* dst, int pos, int n) {
    char tmp[12]; pk_itos(n, tmp);
    return pk_cat(dst, pos, tmp);
}

/* ── Deck management ─────────────────────────────────────────────────── */

void PokerWindow::shuffle() {
    for (int i = 0; i < 52; ++i) m_deck[i] = i;
    /* Fisher-Yates with LCG seeded from jiffies */
    unsigned int s = (unsigned int)jiffies ^ 0xDEAD1337u;
    for (int i = 51; i > 0; --i) {
        s = s * 1664525u + 1013904223u;
        int j = (int)((s >> 8) % (unsigned int)(i + 1));
        int t = m_deck[i]; m_deck[i] = m_deck[j]; m_deck[j] = t;
    }
    m_deck_pos = 0;
}

int PokerWindow::deal_one() {
    return (m_deck_pos < 52) ? m_deck[m_deck_pos++] : 0;
}

/* ── Hand evaluation ─────────────────────────────────────────────────── */

int PokerWindow::evaluate() const {
    for (int i = 0; i < 5; ++i) if (m_hand[i] < 0) return 0;

    int r[5], s[5];
    for (int i = 0; i < 5; ++i) { r[i] = m_hand[i] % 13; s[i] = m_hand[i] / 13; }

    /* Rank frequency */
    int cnt[13] = {};
    for (int i = 0; i < 5; ++i) cnt[r[i]]++;

    /* Flush */
    bool flush = (s[0]==s[1] && s[1]==s[2] && s[2]==s[3] && s[3]==s[4]);

    /* Straight — sort ranks then check consecutive */
    int sr[5] = { r[0], r[1], r[2], r[3], r[4] };
    for (int i = 0; i < 4; ++i)                  /* bubble sort (5 items) */
        for (int j = 0; j < 4-i; ++j)
            if (sr[j] > sr[j+1]) { int t=sr[j]; sr[j]=sr[j+1]; sr[j+1]=t; }
    bool straight = (sr[4]-sr[0]==4 &&
                     sr[1]-sr[0]==1 && sr[2]-sr[1]==1 && sr[3]-sr[2]==1);
    /* Wheel: A-2-3-4-5  (ranks 12,0,1,2,3 → sorted: 0,1,2,3,12) */
    bool wheel = (!straight && sr[0]==0 && sr[1]==1 && sr[2]==2
                             && sr[3]==3 && sr[4]==12);
    if (wheel) straight = true;

    /* Counts */
    int pairs=0, trips=0, quads=0;
    bool high_pair = false;     /* pair of J/Q/K/A (ranks 9..12) */
    for (int rk = 0; rk < 13; ++rk) {
        if      (cnt[rk] == 4) { quads++; }
        else if (cnt[rk] == 3) { trips++; }
        else if (cnt[rk] == 2) { pairs++; if (rk >= 9) high_pair = true; }
    }

    /* Royal = Broadway straight flush (T=8 through A=12) */
    bool royal = flush && straight && !wheel && sr[0] == 8;

    if (royal)             return MUL_ROYAL;
    if (flush && straight) return MUL_SFLUSH;
    if (quads)             return MUL_FOUR;
    if (trips && pairs)    return MUL_FULL;
    if (flush)             return MUL_FLUSH;
    if (straight)          return MUL_STRT;
    if (trips)             return MUL_THREE;
    if (pairs >= 2)        return MUL_TPAIR;
    if (pairs == 1 && high_pair) return MUL_JACKS;
    return 0;
}

const char* PokerWindow::hand_name(int mult) const {
    switch (mult) {
    case MUL_ROYAL:  return "QUINTE FLUSH ROYALE!";
    case MUL_SFLUSH: return "Quinte Flush";
    case MUL_FOUR:   return "Carre";
    case MUL_FULL:   return "Full House";
    case MUL_FLUSH:  return "Couleur";
    case MUL_STRT:   return "Quinte";
    case MUL_THREE:  return "Brelan";
    case MUL_TPAIR:  return "Double Paire";
    case MUL_JACKS:  return "Paire de Valets ou +";
    default:         return "Rien";
    }
}

/* ── Button logic ────────────────────────────────────────────────────── */

void PokerWindow::on_deal() {
    if (m_state == State::Betting) {
        /* ── DEAL phase ── */
        if (casino_get_balance() < m_bet) {
            pk_cat(m_msg, 0, "Solde insuffisant!");
            return;
        }
        casino_modify_balance(-m_bet);
        shuffle();
        for (int i = 0; i < 5; ++i) { m_hand[i] = deal_one(); m_held[i] = false; }
        m_state    = State::Holding;
        m_last_win = 0;
        m_msg[0]   = '\0';
        if (m_deal_btn) m_deal_btn->set_label("TIRER");
        /* Reset all HOLD buttons */
        for (int i = 0; i < 5; ++i)
            if (m_hold_btn[i]) m_hold_btn[i]->set_label("HOLD");
    } else {
        /* ── DRAW phase ── */
        for (int i = 0; i < 5; ++i)
            if (!m_held[i]) m_hand[i] = deal_one();

        int mult = evaluate();
        m_last_win = mult * m_bet;
        if (m_last_win > 0) casino_modify_balance(m_last_win);

        /* Build result message */
        int p = pk_cat(m_msg, 0, hand_name(mult));
        if (m_last_win > 0) {
            p = pk_cat(m_msg, p, "  +");
            p = pk_cat_int(m_msg, p, m_last_win);
            p = pk_cat(m_msg, p, " GC");
        }

        m_state = State::Betting;
        for (int i = 0; i < 5; ++i) m_held[i] = false;
        if (m_deal_btn) m_deal_btn->set_label("DEAL");
        for (int i = 0; i < 5; ++i)
            if (m_hold_btn[i]) m_hold_btn[i]->set_label("HOLD");
    }
}

void PokerWindow::on_hold(int slot) {
    if (m_state != State::Holding) return;
    m_held[slot] = !m_held[slot];
    if (m_hold_btn[slot])
        m_hold_btn[slot]->set_label(m_held[slot] ? "GARDE" : "HOLD");
}

bool PokerWindow::handle_char(int c) {
    if (c == KEY_ESC) { request_close(); return true; }
    /* Bet adjustment: only in Betting state */
    if (m_state == State::Betting) {
        if ((c == '+' || c == KEY_RIGHT) && m_bet_idx < N_BET_LEVELS-1) {
            m_bet = BET_LEVELS[++m_bet_idx]; return true;
        }
        if ((c == '-' || c == KEY_LEFT) && m_bet_idx > 0) {
            m_bet = BET_LEVELS[--m_bet_idx]; return true;
        }
    }
    return false;
}

/* ── Card rendering ──────────────────────────────────────────────────── */

void PokerWindow::draw_card(Graphics& g, int x, int y, int card, bool held) const {
    if (held) {
        g.fill_rect(x-3, y-3, CW+6, CH+6, HELD_HL);
    }

    if (card < 0) {
        /* empty slot */
        g.fill_rect(x, y, CW, CH, 0x0C4A18u);
        g.draw_rect(x, y, CW, CH, 0x2A8A40u);
        return;
    }

    int rank = card % 13;
    int suit = card / 13;

    /* Card body */
    g.fill_rect(x,   y,   CW,   CH,   WHITE);
    g.draw_rect(x,   y,   CW,   CH,   0x444444u);
    g.draw_rect(x+1, y+1, CW-2, CH-2, 0xCCCCCCu);

    /* Red for hearts (2) and diamonds (1), black for clubs (0) and spades (3) */
    unsigned int col = (suit == 1 || suit == 2) ? RED_C : BLK_C;

    static const char* RNK[] = {
        "2","3","4","5","6","7","8","9","10","J","Q","K","A"
    };
    static const char  SYM[] = { 'C', 'D', 'H', 'S' };
    char ss[2] = { SYM[suit], '\0' };

    /* Top-left corner: rank then suit */
    g.draw_str(x+4,  y+4,  RNK[rank], col, WHITE);
    g.draw_str(x+4,  y+20, ss,        col, WHITE);

    /* Centre: suit symbol grid (3×3 for visual presence) */
    int mid_x = x + CW/2 - 4;
    int mid_y = y + CH/2 - 8;
    g.draw_str(mid_x - 8, mid_y - 12, ss, col, GFX_TRANSPARENT);
    g.draw_str(mid_x + 8, mid_y - 12, ss, col, GFX_TRANSPARENT);
    g.draw_str(mid_x,     mid_y,      ss, col, GFX_TRANSPARENT);
    g.draw_str(mid_x - 8, mid_y + 12, ss, col, GFX_TRANSPARENT);
    g.draw_str(mid_x + 8, mid_y + 12, ss, col, GFX_TRANSPARENT);

    /* Bottom-right corner (mirrored): rank then suit */
    int rw = (rank == 8) ? 16 : 8;   /* "10" is wider */
    g.draw_str(x + CW - 4 - rw, y + CH - 28, RNK[rank], col, WHITE);
    g.draw_str(x + CW - 12,     y + CH - 14, ss,        col, WHITE);
}

/* ── draw ────────────────────────────────────────────────────────────── */

void PokerWindow::draw() {
    /* Window::draw() fills client with bg_ (felt green) and renders widgets */
    Window::draw();

    Graphics& g  = Graphics::instance();
    int cx = client_x(), cy = client_y();
    int cw = client_w();

    /* Felt gradient — drawn after Window::draw() fills bg_.
       Only covers the card + info region (y=0..180) so buttons are untouched. */
    g.gradient_rect(cx, cy, cw, 180, FELT_DK, FELT_LT, 1);

    /* ── Balance & bet strip (y=4..26) ──────────────────────────── */
    char buf[64]; int p;

    p = pk_cat(buf, 0, "Solde: ");
    p = pk_cat_int(buf, p, casino_get_balance());
    p = pk_cat(buf, p, " GC");
    g.draw_str(cx+6, cy+6, buf, GOLD, GFX_TRANSPARENT);

    p = pk_cat(buf, 0, "Mise: ");
    p = pk_cat_int(buf, p, m_bet);
    p = pk_cat(buf, p, " GC  [+/-]");
    g.draw_str(cx+200, cy+6, buf, 0xDDDDDDu, GFX_TRANSPARENT);

    if (m_state == State::Holding) {
        g.draw_str(cx + cw - 8*10 - 6, cy+6,
                   "Cliquez HOLD", 0xFFEE44u, GFX_TRANSPARENT);
    }

    /* ── Cards (y=38..178) ───────────────────────────────────────── */
    for (int i = 0; i < 5; ++i) {
        int card_x = cx + CMX + i*(CW+CGP);
        draw_card(g, card_x, cy + CY, m_hand[i], m_held[i]);
    }

    /* ── Status line (y=214..230) ────────────────────────────────── */
    if (m_msg[0]) {
        int sw = pk_slen(m_msg) * 8;
        unsigned int mcol = (m_last_win > 0) ? 0x44FF44u : 0xFF8844u;
        g.draw_str(cx + (cw - sw)/2, cy+214, m_msg, mcol, GFX_TRANSPARENT);
    }

    /* ── Payout table (y=278..396) ───────────────────────────────── */
    static const char* HAND_NAMES[] = {
        "Royal Flush",       "Quinte Flush",  "Carre",
        "Full House",        "Couleur",        "Quinte",
        "Brelan",            "Double Paire",   "Valets ou mieux"
    };
    static const int MULTS[] = {
        MUL_ROYAL, MUL_SFLUSH, MUL_FOUR,
        MUL_FULL,  MUL_FLUSH,  MUL_STRT,
        MUL_THREE, MUL_TPAIR,  MUL_JACKS
    };

    int ty = cy + 278;
    g.draw_str(cx+6, ty, "Main                    Gain x Mise", 0x888888u, GFX_TRANSPARENT);
    g.draw_hline(cx+4, ty+13, cw-8, 0x335533u);
    ty += 16;

    for (int i = 0; i < 9; ++i) {
        /* Highlight the winning hand */
        int  mult    = evaluate();
        bool winning = (m_state == State::Betting && m_last_win > 0
                        && MULTS[i] == mult && mult > 0);
        unsigned int nc = winning ? 0xFFFF44u : 0x88BB88u;

        g.draw_str(cx+6,     ty, HAND_NAMES[i], nc, GFX_TRANSPARENT);
        /* Right-align the multiplier */
        char mbuf[8]; pk_itos(MULTS[i], mbuf);
        int mw = pk_slen(mbuf)*8;
        g.draw_str(cx+cw-mw-6, ty, mbuf, nc, GFX_TRANSPARENT);
        ty += 13;
    }
}

/* ── open_poker_window: extern "C" factory ───────────────────────────── */

extern "C" void open_poker_window(void) {
    auto win = Greg::make_ref<PokerWindow>();
    win->setup(60, 30, 660, 420, "Video Poker  Jacks or Better", 0x1A6030u);

    PokerWindow* raw = win.ptr();

    /* 5 HOLD buttons (positions relative to client origin) */
    static const char* hold_labels[5] = { "HOLD","HOLD","HOLD","HOLD","HOLD" };
    for (int i = 0; i < 5; ++i) {
        auto btn = Greg::make_ref<GUI::Button>(
            PokerWindow::CMX + i*(PokerWindow::CW + PokerWindow::CGP),
            184,
            PokerWindow::CW, 24,
            hold_labels[i],
            [raw, i](){ raw->on_hold(i); });
        raw->m_hold_btn[i] = btn.ptr();
        win->add_widget(Greg::move(btn));
    }

    /* DEAL / DRAW button — centred horizontally */
    {
        int deal_x = (656 - 130) / 2;
        auto btn = Greg::make_ref<GUI::Button>(
            deal_x, 238, 130, 28, "DEAL",
            [raw](){ raw->on_deal(); });
        raw->m_deal_btn = btn.ptr();
        win->add_widget(Greg::move(btn));
    }

    WindowManager::instance().add_window(Greg::move(win));
}
