#ifndef GUI_TEXTINPUT_HPP
#define GUI_TEXTINPUT_HPP

/* GUI::TextInput — single-line editable text field.
   Gains keyboard focus on mouse click; loses it on Escape.
   Fires m_on_return(ctx, text) when Enter is pressed.

   Display: sunken 3D bevel, dark background, blinking cursor when focused.
   Long text scrolls horizontally so the cursor stays visible.             */

#include "Widget.hpp"

namespace GUI {

class TextInput : public Widget {
public:
    static constexpr int MAX_LEN = 127;

    /* on_return: called when the user presses Enter.
       ctx:       arbitrary user pointer forwarded to on_return.
       on_change: called on every character change (optional).             */
    TextInput(int x, int y, int w, int h,
              void (*on_return)(void* ctx, const char* text) = nullptr,
              void* ctx = nullptr,
              void (*on_change)(void* ctx, const char* text) = nullptr);

    void draw(Graphics& g, int wx, int wy) override;
    bool on_event(const Event& e, int wx, int wy) override;
    bool handle_char(int c) override;

    const char* text()  const { return m_text; }
    int         cursor() const { return m_cursor; }

    void set_text(const char* t);
    void clear();
    void set_placeholder(const char* s);

    void set_on_return(void (*fn)(void*, const char*), void* ctx = nullptr)
        { m_on_return = fn; m_ctx = ctx; }

private:
    char m_text[MAX_LEN + 1] {};
    char m_placeholder[48]   {};
    int  m_cursor            { 0 };   /* insert position in m_text */
    int  m_scroll            { 0 };   /* first visible char index */

    void (*m_on_return)(void*, const char*) { nullptr };
    void (*m_on_change)(void*, const char*) { nullptr };
    void*  m_ctx { nullptr };

    void update_scroll();
    void fire_change();
    static int slen(const char* s) { int n=0; while(s[n]) ++n; return n; }
};

} /* namespace GUI */

#endif /* GUI_TEXTINPUT_HPP */
