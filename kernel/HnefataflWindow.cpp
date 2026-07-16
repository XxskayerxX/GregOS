/* HnefataflWindow — « Hnefatafl, le Jeu du Roi » for GregOS.
   Tablut 9×9 vs the realm's AI. Rules engine: kernel/hnefatafl_core.c.
   Mouse: click one of your pieces, then a highlighted destination. The AI
   answers synchronously (negamax depth 3, node-budgeted → bounded time).    */

#include "../include/HnefataflWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

/* ── palette ─────────────────────────────────────────────────────────────── */
static constexpr unsigned int SQ_LIGHT  = 0x16110Cu;   /* obsidian checker    */
static constexpr unsigned int SQ_DARK   = 0x100C09u;
static constexpr unsigned int SQ_THRONE = 0x1E0B0Bu;   /* blood-tinted centre */
static constexpr unsigned int SQ_CORNER = 0x1C160Au;   /* gold-tinted exits   */
static constexpr unsigned int ATT_BODY  = 0x8B1A1Au;   /* blood attacker      */
static constexpr unsigned int ATT_RIM   = 0x4A0E0Eu;
static constexpr unsigned int DEF_BODY  = 0xC9A24Bu;   /* gold defender       */
static constexpr unsigned int DEF_RIM   = 0x6B5223u;
static constexpr unsigned int KING_EYE  = 0x35FF6Au;   /* Drakkar-green rune  */
static constexpr unsigned int SEL_COL   = 0xFFD24Au;   /* selection outline   */
static constexpr unsigned int DOT_COL   = 0xB08A30u;   /* legal-move dot      */
static constexpr unsigned int LAST_COL  = 0xC85018u;   /* last-move ember     */

static int hw_slen(const char* s) { int n = 0; while (s[n]) ++n; return n; }
static char* hw_puts(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* hw_putn(char* p, int v)
{
    char t[8]; int n = 0;
    if (v <= 0) { *p++ = '0'; return p; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}

static void hw_fill_circle(Graphics& g, int cx, int cy, int r, unsigned int col)
{
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r)
                g.put_pixel(cx + dx, cy + dy, col);
}

/* ── game flow ───────────────────────────────────────────────────────────── */
void HnefataflWindow::to_choice()
{
    hn_init(m_board);
    m_phase = CHOOSE; m_human = HN_SIDE_DEF; m_turn = HN_SIDE_ATT;
    m_winner = HN_NONE; m_sel = -1; m_nlegal = 0;
    m_last_from = -1; m_last_to = -1;
    m_lost_att = 0; m_lost_def = 0;
}

void HnefataflWindow::start_game(int human_side)
{
    m_human = human_side;
    m_phase = PLAY;
    if (m_human == HN_SIDE_DEF) ai_turn();   /* attackers (AI) open the game */
    refresh_legal();
}

void HnefataflWindow::refresh_legal()
{
    m_nlegal = hn_gen_moves(m_board, m_turn, m_legal, HN_MAX_MOVES);
}

void HnefataflWindow::play_move(hn_move m)
{
    int mover_att = (m_board[m.from] == HN_ATT);
    int caps = hn_apply(m_board, m);
    if (mover_att && hn_king_pos(m_board) < 0) --caps;   /* le roi n'est pas un soldat */
    if (mover_att) m_lost_def += caps; else m_lost_att += caps;
    m_last_from = m.from; m_last_to = m.to;
    m_sel = -1;
    m_turn = 1 - m_turn;
    m_winner = hn_winner(m_board, m_turn);
    if (m_winner != HN_NONE) m_phase = OVER;
}

void HnefataflWindow::ai_turn()
{
    if (m_phase == OVER || m_turn == m_human) return;
    hn_move m = hn_ai_move(m_board, m_turn, AI_DEPTH);
    play_move(m);
}

/* ── geometry ────────────────────────────────────────────────────────────── */
int HnefataflWindow::cell_at(int px, int py) const
{
    int rx = px - board_x(), ry = py - board_y();
    if (rx < 0 || ry < 0) return -1;
    int x = rx / CELL, y = ry / CELL;
    if (x > 8 || y > 8) return -1;
    return y * 9 + x;
}

/* ── input ───────────────────────────────────────────────────────────────── */
bool HnefataflWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01) &&
        hit_test(e.mouse.x, e.mouse.y)) {
        int mx = e.mouse.x, my = e.mouse.y;

        if (m_phase == CHOOSE) {
            /* two option rows centred on the board area */
            int bx = board_x(), by = board_y();
            int ox = bx + 30, ow = 9 * CELL - 60;
            if (mx >= ox && mx < ox + ow) {
                if (my >= by + 130 && my < by + 170) { start_game(HN_SIDE_DEF); return true; }
                if (my >= by + 190 && my < by + 230) { start_game(HN_SIDE_ATT); return true; }
            }
            return Window::on_event(e);
        }

        if (m_phase == PLAY && m_turn == m_human) {
            int idx = cell_at(mx, my);
            if (idx >= 0) {
                int v = m_board[idx];
                bool mine = (v != HN_EMPTY) &&
                            ((m_human == HN_SIDE_ATT) ? (v == HN_ATT)
                                                      : (v == HN_DEF || v == HN_KING));
                if (mine) { m_sel = idx; return true; }
                if (m_sel >= 0) {
                    for (int i = 0; i < m_nlegal; ++i)
                        if (m_legal[i].from == m_sel && m_legal[i].to == idx) {
                            play_move(m_legal[i]);
                            if (m_phase == PLAY) { ai_turn(); refresh_legal(); }
                            return true;
                        }
                }
                m_sel = -1;
                return true;
            }
        }
    }
    return Window::on_event(e);
}

bool HnefataflWindow::handle_char(int c)
{
    if (c == KEY_ESC)         { request_close(); return true; }
    if (c == 'n' || c == 'N') { to_choice();     return true; }
    if (m_phase == CHOOSE) {
        if (c == 'd' || c == 'D') { start_game(HN_SIDE_DEF); return true; }
        if (c == 'a' || c == 'A') { start_game(HN_SIDE_ATT); return true; }
    }
    return false;
}

/* ── rendering ───────────────────────────────────────────────────────────── */
void HnefataflWindow::draw_board() const
{
    Graphics& g = Graphics::instance();
    int bx = board_x(), by = board_y();

    for (int y = 0; y < 9; ++y)
        for (int x = 0; x < 9; ++x) {
            int idx = y * 9 + x;
            unsigned int bg = ((x + y) & 1) ? SQ_DARK : SQ_LIGHT;
            if (idx == 40) bg = SQ_THRONE;
            else if (idx == 0 || idx == 8 || idx == 72 || idx == 80) bg = SQ_CORNER;
            int px = bx + x * CELL, py = by + y * CELL;
            g.fill_rect(px, py, CELL, CELL, bg);

            if (idx == 40) {                          /* throne rune ◆ */
                for (int d = 0; d <= 6; ++d) {
                    g.draw_hline(px + CELL/2 - d, py + CELL/2 - (6 - d), 2*d + 1, 0x3A1010u);
                    g.draw_hline(px + CELL/2 - d, py + CELL/2 + (6 - d), 2*d + 1, 0x3A1010u);
                }
            }
            if (idx == 0 || idx == 8 || idx == 72 || idx == 80) {  /* exit mark */
                g.draw_rect(px + 14, py + 14, CELL - 28, CELL - 28, Theme::GOLD_DEEP);
            }
        }

    g.draw_rect(bx - 1, by - 1, 9 * CELL + 2, 9 * CELL + 2, Theme::GOLD_DEEP);

    /* last move trace */
    if (m_last_from >= 0) {
        int fx = bx + (m_last_from % 9) * CELL, fy = by + (m_last_from / 9) * CELL;
        int tx = bx + (m_last_to   % 9) * CELL, ty = by + (m_last_to   / 9) * CELL;
        g.draw_rect(fx, fy, CELL, CELL, LAST_COL);
        g.draw_rect(tx, ty, CELL, CELL, LAST_COL);
    }

    /* selection + legal destinations */
    if (m_phase == PLAY && m_sel >= 0) {
        int sx = bx + (m_sel % 9) * CELL, sy = by + (m_sel / 9) * CELL;
        g.draw_rect(sx, sy, CELL, CELL, SEL_COL);
        g.draw_rect(sx + 1, sy + 1, CELL - 2, CELL - 2, SEL_COL);
        for (int i = 0; i < m_nlegal; ++i)
            if (m_legal[i].from == m_sel) {
                int dx = bx + (m_legal[i].to % 9) * CELL + CELL / 2;
                int dy = by + (m_legal[i].to / 9) * CELL + CELL / 2;
                hw_fill_circle(g, dx, dy, 4, DOT_COL);
            }
    }

    for (int i = 0; i < 81; ++i)
        if (m_board[i] != HN_EMPTY) draw_piece(i);
}

void HnefataflWindow::draw_piece(int idx) const
{
    Graphics& g = Graphics::instance();
    int cx = board_x() + (idx % 9) * CELL + CELL / 2;
    int cy = board_y() + (idx / 9) * CELL + CELL / 2;
    int v = m_board[idx];

    if (v == HN_ATT) {
        hw_fill_circle(g, cx, cy, 13, ATT_RIM);
        hw_fill_circle(g, cx, cy, 11, ATT_BODY);
        hw_fill_circle(g, cx - 3, cy - 4, 3, 0xB84040u);      /* glint     */
    } else {
        hw_fill_circle(g, cx, cy, 13, DEF_RIM);
        hw_fill_circle(g, cx, cy, 11, DEF_BODY);
        if (v == HN_KING) {
            hw_fill_circle(g, cx, cy, 6, 0x2A2008u);          /* dark iris */
            hw_fill_circle(g, cx, cy, 4, KING_EYE);           /* the Eye   */
            g.draw_hline(cx - 7, cy - 12, 15, Theme::GOLD_HI);/* crown bar */
            g.fill_rect(cx - 7, cy - 15, 3, 4, Theme::GOLD_HI);
            g.fill_rect(cx - 1, cy - 16, 3, 5, Theme::GOLD_HI);
            g.fill_rect(cx + 5, cy - 15, 3, 4, Theme::GOLD_HI);
        } else {
            hw_fill_circle(g, cx - 3, cy - 4, 3, 0xE8CC80u);  /* glint     */
        }
    }
}

void HnefataflWindow::draw_status() const
{
    Graphics& g = Graphics::instance();
    int sy = client_y() + 4;
    char buf[96]; char* p = buf;

    if (m_phase == CHOOSE) {
        p = hw_puts(p, "Choisissez votre camp");
    } else if (m_phase == OVER) {
        p = hw_puts(p, (m_winner == HN_WIN_DEF) ? "Victoire des Defenseurs"
                                                : "Victoire des Assaillants");
    } else {
        p = hw_puts(p, "Trait : ");
        p = hw_puts(p, (m_turn == HN_SIDE_ATT) ? "Assaillants" : "Defenseurs");
        p = hw_puts(p, (m_turn == m_human) ? " (vous)" : " (IA)");
    }
    *p = 0;
    g.draw_str(client_x() + MARGIN, sy + 4, buf, Theme::AMBER, GFX_TRANSPARENT);

    if (m_phase != CHOOSE) {
        p = buf;
        p = hw_puts(p, "Pertes  A:"); p = hw_putn(p, m_lost_att);
        p = hw_puts(p, "  D:");       p = hw_putn(p, m_lost_def);
        *p = 0;
        int w = hw_slen(buf) * 8;
        g.draw_str(client_x() + client_w() - MARGIN - w, sy + 4, buf,
                   Theme::ASH, GFX_TRANSPARENT);
    }
}

void HnefataflWindow::draw_choice() const
{
    Graphics& g = Graphics::instance();
    int bx = board_x(), by = board_y();
    int ox = bx + 30, ow = 9 * CELL - 60;

    g.fill_rect(ox, by + 90, ow, 160, 0x0C0906u);
    g.draw_rect(ox, by + 90, ow, 160, Theme::GOLD_DEEP);
    const char* t = "H N E F A T A F L";
    g.draw_str(ox + (ow - hw_slen(t) * 8) / 2, by + 102, t, Theme::GOLD_HI, GFX_TRANSPARENT);

    g.fill_rect(ox + 10, by + 130, ow - 20, 34, 0x141006u);
    g.draw_rect(ox + 10, by + 130, ow - 20, 34, DEF_BODY);
    const char* d = "[D] Defendre le Roi";
    g.draw_str(ox + (ow - hw_slen(d) * 8) / 2, by + 139, d, DEF_BODY, GFX_TRANSPARENT);

    g.fill_rect(ox + 10, by + 190, ow - 20, 34, 0x140808u);
    g.draw_rect(ox + 10, by + 190, ow - 20, 34, ATT_BODY);
    const char* a = "[A] Mener l'Assaut";
    g.draw_str(ox + (ow - hw_slen(a) * 8) / 2, by + 199, a, 0xC04848u, GFX_TRANSPARENT);
}

void HnefataflWindow::draw_over() const
{
    Graphics& g = Graphics::instance();
    int bx = board_x(), by = board_y();
    int pw = 280, ph = 70;
    int px = bx + (9 * CELL - pw) / 2, py = by + (9 * CELL - ph) / 2;

    bool def_won = (m_winner == HN_WIN_DEF);
    unsigned int col = def_won ? Theme::GOLD_HI : 0xE04030u;
    g.fill_rect(px, py, pw, ph, 0x0C0906u);
    g.draw_rect(px, py, pw, ph, col);
    g.draw_rect(px + 1, py + 1, pw - 2, ph - 2, col);

    /* a side also wins when its opponent has no legal move — say so honestly */
    int k = hn_king_pos(m_board);
    bool escaped = (k == 0 || k == 8 || k == 72 || k == 80);
    const char* t1 = def_won
        ? (escaped ? "LE ROI S'EST ECHAPPE !" : "L'ASSAUT EST PARALYSE !")
        : (k < 0   ? "LE ROI EST TOMBE"       : "LE CAMP DU ROI EST FIGE");
    const char* t2 = "[N] Nouvelle partie";
    g.draw_str(px + (pw - hw_slen(t1) * 8) / 2, py + 16, t1, col, GFX_TRANSPARENT);
    g.draw_str(px + (pw - hw_slen(t2) * 8) / 2, py + 42, t2, Theme::ASH, GFX_TRANSPARENT);
}

void HnefataflWindow::draw()
{
    Window::draw();
    Graphics& g = Graphics::instance();
    g.fill_rect(client_x(), client_y(), client_w(), client_h(), 0x0A0806u);

    /* fixed 360×360 layout: don't paint past a user-shrunk frame */
    if (client_w() < 2 * MARGIN + 9 * CELL ||
        client_h() < 4 + HDR_H + 9 * CELL + FOOT_H + 4) {
        g.draw_str(client_x() + 6, client_y() + 6, "Trop petit",
                   Theme::ASH, GFX_TRANSPARENT);
        return;
    }

    draw_status();
    draw_board();
    if (m_phase == CHOOSE) draw_choice();
    if (m_phase == OVER)   draw_over();

    const char* hint = "[N] Nouvelle partie   [Echap] Quitter";
    g.draw_str(client_x() + MARGIN, board_y() + 9 * CELL + 6, hint,
               Theme::GOLD_DEEP, GFX_TRANSPARENT);
}

/* ── factory ─────────────────────────────────────────────────────────────── */
extern "C" void open_hnefatafl_window(void)
{
    auto win = Greg::make_ref<HnefataflWindow>();
    /* client: 8 + 360 + 8 = 376 wide; 4+24 + 360 + 20+4 = 412 tall
       window: 376+4 = 380 × 412+24 = 436                                    */
    win->setup(210, 60, 380, 436, "Hnefatafl - le Jeu du Roi", 0x0A0806u);
    WindowManager::instance().add_window(Greg::move(win));
}
