/* MinesweeperWindow — « Démineur du Donjon » for GregOS.
   Mines = Drakkar's eggs. Left-click uncovers, right-click plants a banner,
   click the crest to restart. Deterministic RNG (jiffies-seeded), no libc.  */

#include "../include/MinesweeperWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

extern "C" volatile unsigned long jiffies;

/* ── palette ─────────────────────────────────────────────────────────────── */
static const unsigned int NUMCOL[9] = {
    0x000000u,
    0x5AA0FFu,  /* 1 blue    */  0x4CD964u, /* 2 green   */
    0xFF5A4Cu,  /* 3 red     */  0xC08CFFu, /* 4 violet  */
    0xFFB000u,  /* 5 amber   */  0x40E0D0u, /* 6 teal    */
    0xF0F0F0u,  /* 7 white   */  0x9AA0A0u, /* 8 grey    */
};
static constexpr unsigned int TILE_FACE = 0x2A2622u;   /* covered tile        */
static constexpr unsigned int TILE_LT   = 0x4E4238u;   /* raised highlight    */
static constexpr unsigned int TILE_DK   = 0x140F0Cu;   /* raised shadow       */
static constexpr unsigned int OPEN_BG   = 0x161210u;   /* uncovered tile      */
static constexpr unsigned int OPEN_GRID = 0x0A0806u;
static constexpr unsigned int BOOM_BG   = 0x5A1410u;   /* the fatal egg's tile */

/* ── small helpers ───────────────────────────────────────────────────────── */
static int ms_slen(const char* s){ int n=0; while(s[n]) ++n; return n; }

/* right-aligned 3-digit counter into a fixed 4-byte buffer */
static void ms_ctr3(int v, char* out)
{
    bool neg = v < 0; if (neg) v = -v; if (v > 999) v = 999;
    out[0] = neg ? '-' : (char)('0' + (v / 100) % 10);
    out[1] = (char)('0' + (v / 10) % 10);
    out[2] = (char)('0' + v % 10);
    out[3] = 0;
}

static int ms_isqrt(int n){ if(n<=0) return 0; int x=n,y=(x+1)/2; while(y<x){x=y;y=(x+n/x)/2;} return x; }

static void ms_fill_circle(Graphics& g, int cx, int cy, int rad, unsigned int col)
{
    for (int dy = -rad; dy <= rad; ++dy) {
        int w = ms_isqrt(rad*rad - dy*dy);
        g.draw_hline(cx - w, cy + dy, 2*w + 1, col);
    }
}

/* ── game state ──────────────────────────────────────────────────────────── */

unsigned int MinesweeperWindow::rnd()
{
    m_seed = m_seed * 1103515245u + 12345u;
    return (m_seed >> 16) & 0x7FFFu;
}

void MinesweeperWindow::new_game()
{
    for (int i = 0; i < ROWS * COLS; ++i) { m_mine[i] = 0; m_state[i] = HIDDEN; m_adj[i] = 0; }
    m_status  = PLAYING;
    m_placed  = false;
    m_flags   = 0;
    m_lost_idx = -1;
    m_start_j = 0;
    m_end_j   = 0;
    m_seed    = 0xC0FFEEu;
}

void MinesweeperWindow::place_mines(int safe)
{
    int sr = safe / COLS, sc = safe % COLS;
    int placed = 0;
    while (placed < MINES) {
        int idx = (int)(rnd() % (unsigned)(ROWS * COLS));
        if (m_mine[idx]) continue;
        int r = idx / COLS, c = idx % COLS;
        /* keep the first click and its 8 neighbours clear → always opens area */
        if (r >= sr - 1 && r <= sr + 1 && c >= sc - 1 && c <= sc + 1) continue;
        m_mine[idx] = 1; ++placed;
    }
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) {
            if (m_mine[r*COLS + c]) continue;
            int n = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    int nr = r + dr, nc = c + dc;
                    if (nr >= 0 && nr < ROWS && nc >= 0 && nc < COLS && m_mine[nr*COLS + nc]) ++n;
                }
            m_adj[r*COLS + c] = (unsigned char)n;
        }
    m_placed = true;
}

void MinesweeperWindow::reveal(int idx)
{
    if (m_status != PLAYING) return;
    if (m_state[idx] != HIDDEN) return;

    if (!m_placed) {
        m_seed = (unsigned int)(jiffies * 2654435761u) ^ 0x9E3779B9u ^ (unsigned int)(idx * 40503);
        place_mines(idx);
        m_start_j = jiffies ? jiffies : 1;
    }

    if (m_mine[idx]) {                       /* woke the dragon */
        m_lost_idx = idx;
        for (int i = 0; i < ROWS * COLS; ++i)
            if (m_mine[i]) m_state[i] = REVEALED;
        m_status = LOST;
        m_end_j  = jiffies;
        return;
    }

    /* iterative flood-fill of the connected empty region */
    int stack[ROWS * COLS]; int sp = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        int cur = stack[--sp];
        if (m_state[cur] != HIDDEN) continue;
        m_state[cur] = REVEALED;
        if (m_adj[cur] == 0) {
            int r = cur / COLS, c = cur % COLS;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    int nr = r + dr, nc = c + dc;
                    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) continue;
                    int ni = nr*COLS + nc;
                    if (m_state[ni] == HIDDEN && !m_mine[ni]) stack[sp++] = ni;
                }
        }
    }
    check_win();
}

void MinesweeperWindow::toggle_flag(int idx)
{
    if (m_status != PLAYING) return;
    if (m_state[idx] == HIDDEN)      { m_state[idx] = FLAGGED; ++m_flags; }
    else if (m_state[idx] == FLAGGED){ m_state[idx] = HIDDEN;  --m_flags; }
}

void MinesweeperWindow::check_win()
{
    for (int i = 0; i < ROWS * COLS; ++i)
        if (!m_mine[i] && m_state[i] != REVEALED) return;   /* safe tile left */
    m_status = WON;
    m_end_j  = jiffies;
    for (int i = 0; i < ROWS * COLS; ++i)                    /* banner every egg */
        if (m_mine[i] && m_state[i] != FLAGGED) { m_state[i] = FLAGGED; ++m_flags; }
}

/* ── hit tests ───────────────────────────────────────────────────────────── */

int MinesweeperWindow::cell_at(int px, int py) const
{
    int gx = grid_x(), gy = grid_y();
    if (px < gx || py < gy) return -1;
    int c = (px - gx) / CELL, r = (py - gy) / CELL;
    if (c < 0 || c >= COLS || r < 0 || r >= ROWS) return -1;
    return r * COLS + c;
}

bool MinesweeperWindow::crest_hit(int px, int py) const
{
    int fw = 96, fh = 30;
    int fx = client_x() + (client_w() - fw) / 2, fy = client_y() + 8;
    return px >= fx && px < fx + fw && py >= fy && py < fy + fh;
}

/* ── input ───────────────────────────────────────────────────────────────── */

bool MinesweeperWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_BUTTON && hit_test(e.mouse.x, e.mouse.y)) {
        int mx = e.mouse.x, my = e.mouse.y;
        if (e.mouse.buttons & 0x01) {              /* left: crest reset or uncover */
            if (crest_hit(mx, my)) { new_game(); return true; }
            int idx = cell_at(mx, my);
            if (idx >= 0) { reveal(idx); return true; }
        } else if (e.mouse.buttons & 0x02) {       /* right: plant / pull banner   */
            int idx = cell_at(mx, my);
            if (idx >= 0) { toggle_flag(idx); return true; }
        }
    }
    return Window::on_event(e);
}

bool MinesweeperWindow::handle_char(int c)
{
    if (c == 'n' || c == 'N' || c == 'r' || c == 'R') { new_game(); return true; }
    if (c == KEY_ESC) { request_close(); return true; }
    return false;
}

/* ── rendering ───────────────────────────────────────────────────────────── */

static void ms_draw_flag(Graphics& g, int x, int y)
{
    int px = x + MinesweeperWindow::CELL / 2 - 1;
    g.draw_vline(px, y + 6, MinesweeperWindow::CELL - 14, 0xC9A24Bu);   /* gold pole */
    for (int i = 0; i < 6; ++i)
        g.draw_hline(px - 6 + i, y + 7 + i, 6 - i, 0xC83028u);          /* blood pennant */
    g.fill_rect(x + 6, y + MinesweeperWindow::CELL - 9, MinesweeperWindow::CELL - 13, 3, 0x8B6A2Au);
}

static void ms_draw_egg(Graphics& g, int x, int y)
{
    int cx = x + (MinesweeperWindow::CELL - 1) / 2, cy = y + (MinesweeperWindow::CELL - 1) / 2;
    ms_fill_circle(g, cx, cy + 1, 8, 0x8B1A1Au);       /* blood shell        */
    ms_fill_circle(g, cx, cy + 1, 8, 0x8B1A1Au);
    ms_fill_circle(g, cx - 2, cy - 2, 3, 0xFF6A20u);   /* ember glow         */
    g.draw_vline(cx - 3, cy - 5, 12, 0x3A0A08u);       /* cracks             */
    g.draw_vline(cx + 3, cy - 4, 11, 0x3A0A08u);
    g.put_pixel(cx - 4, cy - 4, 0xFFE0A0u);            /* hot speck          */
}

void MinesweeperWindow::draw_cell(int idx) const
{
    Graphics& g = Graphics::instance();
    int r = idx / COLS, c = idx % COLS;
    int x = grid_x() + c * CELL, y = grid_y() + r * CELL;
    int s = CELL - 1;
    unsigned char st = m_state[idx];

    if (st == REVEALED) {
        bool boom = (m_status == LOST && idx == m_lost_idx);
        g.fill_rect(x, y, s, s, boom ? BOOM_BG : OPEN_BG);
        g.draw_hline(x, y, s, OPEN_GRID);
        g.draw_vline(x, y, s, OPEN_GRID);
        if (m_mine[idx]) {
            ms_draw_egg(g, x, y);
        } else if (m_adj[idx] > 0) {
            g.draw_char(x + (s - 8) / 2, y + (s - 16) / 2,
                        (char)('0' + m_adj[idx]), NUMCOL[m_adj[idx]], GFX_TRANSPARENT);
        }
    } else {
        g.fill_rect(x, y, s, s, TILE_FACE);
        g.draw_hline(x, y,       s, TILE_LT);
        g.draw_vline(x, y,       s, TILE_LT);
        g.draw_hline(x, y + s-1, s, TILE_DK);
        g.draw_vline(x + s-1, y, s, TILE_DK);
        if (st == FLAGGED) {
            /* on loss, a banner on a non-egg reads as a wrong guess */
            if (m_status == LOST && !m_mine[idx]) {
                g.draw_char(x + (s - 8) / 2, y + (s - 16) / 2, 'X', 0xFF4030u, GFX_TRANSPARENT);
            } else {
                ms_draw_flag(g, x, y);
            }
        }
    }
}

void MinesweeperWindow::draw_header() const
{
    Graphics& g = Graphics::instance();
    int cx = client_x(), cy = client_y(), cw = client_w();

    g.fill_rect(cx, cy, cw, HDR_H - 4, 0x0E0B0Au);
    g.draw_hline(cx, cy + HDR_H - 4, cw, Theme::GOLD_DEEP);

    /* left — eggs remaining (mines minus banners) on a sunken red readout */
    char buf[4]; ms_ctr3(MINES - m_flags, buf);
    int lx = cx + 8, ly = cy + 8;
    g.fill_rect(lx, ly, 3*8 + 8, 22, 0x1A0505u);
    g.draw_rect(lx, ly, 3*8 + 8, 22, 0x3A0A08u);
    g.draw_str(lx + 4, ly + 3, buf, 0xFF5A4Cu, GFX_TRANSPARENT);

    /* centre — the crest / status button (click = new game) */
    int fw = 96, fh = 30, fx = cx + (cw - fw) / 2, fy = cy + 8;
    unsigned int face = (m_status == WON) ? 0x1A4A20u
                      : (m_status == LOST) ? 0x501818u : 0x2A2418u;
    unsigned int txtc = (m_status == WON) ? 0x8CFFB0u
                      : (m_status == LOST) ? 0xFF8C7Cu : Theme::GOLD;
    g.fill_rect(fx, fy, fw, fh, face);
    g.draw_hline(fx, fy,      fw, Theme::GOLD_DIM);
    g.draw_vline(fx, fy,      fh, Theme::GOLD_DIM);
    g.draw_hline(fx, fy+fh-1, fw, Theme::GOUFFRE);
    g.draw_vline(fx+fw-1, fy, fh, Theme::GOUFFRE);
    const char* lbl = (m_status == WON) ? "VICTOIRE"
                    : (m_status == LOST) ? "MORT" : "CHASSE";
    g.draw_str(fx + (fw - ms_slen(lbl)*8)/2, fy + (fh - 16)/2 + 2, lbl, txtc, GFX_TRANSPARENT);

    /* right — timer (seconds since first uncover) */
    unsigned long el = 0;
    if (m_placed) {
        unsigned long ref = (m_status == PLAYING) ? jiffies : m_end_j;
        el = (ref - m_start_j) / 100;
    }
    char tb[4]; ms_ctr3((int)el, tb);
    int rx = cx + cw - 8 - (3*8 + 8), ry = cy + 8;
    g.fill_rect(rx, ry, 3*8 + 8, 22, 0x051005u);
    g.draw_rect(rx, ry, 3*8 + 8, 22, 0x0A2A0Au);
    g.draw_str(rx + 4, ry + 3, tb, 0x4CD964u, GFX_TRANSPARENT);
}

void MinesweeperWindow::draw()
{
    Window::draw();
    Graphics& g = Graphics::instance();

    /* fixed 10×10 layout: don't paint past a user-shrunk frame */
    if (client_w() < 2 * MARGIN + COLS * CELL ||
        client_h() < HDR_H + ROWS * CELL + MARGIN) {
        g.draw_str(client_x() + 6, client_y() + 6, "Trop petit",
                   0x9AA0A0u, GFX_TRANSPARENT);
        return;
    }

    /* board backdrop + frame */
    int bx = grid_x() - 2, by = grid_y() - 2;
    int bw = COLS * CELL + 3, bh = ROWS * CELL + 3;
    g.fill_rect(client_x(), client_y() + HDR_H - 4, client_w(),
                client_h() - HDR_H + 4, 0x0A0908u);
    g.draw_rect(bx, by, bw, bh, Theme::GOLD_DEEP);

    draw_header();
    for (int i = 0; i < ROWS * COLS; ++i) draw_cell(i);
}

/* ── factory ─────────────────────────────────────────────────────────────── */

extern "C" void open_minesweeper_window(void)
{
    auto win = Greg::make_ref<MinesweeperWindow>();
    /* client_w = 2*MARGIN + COLS*CELL = 16 + 280 = 296 → window 300
       client_h = HDR_H + ROWS*CELL + MARGIN = 46 + 280 + 8 = 334 → window 358 */
    win->setup(250, 54, 300, 360, "Demineur du Donjon", 0x141210u);
    WindowManager::instance().add_window(Greg::move(win));
}
