/* TerminalWindow — GUI wrapper around TerminalEmulator
   Renders the terminal grid into the window's client area.
   Freestanding: no libc, no exceptions.                                  */

#include "../include/TerminalWindow.hpp"
#include "../include/TerminalEmulator.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/tty.h"

extern "C" volatile unsigned long jiffies;

/* ── tty_create_terminal_window — C bridge for kernel.c ──────────────── */

extern "C" void tty_create_terminal_window(int x, int y, int w, int h,
                                            const char* title)
{
    kb_inject_flush();  /* discard any phantom chars from boot/login path */
    auto tw = Greg::make_ref<TerminalWindow>();
    /* Black client background: terminal cells cover it fully, avoids flash */
    tw->setup(x, y, w, h, title, 0x0D0D0Du);  /* dark terminal background */
    tw->init_terminal();
    tw->set_focused(true);  /* navy gradient title bar from first frame */
    /* This terminal window becomes the active TTY0 — shell output lands here */
    tw->terminal().install_as_tty0();
    WindowManager::instance().add_window(Greg::move(tw));
}

/* ══════════════════════════════════════════════════════════════════════
   TerminalWindow methods
   ══════════════════════════════════════════════════════════════════════ */

bool TerminalWindow::init_terminal()
{
    int cols = client_w() / TerminalEmulator::FONT_W;
    int rows = client_h() / TerminalEmulator::FONT_H;
    if (cols < 4) cols = 4;
    if (rows < 2) rows = 2;
    return m_term.init(cols, rows);
}

/* ── on_removed: called by WindowManager before releasing the RefPtr ── */

void TerminalWindow::on_removed()
{
    /* If this terminal is the active TTY0, restore the desktop ambient tty */
    if (TerminalEmulator::tty0() == &m_term)
        tty_restore_desktop_tty0();
}

/* ── draw_cell: blit one terminal cell at absolute pixel (px, py) ──── */

void TerminalWindow::draw_cell(int px, int py, const TermCell& cell)
{
    unsigned char ch = static_cast<unsigned char>(cell.ch);
    /* Font is CP437 8×16 — 256 glyphs (0x00–0xFF). Allow all printable CP437
       including box-drawing (≥0x80) and special symbols (<0x20).
       Only true control codes that must never reach here: ESC/LF/CR/BS/TAB
       are consumed by put_char() before cells are written. */
    if (ch < 0x09) ch = ' ';  /* NUL..BEL: blank */
    Graphics::instance().draw_char(px, py, ch, cell.fg, cell.bg);
}

/* ── draw_cursor: solid block at emulator cursor position ────────────── */

void TerminalWindow::draw_cursor()
{
    if (!m_term.ready()) return;
    if ((jiffies / 50) % 2 != 0) return;

    int px = client_x() + m_term.cursor_col() * TerminalEmulator::FONT_W;
    int py = client_y() + m_term.cursor_row() * TerminalEmulator::FONT_H;
    /* Solid block cursor — never draw_char here; extended-ASCII glyphs
       in the bitmap font render incorrectly (appear as Latin-1 letters). */
    Graphics::instance().fill_rect(px, py,
        TerminalEmulator::FONT_W, TerminalEmulator::FONT_H, 0xCCCCCCu);
}

/* ── draw: chrome → grid → cursor ────────────────────────────────────── */

void TerminalWindow::draw()
{
    Window::draw();  /* fills chrome + client area (bg_) */

    if (!m_term.ready()) return;

    /* Pixel origin of the client area — absolute screen coordinates.
       pixel_x = _x + BORDER_W + col * FONT_W
       pixel_y = _y + TITLE_H  + row * FONT_H                          */
    int ox = client_x();   /* = _x + BORDER_W */
    int oy = client_y();   /* = _y + TITLE_H  */

    /* Clip all rendering to the client area so characters never bleed
       onto the window chrome or outside the window boundary.           */
    Graphics& g = Graphics::instance();
    int cx1, cy1, cx2, cy2; g.get_clip(cx1, cy1, cx2, cy2);
    g.set_clip(ox, oy, client_w(), client_h());

    for (int row = 0; row < m_term.rows(); ++row) {
        for (int col = 0; col < m_term.cols(); ++col) {
            const TermCell& cell = m_term.cell_at(col, row);
            draw_cell(ox + col * TerminalEmulator::FONT_W,
                      oy + row * TerminalEmulator::FONT_H,
                      cell);
        }
    }

    if (focused())
        draw_cursor();

    g.set_clip_raw(cx1, cy1, cx2, cy2);  /* restore the outer (per-window) clip */
}

/* ── handle_char: bridge WM keyboard → shell loop via kb_inject_char ── */

bool TerminalWindow::handle_char(int c)
{
    /* Never call m_term.put_char() here — the shell echoes via tty_putc_hook.
       Injecting into kb_inject_buf makes get_monitor_char() return c to the
       shell loop, which then handles echo, history, command execution, etc. */
    kb_inject_char(c);
    return true;
}

/* ── on_event: mouse handled by Window; keyboard routed via handle_char ─ */

bool TerminalWindow::on_event(const Event& e)
{
    return Window::on_event(e);
}
