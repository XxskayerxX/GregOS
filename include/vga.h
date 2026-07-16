#ifndef VGA_H
#define VGA_H

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_COLOR_WHITE_ON_BLACK 0x0F

#ifdef __cplusplus
extern "C" {
#endif

extern int term_col;
extern int term_row;

void term_init(void);
void term_print(const char* str);
void term_putc(char c);
void term_set_color(unsigned char fg, unsigned char bg);
void term_move_cursor(int x, int y);


void term_set_scroll_region(int top, int bot);


void        term_capture_start(void);
const char* term_capture_end(void);


void sb_scroll_up(void);
void sb_scroll_down(void);
int  sb_is_active(void);
void sb_exit(void);

/* GUI mirror — when set, every character actually written to the VGA buffer
   is also forwarded to this callback so the GUI can maintain its own copy. */
void vga_set_gui_mirror(void (*fn)(char));
void vga_mirror_pause(int paused);

/* Shadow buffer — reliable software copy of the VGA text buffer for VBE blit. */
const unsigned short* vga_shadow_ptr(void);

/* Convert a 4-bit VGA palette index (0-15) to a 24-bit RGB color.
   Used by TerminalEmulator to map shell color codes to framebuffer values. */
unsigned int vga_color_to_rgb(unsigned char idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
