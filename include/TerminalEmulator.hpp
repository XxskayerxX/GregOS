#ifndef TERMINAL_EMULATOR_HPP
#define TERMINAL_EMULATOR_HPP

#include "Greg/Greg.h"
#include "Theme.hpp"

/* ── TermCell: one character position in the grid ────────────────────────
   Stores the character, foreground color, and background color.
   Default: space on pure black with neon-green foreground.              */
struct TermCell {
    char         ch  { ' ' };
    unsigned int fg  { Theme::FG_PRIMARY };
    unsigned int bg  { 0xFFFFFFFFu };   /* GFX_TRANSPARENT: no bg box by default */
};

/* ── TerminalEmulator ─────────────────────────────────────────────────────
   Stateful VT100-minimal character grid.  Pure data model — no graphics
   calls.  A renderer (TerminalWindow::draw or draw_desktop_log) reads the
   grid via cell_at() and blits it with Graphics::draw_char().

   Geometry:
     columns × rows cells; each cell is GFX_FONT_W × GFX_FONT_H pixels.
     Default: 80 × 25 (classic terminal; override in init()).

   Control characters handled by put_char():
     '\n'  → advance cursor to col=0, next row (scroll if at bottom)
     '\r'  → carriage return: col → 0
     '\b'  → backspace: col-- (clamp to 0), overwrite cursor cell with ' '
     '\t'  → advance to next 8-column tab stop
     other → print at cursor position, advance col (wrap + scroll as needed)

   Colors: set_color() changes the color applied to subsequent characters.
   reset_color() restores Theme defaults.

   I/O routing:
     install_as_tty0()  — registers this instance as the global TTY0.
                          After this call, tty_putc() routes here.
     tty0()             — returns the global TTY0 pointer (or nullptr).    */

class TerminalEmulator {
public:
    /* Font cell dimensions (match GFX_FONT_W / GFX_FONT_H in gfx.h) */
    static constexpr int FONT_W = 8;
    static constexpr int FONT_H = 16;

    static constexpr int DEFAULT_COLS = 80;
    static constexpr int DEFAULT_ROWS = 25;

    /* ── Lifecycle ────────────────────────────────────────────────────── */

    /* Allocate the character grid.  Must be called before any write.
       Pass cols=0 / rows=0 to auto-compute from pixel dimensions.
       Returns false on OOM (kmalloc failure).                            */
    bool init(int cols = DEFAULT_COLS, int rows = DEFAULT_ROWS);

    bool ready() const { return m_ready; }

    /* ── Write interface ──────────────────────────────────────────────── */

    void put_char(char c);

    void write(const char* s);
    void write(const Greg::String& s) { write(s.characters()); }

    /* ── Terminal control ─────────────────────────────────────────────── */

    /* Fill entire grid with spaces using the current colors */
    void clear();

    /* Set the color applied to subsequent put_char() calls */
    void set_color(unsigned int fg, unsigned int bg);

    /* Map a 4-bit VGA palette index pair to 24-bit RGB and call set_color().
       Called via tty_set_color_hook when the shell uses term_set_color().   */
    void set_vga_color(unsigned char vga_fg, unsigned char vga_bg);

    /* Restore default palette */
    void reset_color();

    /* Move cursor to an absolute position (clamped to grid bounds) */
    void set_cursor(int col, int row);

    /* ── Read-only grid access (renderer interface) ───────────────────── */

    int cols()       const { return m_cols; }
    int rows()       const { return m_rows; }
    int cursor_col() const { return m_cur_col; }
    int cursor_row() const { return m_cur_row; }

    /* Returns a default-constructed TermCell if (col, row) is out of range */
    const TermCell& cell_at(int col, int row) const;

    /* ── I/O routing ──────────────────────────────────────────────────── */

    /* Register this instance as TTY0 (the global desktop terminal) and
       install the tty_putc_hook so that tty_putc() routes here.
       Idempotent: calling multiple times is safe.                        */
    void install_as_tty0();

    /* Access the global TTY0 (nullptr if install_as_tty0 was never called) */
    static TerminalEmulator* tty0();

private:
    /* ── Internal helpers ─────────────────────────────────────────────── */

    void        scroll_up();                  /* shift rows 1..N-1 → 0..N-2, clear last */
    void        newline();                    /* col=0, advance row with scroll           */
    TermCell&   cell_mut(int col, int row);   /* mutable cell access                      */
    void        apply_sgr();                  /* process accumulated SGR params           */

    /* ── Grid storage ────────────────────────────────────────────────── */

    Greg::Vector<TermCell> m_buffer;   /* flat: index = row * m_cols + col */
    int          m_cols    { 0 };
    int          m_rows    { 0 };
    int          m_cur_col { 0 };
    int          m_cur_row { 0 };
    unsigned int m_fg      { Theme::FG_PRIMARY };
    unsigned int m_bg      { 0xFFFFFFFFu };    /* GFX_TRANSPARENT */
    bool         m_ready   { false };

    /* ── ANSI escape sequence parser ─────────────────────────────────── */
    /* States: 0=NORMAL  1=GOT_ESC  2=IN_CSI                             */
    int          m_ansi_state   { 0 };
    int          m_ansi_params[8] {};
    int          m_ansi_nparams { 0 };
    int          m_ansi_cur     { 0 };
    bool         m_ansi_private { false };  /* '?' private mode flag */

    /* ── Global TTY0 singleton ────────────────────────────────────────── */

    /* Defined in kernel/TerminalEmulator.cpp */
    static TerminalEmulator* s_tty0;
};

#endif /* TERMINAL_EMULATOR_HPP */
