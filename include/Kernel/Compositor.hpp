#ifndef KERNEL_COMPOSITOR_HPP
#define KERNEL_COMPOSITOR_HPP

#include "../Greg/Greg.h"
#include "../Greg/RefPtr.hpp"
#include "../Greg/Vector.hpp"
#include "../event.h"
#include "../Compositor/Application.hpp"

class Window;
class TerminalEmulator;

namespace Kernel {

/* ── Compositor ───────────────────────────────────────────────────────────────
   Owns desktop rendering: background, TTY log overlay, desktop icons,
   taskbar, hardware cursor.  Separate from WindowManager (event routing +
   window list) so each class has a single responsibility.

   Also owns the Application list (Phase 3+): fullscreen apps (e.g. ArcadeApp)
   register here and prevent normal compositor rendering while active.

   SerenityOS analogy: this is the early form of the future WindowServer
   compositor — it composites the scene into the back-buffer and swaps.    */

class Compositor {
public:
    static Compositor& instance();

    /* TTY0 reference — used for the ambient desktop log overlay. */
    void              set_tty0(TerminalEmulator* t) { m_tty0 = t; }
    TerminalEmulator* tty0()                  const { return m_tty0; }

    /* Mouse cursor position — updated by WindowManager on every mouse event.
       The cursor is drawn as part of the composited scene (see draw_cursor),
       so setting the position here is all that's needed.                    */
    void set_cursor(int x, int y) { m_mx = x; m_my = y; }
    int  cursor_x()         const { return m_mx; }
    int  cursor_y()         const { return m_my; }

    /* Register a Compositor::Application (e.g. ArcadeApp).
       Fullscreen apps suppress normal rendering until they close.         */
    void add_app(Greg::RefPtr<::Compositor::Application> app);

    /* True if any registered fullscreen Application is currently active.  */
    bool has_fullscreen_app() const;

    /* Full repaint: desktop → window list → taskbar → cursor → swap_buffers.
       Skips rendering if a fullscreen Application is active.
       Also purges Applications whose close_requested() is true.           */
    void compose(Greg::Vector<Greg::RefPtr<Window>>& windows);

    /* Inject an event into the WindowManager's dispatch path.
       This is the preferred entry point for Phase 4+ event submission;
       it decouples kernel.c from direct WM calls.                         */
    void submit_event(const Event& e);

private:
    void draw_wallpaper();   /* precomputed vertical gradient backdrop   */
    void draw_desktop();     /* scan lines + TTY0 log overlay            */
    void draw_desk_icons();  /* 9 shortcuts in two columns              */
    void draw_taskbar(Greg::Vector<Greg::RefPtr<Window>>& windows);
                             /* Start btn + window buttons + clock       */

    void draw_cursor();      /* pixel-art arrow, drawn into the scene each frame */

    TerminalEmulator* m_tty0 { nullptr };
    int m_mx { 400 };
    int m_my { 300 };

    Greg::Vector<Greg::RefPtr<::Compositor::Application>> m_apps;
};

} /* namespace Kernel */

#endif /* KERNEL_COMPOSITOR_HPP */
