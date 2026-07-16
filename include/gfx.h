#ifndef GFX_H
#define GFX_H

#define GFX_FONT_W 8
#define GFX_FONT_H 16

#define GFX_RGB(r,g,b) \
    (((unsigned int)(r) << 16) | ((unsigned int)(g) << 8) | (unsigned int)(b))

/* Pass as `bg` to draw_char / draw_str to skip drawing background pixels */
#define GFX_TRANSPARENT 0xFFFFFFFFu

#ifdef __cplusplus
extern "C" {
#endif

void gfx_init(unsigned int fb_addr, unsigned int pitch,
              int width, int height, int bpp);
int  gfx_active(void);
int  gfx_width(void);
int  gfx_height(void);

void gfx_put_pixel(int x, int y, unsigned int color);
void gfx_fill_rect(int x, int y, int w, int h, unsigned int color);
void gfx_draw_hline(int x, int y, int len, unsigned int color);
void gfx_draw_vline(int x, int y, int len, unsigned int color);
void gfx_draw_rect(int x, int y, int w, int h, unsigned int color);

void gfx_draw_char(int x, int y, unsigned char c,
                   unsigned int fg, unsigned int bg);
void gfx_draw_str(int x, int y, const char *s,
                  unsigned int fg, unsigned int bg);

void gfx_gradient_rect(int x, int y, int w, int h,
                        unsigned int c1, unsigned int c2, int vertical);

/* Copy Graphics back-buffer → VGA framebuffer (opt-in double buffering). */
void gfx_swap_buffers(void);

/* Decode a 24-bit uncompressed BMP from a memory pointer and blit it
   onto the back-buffer at (start_x, start_y). Bottom-up / BGR handled. */
void gfx_draw_bmp_memory(const unsigned char* data, int start_x, int start_y);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GFX_H */
