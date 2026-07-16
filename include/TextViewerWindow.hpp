#ifndef TEXTVIEWERWINDOW_HPP
#define TEXTVIEWERWINDOW_HPP

#include "Window.hpp"

/* ── TextViewerWindow: read-only paged text viewer ───────────────────────
   Displays a null-terminated text buffer line by line in a white client
   area. Supports scrolling with KEY_UP / KEY_DOWN / PAGE_UP / PAGE_DOWN.
   Created by FileManagerWindow when the user opens a VFS file.           */

class TextViewerWindow : public Window {
public:
    static constexpr int MAX_TEXT   = 4096;
    static constexpr int LINE_BUF   = 128;  /* max visible chars per line */
    static constexpr int LH         = 17;   /* line height in pixels */

    TextViewerWindow() = default;

    /* Copy text into the internal buffer and reset the scroll position. */
    void set_text(const char* text, int len);

    void draw() override;
    bool handle_char(int c) override;

private:
    char m_text[MAX_TEXT];
    int  m_len    { 0 };
    int  m_lines  { 0 };   /* total logical line count */
    int  m_scroll { 0 };   /* index of top visible line */

    void count_lines();
    int  visible_rows() const;
};

#endif /* TEXTVIEWERWINDOW_HPP */
