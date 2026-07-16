#ifndef COMPOSITOR_APPLICATION_HPP
#define COMPOSITOR_APPLICATION_HPP

/* Compositor::Application — abstract base for all GUI applications.
   SerenityOS analogue: LibGUI::Application / LibGUI::Widget root.

   Subclass this to create a self-contained application that the
   Compositor owns and dispatches events to. Windows are the current
   concrete implementation; games and other apps will follow.

   Freestanding: no libc, no exceptions.                              */

#include "../Greg/Greg.h"
#include "../event.h"

class Graphics;

namespace Compositor {

class Application : public Greg::RefCounted<Application> {
public:
    virtual ~Application() = default;

    /* Called once per frame by Compositor::compose() if visible. */
    virtual void paint(Graphics& g) = 0;

    /* Deliver an input event. Return true if consumed. */
    virtual bool on_event(const Event& e) = 0;

    /* Geometry — screen-space bounding box. */
    virtual int x()      const = 0;
    virtual int y()      const = 0;
    virtual int width()  const = 0;
    virtual int height() const = 0;

    /* Lifecycle */
    virtual bool is_visible()        const { return true; }
    virtual bool close_requested()   const { return false; }
    virtual void on_removed()              {}

    /* Fullscreen apps (e.g. ArcadeApp) take over the entire framebuffer.
       Compositor::compose() skips normal rendering while one is active. */
    virtual bool is_fullscreen()     const { return false; }
};

} /* namespace Compositor */

#endif /* COMPOSITOR_APPLICATION_HPP */
