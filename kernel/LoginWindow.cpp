/* LoginWindow.cpp — login screen for GregOS.
   Freestanding: no libc, no exceptions.
   Phases:
     LOADING — Win98-style progress dialog with animated orange bar
     LOGIN   — password field; password "admin" → g_login_done=1 → desktop

   Background: procedural CRT-Norse backdrop (obsidian tube + "Anneau des
   Cycles" rune ring), drawn by draw_crt_bg() using the Theme phosphor ramps —
   no embedded BMP, matching the desktop compositor wallpaper.              */

#include "../include/LoginWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/art_drakkar.h"   /* menacing dragon (boot phase)  */
#include "../include/art_greg.h"      /* Chevalier Noir (login phase)  */

extern "C" volatile unsigned long jiffies;

extern "C" { int g_login_done = 0; }

/* ── Menacing ASCII-art of DRAKKAR (dragon) & GREG (Chevalier Noir) ─────────
   Generated from the reference mockups. Rendered with the 8×16 CP437 font, so
   it IS terminal ASCII-art — « Le Terminal du Royaume ». Palette index → colour;
   index 0 = transparent (lets the backdrop show through).                    */
static const unsigned int LW_ART_PAL[14] = {
    0x000000u,                                     /* 0  transparent          */
    0x1A0505u, 0x561212u, 0x8B1A1Au, 0xC83028u,    /* 1..4  sang (blood ramp) */
    0xFF5A1Fu, 0xFFB066u,                          /* 5..6  braise (ember)    */
    0x36FF74u, 0xB6FFC9u,                          /* 7..8  vert (œil/eye)    */
    0xC8C8C0u, 0xC9A24Bu,                          /* 9 os, 10 or             */
    0x4A5548u, 0x8C8C84u, 0xE8E4D8u,               /* 11 cendre,12 acier,13 os-vif */
};

/* ── Living-fire animation helpers (integer only, freestanding) ──────────────
   The guardians breathe: Drakkar's eye pulses, his fiery hide flickers, and
   GREG's forged-iron red glares like a cooling blade. Achieved by rebuilding a
   14-entry palette every frame and modulating a few indices — the glyph grid
   itself never moves (the composition the mockups nailed stays put).          */

/* Triangle wave 0..255 over `period` jiffies, offset by `ph`. */
static unsigned int lw_tri(unsigned long t, unsigned long period, unsigned long ph)
{
    unsigned long x    = (t + ph) % period;
    unsigned long half = period / 2;
    unsigned long up   = (x < half) ? x : (period - x);   /* 0..half ramp */
    return (unsigned int)(up * 255u / (half ? half : 1));
}

/* Linear blend a→b by t/255 (per channel, signed-safe). */
static unsigned int lw_mix(unsigned int a, unsigned int b, unsigned int t)
{
    int ar=(a>>16)&0xFF, ag=(a>>8)&0xFF, ab=a&0xFF;
    int br=(b>>16)&0xFF, bg=(b>>8)&0xFF, bb=b&0xFF;
    int r = ar + (br-ar)*(int)t/255;
    int gg= ag + (bg-ag)*(int)t/255;
    int bl= ab + (bb-ab)*(int)t/255;
    return ((unsigned)r<<16) | ((unsigned)gg<<8) | (unsigned)bl;
}

/* Scale a colour's brightness by s/255 (0 = black, 255 = unchanged). */
static unsigned int lw_scale(unsigned int c, unsigned int s)
{
    unsigned int r=((c>>16)&0xFF)*s/255, g=((c>>8)&0xFF)*s/255, b=(c&0xFF)*s/255;
    return (r<<16)|(g<<8)|b;
}

/* Integer bit-mix hash — deterministic pseudo-random per ember (no RNG/float). */
static unsigned int lw_hash(unsigned int x)
{
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return x;
}

/* ── Ember storm: sparks rising off DRAKKAR during boot ──────────────────────
   A field of deterministic embers born hot at the bottom, drifting up while
   they cool from white-hot → braise → deep blood, then burn out near the top.
   Drawn between the dragon and the loading dialog, so they rise IN FRONT of
   Drakkar while the panel stays crisp. Pure function of jiffies + index.    */
static void lw_draw_embers()
{
    Graphics& g = Graphics::instance();
    const int N = 96;
    for (int i = 0; i < N; ++i) {
        unsigned int h  = lw_hash((unsigned)i * 2654435761u + 1u);
        unsigned int h2 = lw_hash(h ^ 0x9E3779B9u);

        int           base_x = 52 + (int)(h % 698u);        /* 52..749         */
        unsigned long period = 130u + (h2 % 150u);          /* rise time (j)   */
        unsigned long phase  = h2 % period;
        unsigned long lifej  = (jiffies + phase) % period;
        unsigned int  lifef  = (unsigned int)(lifej * 255u / period);  /* 0↑255 */

        int y = 526 - (int)((526 - 8) * (int)lifej / (int)period);     /* rise up */

        int amp  = 2 + (int)(h % 4u);
        int sway = (int)lw_tri(jiffies, 34u + (h % 40u), phase) - 128;  /* ±128  */
        int x    = base_x + sway * amp / 128;

        /* Cool as it climbs: white-hot → ember → deep blood. */
        unsigned int col = (lifef < 120u)
            ? lw_mix(0xFFE6B4u, 0xFF6A20u, lifef * 255u / 120u)
            : lw_mix(0xFF6A20u, 0x5E1206u, (lifef - 120u) * 255u / 135u);

        /* Burn-out fade near the top + a fast per-ember twinkle. */
        unsigned int fade = (lifef > 200u) ? (255u - (lifef - 200u) * 255u / 55u) : 255u;
        unsigned int tw   = 175u + lw_tri(jiffies, 7u + (h % 11u), h) * 80u / 255u;
        col = lw_scale(col, fade * tw / 255u);

        if (x < 2 || x > 797 || y < 2 || y > 596) continue;

        if ((h & 3u) == 0u && lifef < 170u) {          /* fat, hot spark + white core */
            g.fill_rect(x, y, 2, 2, col);
            if (lifef < 90u) g.put_pixel(x, y, lw_mix(col, 0xFFF2D0u, 150u));
        } else {
            g.put_pixel(x, y, col);
            if (lifef < 80u) g.put_pixel(x, y + 1, col);   /* rising tail when newborn */
        }
    }
}

static void lw_draw_art(int x0, int y0, const unsigned char* ch,
                        const unsigned char* pal, int cols, int rows,
                        const unsigned int* palette)
{
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            unsigned char p = pal[r * cols + c];
            if (!p) continue;
            gfx_draw_char(x0 + c * 8, y0 + r * 16, ch[r * cols + c],
                          palette[p], GFX_TRANSPARENT);
        }
}

/* ── Tiny helpers ─────────────────────────────────────────────────────────── */

static int lw_slen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

static void lw_itos(int n, char* buf) {
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    char t[12]; int ti = 0;
    while (n > 0) { t[ti++] = '0' + n % 10; n /= 10; }
    int i = 0;
    for (int k = ti-1; k >= 0; --k) buf[i++] = t[k];
    buf[i] = '\0';
}

/* ── Constructor ─────────────────────────────────────────────────────────── */

LoginWindow::LoginWindow()
    : m_phase(PHASE_LOADING), m_boot_j(0)
    , m_pwd_len(0), m_err_j(0)
{
    m_pwd[0] = '\0';
}

/* ── draw_loading_box ────────────────────────────────────────────────────── */

void LoginWindow::draw_loading_box(int pct)
{
    Graphics& g = Graphics::instance();

    const int bx = 170, by = 315, bw = 460, bh = 155;

    /* Cadre royal — métal du royaume (ex-biseau Win98) */
    g.draw_hline(bx,      by,       bw, Theme::GOLD_DIM);
    g.draw_vline(bx,      by,       bh, Theme::GOLD_DIM);
    g.draw_hline(bx,      by+bh-1,  bw, Theme::GOUFFRE);
    g.draw_vline(bx+bw-1, by,       bh, Theme::GOUFFRE);
    g.draw_hline(bx+1,    by+1,     bw-2, Theme::GOLD);
    g.draw_vline(bx+1,    by+1,     bh-2, Theme::GOLD);
    g.draw_hline(bx+1,    by+bh-2,  bw-2, Theme::GOUFFRE);
    g.draw_vline(bx+bw-2, by+1,     bh-2, Theme::GOUFFRE);

    /* Corps obsidienne */
    g.fill_rect(bx+2, by+2, bw-4, bh-4, Theme::WIN_BG);

    /* Cartouche de titre (ambre sombre → or) */
    int tb_h = 22;
    g.gradient_rect(bx+2, by+2, bw-4, tb_h, Theme::TITLE_FOCUS_A, Theme::TITLE_FOCUS_B, 0);
    g.draw_str(bx+7, by+5, "Initialisation du royaume...", Theme::TITLE_FG, GFX_TRANSPARENT);

    int body_y = by + 2 + tb_h + 6;

    /* Journal d'amorçage (voix de la machine = vert phosphore) */
    int lx = bx + 14;
    unsigned int lc = Theme::GREEN;
    g.draw_str(lx, body_y,      "Preparation du noyau...",           lc, GFX_TRANSPARENT);
    if (pct >= 33)
        g.draw_str(lx, body_y+16, "Chargement des pilotes...",       lc, GFX_TRANSPARENT);
    if (pct >= 66)
        g.draw_str(lx, body_y+32, "Configuration de l interface...", lc, GFX_TRANSPARENT);

    /* Barre de progression segmentée (ambre royal) */
    int pb_x = bx + 14, pb_y = by + bh - 48;
    int pb_w = bw - 28, pb_h = 16;
    g.fill_rect(pb_x, pb_y, pb_w, pb_h, Theme::WIN_BG_PURE);
    g.draw_rect(pb_x, pb_y, pb_w, pb_h, Theme::GOLD_DIM);

    int filled_px = pb_w * pct / 100;
    int blk = 13, gap = 2, x = pb_x + 1;
    while (x + blk <= pb_x + filled_px) {
        g.fill_rect(x, pb_y+1, blk, pb_h-2, Theme::AMBER);
        x += blk + gap;
    }
    int leftover = pb_x + filled_px - x;
    if (leftover > 0 && leftover <= blk)
        g.fill_rect(x, pb_y+1, leftover, pb_h-2, Theme::AMBER);

    /* Étiquette de pourcentage */
    char buf[16]; int pi = 0;
    char num[8]; lw_itos(pct, num);
    for (int k = 0; num[k]; ++k) buf[pi++] = num[k];
    const char* suf = "% termine";
    for (int k = 0; suf[k]; ++k) buf[pi++] = suf[k];
    buf[pi] = '\0';
    g.draw_str(pb_x, pb_y + pb_h + 4, buf, Theme::AMBER, GFX_TRANSPARENT);
}

/* ── draw_login_box ──────────────────────────────────────────────────────── */

void LoginWindow::draw_login_box()
{
    Graphics& g = Graphics::instance();

    const int bx = 200, by = 476, bw = 400, bh = 90;

    /* Cadre royal */
    g.draw_hline(bx,      by,       bw, Theme::GOLD_DIM);
    g.draw_vline(bx,      by,       bh, Theme::GOLD_DIM);
    g.draw_hline(bx,      by+bh-1,  bw, Theme::GOUFFRE);
    g.draw_vline(bx+bw-1, by,       bh, Theme::GOUFFRE);
    g.draw_hline(bx+1,    by+1,     bw-2, Theme::GOLD);
    g.draw_vline(bx+1,    by+1,     bh-2, Theme::GOLD);
    g.draw_hline(bx+1,    by+bh-2,  bw-2, Theme::GOUFFRE);
    g.draw_vline(bx+bw-2, by+1,     bh-2, Theme::GOUFFRE);
    g.fill_rect(bx+2, by+2, bw-4, bh-4, Theme::WIN_BG);

    /* Label */
    g.draw_str(bx+12, by+10, "Mot de passe root@gregos :", Theme::AMBER, GFX_TRANSPARENT);

    /* Champ de saisie encastré (sombre) */
    int pf_x = bx+12, pf_y = by+28, pf_w = bw-24, pf_h = 20;
    g.fill_rect(pf_x, pf_y, pf_w, pf_h, Theme::WIN_BG_PURE);
    g.draw_hline(pf_x,        pf_y,        pf_w, Theme::GOUFFRE);
    g.draw_vline(pf_x,        pf_y,        pf_h, Theme::GOUFFRE);
    g.draw_hline(pf_x+1,      pf_y+1,      pf_w-2, Theme::GOUFFRE);
    g.draw_vline(pf_x+1,      pf_y+1,      pf_h-2, Theme::GOUFFRE);
    g.draw_hline(pf_x,        pf_y+pf_h-1, pf_w,   Theme::GOLD_DIM);
    g.draw_vline(pf_x+pf_w-1, pf_y,        pf_h,   Theme::GOLD_DIM);

    /* Asterisks */
    for (int i = 0; i < m_pwd_len; ++i)
        g.draw_char(pf_x+5 + i*8, pf_y+2, '*', Theme::AMBER, GFX_TRANSPARENT);

    /* Blinking cursor (50-jiffie period ≈ 0.5 s) */
    if ((jiffies / 50) % 2 == 0) {
        int cur_x = pf_x + 5 + m_pwd_len * 8;
        g.draw_vline(cur_x, pf_y+2, pf_h-4, Theme::AMBER);
    }

    /* Error / hint */
    if (m_err_j && (jiffies - m_err_j) < ERR_J) {
        g.fill_rect(bx+12, by+56, bw-24, 14, Theme::EMBER_DEEP);
        g.draw_rect(bx+12, by+56, bw-24, 14, Theme::EMBER);
        g.draw_str(bx+14, by+58, "Mot de passe incorrect !", Theme::EMBER_HI, GFX_TRANSPARENT);
    } else {
        g.draw_str(bx+12, by+58, "[ Entree = valider ]", Theme::ASH, GFX_TRANSPARENT);
    }
}

/* ── draw_footer ─────────────────────────────────────────────────────────── */

void LoginWindow::draw_footer()
{
    Graphics& g = Graphics::instance();
    g.draw_hline(0, 572, 800, Theme::GOLD_DEEP);
    const char* ft = "GregOS  |  Sceau de GREG 1er, Seigneur du Kernel  |  MMXXVI";
    g.draw_str((800 - lw_slen(ft)*8)/2, 577, ft, Theme::GOLD_DIM, GFX_TRANSPARENT);
}

/* ── draw_crt_bg: procedural CRT-Norse backdrop ──────────────────────────────
   Obsidienne de tube cathodique + léger dégradé vertical, champ de runes
   épars, et l'« Anneau des Cycles » (grand cercle or-profond) avec marqueurs
   cardinaux ambrés — assorti au wallpaper du compositeur. Sans alpha, sans
   BMP : tout est peint sur le back-buffer avec les primitives Graphics.     */

static void draw_crt_bg()
{
    Graphics& g = Graphics::instance();
    const int W = 800, H = 600;

    /* Obsidienne : dégradé vertical subtil (haut légèrement plus clair). */
    g.gradient_rect(0, 0, W, H, 0x0C1010u, 0x060907u, 0);

    /* Champ de runes : points très discrets sur une grille lâche. */
    for (int y = 24; y < H - 40; y += 48)
        for (int x = 24; x < W; x += 48)
            g.put_pixel(x, y, 0x141810u);

    /* Anneau des Cycles — grand cercle runique centré (double trait). */
    const int cx = W / 2, cy = H / 2 - 40;
    for (int band = 0; band < 2; ++band) {
        int R = 210 - band;
        for (int dy = 0; dy < 2 * R; ++dy) {
            int rr = R * R - (dy - R) * (dy - R);
            if (rr < 0) continue;
            int xx = 0;
            while ((xx + 1) * (xx + 1) <= rr) ++xx;
            g.put_pixel(cx - xx, cy - R + dy, 0x1E1608u);
            g.put_pixel(cx + xx, cy - R + dy, 0x1E1608u);
        }
    }

    /* Marqueurs cardinaux — accents ambrés sur l'anneau. */
    g.fill_rect(cx - 1,   cy - 220, 2,  18, Theme::AMBER_DIM);
    g.fill_rect(cx - 1,   cy + 202, 2,  18, Theme::AMBER_DIM);
    g.fill_rect(cx - 220, cy - 1,   18, 2,  Theme::AMBER_DIM);
    g.fill_rect(cx + 202, cy - 1,   18, 2,  Theme::AMBER_DIM);
}

/* ── draw: procedural background + dialogs ──────────────────────────────── */

void LoginWindow::draw()
{
    /* Latch animation clock on very first frame */
    if (m_boot_j == 0)
        m_boot_j = jiffies ? jiffies : 1;

    unsigned long elapsed = jiffies - m_boot_j;

    /* Phase transition: loading → login */
    if (m_phase == PHASE_LOADING && elapsed >= LOAD_J + PAUSE_J)
        m_phase = PHASE_LOGIN;

    int pct = (elapsed >= LOAD_J) ? 100 : (int)(elapsed * 100 / LOAD_J);

    /* Procedural CRT-Norse backdrop (replaces the old embedded BMP). */
    draw_crt_bg();

    /* ── Rebuild the guardian palette for this frame — bring them to life. ──
       Base colours copied, then the "hot" indices modulated so the art
       smoulders and glares instead of sitting flat.                          */
    unsigned int pal[14];
    for (int i = 0; i < 14; ++i) pal[i] = LW_ART_PAL[i];

    /* Forged-iron RED (idx 4): slow, sinister breath — GREG's eyes/horns and
       Drakkar's hide swell from cooled blood to hot metal (~1.2 s). */
    {
        unsigned int t = lw_tri(jiffies, 120, 0);
        pal[4] = lw_mix(0x8B1A1Au, 0xFF3A24u, t);
    }
    /* Dragon-fire EMBER (idx 5,6): fast, irregular flicker from two coprime
       triangles — the breath never holds a single shade. */
    {
        unsigned int f = (lw_tri(jiffies, 13, 0) + lw_tri(jiffies, 21, 7)) / 2;
        pal[5] = lw_mix(0xB83810u, 0xFF7A2Cu, f);      /* deep ↔ bright braise */
        pal[6] = lw_mix(0xE08838u, 0xFFE0A0u, f);      /* white-hot tips        */
    }
    /* Drakkar's GREEN eye (idx 7,8): a slow malevolent glow with an occasional
       brighter flare (~1.6 s) — it watches you boot. */
    {
        unsigned int e = lw_tri(jiffies, 160, 0);
        pal[7] = lw_mix(0x1F9E4Cu, 0x5BFF92u, e);
        pal[8] = lw_mix(0x7ADFA0u, 0xE6FFEEu, e);
    }

    /* The guardians of the realm, in menacing ASCII-art (from the mockups):
       DRAKKAR the dragon rises as the kernel boots, then GREG 1ᵉʳ — the
       Chevalier Noir — bars the gate until you speak the word.               */
    if (m_phase == PHASE_LOADING) {
        lw_draw_art(0, 6, &drakkar_ch[0][0], &drakkar_pal[0][0],
                    DRAKKAR_COLS, DRAKKAR_ROWS, pal);
        lw_draw_embers();          /* sparks rise off the dragon as it wakes */
    } else {
        lw_draw_art((800 - GREG_COLS * 8) / 2, 2, &greg_ch[0][0], &greg_pal[0][0],
                    GREG_COLS, GREG_ROWS, pal);
    }

    draw_loading_box(pct);
    if (m_phase == PHASE_LOGIN)
        draw_login_box();
    draw_footer();
}

/* ── handle_char: absorb during load, capture pwd during login ───────────── */

bool LoginWindow::handle_char(int c)
{
    if (m_phase == PHASE_LOADING) return true;

    if (c == '\n' || c == '\r') {
        m_pwd[m_pwd_len] = '\0';
        bool ok = (m_pwd_len == 5)
               && (m_pwd[0]=='a') && (m_pwd[1]=='d') && (m_pwd[2]=='m')
               && (m_pwd[3]=='i') && (m_pwd[4]=='n');
        if (ok) {
            g_login_done = 1;
            request_close();
        } else {
            m_err_j   = jiffies ? jiffies : 1;
            m_pwd_len = 0;
        }
        return true;
    }

    if (c == '\b' && m_pwd_len > 0)            { --m_pwd_len; return true; }
    if (c >= 32 && c <= 126 && m_pwd_len < 31) { m_pwd[m_pwd_len++] = (char)c; }
    return true;
}

/* ── open_login_window: extern "C" factory called from kernel.c ──────────── */

extern "C" void open_login_window(void)
{
    auto w = Greg::make_ref<LoginWindow>();
    w->setup(0, 0, 800, 600, "", 0x000000u);
    w->set_focused(true);
    WindowManager::instance().add_window(Greg::move(w));
}
