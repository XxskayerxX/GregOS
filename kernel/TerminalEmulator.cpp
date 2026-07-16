/* TerminalEmulator — VTerm data model + TTY0 C bridge
   Freestanding: no libc. All memory via Greg::Vector / kmalloc.        */

#include "../include/TerminalEmulator.hpp"
#include "../include/tty.h"
#include "../include/vga.h"
#include "../include/Graphics.hpp"
#include "../include/WindowManager.hpp"

/* ── Static member definition ────────────────────────────────────────── */
TerminalEmulator* TerminalEmulator::s_tty0 = nullptr;

/* ── Global TTY0 instance (BSS zero-init, activated by tty_system_init) */
static TerminalEmulator g_tty0;

/* ══════════════════════════════════════════════════════════════════════
   TTY C bridge
   ══════════════════════════════════════════════════════════════════════ */

/* Character output hook */
void (*tty_putc_hook)(char) = nullptr;

/* Color hook — set alongside tty_putc_hook */
void (*tty_set_color_hook)(unsigned char, unsigned char) = nullptr;

/* Cursor-move hook — set alongside tty_putc_hook */
void (*tty_move_cursor_hook)(int, int) = nullptr;

/* Adapter: installed as tty_putc_hook */
extern "C" void tty0_char_cb(char c)
{
    if (TerminalEmulator::tty0())
        TerminalEmulator::tty0()->put_char(c);
}

/* Color adapter: installed as tty_set_color_hook */
static void tty0_color_cb(unsigned char fg, unsigned char bg)
{
    if (TerminalEmulator::tty0())
        TerminalEmulator::tty0()->set_vga_color(fg, bg);
}

/* Cursor-move adapter: installed as tty_move_cursor_hook */
static void tty0_move_cursor_cb(int x, int y)
{
    if (TerminalEmulator::tty0())
        TerminalEmulator::tty0()->set_cursor(x, y);
}

extern "C" void tty_putc(char c)
{
    if (tty_putc_hook) tty_putc_hook(c);
}

extern "C" void tty_set_color(unsigned char fg, unsigned char bg)
{
    if (tty_set_color_hook) tty_set_color_hook(fg, bg);
}

/* ── tty_system_init: initialize the ambient desktop terminal ──────── */

extern "C" void tty_system_init(void)
{
    if (g_tty0.ready()) return;   /* idempotent */

    Graphics& g = Graphics::instance();
    int cols = TerminalEmulator::DEFAULT_COLS;
    int rows = TerminalEmulator::DEFAULT_ROWS;
    if (g.active()) {
        cols = (g.width()  - 8)  / TerminalEmulator::FONT_W;
        rows = (g.height() - 34) / TerminalEmulator::FONT_H;
        if (cols < 40) cols = 40;
        if (rows < 10) rows = 10;
    }

    g_tty0.init(cols, rows);
    /* Desktop ambient: dim teal text on transparent teal bg */
    g_tty0.set_color(Theme::SYSLOG_DIM, 0x000000u);
    g_tty0.install_as_tty0();

    WindowManager::instance().set_tty0(&g_tty0);
}

/* ── tty_restore_desktop_tty0: fallback after TerminalWindow closes ── */

extern "C" void tty_clear(void)
{
    if (TerminalEmulator::tty0())
        TerminalEmulator::tty0()->clear();
}

extern "C" void tty_restore_desktop_tty0(void)
{
    /* Only restore if some other emulator (a TerminalWindow) had taken over */
    if (TerminalEmulator::tty0() != &g_tty0)
        g_tty0.install_as_tty0();
}

/* ══════════════════════════════════════════════════════════════════════
   TerminalEmulator methods
   ══════════════════════════════════════════════════════════════════════ */

bool TerminalEmulator::init(int cols, int rows)
{
    if (m_ready || cols <= 0 || rows <= 0) return false;

    m_cols = cols;
    m_rows = rows;
    reset_color();   /* set m_fg/m_bg before filling blank cells */

    TermCell blank; blank.fg = m_fg; blank.bg = m_bg;
    for (int i = 0; i < cols * rows; ++i) {
        if (!m_buffer.append(blank)) return false;
    }

    m_ready = (m_buffer.size() == static_cast<Greg::usize>(cols * rows));
    return m_ready;
}

/* ── Internal cell access ─────────────────────────────────────────────── */

static TermCell s_oob_cell;

const TermCell& TerminalEmulator::cell_at(int col, int row) const
{
    if (col < 0 || col >= m_cols || row < 0 || row >= m_rows)
        return s_oob_cell;
    return m_buffer[static_cast<Greg::usize>(row * m_cols + col)];
}

TermCell& TerminalEmulator::cell_mut(int col, int row)
{
    return m_buffer[static_cast<Greg::usize>(row * m_cols + col)];
}

/* ── Scroll ───────────────────────────────────────────────────────────── */

void TerminalEmulator::scroll_up()
{
    for (int row = 0; row < m_rows - 1; ++row)
        for (int col = 0; col < m_cols; ++col)
            cell_mut(col, row) = cell_at(col, row + 1);

    TermCell blank;
    blank.ch = ' '; blank.fg = m_fg; blank.bg = m_bg;
    for (int col = 0; col < m_cols; ++col)
        cell_mut(col, m_rows - 1) = blank;
}

/* ── ANSI SGR color tables ────────────────────────────────────────────── */

static const unsigned int s_ansi_fg[8] = {
    0x1C1C1Cu, 0xCC0000u, 0x00AA00u, 0xCCAA00u,
    0x0055CCu, 0xCC00CCu, 0x00AAAAu, 0xAAAAAAu
};
static const unsigned int s_ansi_bright[8] = {
    0x555555u, 0xFF5555u, 0x55FF55u, 0xFFFF55u,
    0x5555FFu, 0xFF55FFu, 0x55FFFFu, 0xFFFFFFu
};

void TerminalEmulator::apply_sgr()
{
    for (int i = 0; i < m_ansi_nparams; ++i) {
        int p = m_ansi_params[i];
        if      (p == 0)              { reset_color(); }
        else if (p >= 30 && p <= 37)  { m_fg = s_ansi_fg[p - 30]; }
        else if (p == 39)             { m_fg = 0xCCCCCCu; }  /* restore default fg */
        else if (p >= 40 && p <= 47)  { m_bg = s_ansi_fg[p - 40]; }
        else if (p == 49)             { m_bg = 0x0D0D0Du; }  /* restore default bg */
        else if (p >= 90 && p <= 97)  { m_fg = s_ansi_bright[p - 90]; }
        else if (p >= 100 && p <= 107){ m_bg = s_ansi_bright[p - 100]; }
    }
    m_ansi_nparams = 0;
}

/* ── put_char ─────────────────────────────────────────────────────────── */

void TerminalEmulator::put_char(char c)
{
    if (!m_ready) return;

    unsigned char uc = static_cast<unsigned char>(c);

    /* ── ANSI escape sequence state machine ── */
    if (m_ansi_state == 1) {          /* GOT_ESC: expecting '[' */
        if (c == '[') {
            m_ansi_state   = 2;
            m_ansi_nparams = 0;
            m_ansi_cur     = 0;
        } else {
            m_ansi_state = 0;         /* unknown escape, abort */
        }
        return;
    }
    if (m_ansi_state == 2) {          /* IN_CSI: accumulating params */
        if (c == '?') { m_ansi_private = true; return; }
        if (uc >= '0' && uc <= '9') {
            m_ansi_cur = m_ansi_cur * 10 + (uc - '0');
            return;
        }
        if (c == ';') {
            if (m_ansi_nparams < 8) m_ansi_params[m_ansi_nparams++] = m_ansi_cur;
            m_ansi_cur = 0;
            return;
        }
        /* Final byte — flush last param then dispatch */
        if (m_ansi_nparams < 8) m_ansi_params[m_ansi_nparams++] = m_ansi_cur;
        if (!m_ansi_private) {
            int p0 = m_ansi_params[0];
            int p1 = (m_ansi_nparams >= 2) ? m_ansi_params[1] : 0;
            switch (c) {
            case 'm':
                apply_sgr();
                break;
            case 'H': case 'f': {
                int row = (p0 > 0) ? p0 - 1 : 0;
                int col = (p1 > 0) ? p1 - 1 : 0;
                set_cursor(col, row);
                break;
            }
            case 'J':
                if (p0 == 2) clear();
                break;
            case 'K': {
                TermCell blank; blank.ch = ' '; blank.fg = m_fg; blank.bg = m_bg;
                for (int c2 = m_cur_col; c2 < m_cols; ++c2)
                    cell_mut(c2, m_cur_row) = blank;
                break;
            }
            case 'A': { int n = p0>0?p0:1; m_cur_row = m_cur_row-n<0 ? 0 : m_cur_row-n; break; }
            case 'B': { int n = p0>0?p0:1; m_cur_row = m_cur_row+n>=m_rows ? m_rows-1 : m_cur_row+n; break; }
            case 'C': { int n = p0>0?p0:1; m_cur_col = m_cur_col+n>=m_cols ? m_cols-1 : m_cur_col+n; break; }
            case 'D': { int n = p0>0?p0:1; m_cur_col = m_cur_col-n<0 ? 0 : m_cur_col-n; break; }
            default: break;
            }
        }
        m_ansi_private = false;
        m_ansi_state   = 0;
        return;
    }
    /* ESC (0x1B) triggers the sequence; intercepted before switch() */
    if (uc == 0x1Bu) { m_ansi_state = 1; return; }

    switch (c) {
    case '\n':
        m_cur_col = 0;
        if (++m_cur_row >= m_rows) { scroll_up(); m_cur_row = m_rows - 1; }
        break;

    case '\r':
        m_cur_col = 0;
        break;

    case '\b':
        if (m_cur_col > 0) {
            --m_cur_col;
            TermCell& cell = cell_mut(m_cur_col, m_cur_row);
            cell.ch = ' '; cell.fg = m_fg; cell.bg = m_bg;
        }
        break;

    case '\t': {
        int stop = ((m_cur_col / 8) + 1) * 8;
        if (stop > m_cols) stop = m_cols;
        TermCell blank; blank.ch = ' '; blank.fg = m_fg; blank.bg = m_bg;
        while (m_cur_col < stop)
            cell_mut(m_cur_col++, m_cur_row) = blank;
        if (m_cur_col >= m_cols) {
            m_cur_col = 0;
            if (++m_cur_row >= m_rows) { scroll_up(); m_cur_row = m_rows - 1; }
        }
        break;
    }

    default:
        if (static_cast<unsigned char>(c) < 0x20) break;

        TermCell& cell = cell_mut(m_cur_col, m_cur_row);
        cell.ch = c; cell.fg = m_fg; cell.bg = m_bg;

        if (++m_cur_col >= m_cols) {
            m_cur_col = 0;
            if (++m_cur_row >= m_rows) { scroll_up(); m_cur_row = m_rows - 1; }
        }
        break;
    }
}

/* ── write ────────────────────────────────────────────────────────────── */

void TerminalEmulator::write(const char* s)
{
    if (!s) return;
    while (*s) put_char(*s++);
}

/* ── Terminal control ─────────────────────────────────────────────────── */

void TerminalEmulator::clear()
{
    TermCell blank; blank.ch = ' '; blank.fg = m_fg; blank.bg = m_bg;
    for (int row = 0; row < m_rows; ++row)
        for (int col = 0; col < m_cols; ++col)
            cell_mut(col, row) = blank;
    m_cur_col = 0;
    m_cur_row = 0;
}

void TerminalEmulator::set_color(unsigned int fg, unsigned int bg)
{
    m_fg = fg;
    m_bg = bg;
}

void TerminalEmulator::set_vga_color(unsigned char vga_fg, unsigned char vga_bg)
{
    m_fg = vga_color_to_rgb(vga_fg);
    m_bg = (vga_bg == 0) ? 0x0D0D0Du : vga_color_to_rgb(vga_bg);
}

void TerminalEmulator::reset_color()
{
    m_fg = 0xCCCCCCu;   /* light gray — readable on dark terminal */
    m_bg = 0x0D0D0Du;   /* near-black background */
}

void TerminalEmulator::set_cursor(int col, int row)
{
    m_cur_col = (col < 0) ? 0 : (col >= m_cols ? m_cols - 1 : col);
    m_cur_row = (row < 0) ? 0 : (row >= m_rows ? m_rows - 1 : row);
}

/* ── TTY0 singleton ──────────────────────────────────────────────────── */

void TerminalEmulator::install_as_tty0()
{
    s_tty0               = this;
    tty_putc_hook        = tty0_char_cb;
    tty_set_color_hook   = tty0_color_cb;
    tty_move_cursor_hook = tty0_move_cursor_cb;
    vga_mirror_pause(1);  /* suppress duplicate VGA text rendering */
}

TerminalEmulator* TerminalEmulator::tty0()
{
    return s_tty0;
}
