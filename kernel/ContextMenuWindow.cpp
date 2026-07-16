/* ContextMenuWindow — right-click desktop context menu.
   Freestanding: no libc, no exceptions.                                    */

#include "../include/ContextMenuWindow.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/tty.h"

extern "C" void open_file_manager(void);
extern "C" void open_calc_window(void);
extern "C" void open_system_window(void);
extern "C" void open_system_monitor_window(void);
extern "C" void open_casino_window(void);
extern "C" void open_browser_window(void);
extern "C" void open_paint_window(void);
extern "C" void open_clock_window(void);
extern "C" void open_dungeon_window(void);
extern "C" void open_hnefatafl_window(void);

static const char* s_labels[ContextMenuWindow::N_ITEMS] = {
    "Nouveau Terminal",
    "Explorateur",
    "GregNet (Web)",
    "GregPaint",
    "Horloge",
    "Calculatrice",
    "Casino",
    "Moniteur",
    "Systeme",
    "Donjon",
    "Hnefatafl",
};

/* ── bevel helper ─────────────────────────────────────────────────────── */

static void cm_bevel(Graphics& g, int x, int y, int w, int h)
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

void ContextMenuWindow::draw()
{
    if (!_visible) return;
    Graphics& g = Graphics::instance();

    /* Shadow (2px offset, semi-dark) */
    g.fill_rect(_x + 3, _y + 3, _w, _h, Theme::GOUFFRE);

    /* Body + bevel */
    g.fill_rect(_x, _y, _w, _h, Theme::WIN_BG);
    cm_bevel(g, _x, _y, _w, _h);

    /* Items */
    for (int i = 0; i < N_ITEMS; ++i) {
        int iy  = _y + 2 + i * ITEM_H;
        bool hv = (i == m_hovered);

        unsigned int ibg = hv ? Theme::TITLE_FOCUS_A : Theme::WIN_BG;
        unsigned int ifg = hv ? Theme::AMBER_HI      : Theme::AMBER;

        g.fill_rect(_x + 2, iy, _w - 4, ITEM_H, ibg);

        /* Small colored square icon per item — sur les rampes de phosphore */
        static const unsigned int s_icon_color[N_ITEMS] = {
            Theme::GREEN,        /* terminal */
            Theme::GOLD,         /* folder   */
            Theme::TEAL_ARCANE,  /* gregnet  */
            Theme::EMBER,        /* paint    */
            Theme::VELLUM,       /* clock    */
            Theme::AMBER,        /* calc     */
            Theme::GOLD_HI,      /* casino   */
            Theme::GREEN_HI,     /* monitor  */
            Theme::ASH,          /* system   */
            Theme::BLOOD_MID,    /* donjon   */
            Theme::GOLD,         /* hnefatafl */
        };
        g.fill_rect(_x + 6, iy + (ITEM_H - 8) / 2, 8, 8, s_icon_color[i]);
        g.draw_str(_x + 20, iy + (ITEM_H - 16) / 2, s_labels[i], ifg,
                   GFX_TRANSPARENT);
    }
}

/* ── item_at ──────────────────────────────────────────────────────────── */

int ContextMenuWindow::item_at(int mx, int my) const
{
    (void)mx;
    int iy0 = _y + 2;
    if (my < iy0) return -1;
    int idx = (my - iy0) / ITEM_H;
    if (idx < 0 || idx >= N_ITEMS) return -1;
    return idx;
}

/* ── launch_item ─────────────────────────────────────────────────────── */

void ContextMenuWindow::launch_item(int idx)
{
    switch (idx) {
    case 0: tty_create_terminal_window(90, 50, 600, 420, "terminal"); break;
    case 1: open_file_manager();          break;
    case 2: open_browser_window();        break;
    case 3: open_paint_window();          break;
    case 4: open_clock_window();          break;
    case 5: open_calc_window();           break;
    case 6: open_casino_window();         break;
    case 7: open_system_monitor_window(); break;
    case 8: open_system_window();         break;
    case 9: open_dungeon_window();        break;
    case 10: open_hnefatafl_window();     break;
    }
}

/* ── on_event ─────────────────────────────────────────────────────────── */

bool ContextMenuWindow::on_event(const Event& e)
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
        /* Click outside — close */
        request_close();
        return false;
    }

    return false;
}
