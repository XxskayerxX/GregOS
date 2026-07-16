/* SystemWindow — live system info panel for the GregOS desktop.
   Replaces the old gui_open_sysinfo() direct-render approach with a proper
   WM window that redraws cleanly every frame.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/SystemWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/GUI/Button.hpp"
#include "../include/gfx.h"
#include "../include/tty.h"

extern "C" volatile unsigned long jiffies;
extern "C" void get_time_string(char* buf);   /* non-static in kernel.c */
extern "C" void get_date_string(char* buf);

/* ── Minimal freestanding string helpers ─────────────────────────────── */

static int sw_itos(int n, char* buf)
{
    if (n <= 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    char t[12]; int ti = 0;
    while (n > 0) { t[ti++] = '0' + n % 10; n /= 10; }
    for (int k = 0; k < ti; ++k) buf[k] = t[ti - 1 - k];
    buf[ti] = '\0';
    return ti;
}

static int sw_cat(char* dst, int pos, const char* s)
{
    while (*s) dst[pos++] = *s++;
    dst[pos] = '\0';
    return pos;
}

static int sw_cat_int(char* dst, int pos, int n)
{
    char tmp[12]; sw_itos(n, tmp);
    return sw_cat(dst, pos, tmp);
}

/* ── draw ─────────────────────────────────────────────────────────────── */

void SystemWindow::draw()
{
    Window::draw();   /* title bar chrome + client area fill */

    Graphics& g  = Graphics::instance();
    unsigned int bg = bg_;
    int x0 = client_x() + 6;
    int y  = client_y() + 4;
    const int LH = 17;   /* line height (font 16 + 1px gap) */

    /* ── ASCII dragon art (green) + OS labels (right column) ────────── */
    static const char* art[5] = {
        "  ==(W{==========-",
        "    ||  (.--.)    ",
        "    | \\,|**|,__  ",
        " ___/-==|  /`\\_.  ",
        "(^(~     `-' -~`  ",
    };
    static const char* labels[5] = {
        "",
        "  GregOS v2.0",
        "  OS:     GregOS 2.0 i386",
        "  Kernel: bare-metal C/ASM",
        "  Shell:  gregsh",
    };
    int col2 = x0 + 20 * 8;   /* right column starts after 20-char art */
    for (int i = 0; i < 5; ++i) {
        g.draw_str(x0,   y + i * LH, art[i],    Theme::GREEN, bg);  /* phosphore */
        if (labels[i][0])
            g.draw_str(col2, y + i * LH, labels[i], Theme::FG_PRIMARY, bg);  /* black */
    }
    y += 5 * LH + 4;

    /* Separator */
    g.draw_hline(client_x() + 2, y,     client_w() - 4, Theme::BEVEL_OUTER_DK);
    g.draw_hline(client_x() + 2, y + 1, client_w() - 4, Theme::BEVEL_OUTER_LT);
    y += 9;

    /* ── Dynamic stats ────────────────────────────────────────────────── */
    char ts[12], ds[12];
    get_time_string(ts);
    get_date_string(ds);

    unsigned long sec = jiffies / 100;
    int uh  = (int)(sec / 3600);
    int um  = (int)((sec % 3600) / 60);
    int us2 = (int)(sec % 60);

    char line[64]; int p;

    /* Date & Heure on same line */
    p = sw_cat(line, 0, "  Date:   "); p = sw_cat(line, p, ds);
    g.draw_str(x0,          y, line, Theme::FG_PRIMARY, bg);
    p = sw_cat(line, 0, "Heure:  "); p = sw_cat(line, p, ts);
    g.draw_str(x0 + 22*8,   y, line, Theme::FG_PRIMARY, bg);
    y += LH;

    /* Uptime */
    p = sw_cat(line, 0, "  Uptime: ");
    p = sw_cat_int(line, p, uh);  p = sw_cat(line, p, "h ");
    p = sw_cat_int(line, p, um);  p = sw_cat(line, p, "m ");
    p = sw_cat_int(line, p, us2); p = sw_cat(line, p, "s");
    g.draw_str(x0, y, line, Theme::FG_PRIMARY, bg);
    y += LH;

    /* Hardware */
    g.draw_str(x0, y, "  CPU:    QEMU i386 Virtual CPU", Theme::FG_PRIMARY, bg); y += LH;
    g.draw_str(x0, y, "  RAM:    256 MB",                Theme::FG_PRIMARY, bg); y += LH;
    g.draw_str(x0, y, "  GPU:    Bochs VBE 800x600x32",  Theme::FG_PRIMARY, bg); y += LH;

    /* FS */
    p = sw_cat(line, 0, "  FS:     ");
    p = sw_cat_int(line, p, sys_get_file_count());
    p = sw_cat(line, p, "/64 entrees");
    g.draw_str(x0, y, line, Theme::FG_PRIMARY, bg);
    y += LH;

    /* Casino balance */
    p = sw_cat(line, 0, "  Casino: ");
    p = sw_cat_int(line, p, casino_get_balance());
    p = sw_cat(line, p, " GregCoins");
    g.draw_str(x0, y, line, Theme::FG_PRIMARY, bg);
}

/* ── open_system_window: extern "C" bridge ───────────────────────────── */

extern "C" void open_system_window(void)
{
    auto win = Greg::make_ref<SystemWindow>();
    win->setup(150, 80, 500, 290, "Informations Systeme", Theme::WIN_BG);

    /* "Fermer" button — anchored to client bottom.
       client_h = 290 - TITLE_H(22) - BORDER_W(2) = 266.
       y = client_h - btn_h(24) - margin(8) = 234.  Content ends at ~220.  */
    SystemWindow* raw = win.ptr();
    int btn_y = raw->client_h() - 32;   /* 24px button + 8px bottom margin */
    auto close_btn = Greg::make_ref<GUI::Button>(
        10, btn_y, 90, 24, "Fermer",
        [raw](){ raw->request_close(); });
    win->add_widget(Greg::move(close_btn));

    WindowManager::instance().add_window(Greg::move(win));
}
