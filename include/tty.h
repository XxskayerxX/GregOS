#ifndef TTY_H
#define TTY_H

/* ── TTY I/O Routing — C-compatible hook ────────────────────────────────
   Bridges the existing C output path (term_putc / vga.c) to the new
   TerminalEmulator subsystem without rewriting the 70+ kernel commands.

   Activation sequence (Phase 5):
     1. Kernel boots → tty_putc_hook = NULL → output goes to raw VGA.
     2. GUI init calls TerminalEmulator::install_as_tty0():
          - Allocates TTY0 buffer.
          - Sets  tty_putc_hook = tty0_char_cb  (the adapter below).
          - Calls vga_mirror_pause(1) to silence the VGA text path.
     3. All subsequent term_putc(c) calls in vga.c check tty_putc_hook
        and call it; output lands in the TerminalEmulator grid.
     4. On the next WindowManager::draw() tick, draw_desktop_log()
        renders the grid onto the black framebuffer.

   vga.c modification required (Phase 5 implementation):
     At the top of term_putc(), add:
       if (tty_putc_hook) { tty_putc_hook(c); return; }

   This keeps the VGA fallback intact for early-boot diagnostics.        */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Character output hook ───────────────────────────────────────────────
   NULL → direct VGA output; !NULL → active TerminalEmulator.           */
extern void (*tty_putc_hook)(char c);
void tty_putc(char c);
void tty0_char_cb(char c);

/* ── Color hook ──────────────────────────────────────────────────────────
   Set alongside tty_putc_hook. term_set_color() in vga.c calls this so
   the emulator's fg/bg tracks VGA palette changes from the shell.      */
extern void (*tty_set_color_hook)(unsigned char fg, unsigned char bg);
void tty_set_color(unsigned char fg, unsigned char bg);

/* ── Cursor-move hook ────────────────────────────────────────────────────
   Set alongside tty_putc_hook. term_move_cursor() in vga.c calls this
   so the TerminalEmulator cursor tracks VGA cursor positioning (nano).  */
extern void (*tty_move_cursor_hook)(int x, int y);

/* ── High-level init ─────────────────────────────────────────────────────*/
void tty_system_init(void);
void tty_create_terminal_window(int x, int y, int w, int h, const char* title);

/* Restore desktop TTY0 (g_tty0) as the active terminal emulator.
   Called when a TerminalWindow is destroyed so output falls back to the
   ambient desktop log.                                                  */
void tty_restore_desktop_tty0(void);

/* ── WindowManager C bridges ─────────────────────────────────────────────
   These wrap C++ calls for use from kernel.c (C).                      */

/* Full redraw: clear to teal, draw desktop log + all windows, swap.    */
void wm_draw(void);

/* Drain the EventQueue and dispatch each event through WindowManager.
   Keyboard events reach route_key → handle_char → kb_inject_char.
   Called by get_monitor_char() in GUI mode on every hlt wake-up.      */
void wm_pump_events(void);

/* Offer a processed keycode to the focused window.
   Returns 1 if a window consumed it (caller should not process further).*/
int  wm_handle_key(int c);

/* Returns 1 if any windows are currently open in the WindowManager.
   Used by the shell loop to bypass the desk_state==0 keyboard guard.    */
int  wm_has_windows(void);

/* ── Login window ────────────────────────────────────────────────────────*/
void open_login_window(void);

/* ── File Manager ────────────────────────────────────────────────────────*/
void open_file_manager(void);

/* ── Terminal control ────────────────────────────────────────────────────*/
void tty_clear(void);   /* clear the active TerminalEmulator grid */

/* ── Casino ──────────────────────────────────────────────────────────────*/
void open_casino_window(void);
int  casino_get_balance(void);
void casino_modify_balance(int delta);  /* add delta (negative = debit) */
void launch_casino_game(int n);  /* 0=BJ 1=Roulette 2=Slots 3=Plinko */
void open_poker_window(void);

/* ── System info window ──────────────────────────────────────────────────*/
void open_system_window(void);

/* ── Calculator window ───────────────────────────────────────────────────*/
void open_calc_window(void);

/* ── New apps: web browser, paint, clock ─────────────────────────────────*/
void open_browser_window(void);
void open_browser_window_url(const char* url);
void open_paint_window(void);
void open_clock_window(void);

/* ── Games: Démineur du Donjon ───────────────────────────────────────────*/
void open_minesweeper_window(void);

/* ── System Monitor window ───────────────────────────────────────────────*/
void open_system_monitor_window(void);
unsigned int kmalloc_used(void);
unsigned int kmalloc_total(void);

/* ── Arcade games launcher ───────────────────────────────────────────────*/
void open_games_window(void);           /* opens GamesWindow in the WM      */
void launch_arcade_game(int n);         /* 0=Snake…9=Clock, blocks until done*/

/* ── Start Menu bridges ──────────────────────────────────────────────────
   Launch wrappers for static kernel.c functions exposed for StartMenuWindow */
void start_menu_launch_games(void);
void start_menu_launch_sysinfo(void);

/* Toggle the start-menu popup (WM creates or closes it). */
void wm_toggle_start_menu(void);

/* Returns number of existing entries in the in-memory filesystem. */
int sys_get_file_count(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TTY_H */
