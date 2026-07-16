#ifndef WIDGET_HPP
#define WIDGET_HPP

#include "Event.hpp"

/* ── Widget: abstract base class for GUI elements ──────────────────────
   Freestanding C++11. draw() renders to the back buffer via
   Graphics::instance(). on_event() is called by WindowManager for every
   input event that hits this widget; returns true if the event is consumed
   (stops propagation), false otherwise.                                   */

class Widget {
public:
    Widget(int x, int y, int w, int h)
        : _x(x), _y(y), _w(w), _h(h), _visible(true) {}

    virtual ~Widget() {}

    virtual void draw() = 0;
    virtual bool on_event(const Event& e) = 0;

    /* Point-in-rect test */
    bool hit_test(int mx, int my) const {
        return _visible
            && mx >= _x && mx < _x + _w
            && my >= _y && my < _y + _h;
    }

    void set_visible(bool v) { _visible = v; }
    bool visible() const     { return _visible; }

    int x() const { return _x; }
    int y() const { return _y; }
    int w() const { return _w; }
    int h() const { return _h; }

protected:
    int  _x, _y, _w, _h;
    bool _visible;
};

#endif /* WIDGET_HPP */
