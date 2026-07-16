/* StartMenuWindow — popup app launcher above the taskbar.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/StartMenuWindow.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/tty.h"

extern "C" void start_menu_launch_games(void);
extern "C" void start_menu_launch_sysinfo(void);
extern "C" void open_calc_window(void);
extern "C" void open_system_monitor_window(void);
extern "C" void open_browser_window(void);
extern "C" void open_paint_window(void);
extern "C" void open_clock_window(void);
extern "C" void open_minesweeper_window(void);
extern "C" void open_dungeon_window(void);
extern "C" void open_hnefatafl_window(void);

static const char* s_labels[StartMenuWindow::N_ITEMS] = {
    "GregNet (Web)",
    "Terminal",
    "Fichiers",
    "GregPaint",
    "Horloge",
    "Casino",
    "Jeux",
    "Calculatrice",
    "Demineur",
    "Donjon",
    "Hnefatafl",
    "Moniteur",
    "Systeme",
};

/* ── Inline 2-line raised bevel (can't call static draw_bevel from wm) ── */
static void sm_bevel(Graphics& g, int x, int y, int w, int h)
{
    g.draw_hline(x,         y,         w, Theme::BEVEL_OUTER_LT);
    g.draw_vline(x,         y,         h, Theme::BEVEL_OUTER_LT);
    g.draw_hline(x,         y + h - 1, w, Theme::BEVEL_OUTER_DK);
    g.draw_vline(x + w - 1, y,         h, Theme::BEVEL_OUTER_DK);
    if (w > 2 && h > 2) {
        g.draw_hline(x + 1,     y + 1,     w - 2, Theme::BEVEL_INNER_LT);
        g.draw_vline(x + 1,     y + 1,     h - 2, Theme::BEVEL_INNER_LT);
        g.draw_hline(x + 1,     y + h - 2, w - 2, Theme::BEVEL_INNER_DK);
        g.draw_vline(x + w - 2, y + 1,     h - 2, Theme::BEVEL_INNER_DK);
    }
}

/* ── draw ─────────────────────────────────────────────────────────────── */

void StartMenuWindow::draw()
{
    if (!_visible) return;
    Graphics& g = Graphics::instance();

    /* Body fill + raised bevel */
    g.fill_rect(_x, _y, _w, _h, Theme::WIN_BG);
    sm_bevel(g, _x, _y, _w, _h);

    /* Header: navy gradient (same as focused title bar) */
    g.gradient_rect(_x + 2, _y + 2, _w - 4, HEADER_H - 2,
                    Theme::TITLE_FOCUS_A, Theme::TITLE_FOCUS_B, 0);
    g.draw_str(_x + 8, _y + 4, "GregOS", Theme::TITLE_FG, GFX_TRANSPARENT);

    /* Separator (sunken line pair) */
    int sep_y = _y + HEADER_H;
    g.draw_hline(_x + 2, sep_y,     _w - 4, Theme::BEVEL_OUTER_DK);
    g.draw_hline(_x + 2, sep_y + 1, _w - 4, Theme::BEVEL_OUTER_LT);

    /* Items */
    for (int i = 0; i < N_ITEMS; ++i) {
        int iy  = _y + HEADER_H + 2 + i * ITEM_H;
        bool hv = (i == m_hovered);
        unsigned int ibg = hv ? Theme::TITLE_FOCUS_A : Theme::WIN_BG;
        unsigned int ifg = hv ? Theme::AMBER_HI      : Theme::AMBER;
        g.fill_rect(_x + 2, iy, _w - 4, ITEM_H, ibg);
        g.draw_str(_x + 10, iy + (ITEM_H - 16) / 2, s_labels[i], ifg,
                   GFX_TRANSPARENT);
    }
}

/* ── item_at: returns item index under (mx,my), or -1 ────────────────── */

int StartMenuWindow::item_at(int mx, int my) const
{
    (void)mx;
    int iy0 = _y + HEADER_H + 2;
    if (my < iy0) return -1;
    int idx = (my - iy0) / ITEM_H;
    if (idx < 0 || idx >= N_ITEMS) return -1;
    return idx;
}

/* ── launch_item ─────────────────────────────────────────────────────── */

void StartMenuWindow::launch_item(int idx)
{
    switch (idx) {
    case 0: open_browser_window();        break;
    case 1: tty_create_terminal_window(90, 50, 600, 420, "terminal"); break;
    case 2: open_file_manager();          break;
    case 3: open_paint_window();          break;
    case 4: open_clock_window();          break;
    case 5: open_casino_window();         break;
    case 6: start_menu_launch_games();    break;
    case 7: open_calc_window();           break;
    case 8: open_minesweeper_window();    break;
    case 9: open_dungeon_window();        break;
    case 10: open_hnefatafl_window();     break;
    case 11: open_system_monitor_window(); break;
    case 12: open_system_window();        break;
    }
}

/* ── on_event ─────────────────────────────────────────────────────────── */

bool StartMenuWindow::on_event(const Event& e)
{
    if (!_visible) return false;

    if (e.type == EVT_MOUSE_MOVE) {
        bool inside = hit_test(e.mouse.x, e.mouse.y);
        m_hovered = inside ? item_at(e.mouse.x, e.mouse.y) : -1;
        return inside;
    }

    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
        if (hit_test(e.mouse.x, e.mouse.y)) {
            int idx = item_at(e.mouse.x, e.mouse.y);
            if (idx >= 0) {
                launch_item(idx);
                request_close();
            }
            return true;
        }
    }

    return false;
}
