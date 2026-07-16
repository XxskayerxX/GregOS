#include "../include/keyboard.h"
#include "../include/ports.h"
#include "../include/event.h"
#include "../include/tty.h"

#define KEY_UP     128
#define KEY_DOWN   129
#define KEY_LEFT   130
#define KEY_RIGHT  131
#define KEY_CTRL_S 132
#define KEY_CTRL_C 133
#define KEY_PGUP   134
#define KEY_PGDN   135
#define KEY_HOME   136
#define KEY_END    137
#define KEY_DELETE 138
#define KEY_INSERT 139
#define KEY_TAB    '\t'

static int shift_pressed = 0;
static int ctrl_pressed  = 0;
static int extended      = 0;

char keymap_lower[128] = {
    0, 0x1B, '&', 0xE9, '"', '\'', '(', '-', 0xE8, '_', 0xE7, 0xE0, ')', '=', '\b',
    '\t', 'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '^', '$', '\n',
    0, 'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 0xF9, '*',
    0, '<', 'w', 'x', 'c', 'v', 'b', 'n', ',', ';', ':', '!', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

char keymap_upper[128] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xF8, '+', '\b',
    '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xA8, 0xA3, '\n',
    0, 'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M', '%', 0xB5,
    0, '>', 'W', 'X', 'C', 'V', 'B', 'N', '?', '.', '/', 0xA7, 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};


/* Raw VGA debug markers — bypass GUI entirely, visible even on freeze */
#define VGA_DBG(pos, ch) \
    (((volatile unsigned short*)0xB8000)[(pos)] = (unsigned short)(0x4F00u | (unsigned char)(ch)))

#define KB_BUF_SIZE 64
static volatile unsigned char kb_buf[KB_BUF_SIZE];
static volatile unsigned char kb_head = 0;
static volatile unsigned char kb_tail = 0;

/* ── Injection buffer: processed ints from the WM event bridge ───────────
   Separate from kb_buf (which holds raw scancodes) so that pre-processed
   chars injected by TerminalWindow::handle_char() bypass process_scancode. */
#define KB_INJECT_SIZE 64
static volatile int           kb_inject_buf[KB_INJECT_SIZE];
static volatile unsigned char kb_inj_head = 0;
static volatile unsigned char kb_inj_tail = 0;

void kb_inject_flush(void) {
    kb_inj_head = kb_inj_tail = 0;
}

void kb_inject_char(int c) {
    unsigned char next = (kb_inj_head + 1) & (KB_INJECT_SIZE - 1);
    if (next != kb_inj_tail) {
        kb_inject_buf[kb_inj_head] = c;
        kb_inj_head = next;
    }
}

int kb_idt_active = 0;


void irq1_handler(void) {
    unsigned char sc = port_byte_in(0x60);
    if (is_gui_active) {
        /* GUI mode: route through EventQueue so WindowManager can dispatch
           (Ctrl+Q, window focus, TerminalWindow → kb_inject_char bridge). */
        event_push_key(sc);
    } else {
        unsigned char next = (kb_head + 1) & (KB_BUF_SIZE - 1);
        if (next != kb_tail) { kb_buf[kb_head] = sc; kb_head = next; }
    }
    port_byte_out(0x20, 0x20);
}


static int process_scancode(unsigned char scancode) {
    if (scancode == 0xE0) { extended = 1; return 0; }

    if (extended) {
        extended = 0;
        if (scancode == 0x48) return KEY_UP;
        if (scancode == 0x50) return KEY_DOWN;
        if (scancode == 0x4B) return KEY_LEFT;
        if (scancode == 0x4D) return KEY_RIGHT;
        if (scancode == 0x49) return KEY_PGUP;
        if (scancode == 0x51) return KEY_PGDN;
        if (scancode == 0x47) return KEY_HOME;
        if (scancode == 0x4F) return KEY_END;
        if (scancode == 0x53) return KEY_DELETE;
        if (scancode == 0x52) return KEY_INSERT;
        if (scancode == 0x1D) { ctrl_pressed = 1; return 0; }
        if (scancode == 0x9D) { ctrl_pressed = 0; return 0; }
        return 0;
    }

    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return 0; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return 0; }
    if (scancode == 0x1D) { ctrl_pressed = 1; return 0; }
    if (scancode == 0x9D) { ctrl_pressed = 0; return 0; }

    if (scancode & 0x80) return 0;
    if (scancode > 127)  return 0;

    char c = shift_pressed ? keymap_upper[scancode] : keymap_lower[scancode];

    if (ctrl_pressed) {
        if (c == 's' || c == 'S') return KEY_CTRL_S;
        if (c == 'c' || c == 'C') return KEY_CTRL_C;
        return 0;
    }

    return c;
}

int kb_scancode_to_char(unsigned char sc) {
    return process_scancode(sc);
}

int get_monitor_char(void) {

    if (is_gui_active) {
        /* GUI mode: pump the EventQueue so WindowManager can route keyboard
           events through dispatch_event → route_key → TerminalWindow::handle_char
           → kb_inject_char. Then return from the injection buffer (pre-processed
           chars, no second pass through process_scancode).
           Semi-non-blocking: returns 0 after hlt so the outer shell loop can
           still poll the mouse and call wm_draw() for cursor blink. */
        wm_pump_events();
        if (kb_inj_head != kb_inj_tail) {
            int c = kb_inject_buf[kb_inj_tail];
            kb_inj_tail = (kb_inj_tail + 1) & (KB_INJECT_SIZE - 1);
            return c;
        }
        __asm__ volatile("hlt");
        return 0;
    }

    /* Non-GUI mode: raw scancode path (original behaviour). */
    if (kb_head != kb_tail) {
        unsigned char sc = kb_buf[kb_tail];
        kb_tail = (kb_tail + 1) & (KB_BUF_SIZE - 1);
        return process_scancode(sc);
    }

    if (kb_idt_active) return 0;
    unsigned char status = port_byte_in(0x64);
    if (!(status & 0x01)) return 0;
    return process_scancode(port_byte_in(0x60));
}
