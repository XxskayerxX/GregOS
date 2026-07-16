#ifndef GUI_GRAPHWIDGET_HPP
#define GUI_GRAPHWIDGET_HPP

/* ── GUI::GraphWidget — ring-buffer area chart widget ─────────────────────────
   Stores up to BUF_SIZE integer samples in a circular buffer.
   draw() renders them as a filled area chart with a sunken 3D border.
   push_value(int) appends a new sample.  No events handled (read-only).
   Freestanding: no libc, no exceptions.                                      */

#include "Widget.hpp"

namespace GUI {

class GraphWidget : public Widget {
public:
    static constexpr int BUF_SIZE = 64;

    GraphWidget(int x, int y, int w, int h)
        : Widget(x, y, w, h) {}

    void push_value(int val);
    void draw(Graphics& g, int wx, int wy) override;

private:
    int m_buf[BUF_SIZE] {};
    int m_head  { 0 };
    int m_count { 0 };
    int m_max   { 1 };   /* ≥ 1 to avoid div-by-zero */
};

} /* namespace GUI */

#endif /* GUI_GRAPHWIDGET_HPP */
