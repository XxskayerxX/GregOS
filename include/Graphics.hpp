#ifndef GRAPHICS_HPP
#define GRAPHICS_HPP

/* ── Graphics: framebuffer driver + double buffer ──────────────────────
   Freestanding C++11, no exceptions, no RTTI, no global ctors needed.
   Call init() once at boot. All draw_* methods write to the back buffer.
   The extern "C" gfx_* wrappers also write to the same back buffer.
   Call swap_buffers() (or gfx_swap_buffers()) once per frame to blit
   the finished back buffer to the VGA LFB via rep movsl.               */

class Graphics {
public:
    /* Call once when the VBE framebuffer is known */
    void init(unsigned int fb_addr, unsigned int pitch_bytes,
              int width, int height);

    /* Present: back buffer → VGA linear framebuffer */
    void swap_buffers();

    /* Partial present: copy one back-buffer rectangle to the front buffer.
       Used to restore the background under a moving software cursor without
       a full swap_buffers().  Rectangle is clipped to screen bounds.       */
    void present_rect(int x, int y, int w, int h);

    /* Write a single pixel straight to the FRONT (visible) framebuffer,
       bypassing the back buffer — for the software cursor overlay.         */
    void put_pixel_front(int x, int y, unsigned int color);

    /* CRT scanline pass — darken every other back-buffer row for a phosphor
       tube look. Applied to the finished frame just before swap_buffers().
       `shift` picks the intensity via a packed per-channel right-shift
       subtract (2 → −25%, 3 → −12.5%); default 2.                          */
    void apply_scanlines(int shift = 2);

    /* Full-screen back-buffer copy helpers (fb_w*fb_h dwords, tight rep movsl).
       Used to cache a static layer (e.g. the wallpaper) once and restore it
       each frame instead of recomputing it.                                 */
    void snapshot_back(unsigned int* dst) const;  /* back → dst */
    void blit_back(const unsigned int* src);       /* src  → back */

    /* Clear back buffer */
    void clear(unsigned int color = 0x000000);

    /* Clip rectangle — constrains all subsequent draw calls.
       set_clip() activates it; clear_clip() resets to full screen.
       Implemented as [x1,x2) × [y1,y2) half-open intervals.           */
    void set_clip(int x, int y, int w, int h) {
        clip_x1 = x; clip_y1 = y;
        clip_x2 = x + w; clip_y2 = y + h;
    }
    void clear_clip() {
        clip_x1 = 0; clip_y1 = 0;
        clip_x2 = fb_w; clip_y2 = fb_h;
    }

    /* Primitive writes to the BACK BUFFER */
    inline void put_pixel(int x, int y, unsigned int color) {
        if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h)
            return;
        if (x < clip_x1 || x >= clip_x2 || y < clip_y1 || y >= clip_y2)
            return;
        back[(unsigned)y * (unsigned)fb_w + (unsigned)x] = color;
    }
    void fill_rect(int x, int y, int w, int h, unsigned int color);
    void draw_hline(int x, int y, int len, unsigned int color);
    void draw_vline(int x, int y, int len, unsigned int color);
    void draw_rect(int x, int y, int w, int h, unsigned int color);
    void draw_char(int x, int y, unsigned char c,
                   unsigned int fg, unsigned int bg);
    void draw_str(int x, int y, const char* s,
                  unsigned int fg, unsigned int bg);
    void gradient_rect(int x, int y, int w, int h,
                       unsigned int c1, unsigned int c2, int vertical);

    /* Blit a decoded pixel array (XRGB32, top-down, w*h pixels).
       Pixels equal to GFX_TRANSPARENT (0xFFFFFFFF) are skipped.          */
    void draw_image(int x, int y, int w, int h, const unsigned int* pixels);

    bool active() const { return fb_ready; }
    int  width()  const { return fb_w; }
    int  height() const { return fb_h; }

    static Graphics& instance();

private:
    volatile unsigned int* fb;      /* VGA LFB (physical, mapped) */
    unsigned int*          back;    /* software back buffer        */
    unsigned int           pitch_u32;
    int                    fb_w;
    int                    fb_h;
    bool                   fb_ready;
    /* Clip rect — half-open [x1,x2)×[y1,y2); zero-init = no clip until init() */
    int                    clip_x1, clip_y1, clip_x2, clip_y2;
    /* no constructor: zero-init from BSS is sufficient */
};

/* Color helper (same formula as GFX_RGB macro in gfx.h) */
static inline unsigned int rgb(unsigned char r, unsigned char g,
                                unsigned char b) {
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

#endif /* GRAPHICS_HPP */
