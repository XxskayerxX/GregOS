/* PaintWindow — "GregPaint" raster paint application.
   Freestanding: no libc, no exceptions. Canvas is an XRGB32 heap buffer.    */

#include "../include/PaintWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"

extern "C" void* kmalloc(unsigned int size);
extern "C" void  kfree(void* p);

/* ── 16-colour palette ───────────────────────────────────────────────────── */
static const unsigned int PALETTE[16] = {
    0x000000u, 0x808080u, 0x800000u, 0xFF0000u,
    0x808000u, 0xFFFF00u, 0x008000u, 0x00FF00u,
    0x008080u, 0x00FFFFu, 0x000080u, 0x0000FFu,
    0x800080u, 0xFF00FFu, 0xFFFFFFu, 0xC08040u
};

static const char* const TOOL_LBL[PaintWindow::T_COUNT] = {
    "Cr", "Li", "Re", "Rf", "Ce", "Rp", "Go"
};

/* ── lifecycle ───────────────────────────────────────────────────────────── */
PaintWindow::~PaintWindow() { if (m_canvas) { kfree(m_canvas); m_canvas = nullptr; } }

void PaintWindow::on_removed() { if (m_canvas) { kfree(m_canvas); m_canvas = nullptr; } }

void PaintWindow::init_canvas() {
    m_canvas = (unsigned int*)kmalloc((unsigned)(CANVAS_W * CANVAS_H * 4));
    if (m_canvas)
        for (int i = 0; i < CANVAS_W * CANVAS_H; ++i) m_canvas[i] = 0xFFFFFFu;
}

/* ── canvas primitives (all clamp to canvas bounds) ──────────────────────── */
void PaintWindow::put_c(int x, int y, unsigned int col) {
    if (!m_canvas) return;
    if ((unsigned)x >= (unsigned)CANVAS_W || (unsigned)y >= (unsigned)CANVAS_H) return;
    m_canvas[y * CANVAS_W + x] = col;
}

void PaintWindow::brush(int x, int y, unsigned int col) {
    int t = m_size;                      /* 1 / 3 / 5 → half-widths 0/1/2  */
    int h = t / 2;
    for (int oy = -h; oy <= h; ++oy)
        for (int ox = -h; ox <= h; ++ox)
            put_c(x + ox, y + oy, col);
}

void PaintWindow::line_c(int x0, int y0, int x1, int y1, unsigned int col, int th) {
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (th <= 1) put_c(x0, y0, col);
        else { int h = th / 2; for (int oy=-h; oy<=h; ++oy) for (int ox=-h; ox<=h; ++ox) put_c(x0+ox, y0+oy, col); }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void PaintWindow::rect_c(int x0, int y0, int x1, int y1, unsigned int col, bool fill) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (fill) {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) put_c(x, y, col);
    } else {
        int th = m_size;
        line_c(x0, y0, x1, y0, col, th); line_c(x0, y1, x1, y1, col, th);
        line_c(x0, y0, x0, y1, col, th); line_c(x1, y0, x1, y1, col, th);
    }
}

void PaintWindow::circle_c(int x0, int y0, int x1, int y1, unsigned int col) {
    int dx = x1 - x0, dy = y1 - y0;
    int r = dx*dx + dy*dy; int rad = 0; while ((rad+1)*(rad+1) <= r) rad++;
    int x = rad, y = 0, err = 0;
    while (x >= y) {
        int pts[8][2] = { {x0+x,y0+y},{x0+y,y0+x},{x0-y,y0+x},{x0-x,y0+y},
                          {x0-x,y0-y},{x0-y,y0-x},{x0+y,y0-x},{x0+x,y0-y} };
        for (int i = 0; i < 8; ++i) {
            if (m_size <= 1) put_c(pts[i][0], pts[i][1], col);
            else { int h=m_size/2; for(int a=-h;a<=h;++a) for(int b=-h;b<=h;++b) put_c(pts[i][0]+a, pts[i][1]+b, col); }
        }
        y++; if (err <= 0) err += 2*y + 1; if (err > 0) { x--; err -= 2*x + 1; }
    }
}

/* Iterative 4-connected flood fill with an explicit heap stack. */
void PaintWindow::flood(int x, int y, unsigned int col) {
    if (!m_canvas) return;
    if ((unsigned)x >= (unsigned)CANVAS_W || (unsigned)y >= (unsigned)CANVAS_H) return;
    unsigned int target = m_canvas[y * CANVAS_W + x];
    if (target == col) return;
    int* stack = (int*)kmalloc((unsigned)(CANVAS_W * CANVAS_H * (int)sizeof(int)));
    if (!stack) return;
    int sp = 0;
    stack[sp++] = y * CANVAS_W + x;
    while (sp > 0) {
        int idx = stack[--sp];
        if (m_canvas[idx] != target) continue;
        int px = idx % CANVAS_W, py = idx / CANVAS_W;
        /* extend left/right on this scanline */
        int lft = px; while (lft > 0 && m_canvas[py*CANVAS_W + lft - 1] == target) lft--;
        int rgt = px; while (rgt < CANVAS_W-1 && m_canvas[py*CANVAS_W + rgt + 1] == target) rgt++;
        for (int xx = lft; xx <= rgt; ++xx) {
            m_canvas[py*CANVAS_W + xx] = col;
            if (py > 0 && m_canvas[(py-1)*CANVAS_W + xx] == target && sp < CANVAS_W*CANVAS_H)
                stack[sp++] = (py-1)*CANVAS_W + xx;
            if (py < CANVAS_H-1 && m_canvas[(py+1)*CANVAS_W + xx] == target && sp < CANVAS_W*CANVAS_H)
                stack[sp++] = (py+1)*CANVAS_W + xx;
        }
    }
    kfree(stack);
}

void PaintWindow::clear_canvas(unsigned int col) {
    if (!m_canvas) return;
    for (int i = 0; i < CANVAS_W * CANVAS_H; ++i) m_canvas[i] = col;
}

/* ── persistence: RLE ".gpi" images in the VFS ───────────────────────────────
   Format "GPI1" (all integers little-endian):
     offset 0 : magic 'G','P','I','1'                         (4 bytes)
     offset 4 : width   u16   (== CANVAS_W)                   (2 bytes)
     offset 6 : height  u16   (== CANVAS_H)                   (2 bytes)
     offset 8 : runs, each 5 bytes:
                  count u16   (1..65535 consecutive pixels)   (2 bytes)
                  R,G,B u8×3  (colour, one byte each, R first)(3 bytes)
   Pixels are scanned row-major; canvas values are XRGB32 (0x00RRGGBB), so
   R=(col>>16)&0xFF, G=(col>>8)&0xFF, B=col&0xFF. A run of >65535 identical
   pixels is split into several runs. A mostly-white canvas compresses to a
   few dozen bytes.  Encoded size is capped at GPI_MAX_ENCODED (< the VFS
   4095-byte content limit); larger images are rejected, never truncated.   */

static const char  GPI_NAME[]      = "paint.gpi";
static const int   GPI_MAX_ENCODED = 4000;   /* stay well under FILE_CONTENT_SIZE */
static const int   GPI_MAX_FILES   = 64;     /* VFS slot count (see kernel.c)     */

static bool gpi_name_eq(const char* a, const char* b) {
    for (int i = 0; i < VFS_MAX_NAME; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0')  return true;
    }
    return true;
}

/* Return the entry id of root's "paint.gpi", -1 if absent, -2 on alloc error. */
static int gpi_find_file() {
    VFSEntry* ents = (VFSEntry*)kmalloc((unsigned)(GPI_MAX_FILES * (int)sizeof(VFSEntry)));
    if (!ents) return -2;
    int n = vfs_list_dir(0, ents, GPI_MAX_FILES);
    int id = -1;
    for (int i = 0; i < n; ++i) {
        if (ents[i].type == VFS_TYPE_FILE && gpi_name_eq(ents[i].name, GPI_NAME)) {
            id = ents[i].id;
            break;
        }
    }
    kfree(ents);
    return id;
}

void PaintWindow::set_status(const char* s) {
    int i = 0;
    if (s) while (s[i] && i < (int)sizeof(m_status) - 1) { m_status[i] = s[i]; ++i; }
    m_status[i] = '\0';
}

void PaintWindow::save_canvas() {
    if (!m_canvas) { set_status("Pas de canvas"); return; }

    unsigned char* buf = (unsigned char*)kmalloc(4096);
    if (!buf) { set_status("Erreur memoire"); return; }

    /* Header. */
    int len = 0;
    buf[len++] = 'G'; buf[len++] = 'P'; buf[len++] = 'I'; buf[len++] = '1';
    buf[len++] = (unsigned char)(CANVAS_W & 0xFF); buf[len++] = (unsigned char)((CANVAS_W >> 8) & 0xFF);
    buf[len++] = (unsigned char)(CANVAS_H & 0xFF); buf[len++] = (unsigned char)((CANVAS_H >> 8) & 0xFF);

    /* Runs. */
    const int N = CANVAS_W * CANVAS_H;
    bool too_complex = false;
    int i = 0;
    while (i < N) {
        unsigned int col = m_canvas[i];
        int run = 1;
        while (i + run < N && m_canvas[i + run] == col && run < 65535) ++run;
        if (len + 5 > GPI_MAX_ENCODED) { too_complex = true; break; }
        buf[len++] = (unsigned char)(run & 0xFF);
        buf[len++] = (unsigned char)((run >> 8) & 0xFF);
        buf[len++] = (unsigned char)((col >> 16) & 0xFF);   /* R */
        buf[len++] = (unsigned char)((col >>  8) & 0xFF);   /* G */
        buf[len++] = (unsigned char)( col        & 0xFF);   /* B */
        i += run;
    }

    if (too_complex) { kfree(buf); set_status("Image trop complexe"); return; }

    int id = gpi_find_file();
    if (id == -2) { kfree(buf); set_status("Erreur memoire"); return; }
    if (id <= 0)  id = vfs_create_file(GPI_NAME, 0);
    if (id <= 0)  { kfree(buf); set_status("Erreur disque"); return; }

    int wr = vfs_write_file(id, (const char*)buf, len);
    kfree(buf);
    set_status(wr == len ? "Enregistre : paint.gpi" : "Erreur ecriture");
}

void PaintWindow::load_canvas() {
    if (!m_canvas) { set_status("Pas de canvas"); return; }

    int id = gpi_find_file();
    if (id == -2) { set_status("Erreur memoire"); return; }
    if (id <= 0)  { set_status("paint.gpi introuvable"); return; }

    unsigned char* buf = (unsigned char*)kmalloc(4096);
    if (!buf) { set_status("Erreur memoire"); return; }

    int len = vfs_read_file(id, (char*)buf, 4096);
    if (len < 8 ||
        buf[0] != 'G' || buf[1] != 'P' || buf[2] != 'I' || buf[3] != '1') {
        kfree(buf); set_status("Fichier invalide"); return;
    }
    int w = (int)buf[4] | ((int)buf[5] << 8);
    int h = (int)buf[6] | ((int)buf[7] << 8);
    if (w != CANVAS_W || h != CANVAS_H) {
        kfree(buf); set_status("Fichier invalide"); return;
    }

    /* Decode runs into the canvas, clamping to CANVAS_W*CANVAS_H. */
    const int N = CANVAS_W * CANVAS_H;
    int p = 0, pos = 8;
    while (pos + 5 <= len && p < N) {
        int cnt = (int)buf[pos] | ((int)buf[pos + 1] << 8);
        unsigned int col = ((unsigned int)buf[pos + 2] << 16)
                         | ((unsigned int)buf[pos + 3] <<  8)
                         |  (unsigned int)buf[pos + 4];
        pos += 5;
        for (int k = 0; k < cnt && p < N; ++k) m_canvas[p++] = col;
    }
    kfree(buf);
    set_status("Ouvert : paint.gpi");
}

/* ── bevel helper ────────────────────────────────────────────────────────── */
static void bevel(Graphics& g, int x, int y, int w, int h, bool down) {
    unsigned int lt = down ? Theme::BEVEL_OUTER_DK : Theme::BEVEL_OUTER_LT;
    unsigned int dk = down ? Theme::BEVEL_OUTER_LT : Theme::BEVEL_OUTER_DK;
    g.draw_hline(x, y, w, lt); g.draw_vline(x, y, h, lt);
    g.draw_hline(x, y + h - 1, w, dk); g.draw_vline(x + w - 1, y, h, dk);
}

/* ── toolbar ─────────────────────────────────────────────────────────────── */
void PaintWindow::draw_toolbar() {
    Graphics& g = Graphics::instance();
    int tx = client_x(), ty = client_y() + TOPBAR_H;
    g.fill_rect(tx, ty, TOOLBAR_W, client_h() - TOPBAR_H, Theme::WIN_BG);

    for (int i = 0; i < T_COUNT; ++i) {
        int bx = tx + 4, by = ty + 4 + i * 32;
        bool sel = (m_tool == i);
        g.fill_rect(bx, by, 36, 28, sel ? Theme::AMBER_DEEP : Theme::BTN_FACE);
        bevel(g, bx, by, 36, 28, sel);
        g.draw_str(bx + 10, by + 6, TOOL_LBL[i], Theme::AMBER, GFX_TRANSPARENT);
    }
    /* Brush sizes 1/3/5 */
    int sy = ty + 4 + T_COUNT * 32 + 6;
    const int sizes[3] = { 1, 3, 5 };
    for (int i = 0; i < 3; ++i) {
        int bx = tx + 4, by = sy + i * 22;
        bool sel = (m_size == sizes[i]);
        g.fill_rect(bx, by, 36, 18, sel ? Theme::AMBER_DEEP : Theme::BTN_FACE);
        bevel(g, bx, by, 36, 18, sel);
        char lb[2] = { (char)('0' + sizes[i]), 0 };
        g.draw_str(bx + 14, by + 1, lb, Theme::AMBER, GFX_TRANSPARENT);
    }
}

/* ── palette ─────────────────────────────────────────────────────────────── */
void PaintWindow::draw_palette() {
    Graphics& g = Graphics::instance();
    int px = client_x(), py = client_y() + client_h() - PALETTE_H;
    g.fill_rect(px, py, client_w(), PALETTE_H, Theme::WIN_BG);
    int sw = 20;
    for (int i = 0; i < 16; ++i) {
        int x = px + 40 + i * sw, y = py + 3;
        g.fill_rect(x, y, sw - 2, PALETTE_H - 6, PALETTE[i]);
        g.draw_rect(x, y, sw - 2, PALETTE_H - 6, Theme::ASH);
    }
    /* current-colour swatch */
    g.fill_rect(px + 4, py + 3, 30, PALETTE_H - 6, m_color);
    g.draw_rect(px + 4, py + 3, 30, PALETTE_H - 6, Theme::GOLD_DIM);
}

/* ── elastic shape preview (drawn over the canvas blit) ──────────────────── */
void PaintWindow::draw_preview() {
    if (!m_dragging) return;
    Graphics& g = Graphics::instance();
    int ox = cvx(), oy = cvy();
    g.set_clip(ox, oy, CANVAS_W, CANVAS_H);
    int x0 = ox + m_sx, y0 = oy + m_sy, x1 = ox + m_cx, y1 = oy + m_cy;
    unsigned int col = m_color;
    if (m_tool == T_LINE) {
        /* thin preview line */
        int dx = x1-x0, dy = y1-y0; if (dx<0) dx=-dx; if (dy<0) dy=-dy;
        int sx = x0<x1?1:-1, sy = y0<y1?1:-1, err = dx-dy, cx=x0, cy=y0;
        for (;;) { g.put_pixel(cx, cy, col); if (cx==x1&&cy==y1) break;
            int e2=2*err; if (e2>-dy){err-=dy;cx+=sx;} if (e2<dx){err+=dx;cy+=sy;} }
    } else if (m_tool == T_RECT || m_tool == T_RECTF) {
        int ax=x0,ay=y0,bx=x1,by=y1; if(ax>bx){int t=ax;ax=bx;bx=t;} if(ay>by){int t=ay;ay=by;by=t;}
        if (m_tool == T_RECTF) g.fill_rect(ax, ay, bx-ax+1, by-ay+1, col);
        else g.draw_rect(ax, ay, bx-ax+1, by-ay+1, col);
    } else if (m_tool == T_CIRCLE) {
        int dx=m_cx-m_sx, dy=m_cy-m_sy; int r=dx*dx+dy*dy, rad=0; while((rad+1)*(rad+1)<=r) rad++;
        int x=rad,y=0,err=0;
        while (x>=y) {
            int pts[8][2]={{x0+x,y0+y},{x0+y,y0+x},{x0-y,y0+x},{x0-x,y0+y},{x0-x,y0-y},{x0-y,y0-x},{x0+y,y0-x},{x0+x,y0-y}};
            for(int i=0;i<8;++i) g.put_pixel(pts[i][0],pts[i][1],col);
            y++; if(err<=0) err+=2*y+1; if(err>0){x--; err-=2*x+1;}
        }
    }
    g.clear_clip();
}

/* ── draw ────────────────────────────────────────────────────────────────── */
void PaintWindow::draw() {
    Window::draw();
    Graphics& g = Graphics::instance();

    /* Top bar: Effacer / Sauver / Ouvrir / Quit + status or hint. */
    int bx = client_x(), by = client_y();
    g.fill_rect(bx, by, client_w(), TOPBAR_H, Theme::WIN_BG);
    /* [Effacer] (arms to "Sur?" on first click) */
    g.fill_rect(bx + TB_CLR_X, by + 3, TB_CLR_W, TOPBAR_H - 6, m_clear_arm ? Theme::EMBER : Theme::BTN_FACE);
    bevel(g, bx + TB_CLR_X, by + 3, TB_CLR_W, TOPBAR_H - 6, false);
    g.draw_str(bx + TB_CLR_X + 6, by + 5, m_clear_arm ? "Sur?" : "Effacer", Theme::AMBER, GFX_TRANSPARENT);
    /* [Sauver] */
    g.fill_rect(bx + TB_SAVE_X, by + 3, TB_SAVE_W, TOPBAR_H - 6, Theme::BTN_FACE);
    bevel(g, bx + TB_SAVE_X, by + 3, TB_SAVE_W, TOPBAR_H - 6, false);
    g.draw_str(bx + TB_SAVE_X + 6, by + 5, "Sauver", Theme::AMBER, GFX_TRANSPARENT);
    /* [Ouvrir] */
    g.fill_rect(bx + TB_OPEN_X, by + 3, TB_OPEN_W, TOPBAR_H - 6, Theme::BTN_FACE);
    bevel(g, bx + TB_OPEN_X, by + 3, TB_OPEN_W, TOPBAR_H - 6, false);
    g.draw_str(bx + TB_OPEN_X + 6, by + 5, "Ouvrir", Theme::AMBER, GFX_TRANSPARENT);
    /* [Quit] */
    g.fill_rect(bx + TB_QUIT_X, by + 3, TB_QUIT_W, TOPBAR_H - 6, Theme::BTN_FACE);
    bevel(g, bx + TB_QUIT_X, by + 3, TB_QUIT_W, TOPBAR_H - 6, false);
    g.draw_str(bx + TB_QUIT_X + 6, by + 5, "Quit", Theme::AMBER, GFX_TRANSPARENT);
    /* Status message (if any) else the keyboard hint. */
    const char* msg = m_status[0] ? m_status : "c/l/r/f/g/e/o s/w 1/2/3";
    g.draw_str(bx + TB_QUIT_X + TB_QUIT_W + 8, by + 5, msg,
               m_status[0] ? Theme::GREEN : Theme::FG_DIM, GFX_TRANSPARENT);

    draw_toolbar();

    /* Canvas blit. */
    int ox = cvx(), oy = cvy();
    if (m_canvas) {
        g.set_clip(ox, oy, CANVAS_W, CANVAS_H);
        g.draw_image(ox, oy, CANVAS_W, CANVAS_H, m_canvas);
        g.clear_clip();
        g.draw_rect(ox - 1, oy - 1, CANVAS_W + 2, CANVAS_H + 2, Theme::GOLD_DIM);
    } else {
        g.draw_str(ox + 8, oy + 8, "Erreur: canvas non alloue", Theme::FG_ALERT, Theme::WIN_BG);
    }

    draw_preview();
    draw_palette();
}

/* ── hit-tests ───────────────────────────────────────────────────────────── */
int PaintWindow::tool_at(int mx, int my) const {
    int tx = client_x() + 4, ty = client_y() + TOPBAR_H + 4;
    for (int i = 0; i < T_COUNT; ++i) {
        int by = ty + i * 32;
        if (mx >= tx && mx < tx + 36 && my >= by && my < by + 28) return i;
    }
    return -1;
}
int PaintWindow::size_at(int mx, int my) const {
    int tx = client_x() + 4, sy = client_y() + TOPBAR_H + 4 + T_COUNT * 32 + 6;
    const int sizes[3] = { 1, 3, 5 };
    for (int i = 0; i < 3; ++i) {
        int by = sy + i * 22;
        if (mx >= tx && mx < tx + 36 && my >= by && my < by + 18) return sizes[i];
    }
    return -1;
}
int PaintWindow::palette_at(int mx, int my) const {
    int px = client_x() + 40, py = client_y() + client_h() - PALETTE_H + 3;
    int sw = 20;
    if (my < py || my >= py + PALETTE_H - 6) return -1;
    for (int i = 0; i < 16; ++i) {
        int x = px + i * sw;
        if (mx >= x && mx < x + sw - 2) return i;
    }
    return -1;
}

/* ── events ──────────────────────────────────────────────────────────────── */
bool PaintWindow::on_event(const Event& e) {
    int mx = e.mouse.x, my = e.mouse.y;

    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {   /* press */
        if (!hit_test(mx, my)) return false;
        if (my < client_y()) return Window::on_event(e);           /* title bar */

        /* Top bar buttons */
        int by = client_y();
        if (my >= by && my < by + TOPBAR_H) {
            int bx = client_x();
            if (mx >= bx + TB_CLR_X && mx < bx + TB_CLR_X + TB_CLR_W) {
                if (m_clear_arm) { clear_canvas(0xFFFFFFu); m_clear_arm = 0; }
                else m_clear_arm = 1;
                return true;
            }
            m_clear_arm = 0;
            if (mx >= bx + TB_SAVE_X && mx < bx + TB_SAVE_X + TB_SAVE_W) { save_canvas(); return true; }
            if (mx >= bx + TB_OPEN_X && mx < bx + TB_OPEN_X + TB_OPEN_W) { load_canvas(); return true; }
            if (mx >= bx + TB_QUIT_X && mx < bx + TB_QUIT_X + TB_QUIT_W) { request_close(); return true; }
            return true;
        }
        m_clear_arm = 0;

        int t = tool_at(mx, my);
        if (t >= 0) { m_tool = t; return true; }
        int s = size_at(mx, my);
        if (s >= 0) { m_size = s; return true; }
        int c = palette_at(mx, my);
        if (c >= 0) { m_color = PALETTE[c]; return true; }

        /* Canvas press */
        int ox = cvx(), oy = cvy();
        if (mx >= ox && mx < ox + CANVAS_W && my >= oy && my < oy + CANVAS_H) {
            int px = mx - ox, py = my - oy;
            if (m_tool == T_FILL) { flood(px, py, m_color); return true; }
            m_dragging = true; m_sx = m_cx = m_lx = px; m_sy = m_cy = m_ly = py;
            if (m_tool == T_PENCIL) brush(px, py, m_color);
            else if (m_tool == T_ERASER) brush(px, py, 0xFFFFFFu);
            return true;
        }
        return true;
    }

    if (e.type == EVT_MOUSE_MOVE && (e.mouse.buttons & 0x01) && m_dragging) {  /* drag */
        int ox = cvx(), oy = cvy();
        int px = mx - ox, py = my - oy;
        if (px < 0) px = 0;
        if (px >= CANVAS_W) px = CANVAS_W - 1;
        if (py < 0) py = 0;
        if (py >= CANVAS_H) py = CANVAS_H - 1;
        m_cx = px; m_cy = py;
        if (m_tool == T_PENCIL)      { line_c(m_lx, m_ly, px, py, m_color, m_size); m_lx = px; m_ly = py; }
        else if (m_tool == T_ERASER) { line_c(m_lx, m_ly, px, py, 0xFFFFFFu, m_size < 3 ? 3 : m_size); m_lx = px; m_ly = py; }
        return true;
    }

    if (e.type == EVT_MOUSE_BUTTON && !(e.mouse.buttons & 0x01) && m_dragging) {  /* release */
        m_dragging = false;
        if (m_tool == T_LINE)   line_c(m_sx, m_sy, m_cx, m_cy, m_color, m_size);
        else if (m_tool == T_RECT)   rect_c(m_sx, m_sy, m_cx, m_cy, m_color, false);
        else if (m_tool == T_RECTF)  rect_c(m_sx, m_sy, m_cx, m_cy, m_color, true);
        else if (m_tool == T_CIRCLE) circle_c(m_sx, m_sy, m_cx, m_cy, m_color);
        return true;
    }

    return Window::on_event(e);
}

bool PaintWindow::handle_char(int c) {
    switch (c) {
        case 'c': case 'C': m_tool = T_PENCIL; return true;
        case 'l': case 'L': m_tool = T_LINE;   return true;
        case 'r': case 'R': m_tool = T_RECT;   return true;
        case 'f': case 'F': m_tool = T_FILL;   return true;
        case 'g': case 'G': m_tool = T_ERASER; return true;
        case 'o': case 'O': m_tool = T_CIRCLE; return true;
        case 'e': case 'E': clear_canvas(0xFFFFFFu); return true;
        case 's': case 'S': save_canvas(); return true;
        case 'w': case 'W': load_canvas(); return true;
        case KEY_CTRL_S:    save_canvas(); return true;
        case '1': m_size = 1; return true;
        case '2': m_size = 3; return true;
        case '3': m_size = 5; return true;
        case KEY_ESC: request_close(); return true;
    }
    return false;
}

/* ── factory ─────────────────────────────────────────────────────────────── */
extern "C" void open_paint_window(void) {
    auto w = Greg::make_ref<PaintWindow>();
    w->setup(120, 40, PaintWindow::TOOLBAR_W + PaintWindow::CANVAS_W + 6,
             PaintWindow::TOPBAR_H + PaintWindow::CANVAS_H + PaintWindow::PALETTE_H + 26,
             "GregPaint", Theme::WIN_BG);
    w->init_canvas();
    w->set_focused(true);
    WindowManager::instance().add_window(Greg::move(w));
}
