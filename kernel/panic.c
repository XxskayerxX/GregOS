/* panic.c — Kernel Panic / BSoD handler for GregOS.
   Freestanding: no libc.

   On any unhandled CPU exception the ASM stub calls one of the fault*_handler
   functions here.  We:
     1. Disable interrupts.
     2. Write the panic info to VGA text mode (always safe — raw 0xB8000 write).
     3. If gfx is active, also paint a full graphical blue screen and swap.
     4. Halt forever.                                                          */

#include "../include/panic.h"
#include "../include/gfx.h"

/* ── Bridges for Ring-3 fault recovery ──────────────────────────────────
   A fault that originates at CPL=3 is a misbehaving *userland* process, not
   a kernel bug: we kill just that process and let the kernel run on. These
   come from vga.c/kernel.c and Scheduler.cpp.                              */
extern void         term_print(const char* s);
extern void         term_set_color(unsigned char fg, unsigned char bg);
extern int          scheduler_kill_current(void);   /* 1=killed, 0=main thread */
extern unsigned int scheduler_current_id(void);

static void u32_to_hex(unsigned int v, char* out) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 8; i++) {
        unsigned int n = (v >> ((7 - i) * 4)) & 0xFu;
        out[2 + i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    out[10] = '\0';
}

static void uint_to_dec(unsigned int v, char* out) {
    char t[12]; int i = 0;
    if (!v) { out[0] = '0'; out[1] = '\0'; return; }
    while (v && i < 11) { t[i++] = (char)('0' + v % 10); v /= 10; }
    int j = 0; while (i) out[j++] = t[--i]; out[j] = '\0';
}

/* Returns 1 if the fault was handled (Ring-3 process killed), 0 if the caller
   must fall through to a full kernel panic (Ring-0 fault = real bug).       */
static int try_recover_ring3(struct panic_regs* regs, const char* exc,
                             unsigned int cr2)
{
    if (!regs || (regs->cs & 3u) != 3u) return 0;   /* not a CPL=3 fault */
    if (!scheduler_kill_current())      return 0;   /* main thread → panic */

    char hx[11], pid[12];
    uint_to_dec(scheduler_current_id(), pid);
    term_set_color(0x0C, 0x00);              /* bright red */
    term_print("\n[FAULT] Processus Ring 3 #"); term_print(pid);
    term_print(" tue : "); term_print(exc);
    term_print("  EIP="); u32_to_hex(regs->eip, hx); term_print(hx);
    if (cr2) { term_print("  adresse="); u32_to_hex(cr2, hx); term_print(hx); }
    term_print("\n[FAULT] Le noyau continue de tourner.\n");
    term_set_color(0x0F, 0x00);

    /* Thread is Blocked; wait for the next PIT tick to schedule another
       thread. Control never returns to this (dead) Ring-3 process.        */
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");
}

/* ── VGA text helpers (raw, no dependency on kernel internals) ─────────── */

static volatile unsigned short* const VGA = (volatile unsigned short*)0xB8000;
#define VGA_W 80

static void vga_set(int col, int row, char c, unsigned char attr) {
    VGA[row * VGA_W + col] = ((unsigned short)attr << 8) | (unsigned char)c;
}

static void vga_str(int col, int row, const char* s, unsigned char attr) {
    while (*s && col < VGA_W) vga_set(col++, row, *s++, attr);
}

static void vga_hex(int col, int row, unsigned int v, unsigned char attr) {
    char buf[11];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 9; i >= 2; i--) {
        unsigned int n = v & 0xFu;
        buf[i] = (char)(n < 10 ? '0' + n : 'A' + n - 10);
        v >>= 4;
    }
    buf[10] = '\0';
    vga_str(col, row, buf, attr);
}

/* ── GFX text helpers (used for graphical BSoD) ─────────────────────────── */

static char g_pbuf[128];

static void p_puts(int* px, int* py, const char* s,
                   unsigned int fg, unsigned int bg)
{
    gfx_draw_str(*px, *py, s, fg, bg);
    /* advance x by string length */
    int len = 0;
    while (s[len]) len++;
    *px += len * GFX_FONT_W;
}

static void p_hex(int* px, int* py, unsigned int v,
                  unsigned int fg, unsigned int bg)
{
    char* b = g_pbuf;
    *b++ = '0'; *b++ = 'x';
    for (int i = 7; i >= 0; i--) {
        unsigned int n = (v >> (i * 4)) & 0xFu;
        *b++ = (char)(n < 10 ? '0' + n : 'A' + n - 10);
    }
    *b = '\0';
    p_puts(px, py, g_pbuf, fg, bg);
}


#define NL(px,py)  do { *(px) = LEFT; *(py) += GFX_FONT_H + 4; } while(0)

/* ── kernel_panic ──────────────────────────────────────────────────────── */

void kernel_panic(struct panic_regs* regs,
                  const char* exc_name,
                  unsigned int cr2)
{
    __asm__ volatile("cli");

    /* ── 1. VGA text fallback (always visible, even without framebuffer) ── */

    /* Clear rows 0-9 in red-on-white */
    for (int r = 0; r < 10; r++)
        for (int c = 0; c < VGA_W; c++)
            vga_set(c, r, ' ', 0x4F);  /* white on red */

    vga_str(0, 0, "*** KERNEL PANIC ***", 0x4F);
    vga_str(0, 1, "Exception: ", 0x4F);
    vga_str(11, 1, exc_name, 0x4F);

    int col = 0;
    if (regs) {
        vga_str(0, 3, "EIP      : ", 0x4F); vga_hex(11, 3, regs->eip, 0x4F);
        vga_str(0, 4, "CS       : ", 0x4F); vga_hex(11, 4, regs->cs,  0x4F);
        vga_str(0, 5, "EFLAGS   : ", 0x4F); vga_hex(11, 5, regs->eflags, 0x4F);
        vga_str(0, 6, "ERR_CODE : ", 0x4F); vga_hex(11, 6, regs->err_code, 0x4F);
        vga_str(0, 7, "EAX      : ", 0x4F); vga_hex(11, 7, regs->eax, 0x4F);
        col = 24;
        vga_str(col, 7, "EBX      : ", 0x4F); vga_hex(col+11, 7, regs->ebx, 0x4F);
        vga_str(0, 8, "ECX      : ", 0x4F); vga_hex(11, 8, regs->ecx, 0x4F);
        vga_str(col, 8, "EDX      : ", 0x4F); vga_hex(col+11, 8, regs->edx, 0x4F);
        vga_str(0, 9, "ESI      : ", 0x4F); vga_hex(11, 9, regs->esi, 0x4F);
        vga_str(col, 9, "EDI      : ", 0x4F); vga_hex(col+11, 9, regs->edi, 0x4F);
    }
    vga_str(0, 2, "CR2 (mem): ", 0x4F); vga_hex(11, 2, cr2, 0x4F);

    /* ── 2. Graphical BSoD ──────────────────────────────────────────────── */

    if (gfx_active()) {
        int W = gfx_width();
        int H = gfx_height();

        const unsigned int BLUE  = 0x0000AAu;
        const unsigned int WHITE = 0xFFFFFFu;
        const unsigned int CYAN  = 0x55FFFFu;
        const unsigned int GRAY  = 0xCCCCCCu;

#define LEFT 24
        int px, py;

        /* Solid blue background */
        gfx_fill_rect(0, 0, W, H, BLUE);

        /* Top accent bar */
        gfx_fill_rect(0, 0, W, GFX_FONT_H + 8, WHITE);
        px = LEFT; py = 4;
        p_puts(&px, &py, "*** KERNEL PANIC ***", BLUE, WHITE);

        /* Exception name */
        py = GFX_FONT_H + 20; px = LEFT;
        p_puts(&px, &py, "Exception : ", CYAN, BLUE);
        p_puts(&px, &py, exc_name, WHITE, BLUE);

        /* Separator line */
        py += GFX_FONT_H + 6;
        gfx_draw_hline(LEFT, py, W - LEFT * 2, GRAY);
        py += 8;

        /* Registers */
        if (regs) {
            px = LEFT;
            p_puts(&px, &py, "EIP      : ", GRAY, BLUE);
            p_hex(&px, &py, regs->eip, WHITE, BLUE);
            NL(&px, &py);

            p_puts(&px, &py, "CS       : ", GRAY, BLUE);
            p_hex(&px, &py, regs->cs, WHITE, BLUE);
            NL(&px, &py);

            p_puts(&px, &py, "EFLAGS   : ", GRAY, BLUE);
            p_hex(&px, &py, regs->eflags, WHITE, BLUE);
            NL(&px, &py);

            p_puts(&px, &py, "ERR_CODE : ", GRAY, BLUE);
            p_hex(&px, &py, regs->err_code, WHITE, BLUE);
            NL(&px, &py);
        }

        /* CR2 */
        px = LEFT;
        p_puts(&px, &py, "CR2      : ", GRAY, BLUE);
        p_hex(&px, &py, cr2, WHITE, BLUE);
        NL(&px, &py);

        if (regs) {
            NL(&px, &py);
            p_puts(&px, &py, "EAX=", GRAY, BLUE); p_hex(&px, &py, regs->eax, WHITE, BLUE);
            p_puts(&px, &py, "  EBX=", GRAY, BLUE); p_hex(&px, &py, regs->ebx, WHITE, BLUE);
            p_puts(&px, &py, "  ECX=", GRAY, BLUE); p_hex(&px, &py, regs->ecx, WHITE, BLUE);
            p_puts(&px, &py, "  EDX=", GRAY, BLUE); p_hex(&px, &py, regs->edx, WHITE, BLUE);
            NL(&px, &py);
            p_puts(&px, &py, "ESI=", GRAY, BLUE); p_hex(&px, &py, regs->esi, WHITE, BLUE);
            p_puts(&px, &py, "  EDI=", GRAY, BLUE); p_hex(&px, &py, regs->edi, WHITE, BLUE);
            p_puts(&px, &py, "  EBP=", GRAY, BLUE); p_hex(&px, &py, regs->ebp, WHITE, BLUE);
        }

        /* Bottom footer */
        py = H - GFX_FONT_H - 12;
        gfx_fill_rect(0, py - 4, W, GFX_FONT_H + 16, WHITE);
        px = LEFT;
        p_puts(&px, &py, "Systeme arrete. Redemarrez la machine.", BLUE, WHITE);

        gfx_swap_buffers();
    }

    /* ── 3. Halt forever ─────────────────────────────────────────────────── */
    for (;;) __asm__ volatile("hlt");
}

/* ── Per-exception C handlers (called by ASM stubs) ─────────────────── */

void fault0_handler(struct panic_regs* regs, unsigned int cr2) {
    (void)cr2;
    const char* n = "Division par zero (#DE, ISR 0)";
    if (try_recover_ring3(regs, n, 0)) return;
    kernel_panic(regs, n, 0);
}

void fault6_handler(struct panic_regs* regs, unsigned int cr2) {
    (void)cr2;
    const char* n = "Instruction invalide (#UD, ISR 6)";
    if (try_recover_ring3(regs, n, 0)) return;
    kernel_panic(regs, n, 0);
}

void fault8_handler(struct panic_regs* regs, unsigned int cr2) {
    (void)cr2;
    /* Double Fault is unrecoverable — the CPU could not dispatch a prior
       fault. Always panic, regardless of originating privilege level.     */
    kernel_panic(regs, "Double Fault (#DF, ISR 8)", 0);
}

void fault13_handler(struct panic_regs* regs, unsigned int cr2) {
    (void)cr2;
    const char* n = "General Protection Fault (#GP, ISR 13)";
    if (try_recover_ring3(regs, n, 0)) return;
    kernel_panic(regs, n, 0);
}

void fault14_handler(struct panic_regs* regs, unsigned int cr2) {
    const char* n = "Page Fault (#PF, ISR 14)";
    if (try_recover_ring3(regs, n, cr2)) return;
    kernel_panic(regs, n, cr2);
}
