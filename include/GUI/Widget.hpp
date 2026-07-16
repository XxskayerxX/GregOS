#ifndef GUI_WIDGET_HPP
#define GUI_WIDGET_HPP

/* ── GUI::Widget — base class for in-window child widgets ────────────────────
   Coordinates (m_x, m_y, m_w, m_h) are relative to the parent window's
   client area origin. The window passes (client_x, client_y) as (wx, wy)
   so absolute screen position is simply (wx + m_x, wy + m_y).

   Inherits Greg::RefCounted so RefPtr<GUI::Widget> works correctly.
   Freestanding C++11: no libc, no exceptions, no RTTI.                     */

#include "../Greg/Greg.h"
#include "../Event.hpp"

class Graphics;   /* forward — avoids circular include */

namespace GUI {

class Widget : public Greg::RefCounted<Widget> {
public:
    Widget(int x, int y, int w, int h)
        : m_x(x), m_y(y), m_w(w), m_h(h) {}
    virtual ~Widget() {}

    /* Draw self into the Graphics back-buffer.
       wx/wy = parent window's client_x() / client_y().                    */
    virtual void draw(Graphics& g, int wx, int wy) = 0;

    /* Handle one event. wx/wy = parent window's client origin.
       Return true to consume the event (stop propagation).                */
    virtual bool on_event(const Event& e, int wx, int wy) {
        (void)e; (void)wx; (void)wy; return false;
    }

    /* Absolute point-in-rect test */
    bool hit_test(int mx, int my, int wx, int wy) const {
        int ax = wx + m_x, ay = wy + m_y;
        return mx >= ax && mx < ax + m_w
            && my >= ay && my < ay + m_h;
    }

    int x() const { return m_x; }
    int y() const { return m_y; }
    int w() const { return m_w; }
    int h() const { return m_h; }

    /* Keyboard focus — managed by Window on mouse click */
    bool focused()          const { return m_focused; }
    void set_focused(bool f)      { m_focused = f; on_focus_change(f); }

    /* Called by Window::handle_char() when this widget is focused.
       Return true to consume the key.                                        */
    virtual bool handle_char(int c) { (void)c; return false; }

    /* Override to react to focus gain/loss (e.g. start/stop cursor blink). */
    virtual void on_focus_change(bool /*gained*/) {}

protected:
    int  m_x, m_y, m_w, m_h;
    bool m_focused { false };
};

} /* namespace GUI */

#endif /* GUI_WIDGET_HPP */
