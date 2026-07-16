/* kernel/PS2Keyboard.cpp — Kernel::PS2Keyboard implementation.
   Freestanding: no libc, no exceptions.
   Migrated from drivers/keyboard.c. */

#include "../include/Kernel/PS2Keyboard.hpp"
#include "../include/keyboard.h"
#include "../include/ports.h"
#include "../include/event.h"
#include "../include/tty.h"

/* Forward-declare the C bridge variable so get_char() can reference it
   before the definition at the bottom of this file.                    */
extern "C" int kb_idt_active;

namespace Kernel {

/* ── Keymap tables (French AZERTY, PS/2 scancode set 1) ─────────────── */

static const unsigned char keymap_lower[128] = {
    0, 0x1B, '&', 0xE9, '"', '\'', '(', '-', 0xE8, '_', 0xE7, 0xE0, ')', '=', '\b',
    '\t', 'a', 'z', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '^', '$', '\n',
    0, 'q', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 0xF9, '*',
    0, '<', 'w', 'x', 'c', 'v', 'b', 'n', ',', ';', ':', '!', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char keymap_upper[128] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xF8, '+', '\b',
    '\t', 'A', 'Z', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xA8, 0xA3, '\n',
    0, 'Q', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'M', '%', 0xB5,
    0, '>', 'W', 'X', 'C', 'V', 'B', 'N', '?', '.', '/', 0xA7, 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* ── State ───────────────────────────────────────────────────────────── */

/* Ring buffer sizes as typed constants (avoids #define namespace pollution). */
static constexpr unsigned KB_BUF_SIZE = 64;
static constexpr unsigned KB_INJ_SIZE = 64;

/* Modifier state — written by process_scancode(), read by same function.
   process_scancode() is only called from the main thread (get_char and
   scancode_to_char), never directly from the IRQ handler. volatile is added
   as a defensive measure against -O2 caching across call boundaries.      */
static volatile bool s_shift    = false;
static volatile bool s_ctrl     = false;
static volatile bool s_extended = false;

/* Raw ring buffer — used in non-GUI (shell) mode.
   Head written by IRQ handler, tail read/written by main thread. */
static volatile unsigned char s_kb_buf[KB_BUF_SIZE];
static volatile unsigned char s_kb_head = 0;
static volatile unsigned char s_kb_tail = 0;

/* Injection buffer — pre-processed chars from TerminalWindow::handle_char().
   Head written by main thread (inject_char), tail read by main thread (get_char). */
static volatile int           s_inj_buf[KB_INJ_SIZE];
static volatile unsigned char s_inj_head = 0;
static volatile unsigned char s_inj_tail = 0;

/* ── Private: scancode → character ──────────────────────────────────── */

int PS2Keyboard::process_scancode(unsigned char sc)
{
    if (sc == 0xE0) { s_extended = true; return 0; }

    if (s_extended) {
        s_extended = false;
        if (sc == 0x48) return KEY_UP;
        if (sc == 0x50) return KEY_DOWN;
        if (sc == 0x4B) return KEY_LEFT;
        if (sc == 0x4D) return KEY_RIGHT;
        if (sc == 0x49) return KEY_PGUP;
        if (sc == 0x51) return KEY_PGDN;
        if (sc == 0x47) return KEY_HOME;
        if (sc == 0x4F) return KEY_END;
        if (sc == 0x53) return KEY_DELETE;
        if (sc == 0x52) return KEY_INSERT;
        if (sc == 0x1D) { s_ctrl = true;  return 0; }
        if (sc == 0x9D) { s_ctrl = false; return 0; }
        return 0;
    }

    if (sc == 0x2A || sc == 0x36) { s_shift = true;  return 0; }
    if (sc == 0xAA || sc == 0xB6) { s_shift = false; return 0; }
    if (sc == 0x1D) { s_ctrl = true;  return 0; }
    if (sc == 0x9D) { s_ctrl = false; return 0; }

    /* Bit 7 set = key release; scancodes 0–127 only. One check suffices. */
    if (sc & 0x80) return 0;

    unsigned char c = s_shift ? keymap_upper[sc] : keymap_lower[sc];

    if (s_ctrl) {
        if (c == 's' || c == 'S') return KEY_CTRL_S;
        if (c == 'c' || c == 'C') return KEY_CTRL_C;
        return 0;
    }

    return (int)c;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void PS2Keyboard::handle_irq()
{
    unsigned char sc = port_byte_in(0x60);
    if (is_gui_active) {
        event_push_key(sc);
    } else {
        unsigned char next = (unsigned char)((s_kb_head + 1) & (KB_BUF_SIZE - 1));
        if (next != s_kb_tail) {
            s_kb_buf[s_kb_head] = sc;
            s_kb_head = next;
        }
    }
    port_byte_out(0x20, 0x20);   /* EOI to master PIC */
}

int PS2Keyboard::get_char()
{
    if (is_gui_active) {
        wm_pump_events();
        if (s_inj_head != s_inj_tail) {
            int c = s_inj_buf[s_inj_tail];
            s_inj_tail = (unsigned char)((s_inj_tail + 1) & (KB_INJ_SIZE - 1));
            return c;
        }
        __asm__ volatile("hlt");
        return 0;
    }

    if (s_kb_head != s_kb_tail) {
        unsigned char sc = s_kb_buf[s_kb_tail];
        s_kb_tail = (unsigned char)((s_kb_tail + 1) & (KB_BUF_SIZE - 1));
        return process_scancode(sc);
    }

    /* Fallback: poll the 8042 output buffer directly.
       Only used before IDT is active (kb_idt_active == 0).
       Uses kb_idt_active (the C bridge variable set by kernel.c) — NOT a
       separate class member, so kernel.c's "kb_idt_active = 1" is honoured. */
    if (kb_idt_active) return 0;
    unsigned char status = port_byte_in(0x64);
    if (!(status & 0x01)) return 0;
    return process_scancode(port_byte_in(0x60));
}

void PS2Keyboard::inject_char(int c)
{
    unsigned char next = (unsigned char)((s_inj_head + 1) & (KB_INJ_SIZE - 1));
    if (next != s_inj_tail) {
        s_inj_buf[s_inj_head] = c;
        s_inj_head = next;
    }
}

void PS2Keyboard::inject_flush()
{
    s_inj_head = s_inj_tail = 0;
}

int PS2Keyboard::scancode_to_char(unsigned char sc)
{
    return process_scancode(sc);
}

} /* namespace Kernel */

/* ── C bridges — match keyboard.h exactly ───────────────────────────── */

extern "C" { int kb_idt_active = 0; }

extern "C" void irq1_handler(void)          { Kernel::PS2Keyboard::handle_irq(); }
extern "C" int  get_monitor_char(void)      { return Kernel::PS2Keyboard::get_char(); }
extern "C" void kb_inject_char(int c)       { Kernel::PS2Keyboard::inject_char(c); }
extern "C" void kb_inject_flush(void)       { Kernel::PS2Keyboard::inject_flush(); }
extern "C" int  kb_scancode_to_char(unsigned char sc)
                                            { return Kernel::PS2Keyboard::scancode_to_char(sc); }
