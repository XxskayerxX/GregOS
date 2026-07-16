/* SystemMonitorWindow — kernel heap usage monitor for GregOS.
   Samples k_heap_pos every second via jiffies, feeds a GraphWidget.
   Freestanding: no libc, no exceptions.                                      */

#include "../include/SystemMonitorWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/GUI/Button.hpp"
#include "../include/tty.h"

extern "C" volatile unsigned long jiffies;
extern "C" unsigned int kmalloc_used(void);
extern "C" unsigned int kmalloc_total(void);

/* ── Minimal freestanding string helpers ─────────────────────────────── */

int SystemMonitorWindow::sm_itos(unsigned int n, char* buf)
{
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return 1; }
    char t[12]; int ti = 0;
    while (n > 0) { t[ti++] = '0' + n % 10; n /= 10; }
    for (int k = 0; k < ti; ++k) buf[k] = t[ti - 1 - k];
    buf[ti] = '\0';
    return ti;
}

int SystemMonitorWindow::sm_cat(char* dst, int pos, const char* s)
{
    while (*s) dst[pos++] = *s++;
    dst[pos] = '\0';
    return pos;
}

/* ── draw ─────────────────────────────────────────────────────────────── */

void SystemMonitorWindow::draw()
{
    /* Push a new heap sample once per second (100 jiffies ≈ 1 s).
       kmalloc_used() is always ≤ kmalloc_total() (32 MB) so no overflow. */
    unsigned long now = jiffies;
    if (now - m_last_push >= 100) {
        m_last_push = now;
        unsigned int ku = kmalloc_used();
        m_graph.push_value((int)(ku > 0 ? ku / 1024 : 0));
    }

    Window::draw();   /* chrome + client fill + child widgets (buttons) */

    Graphics& g  = Graphics::instance();
    int x0 = client_x() + 8;
    int y  = client_y() + 8;
    const unsigned int BG = bg_;

    /* ── Header ──────────────────────────────────────────────────────── */
    g.draw_str(x0, y, "== Moniteur Systeme ==", Theme::AMBER_HI, BG);
    y += 20;

    /* ── Heap stats ──────────────────────────────────────────────────── */
    /* ── ISOLATION: use raw values, no 32-bit integer division ──────────
       All stats computed via subtraction/shift only. Re-enable division
       once GraphWidget isolation confirms rendering is stable.          */
    unsigned int used_raw  = kmalloc_used();
    unsigned int total_raw = kmalloc_total();   /* always 32 MB = 33554432 */

    /* Convert bytes → KB via shift (avoids any division instruction) */
    unsigned int used_kb = used_raw  >> 10;   /* / 1024 */
    unsigned int tot_kb  = total_raw >> 10;   /* / 1024 */

    char line[80]; char tmp[16]; int p;

    p = sm_cat(line, 0,  "Heap : ");
    sm_itos(used_kb, tmp);  p = sm_cat(line, p, tmp);
    p = sm_cat(line, p, " Ko / ");
    sm_itos(tot_kb,  tmp);  p = sm_cat(line, p, tmp);
    p = sm_cat(line, p, " Ko");
    g.draw_str(x0, y, line, Theme::FG_PRIMARY, BG);
    y += 18;

    /* Percentage via shift: pct ≈ used_kb * 100 / tot_kb.
       For tot_kb = 32768, divide by 32768 = >> 15.
       used_kb * 100 could be up to 3276800 → fits in u32 (< 2^22). */
    unsigned int pct = 0;
    if (tot_kb > 0) {
        /* Only use shifts and addition — zero division risk */
        unsigned int num = used_kb * 100u;   /* max 3276800, fits in u32 */
        pct = (tot_kb == 32768u) ? (num >> 15) : (num / tot_kb);
        if (pct > 100u) pct = 100u;
    }
    unsigned int pcol = pct < 50u ? Theme::GREEN : pct < 80u ? Theme::GOLD : Theme::EMBER;
    p = sm_cat(line, 0, "Utilisation : ");
    sm_itos(pct, tmp); p = sm_cat(line, p, tmp);
    p = sm_cat(line, p, "%");
    g.draw_str(x0, y, line, pcol, BG);
    /* y is now at 46 — graph starts at client_y+60 giving a 14px gap */

    /* ── Graph (draws itself relative to client origin) ─────────────── */
    m_graph.draw(g, client_x(), client_y());

    /* ── Legend below graph (graph bottom = client_y + 60 + 150 = +210) */
    int leg_y = client_y() + 218;
    g.draw_str(x0, leg_y,      "Utilisation memoire kernel (Ko)", Theme::ASH, BG);
    g.draw_str(x0, leg_y + 14, "Vert=normal  Or=eleve  Braise=critique", Theme::ASH, BG);
}

/* ── Factory ─────────────────────────────────────────────────────────── */

extern "C" void open_system_monitor_window(void)
{
    auto win = Greg::make_ref<SystemMonitorWindow>();
    /* 380 × 310 → client: 376 × 286.  Graph at y=60 h=150 ends at y=210. */
    win->setup(180, 60, 380, 310, "Moniteur Systeme", Theme::WIN_BG);

    SystemMonitorWindow* raw = win.ptr();
    int btn_y = raw->client_h() - 32;   /* 286 - 32 = 254 */
    auto close_btn = Greg::make_ref<GUI::Button>(
        10, btn_y, 90, 24, "Fermer",
        [raw](){ raw->request_close(); });
    win->add_widget(Greg::move(close_btn));

    WindowManager::instance().add_window(Greg::move(win));
}
