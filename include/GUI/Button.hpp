#ifndef GUI_BUTTON_HPP
#define GUI_BUTTON_HPP

/* ── GUI::Button — Win95/SerenityOS-style push button ────────────────────────
   Renders a 3D-beveled rectangle (raised at rest, sunken when pressed).
   Fires a Greg::Function<void()> callback on click-release.
   Supports lambdas with captures, function pointers, and functors.          */

#include "Widget.hpp"
#include "../Greg/Function.hpp"

namespace GUI {

class Button : public Widget {
public:
    static constexpr int MAX_LABEL = 32;

    Button(int x, int y, int w, int h, const char* label,
           Greg::Function<void()> on_click = {});

    void draw(Graphics& g, int wx, int wy) override;
    bool on_event(const Event& e, int wx, int wy) override;

    void set_label(const char* label);
    void set_on_click(Greg::Function<void()> fn) { m_on_click = Greg::move(fn); }

    bool is_hovered()  const { return m_hovered;    }
    bool is_pressed()  const { return m_is_pressed; }

private:
    char  m_label[MAX_LABEL];
    bool  m_hovered    { false };
    bool  m_is_pressed { false };
    Greg::Function<void()> m_on_click;
};

} /* namespace GUI */

#endif /* GUI_BUTTON_HPP */
