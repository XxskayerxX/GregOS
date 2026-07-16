#ifndef WINDOWMANAGER_HPP
#define WINDOWMANAGER_HPP

#include "Window.hpp"
#include "Greg/Greg.h"

class TerminalEmulator;
class ContextMenuWindow;

/* ── WindowManager: Z-ordered window stack + TTY routing ────────────────
   Draw order : index 0 → N-1 (back to front / bottom to top).
   Event order: N-1 → 0     (top to bottom, first-consumer wins).

   draw() also performs close-cleanup: removes any window whose
   close_requested() flag is set (calling on_removed() before release).

   ── Global keyboard shortcuts ──
     Ctrl + Q  →  request_close() on the focused window.

   ── Key routing (from C shell loop) ──
     handle_focused_key(c)  offers a processed keycode to the focused
     window. Used by wm_handle_key() C bridge in kernel.c's shell loop.  */

class WindowManager {
public:
    void add_window(Greg::RefPtr<Window> w);
    void remove_window(Window* w);
    void raise(Window* w);

    /* Toggle the start-menu popup (create or close). */
    void toggle_start_menu();

    /* Open the right-click context menu at (x, y). */
    void open_context_menu(int x, int y);

    /* Full repaint: teal clear → desktop log → windows → swap.
       Also purges close-requested windows before drawing.               */
    void draw();

    bool dispatch_event(const Event& e);

    /* Offer a processed key to the currently focused window.
       Returns true if the window consumed it.                           */
    bool handle_focused_key(int c);

    void              set_tty0(TerminalEmulator* t);
    TerminalEmulator* tty0()                  const;

    Greg::usize window_count() const { return windows_.size(); }

    Greg::Vector<Greg::RefPtr<Window>>& windows() { return windows_; }

    static WindowManager& instance();

    static constexpr int TASKBAR_H = 28;   /* taskbar height in pixels */

private:
    void route_key(const Event& e);

    /* Handle a left-click at taskbar x on the open-window buttons.
       Returns true if a window button was hit (focus/raise/minimize). */
    bool taskbar_window_click(int mx);

    /* Alt-Tab: focus + raise the next visible, non-popup window. */
    void cycle_focus();

    Greg::Vector<Greg::RefPtr<Window>> windows_;
    bool    ctrl_held_     { false };
    bool    alt_held_      { false };
    Window* m_start_menu   { nullptr };
    Window* m_context_menu { nullptr };
};

#endif /* WINDOWMANAGER_HPP */
