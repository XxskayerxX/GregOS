#include "../include/vga.h"
#include "../include/tty.h"

volatile unsigned short* vga_buffer = (unsigned short*)0xB8000;

static unsigned short vga_shadow[VGA_HEIGHT * VGA_WIDTH];

static inline void vga_put(int idx, unsigned short v) {
    vga_buffer[idx] = v;
    vga_shadow[idx] = v;
}

const unsigned short* vga_shadow_ptr(void) { return vga_shadow; }

int term_col = 0;
int term_row = 0;
unsigned char terminal_color = 0x0F;

static int scroll_top = 0;
static int scroll_bot = VGA_HEIGHT - 1;

void term_set_scroll_region(int top, int bot) {
    if (top < 0)            top = 0;
    if (bot >= VGA_HEIGHT)  bot = VGA_HEIGHT - 1;
    if (top >= bot)         return;
    scroll_top = top;
    scroll_bot = bot;
}


#define SB_LINES 200
static unsigned short sb_buf[SB_LINES][VGA_WIDTH];
static int sb_head   = 0;

static int sb_count  = 0;

static int sb_offset = 0;


static unsigned short sb_saved[VGA_HEIGHT][VGA_WIDTH];
static int sb_saved_col, sb_saved_row;

static void sb_save_scrolled_line(int row) {
    for (int x = 0; x < VGA_WIDTH; x++)
        sb_buf[sb_head][x] = vga_buffer[row * VGA_WIDTH + x];
    sb_head = (sb_head + 1) % SB_LINES;
    if (sb_count < SB_LINES) sb_count++;
}

static void sb_redraw(void) {
    int visible = scroll_bot - scroll_top + 1;
    int total   = sb_count + visible;

    for (int r = 0; r < visible; r++) {


        int vline = total - sb_offset - visible + r;
        int row   = scroll_top + r;

        if (vline < 0) {


            for (int x = 0; x < VGA_WIDTH; x++)
                vga_put(row * VGA_WIDTH + x, (unsigned short)' ' | (0x07 << 8));
        } else if (vline < sb_count) {


            int bi = (sb_head - sb_count + vline + SB_LINES * 4) % SB_LINES;
            for (int x = 0; x < VGA_WIDTH; x++)
                vga_put(row * VGA_WIDTH + x, sb_buf[bi][x]);
        } else {




            int saved_row = scroll_top + (vline - sb_count);
            for (int x = 0; x < VGA_WIDTH; x++)
                vga_put(row * VGA_WIDTH + x, sb_saved[saved_row][x]);
        }
    }



    const char* msg = "  [SCROLLBACK]  PgUp = page haut  |  PgDn = page bas  |  autre touche = quitter  ";
    unsigned short color = (unsigned short)(0x4E << 8);

    for (int x = 0; x < VGA_WIDTH; x++)
        vga_put(24 * VGA_WIDTH + x, (unsigned short)' ' | color);
    int len = 0; while (msg[len]) len++;
    int sx = (VGA_WIDTH - len) / 2;
    if (sx < 0) sx = 0;
    for (int i = 0; msg[i] && sx + i < VGA_WIDTH; i++)
        vga_put(24 * VGA_WIDTH + sx + i, (unsigned short)(unsigned char)msg[i] | color);
}

void sb_scroll_up(void) {
    int visible = scroll_bot - scroll_top + 1;

    if (sb_offset == 0) {
        if (sb_count == 0) return;



        for (int y = 0; y < VGA_HEIGHT; y++)
            for (int x = 0; x < VGA_WIDTH; x++)
                sb_saved[y][x] = vga_buffer[y * VGA_WIDTH + x];
        sb_saved_col = term_col;
        sb_saved_row = term_row;
    }



    sb_offset += visible;



    int max_offset = sb_count + visible - 1;
    if (sb_offset > max_offset) sb_offset = max_offset;

    sb_redraw();
}

void sb_scroll_down(void) {
    if (sb_offset == 0) return;
    int visible = scroll_bot - scroll_top + 1;
    sb_offset -= visible;
    if (sb_offset <= 0) {
        sb_exit();
        return;
    }
    sb_redraw();
}

void sb_exit(void) {
    if (sb_offset == 0) return;
    sb_offset = 0;


    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_put(y * VGA_WIDTH + x, sb_saved[y][x]);
    term_col = sb_saved_col;
    term_row = sb_saved_row;
}

int sb_is_active(void) { return sb_offset > 0; }


static char capture_buf[4096];
static int  capture_pos      = 0;
static int  capture_mode     = 0;
static int  capture_save_col = 0;
static int  capture_save_row = 0;

void term_capture_start(void) {
    capture_buf[0] = '\0';
    capture_pos      = 0;
    capture_mode     = 1;
    capture_save_col = term_col;
    capture_save_row = term_row;
}

const char* term_capture_end(void) {
    capture_mode = 0;
    if (capture_pos < 4096) capture_buf[capture_pos] = '\0';
    else                    capture_buf[4095] = '\0';
    term_col = capture_save_col;
    term_row = capture_save_row;
    return capture_buf;
}


#define ANSI_STATE_NORMAL  0
#define ANSI_STATE_ESC     1
#define ANSI_STATE_PARAM   2

#define MAX_ANSI_PARAMS 8
#define ANSI_PARAM_MAX  999

int ansi_state       = ANSI_STATE_NORMAL;
int ansi_params[MAX_ANSI_PARAMS];
int ansi_param_count = 0;
int ansi_buf_val     = 0;

static void (*s_gui_mirror)(char) = 0;
static int   s_mirror_paused     = 0;

void vga_set_gui_mirror(void (*fn)(char)) { s_gui_mirror = fn; }
void vga_mirror_pause(int p)              { s_mirror_paused = p; }

#define GUI_MIRROR(c) \
    do { if (s_gui_mirror && !s_mirror_paused && !capture_mode) s_gui_mirror(c); } while(0)

/* Standard CGA/VGA 16-color → 24-bit RGB mapping */
static const unsigned int vga_palette[16] = {
    0x000000u, 0x0000AAu, 0x00AA00u, 0x00AAAAu,
    0xAA0000u, 0xAA00AAu, 0xAA5500u, 0xAAAAAAu,
    0x555555u, 0x5555FFu, 0x55FF55u, 0x55FFFFu,
    0xFF5555u, 0xFF55FFu, 0xFFFF55u, 0xFFFFFFu,
};

unsigned int vga_color_to_rgb(unsigned char idx) {
    return vga_palette[idx & 0x0Fu];
}

void term_set_color(unsigned char fg, unsigned char bg) {
    terminal_color = (bg << 4) | (fg & 0x0F);
    if (tty_set_color_hook) tty_set_color_hook(fg, bg);
}

void term_init(void) {
    term_col   = 0;
    term_row   = 0;
    scroll_top = 0;
    scroll_bot = VGA_HEIGHT - 1;
    sb_offset  = 0;
    sb_head    = 0;
    sb_count   = 0;
    ansi_state = ANSI_STATE_NORMAL;
    term_set_color(0x0F, 0x00);
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_put(y * VGA_WIDTH + x, (unsigned short)' ' | (terminal_color << 8));
}

void term_scroll(void) {


    sb_save_scrolled_line(scroll_top);

    for (int y = scroll_top + 1; y <= scroll_bot; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            vga_put((y-1) * VGA_WIDTH + x, vga_shadow[y * VGA_WIDTH + x]);
    for (int x = 0; x < VGA_WIDTH; x++)
        vga_put(scroll_bot * VGA_WIDTH + x,
            (unsigned short)' ' | (terminal_color << 8));
    term_row = scroll_bot;
    term_col = 0;
}

static void ansi_apply_params(void) {
    if (ansi_param_count < MAX_ANSI_PARAMS)
        ansi_params[ansi_param_count++] = ansi_buf_val;

    for (int i = 0; i < ansi_param_count; i++) {
        int p = ansi_params[i];
        unsigned char fg = terminal_color & 0x0F;
        unsigned char bg = (terminal_color >> 4) & 0x0F;

        if (p == 0) {
            term_set_color(0x0F, 0x00);
        } else if (p == 1) {
            term_set_color(fg | 0x08, bg);
        } else if (p >= 30 && p <= 37) {
            static const unsigned char fg_map[8] = {
                0x00, 0x04, 0x02, 0x0E, 0x01, 0x05, 0x03, 0x0F,
            };
            term_set_color(fg_map[p - 30], bg);
        } else if (p >= 40 && p <= 47) {
            static const unsigned char bg_map[8] = {
                0x00, 0x04, 0x02, 0x06, 0x01, 0x05, 0x03, 0x07,
            };
            term_set_color(fg, bg_map[p - 40]);
        } else if (p >= 90 && p <= 97) {
            static const unsigned char bfg_map[8] = {
                0x08, 0x0C, 0x0A, 0x0E, 0x09, 0x0D, 0x0B, 0x0F,
            };
            term_set_color(bfg_map[p - 90], bg);
        } else if (p >= 100 && p <= 107) {
            static const unsigned char bbg_map[8] = {
                0x08, 0x0C, 0x0A, 0x0E, 0x09, 0x0D, 0x0B, 0x0F,
            };
            term_set_color(fg, bbg_map[p - 100]);
        }
    }
}

void term_putc(char c) {
    /* Route to active TerminalEmulator when the hook is installed.
       tty_putc_hook is declared in tty.h (included at top of file). */
    if (tty_putc_hook) { tty_putc_hook(c); return; }

    if (ansi_state == ANSI_STATE_NORMAL) {
        if ((unsigned char)c == 0x1B) {
            ansi_state = ANSI_STATE_ESC;
            return;
        }
    } else if (ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            ansi_state       = ANSI_STATE_PARAM;
            ansi_buf_val     = 0;
            ansi_param_count = 0;
            return;
        }
        ansi_state = ANSI_STATE_NORMAL;
    } else if (ansi_state == ANSI_STATE_PARAM) {
        if (c >= '0' && c <= '9') {
            if (ansi_buf_val <= ANSI_PARAM_MAX)
                ansi_buf_val = ansi_buf_val * 10 + (c - '0');
            return;
        } else if (c == ';') {
            if (ansi_param_count < MAX_ANSI_PARAMS)
                ansi_params[ansi_param_count++] = ansi_buf_val;
            ansi_buf_val = 0;
            return;
        } else if (c == 'm') {
            ansi_apply_params();
            ansi_state = ANSI_STATE_NORMAL;
            return;
        } else {
            ansi_state = ANSI_STATE_NORMAL;
            return;
        }
    }



    if (c == '\b') {
        if (capture_mode) { if (capture_pos > 0) capture_pos--; return; }
        if (term_col > 0) {
            term_col--;
            vga_put(term_row * VGA_WIDTH + term_col,
                (unsigned short)' ' | (unsigned short)(terminal_color << 8));
            GUI_MIRROR('\b');
        }
        return;
    }

    if (c == '\n') {
        if (capture_mode) {
            if (capture_pos < 4095) capture_buf[capture_pos++] = '\n';
            return;
        }
        term_col = 0;
        term_row++;
        GUI_MIRROR('\n');
    } else {
        if (capture_mode) {
            if (c >= 32 && c <= 126 && capture_pos < 4095)
                capture_buf[capture_pos++] = c;
            return;
        }
        if (term_col >= VGA_WIDTH) { term_col = 0; term_row++; }
        if (term_row > scroll_bot)  { term_scroll(); }
        vga_put(term_row * VGA_WIDTH + term_col,
            (unsigned short)(unsigned char)c | (unsigned short)(terminal_color << 8));
        term_col++;
        GUI_MIRROR(c);
    }

    if (term_col >= VGA_WIDTH) {
        term_col = 0;
        term_row++;
    }
    if (term_row > scroll_bot)
        term_scroll();
}

void term_print(const char* str) {
    for (int i = 0; str[i]; i++)
        term_putc(str[i]);
}

void term_move_cursor(int x, int y) {
    if (capture_mode) return;
    if (x < 0) x = 0;
    if (x >= VGA_WIDTH)  x = VGA_WIDTH  - 1;
    if (y < 0) y = 0;
    if (y >= VGA_HEIGHT) y = VGA_HEIGHT - 1;
    term_col = x;
    term_row = y;
    if (tty_move_cursor_hook) tty_move_cursor_hook(x, y);
}
