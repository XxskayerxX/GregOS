#ifndef TERMINAL_WINDOW_HPP
#define TERMINAL_WINDOW_HPP

#include "Window.hpp"
#include "TerminalEmulator.hpp"

/* ── TerminalWindow ───────────────────────────────────────────────────────
   A Window whose client area is a live TerminalEmulator grid.

   Rendering pipeline:
     1. Window::draw() paints the 1px chrome (border + title separator).
     2. TerminalWindow::draw() iterates the TermCell grid and blits each
        character with Graphics::draw_char() at the right pixel offset.
     3. The cursor is drawn as a solid block (Theme::BORDER_FOCUS color)
        that blinks at ~2 Hz using the jiffies counter.

   Terminal sizing:
     init_terminal() auto-computes cols = client_w() / FONT_W
                                    rows = client_h() / FONT_H
     Call it after setup() so that _w/_h are set.

   Keyboard input:
     When focused, printable characters received in on_event() are
     forwarded to m_term.put_char().  Non-printable / control keys
     (arrows, enter, backspace) are handled as escape sequences in
     Phase 6 (shell integration).

   Ownership model:
     Always allocate via Greg::make_ref<TerminalWindow>() and hand the
     RefPtr<TerminalWindow> to WindowManager::add_window().
     The implicit RefPtr<Window>-from-RefPtr<TerminalWindow> conversion
     is provided by the template constructor in Greg::RefPtr.

   Usage:
     auto tw = Greg::make_ref<TerminalWindow>();
     tw->setup(10, 10, 640, 400, "terminal");
     tw->init_terminal();
     WindowManager::instance().add_window(Greg::move(tw));              */

class TerminalWindow : public Window {
public:
    /* Compute grid dimensions from client area, allocate TerminalEmulator.
       Returns false if the window is too small or kmalloc fails.         */
    bool init_terminal();

    /* Called when the window is removed from WindowManager.
       Restores desktop TTY0 if this terminal was the active one.         */
    void on_removed() override;

    /* Direct access to the embedded emulator (I/O routing, read-back) */
    TerminalEmulator&       terminal()       { return m_term; }
    const TerminalEmulator& terminal() const { return m_term; }

    /* ── Widget overrides ─────────────────────────────────────────────── */

    /* Draw chrome via Window::draw(), then render the terminal grid      */
    void draw() override;

    /* Called by WindowManager::route_key() with a pre-processed char.
       Injects the char into kb_inject_buf so get_monitor_char() returns
       it to the shell loop. Never calls m_term.put_char() — the shell
       handles its own echo via tty_putc_hook. Always returns true.      */
    bool handle_char(int c) override;

    /* Mouse events (drag, close button) delegated to Window::on_event() */
    bool on_event(const Event& e) override;

private:
    /* Render one TermCell at pixel position (px, py) in the back-buffer */
    void draw_cell(int px, int py, const TermCell& cell);

    /* Draw the cursor block at the current cursor position */
    void draw_cursor();

    TerminalEmulator m_term;
};

#endif /* TERMINAL_WINDOW_HPP */
