/* CasinoWindow — windowed casino lobby for GregOS WindowManager
   Renders the 4-game selector in the back-buffer.  No blocking loops.
   Freestanding: no libc, no exceptions.                                    */

#include "../include/CasinoWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/tty.h"
#include "../include/event.h"

extern "C" void open_poker_window(void);

extern "C" volatile unsigned long jiffies;

/* ── Casino color palette ─────────────────────────────────────────────── */
static constexpr unsigned int CW_BG   = 0x0B1A10u;  /* dark felt top */
static constexpr unsigned int CW_FELT = 0x145A1Fu;  /* lighter green */
static constexpr unsigned int CW_GOLD = 0xFFD700u;
static constexpr unsigned int CW_GOLD2= 0x887400u;

/* ── Small helper: manual strlen ─────────────────────────────────────── */
static int cw_slen(const char* s) { int n=0; while(s[n]) ++n; return n; }

/* ── Centered draw_str ───────────────────────────────────────────────── */
static void cw_cen(Graphics& g, int cx, int y, const char* s,
                   unsigned int fg, unsigned int bg)
{
    g.draw_str(cx - cw_slen(s)*8/2, y, s, fg, bg);
}

/* ── Game button (back-buffer) ───────────────────────────────────────── */
static void draw_game_btn(Graphics& g, int x, int y, int w, int h,
                          unsigned int bg, unsigned int hl,
                          const char* key, const char* label, const char* sub)
{
    g.fill_rect(x+3, y+3, w, h, 0x000000u);          /* drop shadow */
    g.fill_rect(x,   y,   w, h, bg);
    g.draw_rect(x,   y,   w, h, hl);
    g.draw_rect(x+1, y+1, w-2, h-2, hl);
    /* Key badge (top-left) */
    g.fill_rect(x+6, y+6, 20, 18, hl);
    g.draw_str(x + 6 + (20 - cw_slen(key)*8)/2, y+7, key, bg, GFX_TRANSPARENT);
    /* Label + subtitle centered */
    cw_cen(g, x+w/2, y + h/2 - 20, label, 0xFFFFFFu, GFX_TRANSPARENT);
    cw_cen(g, x+w/2, y + h/2 + 4,  sub,   0xAACC88u, GFX_TRANSPARENT);
}

/* ── draw ─────────────────────────────────────────────────────────────── */

void CasinoWindow::draw()
{
    Window::draw();
    Graphics& g = Graphics::instance();

    int cx = client_x(), cy = client_y();
    int cw = client_w(), ch = client_h();

    /* Felt gradient background */
    g.gradient_rect(cx, cy, cw, ch, CW_BG, CW_FELT, 1);

    /* Title */
    cw_cen(g, cx + cw/2, cy + 10, "~~ CASINO GREGOS ~~", CW_GOLD, GFX_TRANSPARENT);
    g.draw_hline(cx + cw/2 - 120, cy + 30, 240, CW_GOLD2);
    g.draw_hline(cx + cw/2 - 120, cy + 32, 240, CW_GOLD2);

    /* Balance (top-right) */
    int bal = casino_get_balance();
    char bbuf[24];
    int  bi = 0;
    /* "Solde: " */
    const char* pfx = "Solde: ";
    while (*pfx) bbuf[bi++] = *pfx++;
    /* int → string */
    int bv = (bal < 0) ? 0 : bal;
    if (bv == 0) { bbuf[bi++] = '0'; }
    else {
        char tmp[12]; int ti = 0;
        while (bv > 0) { tmp[ti++] = '0' + bv%10; bv /= 10; }
        for (int k = ti-1; k >= 0; --k) bbuf[bi++] = tmp[k];
    }
    bbuf[bi++] = ' '; bbuf[bi++] = 'G'; bbuf[bi++] = 'C'; bbuf[bi] = '\0';
    unsigned int bal_col = (bal < 50) ? 0xFF5555u : CW_GOLD;
    g.draw_str(cx + cw - bi*8 - 8, cy + 10, bbuf, bal_col, GFX_TRANSPARENT);

    /* 3+2 game button grid (3 top row, 2 bottom row centred) */
    int gap = 10;
    int bw3 = (cw - 4*gap) / 3;   /* button width for 3-col row */
    int bh  = (ch - 102) / 2;
    if (bw3 > 190) bw3 = 190;
    if (bh  > 120) bh  = 120;
    int bw2  = bw3 + gap/2;        /* slightly wider buttons for 2-col row */
    int gx3  = cx + (cw - 3*bw3 - 2*gap) / 2;
    int gx2  = cx + (cw - 2*bw2 - gap)   / 2;
    int gy   = cy + 46;

    /* Row 1: 3 games */
    draw_game_btn(g, gx3,            gy, bw3, bh,
                  0x180804u, 0xCC4400u, "1", "BLACKJACK",      "Battez le croupier");
    draw_game_btn(g, gx3 + bw3+gap,  gy, bw3, bh,
                  0x041204u, 0x22AA22u, "2", "ROULETTE",       "Rouge, noir...");
    draw_game_btn(g, gx3 + 2*(bw3+gap), gy, bw3, bh,
                  0x160620u, 0x8822CCu, "3", "SLOTS",          "Alignez les symboles");

    /* Row 2: 2 games centred */
    draw_game_btn(g, gx2,          gy + bh+gap, bw2, bh,
                  0x001020u, 0x2266CCu, "4", "PLINKO",         "Faites tomber");
    draw_game_btn(g, gx2 + bw2+gap, gy + bh+gap, bw2, bh,
                  0x201008u, 0xCC8822u, "5", "VIDEO POKER",    "Jacks or Better");

    /* Status message (expires after m_status_until) */
    if (m_status[0] && jiffies < m_status_until) {
        cw_cen(g, cx + cw/2, cy + ch - 36, m_status, 0xFFDD88u, GFX_TRANSPARENT);
    } else {
        m_status[0] = '\0';
    }

    /* Hint line */
    cw_cen(g, cx + cw/2, cy + ch - 18,
           "[1-5] Lancer le jeu  [ESC] Fermer", 0x557755u, GFX_TRANSPARENT);
}

/* ── on_event: mouse click → hit-test each game button ───────────────── */

bool CasinoWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
        int mx = e.mouse.x, my = e.mouse.y;

        /* Reproduce the exact geometry used in draw() */
        int cx = client_x(), cy = client_y();
        int cw = client_w(), ch = client_h();
        int gap  = 10;
        int bw3  = (cw - 4*gap) / 3; if (bw3 > 190) bw3 = 190;
        int bh   = (ch - 102)   / 2; if (bh  > 120) bh  = 120;
        int bw2  = bw3 + gap/2;
        int gx3  = cx + (cw - 3*bw3 - 2*gap) / 2;
        int gx2  = cx + (cw - 2*bw2 - gap)   / 2;
        int gy   = cy + 46;

        /* Which button was clicked? (-1 = none) */
        int game = -1;
        /* Row 1 */
        if (mx>=gx3            && mx<gx3+bw3            && my>=gy && my<gy+bh) game=0;
        if (mx>=gx3+bw3+gap    && mx<gx3+2*(bw3+gap)    && my>=gy && my<gy+bh) game=1;
        if (mx>=gx3+2*(bw3+gap)&& mx<gx3+2*(bw3+gap)+bw3&& my>=gy && my<gy+bh) game=2;
        /* Row 2 */
        int gy2 = gy + bh + gap;
        if (mx>=gx2       && mx<gx2+bw2       && my>=gy2 && my<gy2+bh) game=3;
        if (mx>=gx2+bw2+gap && mx<gx2+2*bw2+gap && my>=gy2 && my<gy2+bh) game=4;

        if (game == 4) {
            open_poker_window();
            return true;
        }
        if (game >= 0) {
            request_close();
            launch_casino_game(game);
            return true;
        }
    }
    return Window::on_event(e);
}

/* ── handle_char ─────────────────────────────────────────────────────── */

bool CasinoWindow::handle_char(int c)
{
    if (c >= '1' && c <= '4') {
        request_close();
        launch_casino_game(c - '1');
        return true;
    }
    if (c == '5') {
        open_poker_window();
        return true;
    }
    if (c == KEY_ESC) {
        request_close();
        return true;
    }
    return false;
}

/* ── open_casino_window: extern "C" bridge for kernel.c ──────────────── */

extern "C" void open_casino_window(void)
{
    auto win = Greg::make_ref<CasinoWindow>();
    win->setup(80, 60, 620, 440, "Casino GregOS", 0x000000u);
    WindowManager::instance().add_window(Greg::move(win));
}
