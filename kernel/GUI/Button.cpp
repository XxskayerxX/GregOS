/* GUI::Button — Win95-style push button for GregOS LibGUI.
   Freestanding: no libc, no exceptions.                                   */

#include "../../include/GUI/Button.hpp"
#include "../../include/Graphics.hpp"
#include "../../include/Theme.hpp"
#include "../../include/gfx.h"

static void btn_bevel(Graphics& g, int x, int y, int w, int h, bool raised)
{
    unsigned int lt  = raised ? Theme::BEVEL_OUTER_LT : Theme::BEVEL_OUTER_DK;
    unsigned int lti = raised ? Theme::BEVEL_INNER_LT : Theme::BEVEL_INNER_DK;
    unsigned int dk  = raised ? Theme::BEVEL_OUTER_DK : Theme::BEVEL_OUTER_LT;
    unsigned int dki = raised ? Theme::BEVEL_INNER_DK : Theme::BEVEL_INNER_LT;
    g.draw_hline(x,       y,       w, lt);
    g.draw_vline(x,       y,       h, lt);
    g.draw_hline(x,       y+h-1,   w, dk);
    g.draw_vline(x+w-1,   y,       h, dk);
    if (w > 2 && h > 2) {
        g.draw_hline(x+1,   y+1,   w-2, lti);
        g.draw_vline(x+1,   y+1,   h-2, lti);
        g.draw_hline(x+1,   y+h-2, w-2, dki);
        g.draw_vline(x+w-2, y+1,   h-2, dki);
    }
}

static int btn_slen(const char* s) { int n = 0; while (s[n]) ++n; return n; }

namespace GUI {

Button::Button(int x, int y, int w, int h,
               const char* label, Greg::Function<void()> on_click)
    : Widget(x, y, w, h), m_on_click(Greg::move(on_click))
{
    set_label(label);
}

void Button::set_label(const char* label)
{
    int i = 0;
    while (label[i] && i < MAX_LABEL - 1) { m_label[i] = label[i]; ++i; }
    m_label[i] = '\0';
}

void Button::draw(Graphics& g, int wx, int wy)
{
    int ax = wx + m_x, ay = wy + m_y;
    g.fill_rect(ax, ay, m_w, m_h, Theme::BTN_FACE);
    btn_bevel(g, ax, ay, m_w, m_h, !m_is_pressed);
    if (m_hovered && !m_is_pressed)
        g.draw_rect(ax + 3, ay + 3, m_w - 6, m_h - 6, Theme::GOLD_DIM);
    int llen = btn_slen(m_label);
    int tx = ax + (m_w - llen * 8) / 2 + (m_is_pressed ? 1 : 0);
    int ty = ay + (m_h -       16) / 2 + (m_is_pressed ? 1 : 0);
    g.draw_str(tx, ty, m_label, m_hovered ? Theme::AMBER_HI : Theme::AMBER,
               GFX_TRANSPARENT);
}

bool Button::on_event(const Event& e, int wx, int wy)
{
    if (e.type == EVT_MOUSE_MOVE) {
        bool was  = m_hovered;
        m_hovered = hit_test(e.mouse.x, e.mouse.y, wx, wy);
        return (was != m_hovered);
    }
    if (e.type == EVT_MOUSE_BUTTON) {
        bool pressed = (e.mouse.buttons & 0x01) != 0;
        if (pressed) {
            if (hit_test(e.mouse.x, e.mouse.y, wx, wy)) {
                m_is_pressed = true;
                return true;
            }
        } else {
            if (m_is_pressed) {
                m_is_pressed = false;
                if (m_hovered && m_on_click) m_on_click();
                return true;
            }
        }
    }
    return false;
}

} /* namespace GUI */
