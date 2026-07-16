#ifndef GUI_LABEL_HPP
#define GUI_LABEL_HPP

/* GUI::Label — static text widget.
   Renders one line of text inside its bounding rect.
   Alignment: 0=left, 1=center, 2=right.             */

#include "Widget.hpp"

namespace GUI {

class Label : public Widget {
public:
    static constexpr int MAX_TEXT = 64;
    static constexpr int ALIGN_LEFT   = 0;
    static constexpr int ALIGN_CENTER = 1;
    static constexpr int ALIGN_RIGHT  = 2;

    Label(int x, int y, int w, int h,
          const char* text,
          unsigned int color = 0xCCCCCCu,
          int align = ALIGN_LEFT);

    void draw(Graphics& g, int wx, int wy) override;

    void set_text(const char* t);
    void set_color(unsigned int c) { m_color = c; }
    void set_align(int a)          { m_align = a; }

    const char* text() const { return m_text; }

private:
    char         m_text[MAX_TEXT];
    unsigned int m_color;
    int          m_align;

    static int slen(const char* s) { int n=0; while(s[n]) ++n; return n; }
};

} /* namespace GUI */

#endif /* GUI_LABEL_HPP */
