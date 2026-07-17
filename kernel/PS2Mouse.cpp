/* kernel/PS2Mouse.cpp — Kernel::PS2Mouse implementation.
   Freestanding: no libc, no exceptions.
   Migrated from kernel/kernel.c (irq12_handler + mouse_init block). */

#include "../include/Kernel/PS2Mouse.hpp"
#include "../include/Kernel/ps2mouse_c.h"
#include "../include/ports.h"
#include "../include/gfx.h"
#include "../include/event.h"   /* declares is_gui_active */

namespace Kernel {

/* ── State ───────────────────────────────────────────────────────────── */

/* Text-mode (80×25) cursor position and saved cell. */
static int           s_col       = 40;
static int           s_row       = 12;
static unsigned char s_prev_char = ' ';
static unsigned char s_prev_attr = 0x07;

/* 3-byte PS/2 packet accumulator — only accessed from handle_irq() (IRQ context). */
static volatile int           s_pkt_byte = 0;
static volatile unsigned char s_packet[3];

/* Enabled flag — written by initialize() (main thread),
   read by erase()/draw() which are called from both IRQ and main thread. */
static volatile bool s_enabled = false;

/* Pixel-space position (800×600 clipped).
   Written by handle_irq() (IRQ context), read by gui_x/y/buttons() (main thread).
   volatile prevents the compiler caching these across interrupt boundaries.   */
static volatile int s_gui_x   = 400;
static volatile int s_gui_y   = 300;
static volatile int s_buttons = 0;

/* Text-mode row of the desktop header — cursor cannot go above this. */
static constexpr int HEADER_ROWS = 3;

/* ── Private helpers ─────────────────────────────────────────────────── */

/* Returns true if the controller accepted the write within the timeout. */
bool PS2Mouse::ps2_wait_write()
{
    int t = 100000;
    while (t-- > 0) {
        if (!(port_byte_in(0x64) & 0x02)) return true;
    }
    return false;   /* timeout — controller unresponsive */
}

/* Returns true if the output buffer has data within the timeout. */
bool PS2Mouse::ps2_wait_read()
{
    int t = 100000;
    while (t-- > 0) {
        if (port_byte_in(0x64) & 0x01) return true;
    }
    return false;   /* timeout — no data arrived */
}

void PS2Mouse::erase()
{
    if (!s_enabled) return;
    volatile unsigned short* vga = (volatile unsigned short*)0xB8000;
    vga[s_row * 80 + s_col] =
        ((unsigned short)s_prev_attr << 8) | s_prev_char;
}

void PS2Mouse::draw()
{
    if (!s_enabled) return;
    volatile unsigned short* vga = (volatile unsigned short*)0xB8000;
    unsigned short cell = vga[s_row * 80 + s_col];
    s_prev_char = (unsigned char)(cell & 0xFF);
    s_prev_attr = (unsigned char)((cell >> 8) & 0xFF);
    vga[s_row * 80 + s_col] = (unsigned short)((0x4F << 8) | 0x10);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void PS2Mouse::initialize()
{
    /* Every wait here is BEST-EFFORT, never fatal. An earlier version bailed
       out (`return`) the instant any `ps2_wait_write()` timed out — which on a
       real controller (or a stricter emulator than QEMU) abandoned the whole
       sequence and, crucially, left IRQ12 STILL MASKED: the mouse was dead and
       the cursor never moved, while under QEMU (where the waits always succeed)
       it looked fine. We now push through every step and ALWAYS reach the
       enable-reporting + unmask-IRQ12 tail, so a slow controller still ends up
       live.                                                                    */

    ps2_wait_write();
    port_byte_out(0x64, 0xA8);              /* enable aux port              */

    ps2_wait_write();
    port_byte_out(0x64, 0x20);              /* read config byte command     */
    ps2_wait_read();                        /* best-effort wait (non-fatal) */
    unsigned char cfg = port_byte_in(0x60);
    cfg |= 0x02;                            /* enable IRQ12                 */
    cfg &= (unsigned char)~0x20;            /* enable aux clock             */

    ps2_wait_write();
    port_byte_out(0x64, 0x60);              /* write config byte command    */
    ps2_wait_write();
    port_byte_out(0x60, cfg);

    ps2_wait_write();
    port_byte_out(0x64, 0xD4);              /* route next byte to mouse     */
    ps2_wait_write();
    port_byte_out(0x60, 0xF4);              /* enable reporting             */
    ps2_wait_read();                        /* best-effort wait for ack     */

    /* Drain the ACK and ANY residual bytes so the first movement packet is
       byte-aligned. A single leftover byte here offsets the 3-byte packet
       framing; the self-resync in handle_irq recovers from it, but starting
       clean avoids the first few packets being dropped.                     */
    for (int i = 0; i < 64; ++i) {
        if (!(port_byte_in(0x64) & 0x01)) break;   /* output buffer empty   */
        (void)port_byte_in(0x60);
    }
    s_pkt_byte = 0;                          /* clean packet framing         */

    unsigned char slave_mask = port_byte_in(0xA1);
    port_byte_out(0xA1, slave_mask & (unsigned char)~0x10); /* unmask IRQ12 */

    s_enabled = true;
    draw();
}

extern "C" {
    /* Lightweight diagnostic counters (read by the on-screen mouse readout).
       They cost nothing and let us see, on the *user's* machine, exactly where
       the pipeline breaks: irq==0 => IRQ12 never fires (init/PIC/no PS/2 mouse);
       irq>0 & pkt==0 => bytes arrive but no packet completes (framing);
       pkt>0 but cursor still => event/compose problem; auxclr climbing with irq
       => this controller does NOT set the AUX status bit (bit 5).             */
    volatile unsigned long g_ms_irq    = 0;
    volatile unsigned long g_ms_pkt    = 0;
    volatile unsigned long g_ms_auxclr = 0;
}

/* ── Fullscreen-game mouselook capture ───────────────────────────────────
   When capture is on, the IRQ accumulates the raw (pre-acceleration) relative
   delta and skips both the GUI event push and the text-mode cursor draw, so
   aiming never stalls at a screen edge and nothing is painted over the game. */
static volatile bool s_capture = false;
static volatile int  s_rel_dx  = 0;
static volatile int  s_rel_dy  = 0;

void PS2Mouse::handle_irq()
{
    ++g_ms_irq;
    /* Inline EOI so each exit path can call it without goto. */
    auto eoi = []() {
        port_byte_out(0xA0, 0x20);   /* EOI to slave PIC  */
        port_byte_out(0x20, 0x20);   /* EOI to master PIC */
    };

    /* The i8042 shares ONE output buffer (port 0x60) between keyboard and mouse.
       If OBF (status bit 0) is clear there is genuinely no byte to read, so a
       read would return stale data and desync the parser — skip it. We do NOT,
       however, gate on the AUX bit (status bit 5): IRQ12 firing already means
       the controller has a MOUSE byte queued, and some controllers/emulators
       don't set bit 5 reliably — discarding the byte on a false-negative there
       would starve the parser and freeze the cursor for good. We only COUNT the
       AUX-clear case for diagnosis and always feed the byte to the parser.     */
    unsigned char status = port_byte_in(0x64);
    if (!(status & 0x01)) { eoi(); return; }        /* no data present: skip   */
    if (!(status & 0x20)) ++g_ms_auxclr;            /* diagnostic only          */

    unsigned char data = port_byte_in(0x60);

    /* The first byte of a PS/2 packet ALWAYS has bit 3 set — that is the only
       reliable framing signal. If we're at byte 0 and bit 3 is clear, this is
       not a valid packet start: drop it and stay at byte 0 so the parser
       self-resynchronises after any framing offset (e.g. a stray byte at init).

       Do NOT also reject on the overflow bits (0xC0): on real hardware a fast
       flick produces a legitimate packet whose byte 0 HAS an overflow bit set.
       Rejecting it here would drop that valid byte 0 and then treat its data
       bytes (dx/dy) as byte-0 candidates — one of which may have bit 3 set and
       so gets mis-locked, re-desyncing the stream permanently (cursor freezes
       after a fast move). Overflow packets are instead dropped intact at
       completion below, which keeps the 3-byte framing aligned.              */
    if (s_pkt_byte == 0 && !(data & 0x08)) { eoi(); return; }

    s_packet[s_pkt_byte++] = data;
    if (s_pkt_byte < 3) { eoi(); return; }

    s_pkt_byte = 0;   /* framing advances by exactly 3 → stays aligned         */

    /* Overflow packet (fast flick): the delta doesn't fit 8 bits and is
       unreliable, so drop the movement — but the framing was already reset
       above, so the stream stays perfectly aligned for the next packet.       */
    if (s_packet[0] & 0xC0) { eoi(); return; }

    ++g_ms_pkt;

    int dx = (int)(unsigned char)s_packet[1];
    int dy = (int)(unsigned char)s_packet[2];
    if (s_packet[0] & 0x10) dx |= (int)0xFFFFFF00;   /* sign-extend X */
    if (s_packet[0] & 0x20) dy |= (int)0xFFFFFF00;   /* sign-extend Y */

    if (s_capture) {                                 /* fullscreen mouselook  */
        s_rel_dx += dx;
        s_rel_dy += dy;
        s_buttons = s_packet[0] & 0x07;
        eoi();
        return;
    }

    /* Pointer acceleration: slow, deliberate motions stay 1:1 for precision;
       fast flicks are multiplied so the cursor crosses the screen without
       dragging the mouse across the whole desk.                             */
    {
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        int a   = adx > ady ? adx : ady;
        int mul = (a >= 6) ? 2 : 1;
        dx *= mul;
        dy *= mul;
    }

    /* Pixel-space clamp */
    s_gui_x += dx;
    s_gui_y -= dy;
    if (s_gui_x < 0) s_gui_x = 0;
    { int W = gfx_width();  if (W > 0 && s_gui_x >= W) s_gui_x = W - 1; }
    if (s_gui_y < 0) s_gui_y = 0;
    { int H = gfx_height(); if (H > 0 && s_gui_y >= H) s_gui_y = H - 1; }

    s_buttons = s_packet[0] & 0x07;

    if (is_gui_active) {
        event_push_mouse(s_gui_x, s_gui_y, dx / 3, dy / 3,
                         (unsigned char)s_buttons);
    } else {
        /* Text-mode cursor update */
        erase();
        s_col += dx / 3;
        s_row -= dy / 3;
        if (s_col < 0)           s_col = 0;
        if (s_col > 79)          s_col = 79;
        if (s_row < HEADER_ROWS) s_row = HEADER_ROWS;
        if (s_row > 23)          s_row = 23;
        draw();
    }

    eoi();
}

int PS2Mouse::gui_x()   { return s_gui_x; }
int PS2Mouse::gui_y()   { return s_gui_y; }
int PS2Mouse::buttons() { return s_buttons; }

static void game_mouse_capture(bool on)
{
    s_capture = on;
    s_rel_dx  = 0;
    s_rel_dy  = 0;
}

static void game_mouse_take_rel(int* dx, int* dy)
{
    unsigned long fl;                             /* take + reset atomically */
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    int x = s_rel_dx, y = s_rel_dy;
    s_rel_dx = 0; s_rel_dy = 0;
    __asm__ volatile("push %0; popf" :: "r"(fl) : "cc", "memory");
    if (dx) *dx = x;
    if (dy) *dy = y;
}

} /* namespace Kernel */

/* ── C bridges ───────────────────────────────────────────────────────── */

extern "C" void irq12_handler(void)        { Kernel::PS2Mouse::handle_irq(); }
extern "C" void ps2mouse_init(void)        { Kernel::PS2Mouse::initialize(); }
extern "C" int  ps2mouse_gui_x(void)       { return Kernel::PS2Mouse::gui_x(); }
extern "C" int  ps2mouse_gui_y(void)       { return Kernel::PS2Mouse::gui_y(); }
extern "C" int  ps2mouse_buttons(void)     { return Kernel::PS2Mouse::buttons(); }
extern "C" void ps2mouse_cursor_show(void) { Kernel::PS2Mouse::draw(); }
extern "C" void ps2mouse_cursor_hide(void) { Kernel::PS2Mouse::erase(); }
extern "C" int  ps2mouse_enabled(void)     { return Kernel::s_enabled ? 1 : 0; }

/* Fullscreen-game mouselook bridges (see game_mouse_capture). */
extern "C" void ps2mouse_set_capture(int on)          { Kernel::game_mouse_capture(on != 0); }
extern "C" void ps2mouse_take_rel(int* dx, int* dy)   { Kernel::game_mouse_take_rel(dx, dy); }
