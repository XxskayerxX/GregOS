/* GregOS Window + WindowManager — SerenityOS "Classic 90s" style
   Teal desktop, silver-gray windows, 3D bevel borders, gradient title bars,
   close button. No taskbar, no start menu — pure floating windows.         */

#include "../include/WindowManager.hpp"
#include "../include/Kernel/Compositor.hpp"
#include "../include/StartMenuWindow.hpp"
#include "../include/ContextMenuWindow.hpp"
#include "../include/GamesWindow.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/TerminalEmulator.hpp"
#include "../include/tty.h"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/event.h"
#include "../include/DesktopIcons.hpp"

extern "C" volatile unsigned long jiffies;

/* PS/2 scancode constants used for global shortcuts */
static constexpr unsigned char SC_CTRL_MAKE    = 0x1Du;
static constexpr unsigned char SC_CTRL_BREAK   = 0x9Du;
static constexpr unsigned char SC_Q_AZERTY     = 0x1Eu;
static constexpr unsigned char SC_ALT_MAKE     = 0x38u;
static constexpr unsigned char SC_ALT_BREAK    = 0xB8u;
static constexpr unsigned char SC_TAB          = 0x0Fu;

/* Double-click window (in jiffies) for the title-bar maximize gesture. */
static constexpr unsigned long DBLCLICK_J = 40ul;

static inline int iabs(int v) { return v < 0 ? -v : v; }

/* ══════════════════════════════════════════════════════════════════════
   Window — SerenityOS visual chrome
   ══════════════════════════════════════════════════════════════════════ */

void Window::setup(int x, int y, int w, int h,
                   const Greg::String& title, unsigned int bg)
{
    _x = x; _y = y; _w = w; _h = h;
    _visible   = true;
    bg_        = bg;
    focused_   = false;
    close_req_ = false;
    dragging_  = false;
    resizing_  = false;
    resize_edge_ = RESIZE_NONE;
    maximized_ = false;
    last_title_click_j_ = 0;
    title_     = title;
}

void Window::setup(int x, int y, int w, int h,
                   const char* title, unsigned int bg)
{
    setup(x, y, w, h, Greg::String(title), bg);
}

/* ── draw_bevel: 2-line raised 3D edge ─────────────────────────────── */
static void draw_bevel(Graphics& g, int x, int y, int w, int h, bool raised)
{
    unsigned int lt_out = raised ? Theme::BEVEL_OUTER_LT : Theme::BEVEL_OUTER_DK;
    unsigned int lt_in  = raised ? Theme::BEVEL_INNER_LT : Theme::BEVEL_INNER_DK;
    unsigned int dk_in  = raised ? Theme::BEVEL_INNER_DK : Theme::BEVEL_INNER_LT;
    unsigned int dk_out = raised ? Theme::BEVEL_OUTER_DK : Theme::BEVEL_OUTER_LT;

    /* Outer lines */
    g.draw_hline(x,         y,         w,     lt_out);  /* top */
    g.draw_vline(x,         y,         h,     lt_out);  /* left */
    g.draw_hline(x,         y + h - 1, w,     dk_out);  /* bottom */
    g.draw_vline(x + w - 1, y,         h,     dk_out);  /* right */

    /* Inner lines (one pixel inside) */
    if (w > 2 && h > 2) {
        g.draw_hline(x + 1,     y + 1,     w - 2, lt_in);
        g.draw_vline(x + 1,     y + 1,     h - 2, lt_in);
        g.draw_hline(x + 1,     y + h - 2, w - 2, dk_in);
        g.draw_vline(x + w - 2, y + 1,     h - 2, dk_in);
    }
}

/* ── draw_title_button: raised silver square (button face + bevel) ───── */
static void draw_title_button(Graphics& g, int bx, int by)
{
    g.fill_rect(bx, by, Window::CLOSE_W, Window::CLOSE_W, Theme::BTN_FACE);
    draw_bevel(g, bx, by, Window::CLOSE_W, Window::CLOSE_W, true);
}

void Window::draw()
{
    if (!_visible) return;
    Graphics& g = Graphics::instance();

    /* ── 1. Window body (silver-gray fill) ── */
    g.fill_rect(_x, _y, _w, _h, Theme::WIN_BG);

    /* ── 2. Outer 3D bevel ── */
    draw_bevel(g, _x, _y, _w, _h, true);

    /* ── 3. Title bar gradient (horizontal: navy→blue or gray→light-gray) ── */
    int title_x = _x + BORDER_W;
    int title_y = _y + BORDER_W;
    int title_w = _w - 2 * BORDER_W;
    int title_h = TITLE_H - BORDER_W;  /* height inside the border */

    unsigned int tc1 = focused_ ? Theme::TITLE_FOCUS_A : Theme::TITLE_IDLE_A;
    unsigned int tc2 = focused_ ? Theme::TITLE_FOCUS_B : Theme::TITLE_IDLE_B;
    g.gradient_rect(title_x, title_y, title_w, title_h, tc1, tc2, 0);

    /* ── 4. Title text — left-aligned, vertically centered in bar ── */
    if (!title_.is_empty()) {
        int ty = title_y + (title_h - 16) / 2;
        g.draw_str(title_x + 4, ty, title_.characters(), Theme::TITLE_FG, GFX_TRANSPARENT);
    }

    /* ── 5. Title-bar buttons — [minimize] [maximize] [close] ── */
    {
        const unsigned int gc = Theme::BTN_X;   /* black glyph ink */
        int by = title_btn_y();

        /* [X] close — raised square with X glyph (unchanged position) */
        int cxb = close_btn_x();
        draw_title_button(g, cxb, by);
        g.draw_char(cxb + (CLOSE_W - 8) / 2, by + (CLOSE_W - 16) / 2,
                    'X', gc, GFX_TRANSPARENT);

        /* [□] maximize / restore */
        int mxb = max_btn_x();
        draw_title_button(g, mxb, by);
        if (!maximized_) {
            /* single window: box with a thick "title bar" top edge */
            g.draw_rect (mxb + 4, by + 4, 8, 8, gc);
            g.draw_hline(mxb + 4, by + 5, 8, gc);
        } else {
            /* restore: two overlapping windows */
            g.draw_rect (mxb + 6, by + 4, 6, 6, gc);   /* back  */
            g.draw_hline(mxb + 6, by + 5, 6, gc);
            g.fill_rect (mxb + 4, by + 6, 6, 6, Theme::BTN_FACE); /* clear front */
            g.draw_rect (mxb + 4, by + 6, 6, 6, gc);   /* front */
            g.draw_hline(mxb + 4, by + 7, 6, gc);
        }

        /* [_] minimize — thick bar near the bottom */
        int nxb = min_btn_x();
        draw_title_button(g, nxb, by);
        g.draw_hline(nxb + 4, by + 10, 8, gc);
        g.draw_hline(nxb + 4, by + 11, 8, gc);
    }

    /* ── 6. Client area fill ── */
    g.fill_rect(client_x(), client_y(), client_w(), client_h(), bg_);

    /* ── 7. Resize grip — dotted triangle in bottom-right corner ── */
    {
        int gx = _x + _w - 3;
        int gy = _y + _h - 3;
        unsigned int gc = focused_ ? Theme::GOLD_DIM : Theme::ASH;
        /* 3-dot diagonal pattern (each dot is 2×2) */
        g.fill_rect(gx - 1,  gy - 1,  2, 2, gc);
        g.fill_rect(gx - 5,  gy - 1,  2, 2, gc);
        g.fill_rect(gx - 1,  gy - 5,  2, 2, gc);
        g.fill_rect(gx - 9,  gy - 1,  2, 2, gc);
        g.fill_rect(gx - 5,  gy - 5,  2, 2, gc);
        g.fill_rect(gx - 1,  gy - 9,  2, 2, gc);
    }

    /* ── 8. Child widgets (drawn on top of client background) ── */
    for (Greg::usize i = 0; i < m_widgets.size(); ++i)
        if (!m_widgets[i].is_null())
            m_widgets[i]->draw(g, client_x(), client_y());
}

/* ── Window::edge_at ─────────────────────────────────────────────────── */

int Window::edge_at(int mx, int my) const
{
    /* Only check if mouse is within or near the window boundary */
    if (mx < _x || mx >= _x + _w || my < _y || my >= _y + _h)
        return RESIZE_NONE;

    bool on_S  = (my >= _y + _h - RESIZE_W);
    bool on_E  = (mx >= _x + _w - RESIZE_W);
    bool on_W  = (mx < _x + RESIZE_W);

    /* Skip top resize: it conflicts with the title bar drag zone */

    if (on_S && on_E) return RESIZE_SE;
    if (on_S && on_W) return RESIZE_SW;
    if (on_S)         return RESIZE_S;
    if (on_E)         return RESIZE_E;
    if (on_W)         return RESIZE_W_E;

    return RESIZE_NONE;
}

/* ── Window::add_widget ──────────────────────────────────────────────── */

void Window::add_widget(Greg::RefPtr<GUI::Widget> w)
{
    m_widgets.append(Greg::move(w));
}

/* ── Window::minimize / toggle_maximize ──────────────────────────────── */

void Window::minimize()
{
    set_visible(false);   /* taskbar button restores it (taskbar_window_click) */
    focused_ = false;
}

void Window::toggle_maximize()
{
    if (!maximized_) {
        /* Save current geometry, then fill the screen above the taskbar. */
        restore_x_ = _x; restore_y_ = _y;
        restore_w_ = _w; restore_h_ = _h;
        _x = 0; _y = 0;
        _w = gfx_width();
        _h = gfx_height() - WindowManager::TASKBAR_H;
        maximized_ = true;
    } else {
        _x = restore_x_; _y = restore_y_;
        _w = restore_w_; _h = restore_h_;
        if (_w < MIN_W) _w = MIN_W;
        if (_h < MIN_H) _h = MIN_H;
        maximized_ = false;
    }
}

bool Window::on_event(const Event& e)
{
    if (!_visible) return false;

    if (e.type == EVT_MOUSE_BUTTON) {
        bool pressed = (e.mouse.buttons & 0x01) != 0;
        int  mx = e.mouse.x, my = e.mouse.y;

        if (pressed && hit_test(mx, my)) {
            /* ── Title-bar buttons (BEFORE resize edges, so the right edge of
                  the close button always closes rather than E-resizing) ── */
            int by = title_btn_y();
            bool on_btn_row = (my >= by && my < by + CLOSE_W);

            /* [X] close */
            int cxb = close_btn_x();
            if (on_btn_row && mx >= cxb && mx < cxb + CLOSE_W) {
                request_close();
                return true;
            }
            /* [□] maximize / restore */
            int mxb = max_btn_x();
            if (on_btn_row && mx >= mxb && mx < mxb + CLOSE_W) {
                toggle_maximize();
                return true;
            }
            /* [_] minimize */
            int nxb = min_btn_x();
            if (on_btn_row && mx >= nxb && mx < nxb + CLOSE_W) {
                minimize();
                return true;
            }

            /* ── Resize edge hit-test (before title bar drag) ── */
            int edge = edge_at(mx, my);
            if (edge != RESIZE_NONE) {
                maximized_    = false;   /* manual resize leaves maximized state */
                resizing_     = true;
                resize_edge_  = edge;
                resize_ox_    = mx;
                resize_oy_    = my;
                resize_x0_    = _x;
                resize_y0_    = _y;
                resize_w0_    = _w;
                resize_h0_    = _h;
                return true;
            }

            /* ── Title strip: double-click maximizes, single-click drags ── */
            if (my >= _y && my < _y + TITLE_H) {
                unsigned long now = jiffies;
                if (last_title_click_j_ != 0
                    && (now - last_title_click_j_) < DBLCLICK_J
                    && iabs(mx - last_title_click_x_) < 8
                    && iabs(my - last_title_click_y_) < 8) {
                    toggle_maximize();
                    last_title_click_j_ = 0;   /* consume; no triple-click retoggle */
                    return true;
                }
                last_title_click_j_ = now;
                last_title_click_x_ = mx;
                last_title_click_y_ = my;
                /* Maximized windows stay put; only a restore makes them movable. */
                if (!maximized_) {
                    dragging_ = true;
                    drag_ox_  = mx - _x;
                    drag_oy_  = my - _y;
                }
                return true;
            }

            /* ── Client area: unfocus all widgets, then dispatch ── */
            for (Greg::usize i = m_widgets.size(); i > 0; )
                if (!m_widgets[--i].is_null()) m_widgets[i]->set_focused(false);

            for (Greg::usize i = m_widgets.size(); i > 0; ) {
                --i;
                if (!m_widgets[i].is_null() &&
                    m_widgets[i]->on_event(e, client_x(), client_y()))
                    return true;
            }
            return true;
        }

        if (!pressed) {
            dragging_  = false;
            resizing_  = false;
            resize_edge_ = RESIZE_NONE;
            for (Greg::usize i = m_widgets.size(); i > 0; ) {
                --i;
                if (!m_widgets[i].is_null())
                    m_widgets[i]->on_event(e, client_x(), client_y());
            }
        }
    }

    if (e.type == EVT_MOUSE_MOVE) {
        for (Greg::usize i = m_widgets.size(); i > 0; ) {
            --i;
            if (!m_widgets[i].is_null())
                m_widgets[i]->on_event(e, client_x(), client_y());
        }

        if (resizing_) {
            Graphics& g = Graphics::instance();
            int sw = g.width(), sh = g.height();
            int dx = e.mouse.x - resize_ox_;
            int dy = e.mouse.y - resize_oy_;

            int nx = resize_x0_, ny = resize_y0_;
            int nw = resize_w0_, nh = resize_h0_;

            /* Apply delta per edge */
            if (resize_edge_ == RESIZE_S  || resize_edge_ == RESIZE_SE ||
                resize_edge_ == RESIZE_SW)
                nh = resize_h0_ + dy;

            if (resize_edge_ == RESIZE_E  || resize_edge_ == RESIZE_SE)
                nw = resize_w0_ + dx;

            if (resize_edge_ == RESIZE_W_E || resize_edge_ == RESIZE_SW) {
                nw = resize_w0_ - dx;
                nx = resize_x0_ + dx;
            }

            /* Enforce minimums */
            if (nw < MIN_W) {
                if (resize_edge_ == RESIZE_W_E || resize_edge_ == RESIZE_SW)
                    nx = resize_x0_ + resize_w0_ - MIN_W;
                nw = MIN_W;
            }
            if (nh < MIN_H) nh = MIN_H;

            /* Clamp to screen */
            if (nx < 0) { nw += nx; nx = 0; if (nw < MIN_W) nw = MIN_W; }
            if (ny < 0) ny = 0;
            if (nx + nw > sw) nw = sw - nx;
            if (ny + nh > sh) nh = sh - ny;

            _x = nx; _y = ny; _w = nw; _h = nh;
            return true;
        }

        if (dragging_) {
            Graphics& g = Graphics::instance();
            _x = e.mouse.x - drag_ox_;
            _y = e.mouse.y - drag_oy_;
            if (_x < 0)              _x = 0;
            if (_y < 0)              _y = 0;
            if (_x + _w > g.width()) _x = g.width()  - _w;
            if (_y + _h > g.height())_y = g.height() - _h;
            return true;
        }
    }

    return false;
}

/* ══════════════════════════════════════════════════════════════════════
   WindowManager
   ══════════════════════════════════════════════════════════════════════ */

static WindowManager s_wm;
WindowManager& WindowManager::instance() { return s_wm; }

/* ── Window management ───────────────────────────────────────────────── */

void WindowManager::add_window(Greg::RefPtr<Window> w)
{
    windows_.append(Greg::move(w));
}

void WindowManager::remove_window(Window* w)
{
    windows_.remove_first_matching([w](const Greg::RefPtr<Window>& p) {
        return p.ptr() == w;
    });
}

void WindowManager::raise(Window* w)
{
    for (Greg::usize i = 0; i < windows_.size(); ++i) {
        if (windows_[i].ptr() != w) continue;
        if (i == windows_.size() - 1) return;
        Greg::RefPtr<Window> tmp = windows_[i];
        windows_.remove(i);
        windows_.append(Greg::move(tmp));
        return;
    }
}

/* ── open_context_menu ────────────────────────────────────────────────── */

void WindowManager::open_context_menu(int x, int y)
{
    /* Dismiss any existing context menu */
    if (m_context_menu) {
        m_context_menu->request_close();
        m_context_menu = nullptr;
    }

    int sw = gfx_width(), sh = gfx_height();
    int mw = ContextMenuWindow::MENU_W;
    int mh = ContextMenuWindow::MENU_H;

    /* Clamp so the whole menu stays on screen AND above the taskbar strip
       (otherwise its bottom items sit under the taskbar and are unclickable). */
    int avail = sh - TASKBAR_H;
    if (x + mw > sw) x = sw - mw;
    if (y + mh > avail) y = avail - mh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    auto win = Greg::make_ref<ContextMenuWindow>();
    win->setup(x, y, mw, mh, "", Theme::WIN_BG);
    Window* raw = win.ptr();
    if (windows_.append(Greg::move(win)))
        m_context_menu = raw;   /* only track it if it actually joined the list */
}

/* ── toggle_start_menu ───────────────────────────────────────────────── */

void WindowManager::toggle_start_menu()
{
    if (m_start_menu) {
        m_start_menu->request_close();
        m_start_menu = nullptr;
        return;
    }
    auto win = Greg::make_ref<StartMenuWindow>();
    int H = gfx_height();
    int menu_y = H - TASKBAR_H - StartMenuWindow::MENU_H;
    if (menu_y < 0) menu_y = 0;
    win->setup(0, menu_y, StartMenuWindow::MENU_W, StartMenuWindow::MENU_H,
               "", Theme::WIN_BG);
    Window* raw = win.ptr();
    if (windows_.append(Greg::move(win)))
        m_start_menu = raw;   /* only track it if it actually joined the list */
}

/* ── Window::handle_char — base: forward to focused child widget ─────── */

bool Window::handle_char(int c)
{
    for (Greg::usize i = m_widgets.size(); i > 0; ) {
        --i;
        if (!m_widgets[i].is_null() && m_widgets[i]->focused())
            return m_widgets[i]->handle_char(c);
    }
    return false;
}

/* ── handle_focused_key: bridge for wm_handle_key() C call ──────────── */

bool WindowManager::handle_focused_key(int c)
{
    for (Greg::usize i = windows_.size(); i > 0; ) {
        --i;
        if (!windows_[i].is_null() && windows_[i]->focused())
            return windows_[i]->handle_char(c);
    }
    return false;
}

/* ── set_tty0 / tty0 — delegate to Compositor ────────────────────────── */

void WindowManager::set_tty0(TerminalEmulator* t)
{
    Kernel::Compositor::instance().set_tty0(t);
}

TerminalEmulator* WindowManager::tty0() const
{
    return Kernel::Compositor::instance().tty0();
}

/* ── Render — delegate to Compositor after purge ────────────────────── */

void WindowManager::draw()
{
    /* Clear stale popup pointers before purge */
    if (m_start_menu   && m_start_menu->close_requested())
        m_start_menu   = nullptr;
    if (m_context_menu && m_context_menu->close_requested())
        m_context_menu = nullptr;

    /* Purge close-requested windows */
    for (Greg::usize i = windows_.size(); i > 0; ) {
        --i;
        if (!windows_[i].is_null() && windows_[i]->close_requested()) {
            windows_[i]->on_removed();
            windows_.remove(i);
        }
    }

    Kernel::Compositor::instance().compose(windows_);
}

/* ── Event dispatch ──────────────────────────────────────────────────── */

bool WindowManager::dispatch_event(const Event& e)
{
    /* Track mouse position for cursor rendering */
    if (e.type == EVT_MOUSE_MOVE || e.type == EVT_MOUSE_BUTTON) {
        Kernel::Compositor::instance().set_cursor(e.mouse.x, e.mouse.y);
    }

    /* Clear stale popup pointers */
    if (m_start_menu   && m_start_menu->close_requested())
        m_start_menu   = nullptr;
    if (m_context_menu && m_context_menu->close_requested())
        m_context_menu = nullptr;

    /* ── Right-click: open context menu (desktop only, not on windows) ── */
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x02)) {
        /* Check if click landed on any existing window */
        bool on_window = false;
        for (Greg::usize i = windows_.size(); i > 0; ) {
            --i;
            if (!windows_[i].is_null() && windows_[i]->hit_test(e.mouse.x, e.mouse.y)) {
                on_window = true;
                break;
            }
        }
        if (!on_window) {
            open_context_menu(e.mouse.x, e.mouse.y);
            return true;
        }
    }

    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
        int H = gfx_height();

        /* Dismiss context menu on any left-click outside it */
        if (m_context_menu && !m_context_menu->hit_test(e.mouse.x, e.mouse.y)) {
            m_context_menu->request_close();
            m_context_menu = nullptr;
        }

        /* ── Taskbar left-click ── */
        if (e.mouse.y >= H - TASKBAR_H) {
            if (e.mouse.x >= 4 && e.mouse.x < 94) {
                /* Let toggle decide open-vs-close; do NOT pre-close, or the
                   Start button could never dismiss an open menu. */
                toggle_start_menu();
                return true;
            }
            /* Window buttons — geometry MUST mirror Compositor::draw_taskbar. */
            if (taskbar_window_click(e.mouse.x)) return true;
            if (m_start_menu && !m_start_menu->hit_test(e.mouse.x, e.mouse.y)) {
                m_start_menu->request_close();   /* other taskbar click dismisses */
                m_start_menu = nullptr;
            }
            return true;
        }

        /* ── Start-menu focus-loss ── */
        if (m_start_menu && !m_start_menu->hit_test(e.mouse.x, e.mouse.y)) {
            m_start_menu->request_close();
            m_start_menu = nullptr;
        }
    }

    if (e.type == EVT_KEY_PRESS) {
        unsigned char sc = e.keyboard.scancode;

        if (sc == SC_CTRL_MAKE)  { ctrl_held_ = true;  return true; }
        if (sc == SC_CTRL_BREAK) { ctrl_held_ = false; return true; }
        if (sc == SC_ALT_MAKE)   { alt_held_  = true;  return true; }
        if (sc == SC_ALT_BREAK)  { alt_held_  = false; return true; }

        if (alt_held_ && sc == SC_TAB) { cycle_focus(); return true; }

        if (ctrl_held_ && sc == SC_Q_AZERTY) {
            for (Greg::usize i = windows_.size(); i > 0; ) {
                --i;
                if (!windows_[i].is_null() && windows_[i]->focused()) {
                    windows_[i]->request_close();
                    return true;
                }
            }
            return true;
        }

        route_key(e);
    }

    for (Greg::usize i = windows_.size(); i > 0; ) {
        --i;
        Greg::RefPtr<Window>& wp = windows_[i];
        if (wp.is_null() || !wp->visible()) continue;

        if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
            if (wp->hit_test(e.mouse.x, e.mouse.y)) {
                for (Greg::usize j = 0; j < windows_.size(); ++j)
                    if (!windows_[j].is_null()) windows_[j]->set_focused(false);

                Window* raw = wp.ptr();
                raw->set_focused(true);
                raise(raw);
                return raw->on_event(e);
            }
        }

        if (wp->on_event(e)) return true;
    }

    /* ── Desktop icon hit-test: click landed on empty desktop ────────── */
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
        int mx = e.mouse.x, my = e.mouse.y;
        for (int i = 0; i < DESK_ICON_COUNT; ++i) {
            int ix = DESK_ICONS[i].x, iy = DESK_ICONS[i].y;
            if (mx >= ix && mx < ix + DESK_ICON_W &&
                my >= iy && my < iy + DESK_ICON_H + DESK_ICON_LBLH) {
                switch (i) {
                case 0: tty_create_terminal_window(90, 50, 600, 420, "terminal"); break;
                case 1: open_file_manager();    break;
                case 2: open_browser_window();  break;
                case 3: open_system_window();   break;
                case 4: open_casino_window();   break;
                case 5: open_games_window();    break;
                case 6: open_calc_window();     break;
                case 7: open_paint_window();    break;
                case 8: open_clock_window();    break;
                case 9: open_minesweeper_window(); break;
                }
                break;
            }
        }
    }
    return false;
}

/* ── taskbar_window_click — mirror of Compositor::draw_taskbar geometry ── */

bool WindowManager::taskbar_window_click(int mx)
{
    int W = gfx_width();
    const int BTN_W = 90;
    const int CLK_W = 76, CLK_PAD = 6;
    int cx = W - CLK_W - CLK_PAD;
    int wx = 4 + BTN_W + 6;
    int avail = cx - wx - 6;
    if (avail < 60) return false;

    int n = 0;
    for (Greg::usize i = 0; i < windows_.size(); ++i)
        if (!windows_[i].is_null() && windows_[i]->title().length() > 0) ++n;
    if (n == 0) return false;
    int bw = avail / n;
    if (bw > 150) bw = 150;
    if (bw < 40)  bw = 40;

    int slot = 0;
    for (Greg::usize i = 0; i < windows_.size(); ++i) {
        if (windows_[i].is_null() || windows_[i]->title().length() == 0) continue;
        int x = wx + slot * bw;
        if (x + bw - 4 > cx) break;
        if (mx >= x && mx < x + bw - 4) {
            Window* w = windows_[i].ptr();
            if (w->visible() && w->focused()) {
                w->set_visible(false);          /* minimize             */
                w->set_focused(false);
            } else {
                w->set_visible(true);           /* restore + focus      */
                for (Greg::usize j = 0; j < windows_.size(); ++j)
                    if (!windows_[j].is_null()) windows_[j]->set_focused(false);
                w->set_focused(true);
                raise(w);
            }
            return true;
        }
        ++slot;
    }
    return false;
}

/* ── cycle_focus — Alt-Tab: focus + raise the next eligible window ────── */

void WindowManager::cycle_focus()
{
    Greg::usize n = windows_.size();
    if (n == 0) return;

    /* Index of the currently focused window (n = none). */
    Greg::usize cur = n;
    for (Greg::usize i = 0; i < n; ++i)
        if (!windows_[i].is_null() && windows_[i]->focused()) { cur = i; break; }

    /* Walk forward (wrapping) for the next visible, non-popup window.
       Popups (start menu / context menu) carry an empty title, like the
       taskbar skip logic. */
    Greg::usize start = (cur == n) ? 0 : cur;
    for (Greg::usize step = 1; step <= n; ++step) {
        Greg::usize i = (start + step) % n;
        Greg::RefPtr<Window>& wp = windows_[i];
        if (wp.is_null() || !wp->visible() || wp->title().length() == 0)
            continue;

        Window* w = wp.ptr();
        for (Greg::usize j = 0; j < n; ++j)
            if (!windows_[j].is_null()) windows_[j]->set_focused(false);
        w->set_focused(true);
        raise(w);
        return;
    }
}

/* ── route_key ───────────────────────────────────────────────────────── */

void WindowManager::route_key(const Event& e)
{
    if (e.type != EVT_KEY_PRESS) return;
    int ch = kb_scancode_to_char(e.keyboard.scancode);
    if (ch <= 0 || ch >= 256) return;

    for (Greg::usize i = windows_.size(); i > 0; ) {
        --i;
        if (!windows_[i].is_null() && windows_[i]->focused()) {
            /* handle_char(ch) receives the already-processed char:
               - TerminalWindow: injects into kb_inject_buf → shell loop
               - FileManagerWindow: handles KEY_UP/DOWN, ESC; ignores rest */
            windows_[i]->handle_char(ch);
            return;
        }
    }

    /* No focused window: character goes to ambient desktop terminal. */
    TerminalEmulator* tty = Kernel::Compositor::instance().tty0();
    if (tty)
        tty->put_char(static_cast<char>(ch));
}

/* ── C bridges (callable from kernel.c) ─────────────────────────────── */

extern "C" void wm_draw(void)
{
    if (!gfx_active()) return;
    WindowManager::instance().draw();
}

extern "C" int wm_handle_key(int c)
{
    return WindowManager::instance().handle_focused_key(c) ? 1 : 0;
}

extern "C" int wm_has_windows(void)
{
    return WindowManager::instance().window_count() > 0 ? 1 : 0;
}

extern "C" void wm_pump_events(void)
{
    Event e;
    while (event_pop(&e)) {
        WindowManager::instance().dispatch_event(e);
    }
}

extern "C" void wm_toggle_start_menu(void)
{
    WindowManager::instance().toggle_start_menu();
}
