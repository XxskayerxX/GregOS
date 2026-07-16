/* GUI::Label — static text widget. */

#include "../../include/GUI/Label.hpp"
#include "../../include/Graphics.hpp"
#include "../../include/gfx.h"

namespace GUI {

Label::Label(int x, int y, int w, int h,
             const char* text, unsigned int color, int align)
    : Widget(x, y, w, h), m_color(color), m_align(align)
{
    set_text(text);
}

void Label::set_text(const char* t)
{
    int i = 0;
    if (t) while (t[i] && i < MAX_TEXT - 1) { m_text[i] = t[i]; i++; }
    m_text[i] = '\0';
}

void Label::draw(Graphics& g, int wx, int wy)
{
    int ax = wx + m_x, ay = wy + m_y;
    int len = slen(m_text);
    int tx;
    if (m_align == ALIGN_CENTER)
        tx = ax + (m_w - len * 8) / 2;
    else if (m_align == ALIGN_RIGHT)
        tx = ax + m_w - len * 8 - 2;
    else
        tx = ax + 2;
    int ty = ay + (m_h - 8) / 2;   /* vertically centered */
    g.draw_str(tx, ty, m_text, m_color, GFX_TRANSPARENT);
}

} /* namespace GUI */
