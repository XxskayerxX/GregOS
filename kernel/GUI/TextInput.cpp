/* GUI::TextInput — single-line editable text field.
   Freestanding: no libc, no exceptions.                  */

#include "../../include/GUI/TextInput.hpp"
#include "../../include/Graphics.hpp"
#include "../../include/Theme.hpp"
#include "../../include/gfx.h"
#include "../../include/keyboard.h"
#include "../../include/Kernel/timer_c.h"

namespace GUI {

static constexpr unsigned int BG_FOCUSED   = 0x10101Au;
static constexpr unsigned int BG_UNFOCUSED = 0x1A1A1Au;
static constexpr int          TEXT_MARGIN  = 4;

TextInput::TextInput(int x, int y, int w, int h,
                     void (*on_return)(void*, const char*),
                     void* ctx,
                     void (*on_change)(void*, const char*))
    : Widget(x, y, w, h)
    , m_on_return(on_return), m_on_change(on_change), m_ctx(ctx)
{
    m_text[0] = '\0';
    m_placeholder[0] = '\0';
}

void TextInput::set_placeholder(const char* s)
{
    int i = 0;
    if (s) while (s[i] && i < (int)sizeof(m_placeholder) - 1) { m_placeholder[i]=s[i]; i++; }
    m_placeholder[i] = '\0';
}

void TextInput::set_text(const char* t)
{
    int i = 0;
    if (t) while (t[i] && i < MAX_LEN) { m_text[i]=t[i]; i++; }
    m_text[i] = '\0';
    m_cursor = i;
    update_scroll();
}

void TextInput::clear()
{
    m_text[0] = '\0'; m_cursor = 0; m_scroll = 0;
}

/* Keep m_scroll so that m_cursor is always within the visible window. */
void TextInput::update_scroll()
{
    int vis = (m_w - 2 * TEXT_MARGIN) / 8;
    if (vis < 1) vis = 1;
    if (m_cursor < m_scroll) m_scroll = m_cursor;
    if (m_cursor >= m_scroll + vis) m_scroll = m_cursor - vis + 1;
    if (m_scroll < 0) m_scroll = 0;
}

void TextInput::fire_change()
{
    if (m_on_change) m_on_change(m_ctx, m_text);
}

/* ── draw ──────────────────────────────────────────────────────────────── */

void TextInput::draw(Graphics& g, int wx, int wy)
{
    int ax = wx + m_x, ay = wy + m_y;

    /* Background */
    unsigned int bg = m_focused ? BG_FOCUSED : BG_UNFOCUSED;
    g.fill_rect(ax, ay, m_w, m_h, bg);

    /* Sunken bevel (inverted: dark top/left, light bottom/right) */
    g.draw_hline(ax,        ay,        m_w, Theme::BEVEL_OUTER_DK);
    g.draw_vline(ax,        ay,        m_h, Theme::BEVEL_OUTER_DK);
    g.draw_hline(ax,        ay+m_h-1,  m_w, Theme::BEVEL_OUTER_LT);
    g.draw_vline(ax+m_w-1,  ay,        m_h, Theme::BEVEL_OUTER_LT);
    /* Inner highlight */
    g.draw_hline(ax+1,      ay+1,      m_w-2, 0x0A0A14u);
    g.draw_vline(ax+1,      ay+1,      m_h-2, 0x0A0A14u);

    /* Clip text to inner area */
    int tx0 = ax + TEXT_MARGIN;
    int ty  = ay + (m_h - 8) / 2;
    int vis = (m_w - 2 * TEXT_MARGIN) / 8;
    if (vis < 1) vis = 1;

    const char* str = m_text;
    int len = slen(str);

    if (len == 0 && !m_focused && m_placeholder[0]) {
        /* Placeholder */
        const char* ph = m_placeholder;
        int pl = slen(ph); if (pl > vis) pl = vis;
        char tmp[48]; int ti = 0;
        while (ti < pl && ph[ti]) { tmp[ti] = ph[ti]; ti++; } tmp[ti] = '\0';
        g.draw_str(tx0, ty, tmp, 0x444444u, GFX_TRANSPARENT);
    } else {
        /* Visible slice starting at m_scroll */
        const char* view = str + m_scroll;
        int vlen = len - m_scroll; if (vlen < 0) vlen = 0;
        if (vlen > vis) vlen = vis;
        char tmp[128]; int ti = 0;
        while (ti < vlen && view[ti]) { tmp[ti] = view[ti]; ti++; } tmp[ti] = '\0';
        g.draw_str(tx0, ty, tmp, 0xDDDDDDu, GFX_TRANSPARENT);
    }

    /* Blinking cursor when focused */
    if (m_focused) {
        unsigned int jif = timer_jiffies();
        if ((jif / 50) % 2 == 0) {  /* blink: 50 jiffies = 0.5s at 100Hz */
            int cpos = m_cursor - m_scroll;
            if (cpos >= 0 && cpos <= vis) {
                int cx = tx0 + cpos * 8;
                g.draw_vline(cx, ay + 2, m_h - 4, 0xDDDDDDu);
            }
        }
    }
}

/* ── on_event ──────────────────────────────────────────────────────────── */

bool TextInput::on_event(const Event& e, int wx, int wy)
{
    if (e.type == EVT_MOUSE_BUTTON && (e.mouse.buttons & 0x01)) {
        if (hit_test(e.mouse.x, e.mouse.y, wx, wy)) {
            /* Click inside: gain focus, position cursor at click */
            m_focused = true;
            int rel = e.mouse.x - (wx + m_x + TEXT_MARGIN);
            int col = rel / 8 + m_scroll;
            int len = slen(m_text);
            m_cursor = (col < 0) ? 0 : (col > len) ? len : col;
            update_scroll();
            return true;
        }
    }
    return false;
}

/* ── handle_char ───────────────────────────────────────────────────────── */

bool TextInput::handle_char(int c)
{
    if (!m_focused) return false;

    if (c == KEY_ESC) {
        m_focused = false;
        return true;
    }

    if (c == '\r' || c == '\n') {
        if (m_on_return) m_on_return(m_ctx, m_text);
        return true;
    }

    if (c == '\b' || c == 127) {   /* Backspace */
        if (m_cursor > 0) {
            int len = slen(m_text);
            for (int i = m_cursor - 1; i < len; i++) m_text[i] = m_text[i+1];
            m_cursor--;
            update_scroll();
            fire_change();
        }
        return true;
    }

    if (c == KEY_DELETE) {
        int len = slen(m_text);
        if (m_cursor < len) {
            for (int i = m_cursor; i < len; i++) m_text[i] = m_text[i+1];
            fire_change();
        }
        return true;
    }

    if (c == KEY_LEFT) {
        if (m_cursor > 0) { m_cursor--; update_scroll(); }
        return true;
    }

    if (c == KEY_RIGHT) {
        if (m_cursor < slen(m_text)) { m_cursor++; update_scroll(); }
        return true;
    }

    if (c == KEY_HOME) { m_cursor = 0; update_scroll(); return true; }
    if (c == KEY_END)  { m_cursor = slen(m_text); update_scroll(); return true; }

    /* Printable character: insert at cursor */
    if (c >= 32 && c < 127) {
        int len = slen(m_text);
        if (len < MAX_LEN) {
            for (int i = len; i >= m_cursor; i--) m_text[i+1] = m_text[i];
            m_text[m_cursor++] = (char)c;
            update_scroll();
            fire_change();
        }
        return true;
    }

    return false;
}

} /* namespace GUI */
