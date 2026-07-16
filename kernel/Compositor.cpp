/* Kernel::Compositor — desktop compositing for GregOS.
   Owns: desktop background, TTY0 log overlay, desk icons, taskbar, cursor.
   Called by WindowManager::draw() after window list purge.
   Freestanding: no libc, no exceptions.                                    */

#include "../include/Kernel/Compositor.hpp"
#include "../include/Window.hpp"
#include "../include/TerminalEmulator.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/WindowManager.hpp"  /* TASKBAR_H */
#include "../include/DesktopIcons.hpp"   /* shared icon table */
#include "../include/art_drakkar.h"      /* dragon silhouette watermark */

extern "C" volatile unsigned long jiffies;

namespace Kernel {

/* ── living-desktop helpers (integer only, freestanding) ─────────────────
   The realm breathes: the Anneau des Cycles glows in ember and Drakkar's
   silhouette haunts the obsidian behind your work.                         */
static unsigned int comp_tri(unsigned long t, unsigned long period, unsigned long ph)
{
    unsigned long x = (t + ph) % period, half = period / 2;
    unsigned long up = (x < half) ? x : (period - x);
    return (unsigned int)(up * 255u / (half ? half : 1));
}
static unsigned int comp_mix(unsigned int a, unsigned int b, unsigned int t)
{
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r = ar + (br-ar)*(int)t/255, gg = ag + (bg-ag)*(int)t/255, bl = ab + (bb-ab)*(int)t/255;
    return ((unsigned)r<<16) | ((unsigned)gg<<8) | (unsigned)bl;
}

/* ── singleton ────────────────────────────────────────────────────────── */

static Compositor s_compositor;
Compositor& Compositor::instance() { return s_compositor; }

/* ── local bevel helper (2-line raised 3D edge) ──────────────────────── */

static void comp_bevel(Graphics& g, int x, int y, int w, int h, bool raised)
{
    unsigned int lt_out = raised ? Theme::BEVEL_OUTER_LT : Theme::BEVEL_OUTER_DK;
    unsigned int lt_in  = raised ? Theme::BEVEL_INNER_LT : Theme::BEVEL_INNER_DK;
    unsigned int dk_in  = raised ? Theme::BEVEL_INNER_DK : Theme::BEVEL_INNER_LT;
    unsigned int dk_out = raised ? Theme::BEVEL_OUTER_DK : Theme::BEVEL_OUTER_LT;
    g.draw_hline(x,         y,         w,     lt_out);
    g.draw_vline(x,         y,         h,     lt_out);
    g.draw_hline(x,         y + h - 1, w,     dk_out);
    g.draw_vline(x + w - 1, y,         h,     dk_out);
    if (w > 2 && h > 2) {
        g.draw_hline(x + 1,     y + 1,     w - 2, lt_in);
        g.draw_vline(x + 1,     y + 1,     h - 2, lt_in);
        g.draw_hline(x + 1,     y + h - 2, w - 2, dk_in);
        g.draw_vline(x + w - 2, y + 1,     h - 2, dk_in);
    }
}

/* ── cursor mask — file-scope avoids __cxa_guard ────────────────────── */

static const unsigned char s_cursor_mask[12] = {
    0x80u, 0xC0u, 0xE0u, 0xF0u, 0xF8u, 0xFCu,
    0xF0u, 0xD8u, 0x18u, 0x0Cu, 0x0Cu, 0x06u
};

/* ── desktop icon data — positions come from DesktopIcons.hpp ─────────── */

/* ── draw_wallpaper — precomputed vertical gradient (navy → teal) ─────── */

/* Wallpaper is fully static → paint it once into this cache, then blit the
   cache into the back buffer each frame (a single rep movsl) instead of
   recomputing the gradient + ring every compose. ~1.9 MB of BSS.          */
static unsigned int s_wp_cache[800 * 600];
static int          s_wp_ready = 0;

void Compositor::draw_wallpaper()
{
    Graphics& g = Graphics::instance();
    int sw = g.width(), sh = g.height();

    /* Fast path: restore the pre-rendered wallpaper. */
    if (s_wp_ready) {
        g.blit_back(s_wp_cache);
        return;
    }

    /* Obsidienne de tube : base + vignette verticale très légère (bakée une fois). */
    static unsigned int s_rows[600];
    static int s_ready = 0;
    static int s_h = 0;
    if (!s_ready || s_h != sh) {
        int rows = sh > 600 ? 600 : sh;
        int mid = rows > 1 ? rows / 2 : 1;
        for (int y = 0; y < rows; ++y) {
            int d = (y < mid ? (mid - y) : (y - mid)) * 6 / (mid > 0 ? mid : 1);
            int rr = 0x0B - d; if (rr < 0x05) rr = 0x05;
            int gg = 0x0E - d; if (gg < 0x06) gg = 0x06;
            int bb = 0x0C - d; if (bb < 0x05) bb = 0x05;
            s_rows[y] = ((unsigned)rr << 16) | ((unsigned)gg << 8) | (unsigned)bb;
        }
        s_ready = 1; s_h = sh;
    }
    int rows = sh > 600 ? 600 : sh;
    for (int y = 0; y < rows; ++y)
        g.draw_hline(0, y, sw, s_rows[y]);

    /* Filigrane hanté : la silhouette de DRAKKAR, en sang très profond, tapie
       derrière le plan de travail. Baké une seule fois (coût nul par frame) ;
       tous les glyphes non-vides sont aplatis en une seule teinte spectrale
       à peine plus claire que l'obsidienne — un dragon qui vous observe. */
    {
        const unsigned int SIL = 0x1C0A0Au;              /* blood-deep ghost */
        int x0 = (sw - DRAKKAR_COLS * 8) / 2;
        int y0 = (sh - DRAKKAR_ROWS * 16) / 2 - 10;
        for (int r = 0; r < DRAKKAR_ROWS; ++r)
            for (int c = 0; c < DRAKKAR_COLS; ++c)
                if (drakkar_pal[r][c])
                    gfx_draw_char(x0 + c * 8, y0 + r * 16, drakkar_ch[r][c],
                                  SIL, GFX_TRANSPARENT);
    }

    /* NB: l'Anneau des Cycles + ses marqueurs ne sont PAS bakés ici — ils
       rougeoient par frame (voir draw_desktop) pour un bureau vivant.      */

    /* Snapshot the finished wallpaper so every later frame just blits it. */
    if (sw == 800 && sh == 600) {
        g.snapshot_back(s_wp_cache);
        s_wp_ready = 1;
    }
}

/* ── draw_desktop ────────────────────────────────────────────────────── */

void Compositor::draw_desktop()
{
    Graphics& g = Graphics::instance();
    int sw = g.width(), sh = g.height();

    /* Anneau des Cycles — vivant : le cercle runique respire de l'obsidienne à
       la braise (~2 s) et ses quatre marqueurs cardinaux rougeoient. Redessiné
       par frame par-dessus le wallpaper baké (≈ 900 pixels, négligeable). */
    {
        int cx = sw / 2, cy = sh / 2 - 8;
        const int R = 150;
        unsigned int t   = comp_tri(jiffies, 200, 0);
        unsigned int rgc = comp_mix(0x161206u, 0x3E2208u, t);   /* or profond ↔ braise dim */
        for (int dy = 0; dy < 2 * R; ++dy) {
            int rr = R * R - (dy - R) * (dy - R);
            if (rr < 0) continue;
            int xx = 0; while ((xx + 1) * (xx + 1) <= rr) ++xx;
            g.put_pixel(cx - xx, cy - R + dy, rgc);
            g.put_pixel(cx + xx, cy - R + dy, rgc);
        }
        unsigned int mk = comp_mix(Theme::AMBER_DEEP, 0xC85018u, t);  /* braise chaude au pic */
        g.fill_rect(cx - 1,     cy - R - 6, 2,  12, mk);
        g.fill_rect(cx - 1,     cy + R - 6, 2,  12, mk);
        g.fill_rect(cx - R - 6, cy - 1,     12, 2,  mk);
        g.fill_rect(cx + R - 6, cy - 1,     12, 2,  mk);
    }

    /* OS tag top-right */
    g.draw_str(sw - 152, 6, "GregOS/kernel 0.5", Theme::SYSLOG_DIM, GFX_TRANSPARENT);

    /* Liseré haut/bas du tube (or profond). */
    g.draw_hline(0, 0,      sw, Theme::GOLD_DEEP);
    g.draw_hline(0, sh - 1, sw, Theme::GOLD_DEEP);

    /* Bottom-left prompt ghost */
    g.draw_str(4, sh - 14, "kernel@void:~$_", Theme::SYSLOG_DIM, GFX_TRANSPARENT);

    if (!m_tty0 || !m_tty0->ready()) return;

    bool ambient_active = (TerminalEmulator::tty0() == m_tty0);

    int ox = 4, oy = 20;
    if (ambient_active) {
        for (int row = 0; row < m_tty0->rows(); ++row) {
            for (int col = 0; col < m_tty0->cols(); ++col) {
                const TermCell& cell = m_tty0->cell_at(col, row);
                if (cell.ch != ' ') {
                    g.draw_char(ox + col * TerminalEmulator::FONT_W,
                                oy + row * TerminalEmulator::FONT_H,
                                static_cast<unsigned char>(cell.ch),
                                cell.fg, GFX_TRANSPARENT);
                }
            }
        }

        if ((jiffies / 50) % 2 == 0) {
            int cx = ox + m_tty0->cursor_col() * TerminalEmulator::FONT_W;
            int cy = oy + m_tty0->cursor_row() * TerminalEmulator::FONT_H;
            g.fill_rect(cx, cy, TerminalEmulator::FONT_W, TerminalEmulator::FONT_H,
                        Theme::SYSLOG_DIM);
        }
    }
}

/* ── draw_desk_icons ─────────────────────────────────────────────────── */

void Compositor::draw_desk_icons()
{
    Graphics& g = Graphics::instance();
    static constexpr int IW = 48, IH = 48;

    for (int idx = 0; idx < DESK_ICON_COUNT; ++idx) {
        int x = DESK_ICONS[idx].x, y = DESK_ICONS[idx].y;

        /* Hover highlight: soft rounded panel behind the icon under cursor. */
        bool hover = (m_mx >= x - 2 && m_mx < x + IW + 2 &&
                      m_my >= y - 2 && m_my < y + IH + DESK_ICON_LBLH);
        if (hover) g.fill_rect(x - 3, y - 3, IW + 6, IH + DESK_ICON_LBLH + 4, Theme::AMBER_DEEP);

        /* Icônes recolorisées sur les rampes de phosphore (géométrie inchangée) :
           terminal & système = VERT machine, fichiers/casino/horloge = OR royal,
           GregNet = TEAL arcane, jeux/paint = accents BRAISE/VERT/OR.          */
        switch (idx) {
        case 0: /* Terminal — écran phosphore vert */
            g.fill_rect(x+2, y+2, IW-4, IH-4, Theme::WIN_BG_PURE);
            g.draw_rect(x+2, y+2, IW-4, IH-4, Theme::GREEN_DIM);
            g.draw_str(x+5, y+8,  ">_", Theme::GREEN,     Theme::WIN_BG_PURE);
            g.draw_str(x+5, y+24, ">>", Theme::GREEN_DIM, Theme::WIN_BG_PURE);
            g.fill_rect(x+5, y+30, 16, 2, Theme::GREEN);
            break;
        case 1: /* Fichiers — dossier d'or du royaume */
            g.fill_rect(x+2,  y+14, IW-4,  IH-16, Theme::GOLD_DIM);
            g.fill_rect(x+2,  y+10, 18,    6,      Theme::GOLD_DIM);
            g.fill_rect(x+4,  y+18, IW-8,  IH-22, Theme::GOLD);
            g.draw_rect(x+2,  y+10, IW-4,  IH-12, Theme::GOLD_DEEP);
            g.draw_str(x+10, y+20, "fs", Theme::GOLD_DEEP, Theme::GOLD);
            break;
        case 2: /* GregNet — globe arcane teal, nœuds phosphore */
            for (int dy = 0; dy < IH; ++dy) {
                int rr = 22*22 - (dy-24)*(dy-24);
                if (rr < 0) continue;
                int xx = 0; while ((xx+1)*(xx+1) <= rr) xx++;
                g.draw_hline(x + 24 - xx, y + dy, 2*xx, Theme::TEAL_ARCANE);
            }
            g.fill_rect(x+24-22, y+22, 44, 3, 0x14564Fu);         /* equator */
            g.draw_vline(x+24, y+2, IH-4, 0x14564Fu);             /* prime meridian */
            g.fill_rect(x+14, y+14, 8, 5, Theme::GREEN);          /* land masses */
            g.fill_rect(x+27, y+20, 10, 6, Theme::GREEN);
            g.fill_rect(x+20, y+30, 12, 5, Theme::GREEN);
            for (int dy = 0; dy < IH; ++dy) {                     /* rim */
                int rr = 22*22 - (dy-24)*(dy-24);
                if (rr < 0) continue;
                int xx = 0; while ((xx+1)*(xx+1) <= rr) xx++;
                g.put_pixel(x+24-xx, y+dy, Theme::GREEN_HI);
                g.put_pixel(x+24+xx, y+dy, Theme::GREEN_HI);
            }
            break;
        case 3: /* Systeme — boîtier bronze, écran vert */
            g.fill_rect(x+4,  y+2,  IW-8,  IH-16, Theme::GOLD_DEEP);
            g.fill_rect(x+7,  y+5,  IW-14, IH-22, Theme::WIN_BG_PURE);
            g.draw_str(x+8,  y+8,  ">>", Theme::GREEN,     Theme::WIN_BG_PURE);
            g.draw_str(x+8,  y+18, "OS", Theme::GREEN_DIM, Theme::WIN_BG_PURE);
            g.fill_rect(x+16, y+IH-14, 8,  6, Theme::ASH);
            g.fill_rect(x+8,  y+IH-8,  24, 4, Theme::GOLD_DEEP);
            break;
        case 4: /* Casino — GregCoin d'or */
            g.fill_rect(x+8,  y+4,  28, 28, Theme::GOLD_DIM);
            g.fill_rect(x+10, y+6,  24, 24, Theme::GOLD);
            g.draw_str(x+13, y+11, "GC", Theme::GOLD_DEEP, Theme::GOLD);
            g.fill_rect(x+18, y+34, 6, 6,  Theme::EMBER);
            g.fill_rect(x+20, y+32, 4, 3,  Theme::GOLD);
            break;
        case 5: /* Jeux — borne d'arcade, boutons phosphore */
            g.fill_rect(x+4,  y+10, IW-8,  IH-20, Theme::GOLD_DEEP);
            g.fill_rect(x+2,  y+16, IW-4,  IH-30, Theme::GOLD_DIM);
            g.fill_rect(x+10, y+18, 4, 12, Theme::AMBER);
            g.fill_rect(x+6,  y+22, 12, 4,  Theme::AMBER);
            g.fill_rect(x+28, y+18, 5, 5,  Theme::EMBER);
            g.fill_rect(x+33, y+22, 5, 5,  Theme::GOLD);
            g.fill_rect(x+28, y+26, 5, 5,  Theme::GREEN);
            break;
        case 6: /* Calculatrice — laiton, écran vert */
            g.fill_rect(x+4,  y+2,  40, 44, Theme::GOLD_DIM);
            g.draw_rect(x+4,  y+2,  40, 44, Theme::GOLD_DEEP);
            g.fill_rect(x+7,  y+5,  34, 12, Theme::WIN_BG_PURE);
            g.draw_str (x+22, y+6,  "42",   Theme::GREEN_HI, Theme::WIN_BG_PURE);
            g.draw_hline(x+6, y+18, 36, Theme::GOLD_DEEP);
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 3; ++c) {
                    unsigned int col =
                        (r == 0 && c == 0) ? Theme::EMBER :
                        (r == 3 && c == 2) ? Theme::GREEN :
                        (r == 0)           ? Theme::AMBER_DIM :
                        Theme::ASH;
                    g.fill_rect(x + 7 + c * 13, y + 21 + r * 9, 9, 6, col);
                }
            }
            break;
        case 7: /* Paint — palette de vélin, pigments phosphore */
            for (int dy = 6; dy < 40; ++dy) {
                int rr = 20*20 - (dy-22)*(dy-22);
                if (rr < 0) continue;
                int xx = 0; while ((xx+1)*(xx+1) <= rr) xx++;
                g.draw_hline(x + 22 - xx, y + dy, 2*xx, Theme::VELLUM);
            }
            g.fill_rect(x+12, y+12, 6, 6, Theme::EMBER);
            g.fill_rect(x+24, y+11, 6, 6, Theme::TEAL_ARCANE);
            g.fill_rect(x+30, y+22, 6, 6, Theme::GREEN);
            g.fill_rect(x+14, y+26, 6, 6, Theme::GOLD);
            g.fill_rect(x+22, y+30, 5, 5, Theme::GOUFFRE);   /* thumb hole */
            g.fill_rect(x+34, y+34, 3, 12, Theme::GOLD_DIM); /* brush handle */
            g.fill_rect(x+32, y+30, 6, 6, Theme::ASH);       /* ferrule */
            break;
        case 8: /* Horloge — cadran de vélin, aiguilles d'encre */
            for (int dy = 0; dy < IH; ++dy) {
                int rr = 22*22 - (dy-24)*(dy-24);
                if (rr < 0) continue;
                int xx = 0; while ((xx+1)*(xx+1) <= rr) xx++;
                g.draw_hline(x + 24 - xx, y + dy, 2*xx, Theme::VELLUM);
                g.put_pixel(x+24-xx, y+dy, Theme::GOLD_DEEP);
                g.put_pixel(x+24+xx, y+dy, Theme::GOLD_DEEP);
            }
            g.fill_rect(x+23, y+8,  2, 16, Theme::VELLUM_INK); /* minute hand up */
            g.fill_rect(x+24, y+23, 12, 2, Theme::VELLUM_INK); /* hour hand right */
            g.fill_rect(x+22, y+22, 4, 4, Theme::EMBER);       /* hub */
            break;
        case 9: /* Demineur — plateau du donjon, œuf de braise + bannière */
            g.fill_rect(x+2, y+2, IW-4, IH-4, 0x120E0Cu);
            g.draw_rect(x+2, y+2, IW-4, IH-4, Theme::GOLD_DEEP);
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c) {
                    int tx = x + 5 + c * 13, ty = y + 5 + r * 13;
                    g.fill_rect(tx, ty, 12, 12, 0x2A2622u);
                    g.draw_hline(tx, ty,      12, 0x4E4238u);
                    g.draw_vline(tx, ty,      12, 0x4E4238u);
                    g.draw_hline(tx, ty + 11, 12, 0x140F0Cu);
                    g.draw_vline(tx + 11, ty, 12, 0x140F0Cu);
                }
            {   /* centre tile uncovered — a smouldering egg */
                int ex = x + 18, ey = y + 18;
                g.fill_rect(ex, ey, 12, 12, 0x161210u);
                g.fill_rect(ex + 3, ey + 2, 6, 8, 0x8B1A1Au);
                g.fill_rect(ex + 4, ey + 4, 3, 3, 0xFF6A20u);
                g.put_pixel(ex + 4, ey + 4, 0xFFE0A0u);
            }
            {   /* top-right tile — blood banner on a gold pole */
                int fx = x + 31, fy = y + 5;
                g.draw_vline(fx + 7, fy + 2, 8, 0xC9A24Bu);
                for (int i = 0; i < 4; ++i)
                    g.draw_hline(fx + 3 + i, fy + 2 + i, 4 - i, 0xC83028u);
            }
            break;
        }

        const char* lbl = DESK_ICONS[idx].label;
        int llen = 0; while (lbl[llen]) ++llen;
        int lx = x + (IW - llen * 8) / 2;
        /* subtle text shadow for readability on the wallpaper */
        g.draw_str(lx + 1, y + IH + 3, lbl, Theme::GOUFFRE, GFX_TRANSPARENT);
        g.draw_str(lx, y + IH + 2, lbl, Theme::AMBER, GFX_TRANSPARENT);
    }
}

/* ── draw_taskbar ────────────────────────────────────────────────────── */

static void fmt_time(char* buf, unsigned long j)
{
    unsigned long s = j / 100;
    int h  = (int)(s / 3600 % 24);
    int m  = (int)(s / 60   % 60);
    int sc = (int)(s         % 60);
    buf[0] = '0' + h  / 10; buf[1] = '0' + h  % 10;
    buf[2] = ':';
    buf[3] = '0' + m  / 10; buf[4] = '0' + m  % 10;
    buf[5] = ':';
    buf[6] = '0' + sc / 10; buf[7] = '0' + sc % 10;
    buf[8] = '\0';
}

void Compositor::draw_taskbar(Greg::Vector<Greg::RefPtr<Window>>& windows)
{
    Graphics& g = Graphics::instance();
    int W  = g.width();
    int H  = g.height();
    int ty = H - WindowManager::TASKBAR_H;

    g.fill_rect(0, ty, W, WindowManager::TASKBAR_H, Theme::WIN_BG);
    g.draw_hline(0, ty,     W, Theme::BEVEL_OUTER_LT);
    g.draw_hline(0, H - 1,  W, Theme::BEVEL_OUTER_DK);

    /* Start button with a little flame glyph. */
    const int BTN_W = 90;
    const int BTN_H = WindowManager::TASKBAR_H - 6;
    const int bx = 4, by = ty + 3;
    g.fill_rect(bx, by, BTN_W, BTN_H, Theme::BTN_FACE);
    comp_bevel(g, bx, by, BTN_W, BTN_H, true);
    g.fill_rect(bx + 7, by + 8, 4, 8, 0xE04010u);        /* flame */
    g.fill_rect(bx + 8, by + 5, 3, 6, 0xFFC000u);
    g.draw_str(bx + 16, by + (BTN_H - 16) / 2, "GregOS", Theme::GOLD_HI, GFX_TRANSPARENT);

    /* Clock (right) — real RTC HH:MM:SS in a sunken well. */
    const int CLK_W = 76, CLK_PAD = 6;
    const int cx = W - CLK_W - CLK_PAD;
    const int cy = ty + 4;
    const int clk_h = WindowManager::TASKBAR_H - 8;
    g.fill_rect(cx, cy, CLK_W, clk_h, Theme::WIN_BG);
    comp_bevel(g, cx, cy, CLK_W, clk_h, false);
    char tbuf[12];
    fmt_time(tbuf, jiffies);
    g.draw_str(cx + 8, cy + (clk_h - 16) / 2, tbuf, Theme::GREEN_HI, GFX_TRANSPARENT);

    /* Window buttons for each titled top-level window (skip popups/login,
       which carry empty titles). Laid out between the Start button and clock. */
    int wx = bx + BTN_W + 6;
    int avail = cx - wx - 6;
    if (avail < 60) return;
    /* count titled windows first for width sizing */
    int n = 0;
    for (Greg::usize i = 0; i < windows.size(); ++i)
        if (!windows[i].is_null() && windows[i]->title().length() > 0) ++n;
    if (n == 0) return;
    int bw = avail / n;
    if (bw > 150) bw = 150;
    if (bw < 40)  bw = 40;

    int slot = 0;
    for (Greg::usize i = 0; i < windows.size(); ++i) {
        if (windows[i].is_null() || windows[i]->title().length() == 0) continue;
        Window* w = windows[i].ptr();
        int x = wx + slot * bw;
        if (x + bw - 4 > cx) break;
        bool active = w->focused() && w->visible();
        g.fill_rect(x, by, bw - 4, BTN_H, active ? Theme::AMBER_DEEP : Theme::BTN_FACE);
        comp_bevel(g, x, by, bw - 4, BTN_H, !active);   /* active = pressed */
        /* title, truncated to fit */
        const char* t = w->title().characters();
        int maxc = (bw - 4 - 12) / 8;
        char lbl[24];
        int k = 0;
        for (; t[k] && k < maxc && k < 23; ++k) lbl[k] = t[k];
        lbl[k] = '\0';
        g.draw_str(x + 6 + (active ? 1 : 0), by + (BTN_H - 16) / 2 + (active ? 1 : 0),
                   lbl, active ? Theme::AMBER_HI : Theme::AMBER, GFX_TRANSPARENT);
        ++slot;
    }
}

/* ── draw_cursor — flèche pixel-art, dessinée dans le BACK buffer chaque frame.
   Simple et robuste : le curseur fait partie de la scène composée (juste avant
   swap_buffers), il suit donc m_mx/m_my que le WindowManager met à jour à chaque
   événement souris. Pas d'overlay front-buffer, pas de throttle : la correction
   avant la finesse. (L'ancien overlay figeait le curseur — cf. régression.)   */
void Compositor::draw_cursor()
{
    Graphics& g = Graphics::instance();
    int x = m_mx, y = m_my;
    /* contour noir (dilatation 3×3 du masque) … */
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 8; c++)
            if (s_cursor_mask[r] & (0x80u >> c))
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        g.put_pixel(x + c + dx, y + r + dy, 0x000000u);
    /* … puis le corps blanc par-dessus. */
    for (int r = 0; r < 12; r++)
        for (int c = 0; c < 8; c++)
            if (s_cursor_mask[r] & (0x80u >> c))
                g.put_pixel(x + c, y + r, 0xFFFFFFu);
}

/* ── add_app / has_fullscreen_app ────────────────────────────────────── */

void Compositor::add_app(Greg::RefPtr<::Compositor::Application> app)
{
    m_apps.append(Greg::move(app));
}

bool Compositor::has_fullscreen_app() const
{
    for (Greg::usize i = 0; i < m_apps.size(); ++i)
        if (!m_apps[i].is_null() && m_apps[i]->is_fullscreen() && m_apps[i]->is_visible())
            return true;
    return false;
}

/* ── compose ─────────────────────────────────────────────────────────── */

void Compositor::compose(Greg::Vector<Greg::RefPtr<Window>>& windows)
{
    /* Purge Applications whose lifecycle has ended. */
    for (Greg::usize i = 0; i < m_apps.size(); ) {
        if (!m_apps[i].is_null() && m_apps[i]->close_requested()) {
            m_apps[i]->on_removed();
            m_apps.remove(i);
        } else {
            ++i;
        }
    }

    /* While a fullscreen Application (e.g. ArcadeApp) is active, the game
       thread owns the framebuffer and calls swap_buffers itself — skip the
       normal compositor rendering pass entirely.                           */
    if (has_fullscreen_app())
        return;

    Graphics& g = Graphics::instance();
    draw_wallpaper();

    draw_desktop();
    draw_desk_icons();

    for (Greg::usize i = 0; i < windows.size(); ++i) {
        if (windows[i].is_null() || !windows[i]->visible()) continue;
        /* Drop shadow: dark bands to the right and below the window. */
        Window* w = windows[i].ptr();
        int sx = w->x(), sy = w->y(), sw = w->w(), sh = w->h();
        g.fill_rect(sx + sw, sy + 5, 5, sh, Theme::GOUFFRE);   /* right band */
        g.fill_rect(sx + 5, sy + sh, sw, 5, Theme::GOUFFRE);   /* bottom band */
        /* Systemic clipping: no window can paint outside its own frame
           (fixed layouts on a user-shrunk frame, partial offscreen drags…).
           +5px right/bottom so self-drawn drop shadows (menus, Casino)
           keep their overhang. Inner clippers save/restore, cf Graphics.  */
        g.set_clip(sx, sy, sw + 5, sh + 5);
        windows[i]->draw();
        g.clear_clip();
    }

    draw_taskbar(windows);

    /* CRT phosphor scanlines over the finished scene, then the crisp cursor. */
    g.apply_scanlines(2);
    draw_cursor();

    g.swap_buffers();
}

/* ── submit_event ────────────────────────────────────────────────────── */

void Compositor::submit_event(const Event& e)
{
    WindowManager::instance().dispatch_event(e);
}

} /* namespace Kernel */

/* ── C bridges ──────────────────────────────────────────────────────────── */
extern "C" {

void compositor_submit_event(Event e)
{
    Kernel::Compositor::instance().submit_event(e);
}

void compositor_render(void)
{
    /* Routes through WM::draw() so that closed-window cleanup (purging
       close_requested() entries) happens before Compositor::compose().
       Phase 5: extract WM::cleanup() to break the Compositor→WM→Compositor
       layering and let compositor_render() call compose() directly.        */
    WindowManager::instance().draw();
}

} /* extern "C" */
