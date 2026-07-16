#ifndef GAMES_APP_HPP
#define GAMES_APP_HPP

/* ArcadeApp — Compositor::Application wrapper for the legacy blocking game loops.
   Each arcade game still runs as its original C function (gg_snake, gg_tetris …)
   but inside a dedicated Scheduler thread so the Compositor's main thread is not
   blocked.  While the game thread is alive, ArcadeApp::is_fullscreen() returns
   true, which tells Compositor::compose() to skip normal desktop rendering and
   let the game write directly to the framebuffer.

   Lifecycle:
     1. GamesWindow creates an ArcadeApp and registers it with the Compositor.
     2. launch_arcade_game_async(n) spawns the game thread via the Scheduler.
     3. Game thread runs the blocking game loop, calls gfx_swap_buffers itself.
     4. When the game exits, arcade_game_is_done() returns 1.
     5. ArcadeApp::close_requested() becomes true → Compositor purges it.
     6. Normal desktop rendering resumes on the next compose() frame.          */

#include "Compositor/Application.hpp"

/* C bridge from kernel.c (defined alongside the game functions). */
extern "C" int  arcade_game_is_done(void);
extern "C" void kb_inject_flush(void);

class Graphics;

class ArcadeApp : public Compositor::Application {
public:
    ArcadeApp() = default;

    /* Compositor::Application interface */
    void paint(Graphics&)           override {}      /* game renders directly  */
    bool on_event(const Event&)     override { return true; } /* absorb all    */
    bool is_fullscreen()      const override { return true; }
    bool is_visible()         const override { return true; }
    bool close_requested()    const override { return arcade_game_is_done() != 0; }
    void on_removed()               override { kb_inject_flush(); } /* flush kb */

    int x()      const override { return 0; }
    int y()      const override { return 0; }
    int width()  const override { return 800; }
    int height() const override { return 600; }
};

#endif /* GAMES_APP_HPP */
