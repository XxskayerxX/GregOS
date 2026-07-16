#include "../include/Graphics.hpp"
#include "../include/gfx.h"
#include "font8x16.h"

/* ── Back buffer: ~1.83 MB static in BSS ────────────────────────────── */
static unsigned int s_back[800 * 600];

/* ── Singleton storage (zero-init from BSS — no ctor needed) ────────── */
static Graphics s_graphics;

Graphics& Graphics::instance() { return s_graphics; }

/* ── Graphics::init ─────────────────────────────────────────────────── */
void Graphics::init(unsigned int fb_addr, unsigned int pitch_bytes,
                    int width, int height) {
    fb        = (volatile unsigned int*)fb_addr;
    back      = s_back;
    pitch_u32 = pitch_bytes / 4;
    fb_w      = width;
    fb_h      = height;
    fb_ready  = true;
    /* Clip rect defaults to full screen */
    clip_x1 = 0; clip_y1 = 0; clip_x2 = width; clip_y2 = height;
}

/* ── Graphics::swap_buffers ─────────────────────────────────────────── */
void Graphics::swap_buffers() {
    if (!fb_ready) return;
    const unsigned int* src = back;
    unsigned int*       dst = (unsigned int*)fb; /* volatile cast safe: bulk write */
    if (pitch_u32 == (unsigned int)fb_w) {
        /* Linear framebuffer: single rep movsl — 800×600 = 480 000 dwords */
        int n = fb_w * fb_h;
        __asm__ volatile ("rep movsl"
            : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
    } else {
        /* Padded pitch: copy one row at a time */
        for (int y = 0; y < fb_h; y++) {
            const unsigned int* rs = back + y * fb_w;
            unsigned int*       rd = (unsigned int*)fb + y * pitch_u32;
            int w = fb_w;
            __asm__ volatile ("rep movsl"
                : "+S"(rs), "+D"(rd), "+c"(w) : : "memory");
        }
    }
}

/* ── Graphics::present_rect ─────────────────────────────────────────────
   Copy a rectangle from the back buffer to the visible framebuffer.
   back is tightly packed (fb_w stride); fb may carry pitch padding.       */
void Graphics::present_rect(int x, int y, int w, int h) {
    if (!fb_ready || w <= 0 || h <= 0) return;
    /* Clip to screen */
    if (x < 0)          { w += x; x = 0; }
    if (y < 0)          { h += y; y = 0; }
    if (x + w > fb_w)   w = fb_w - x;
    if (y + h > fb_h)   h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        const unsigned int* rs = back + (unsigned)(y + row) * (unsigned)fb_w + (unsigned)x;
        unsigned int*       rd = (unsigned int*)fb + (unsigned)(y + row) * pitch_u32 + (unsigned)x;
        int n = w;
        __asm__ volatile ("rep movsl"
            : "+S"(rs), "+D"(rd), "+c"(n) : : "memory");
    }
}

/* ── Graphics::put_pixel_front ──────────────────────────────────────────── */
void Graphics::put_pixel_front(int x, int y, unsigned int color) {
    if (!fb_ready) return;
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h) return;
    ((unsigned int*)fb)[(unsigned)y * pitch_u32 + (unsigned)x] = color;
}

/* ── Graphics::snapshot_back / blit_back ────────────────────────────────
   Full-screen back-buffer copies (fb_w*fb_h dwords). back is tightly packed,
   so a single rep movsl moves the whole frame.                             */
void Graphics::snapshot_back(unsigned int* dst) const {
    if (!fb_ready || !dst) return;
    const unsigned int* src = back;
    int n = fb_w * fb_h;
    __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
}

void Graphics::blit_back(const unsigned int* src) {
    if (!fb_ready || !src) return;
    unsigned int* dst = back;
    int n = fb_w * fb_h;
    __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) : : "memory");
}

/* ── Graphics::apply_scanlines ──────────────────────────────────────────
   Darken every other back-buffer row for a CRT phosphor-tube look, in
   place on the finished frame. Uses a packed per-channel subtract
   `c - ((c >> shift) & mask)`: masking after the shift keeps each byte's
   contribution inside its own channel, and since each channel's shifted
   value is <= that channel, no borrow ever crosses a channel boundary.
   shift=2 → −25% (mask 0x3F3F3F); shift=3 → −12.5% (mask 0x1F1F1F).       */
void Graphics::apply_scanlines(int shift) {
    if (!fb_ready) return;
    if (shift < 1) shift = 1;
    if (shift > 7) shift = 7;
    /* Per-byte mask that survives a right-shift of `shift` bits. */
    unsigned int mask = (0xFFu >> shift);
    mask |= (mask << 8) | (mask << 16);
    for (int y = 1; y < fb_h; y += 2) {
        unsigned int* row = back + (unsigned)y * (unsigned)fb_w;
        for (int x = 0; x < fb_w; x++) {
            unsigned int c = row[x];
            row[x] = c - ((c >> shift) & mask);
        }
    }
}

/* ── Graphics::clear ────────────────────────────────────────────────── */
void Graphics::clear(unsigned int color) {
    if (!fb_ready) return;
    unsigned int* p = back;
    int n = fb_w * fb_h;
    for (int i = 0; i < n; i++) p[i] = color;
}

/* ── Graphics::fill_rect ────────────────────────────────────────────── */
void Graphics::fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (!fb_ready) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    /* honour the soft clip rect (put_pixel's twin write path) */
    if (x < clip_x1) { w -= clip_x1 - x; x = clip_x1; }
    if (y < clip_y1) { h -= clip_y1 - y; y = clip_y1; }
    if (x + w > clip_x2) w = clip_x2 - x;
    if (y + h > clip_y2) h = clip_y2 - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        unsigned int* dst = back + (unsigned)(y + row) * (unsigned)fb_w + (unsigned)x;
        for (int col = 0; col < w; col++) dst[col] = color;
    }
}

void Graphics::draw_hline(int x, int y, int len, unsigned int c) {
    fill_rect(x, y, len, 1, c);
}
void Graphics::draw_vline(int x, int y, int len, unsigned int c) {
    fill_rect(x, y, 1, len, c);
}
void Graphics::draw_rect(int x, int y, int w, int h, unsigned int c) {
    draw_hline(x,     y,       w, c);
    draw_hline(x,     y+h-1,   w, c);
    draw_vline(x,     y,       h, c);
    draw_vline(x+w-1, y,       h, c);
}

/* ── Graphics::draw_char ────────────────────────────────────────────── */
void Graphics::draw_char(int x, int y, unsigned char ch,
                          unsigned int fg, unsigned int bg) {
    if (!fb_ready) return;
    const unsigned char* glyph = font8x16[(unsigned)ch];
    bool transp = (bg == 0xFFFFFFFFu);
    for (int row = 0; row < GFX_FONT_H; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < GFX_FONT_W; col++) {
            bool set = (bits & (0x80u >> col)) != 0;
            if (set)          put_pixel(x + col, y + row, fg);
            else if (!transp) put_pixel(x + col, y + row, bg);
        }
    }
}

void Graphics::draw_str(int x, int y, const char* s,
                         unsigned int fg, unsigned int bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += GFX_FONT_H; }
        else { draw_char(cx, y, (unsigned char)*s, fg, bg); cx += GFX_FONT_W; }
        s++;
    }
}

/* ── Graphics::gradient_rect ────────────────────────────────────────── */
void Graphics::gradient_rect(int x, int y, int w, int h,
                              unsigned int c1, unsigned int c2, int vertical) {
    if (!fb_ready || w <= 0 || h <= 0) return;
    int steps = vertical ? h : w;
    for (int i = 0; i < steps; i++) {
        int r1=(int)((c1>>16)&0xFF), g1=(int)((c1>>8)&0xFF), b1=(int)(c1&0xFF);
        int r2=(int)((c2>>16)&0xFF), g2=(int)((c2>>8)&0xFF), b2=(int)(c2&0xFF);
        int r=r1+(r2-r1)*i/steps, g=g1+(g2-g1)*i/steps, b=b1+(b2-b1)*i/steps;
        unsigned int col = GFX_RGB(r, g, b);
        if (vertical) draw_hline(x, y+i, w, col);
        else          draw_vline(x+i, y, h, col);
    }
}

/* ── Graphics::blit_opaque ──────────────────────────────────────────────
   Opaque XRGB image blit: clips against screen AND the soft clip rect,
   then copies whole row spans (no per-pixel transparency test).           */
void Graphics::blit_opaque(int x, int y, int w, int h, const unsigned int* px) {
    if (!fb_ready || !px || w <= 0 || h <= 0) return;
    const int stride = w;                    /* source row length (dwords) */

    /* destination window [dx0,dx1) × [dy0,dy1): intersect target rect,
       screen, and the soft clip rect                                      */
    int dx0 = x, dy0 = y, dx1 = x + w, dy1 = y + h;
    if (dx0 < 0)       dx0 = 0;
    if (dy0 < 0)       dy0 = 0;
    if (dx0 < clip_x1) dx0 = clip_x1;
    if (dy0 < clip_y1) dy0 = clip_y1;
    if (dx1 > fb_w)    dx1 = fb_w;
    if (dy1 > fb_h)    dy1 = fb_h;
    if (dx1 > clip_x2) dx1 = clip_x2;
    if (dy1 > clip_y2) dy1 = clip_y2;
    if (dx0 >= dx1 || dy0 >= dy1) return;

    for (int dy = dy0; dy < dy1; ++dy) {
        const unsigned int* s = px + (long)(dy - y) * stride + (dx0 - x);
        unsigned int* d = back + (unsigned)dy * (unsigned)fb_w + (unsigned)dx0;
        for (int i = 0; i < dx1 - dx0; ++i) d[i] = s[i];
    }
}

/* ── Graphics::draw_image ───────────────────────────────────────────── */
void Graphics::draw_image(int x, int y, int w, int h, const unsigned int* pixels) {
    if (!fb_ready || !pixels) return;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            unsigned int c = pixels[row * w + col];
            if (c != 0xFFFFFFFFu)   /* 0xFFFFFFFF = transparent sentinel */
                put_pixel(x + col, y + row, c);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
   extern "C" wrappers — binary-compatible with the old gfx.c API.
   All draw calls route to s_back (the same back buffer used by the C++
   Graphics class). gfx_swap_buffers() presents the finished frame once.
   ══════════════════════════════════════════════════════════════════════ */
extern "C" {

/* Internal C-side state that mirrors what the C code sees */
static volatile unsigned int* c_fb   = 0;
static unsigned int  c_pitch_u32     = 0;
static int           c_w             = 0;
static int           c_h             = 0;
static int           c_ready         = 0;

void gfx_init(unsigned int fb_addr, unsigned int pitch_bytes,
              int width, int height, int bpp) {
    (void)bpp;
    c_fb        = (volatile unsigned int*)fb_addr;
    c_pitch_u32 = pitch_bytes / 4;
    c_w         = width;
    c_h         = height;
    c_ready     = 1;
    /* Also initialise the C++ Graphics instance */
    Graphics::instance().init(fb_addr, pitch_bytes, width, height);
}

int gfx_active()  { return c_ready; }
int gfx_width()   { return c_w; }
int gfx_height()  { return c_h; }

/* Copy Graphics back-buffer → VGA framebuffer */
void gfx_swap_buffers() { Graphics::instance().swap_buffers(); }

/* Back-buffer helpers — write to s_back, presented by gfx_swap_buffers() */
void gfx_put_pixel(int x, int y, unsigned int color) {
    if (!c_ready || x < 0 || y < 0 || x >= c_w || y >= c_h) return;
    s_back[(unsigned)y * (unsigned)c_w + (unsigned)x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, unsigned int color) {
    if (!c_ready) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c_w) w = c_w - x;
    if (y + h > c_h) h = c_h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        unsigned int* dst =
            s_back + (unsigned)(y + row) * (unsigned)c_w + (unsigned)x;
        for (int col = 0; col < w; col++) dst[col] = color;
    }
}

void gfx_draw_hline(int x, int y, int len, unsigned int color) {
    gfx_fill_rect(x, y, len, 1, color);
}
void gfx_draw_vline(int x, int y, int len, unsigned int color) {
    gfx_fill_rect(x, y, 1, len, color);
}
void gfx_draw_rect(int x, int y, int w, int h, unsigned int color) {
    gfx_draw_hline(x,     y,     w, color);
    gfx_draw_hline(x,     y+h-1, w, color);
    gfx_draw_vline(x,     y,     h, color);
    gfx_draw_vline(x+w-1, y,     h, color);
}

void gfx_draw_char(int x, int y, unsigned char c,
                   unsigned int fg, unsigned int bg) {
    if (!c_ready) return;
    const unsigned char* glyph = font8x16[(unsigned)c];
    int transp = (bg == 0xFFFFFFFFu);
    for (int row = 0; row < GFX_FONT_H; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < GFX_FONT_W; col++) {
            int set = (bits & (0x80u >> col)) != 0;
            if (set)          gfx_put_pixel(x + col, y + row, fg);
            else if (!transp) gfx_put_pixel(x + col, y + row, bg);
        }
    }
}

void gfx_draw_str(int x, int y, const char* s,
                  unsigned int fg, unsigned int bg) {
    int cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += GFX_FONT_H; }
        else { gfx_draw_char(cx, y, (unsigned char)*s, fg, bg); cx += GFX_FONT_W; }
        s++;
    }
}

void gfx_gradient_rect(int x, int y, int w, int h,
                        unsigned int c1, unsigned int c2, int vertical) {
    if (!c_ready || w <= 0 || h <= 0) return;
    int steps = vertical ? h : w;
    for (int i = 0; i < steps; i++) {
        int r1=(int)((c1>>16)&0xFF), g1=(int)((c1>>8)&0xFF), b1=(int)(c1&0xFF);
        int r2=(int)((c2>>16)&0xFF), g2=(int)((c2>>8)&0xFF), b2=(int)(c2&0xFF);
        int r=r1+(r2-r1)*i/steps, g=g1+(g2-g1)*i/steps, b=b1+(b2-b1)*i/steps;
        unsigned int col = GFX_RGB(r, g, b);
        if (vertical) gfx_draw_hline(x, y+i, w, col);
        else          gfx_draw_vline(x+i, y, h, col);
    }
}

/* ── gfx_draw_bmp_memory ─────────────────────────────────────────────────
   Decode a 24-bit uncompressed BMP from a raw memory pointer and blit it
   onto the back-buffer at (start_x, start_y).
   BMP stores rows bottom-up in BGR byte order; we flip Y and swap to RGB. */
void gfx_draw_bmp_memory(const unsigned char* data, int sx, int sy)
{
    if (!c_ready || !data) return;
    if (data[0] != 'B' || data[1] != 'M') return;

    unsigned int pix_off =  (unsigned int)data[0x0A]
                         | ((unsigned int)data[0x0B] <<  8)
                         | ((unsigned int)data[0x0C] << 16)
                         | ((unsigned int)data[0x0D] << 24);

    int bmp_w = (int)( (unsigned int)data[0x12]
                     | ((unsigned int)data[0x13] <<  8)
                     | ((unsigned int)data[0x14] << 16)
                     | ((unsigned int)data[0x15] << 24));

    int bmp_h = (int)( (unsigned int)data[0x16]
                     | ((unsigned int)data[0x17] <<  8)
                     | ((unsigned int)data[0x18] << 16)
                     | ((unsigned int)data[0x19] << 24));

    int bpp = (int)((unsigned int)data[0x1C] | ((unsigned int)data[0x1D] << 8));
    if (bpp != 24) return;

    /* Row stride is padded to a 4-byte boundary */
    int stride = (bmp_w * 3 + 3) & ~3;

    for (int row = 0; row < bmp_h; row++) {
        int dst_y = sy + row;
        if (dst_y < 0 || dst_y >= c_h) continue;

        /* BMP row 0 is the bottom screen row — flip */
        int bmp_row = bmp_h - 1 - row;
        const unsigned char* src =
            data + pix_off + (unsigned)bmp_row * (unsigned)stride;
        unsigned int* dst = s_back + (unsigned)dst_y * (unsigned)c_w;

        for (int col = 0; col < bmp_w; col++) {
            int dst_x = sx + col;
            if (dst_x < 0 || dst_x >= c_w) continue;
            unsigned int b = src[col * 3 + 0];
            unsigned int g = src[col * 3 + 1];
            unsigned int r = src[col * 3 + 2];
            dst[dst_x] = (r << 16) | (g << 8) | b;
        }
    }
}

} /* extern "C" */
