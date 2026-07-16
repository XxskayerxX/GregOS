/* TextViewerWindow — read-only paged text viewer for GregOS.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/TextViewerWindow.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

/* ── set_text ─────────────────────────────────────────────────────────── */

void TextViewerWindow::set_text(const char* text, int len)
{
    if (len >= MAX_TEXT) len = MAX_TEXT - 1;
    for (int i = 0; i < len; ++i) m_text[i] = text[i];
    m_text[len] = '\0';
    m_len    = len;
    m_scroll = 0;
    count_lines();
}

void TextViewerWindow::count_lines()
{
    m_lines = 1;
    for (int i = 0; i < m_len; ++i)
        if (m_text[i] == '\n') ++m_lines;
}

int TextViewerWindow::visible_rows() const
{
    int h = client_h();
    return (h > 0) ? h / LH : 1;
}

/* ── draw ─────────────────────────────────────────────────────────────── */

void TextViewerWindow::draw()
{
    Window::draw();

    Graphics& g   = Graphics::instance();
    int x0        = client_x() + 4;
    int y0        = client_y() + 2;
    int max_chars = (client_w() - 8) / 8;   /* chars that fit per line */
    int vrows     = visible_rows();

    /* Parchment (vellum) client background — surface document. */
    g.fill_rect(client_x(), client_y(), client_w(), client_h(), Theme::VELLUM);

    /* Skip m_scroll lines, then render up to vrows */
    int line = 0;
    int i    = 0;

    /* Fast-forward to scroll offset */
    while (i < m_len && line < m_scroll) {
        if (m_text[i] == '\n') ++line;
        ++i;
    }

    /* Render visible lines */
    char linebuf[LINE_BUF + 1];
    int screen_row = 0;
    while (screen_row < vrows) {
        int ls  = i;
        while (i < m_len && m_text[i] != '\n') ++i;

        int ll = i - ls;
        if (ll > max_chars) ll = max_chars;
        for (int j = 0; j < ll; ++j) linebuf[j] = m_text[ls + j];
        linebuf[ll] = '\0';

        if (ll > 0)
            g.draw_str(x0, y0 + screen_row * LH, linebuf, Theme::VELLUM_INK, Theme::VELLUM);

        ++screen_row;
        if (i >= m_len) break;
        ++i;   /* skip '\n' */
    }

    /* Scroll indicator bottom-right: "[line/total]" */
    if (m_lines > vrows) {
        char sbuf[24]; int sp = 0;
        sbuf[sp++] = '[';
        /* append int */
        auto app = [&](int n) {
            char t[8]; int ti = 0;
            if (n == 0) { sbuf[sp++]='0'; return; }
            while (n > 0) { t[ti++]='0'+n%10; n/=10; }
            for (int k = ti-1; k >= 0; --k) sbuf[sp++] = t[k];
        };
        app(m_scroll + 1); sbuf[sp++] = '/'; app(m_lines);
        sbuf[sp++] = ']'; sbuf[sp] = '\0';

        int sx = client_x() + client_w() - sp * 8 - 4;
        int sy = client_y() + client_h() - LH - 2;
        g.draw_str(sx, sy, sbuf, Theme::GOLD_DIM, Theme::VELLUM);
    }
}

/* ── handle_char ─────────────────────────────────────────────────────── */

bool TextViewerWindow::handle_char(int c)
{
    int vrows  = visible_rows();
    int max_sc = m_lines - vrows;
    if (max_sc < 0) max_sc = 0;

    if (c == KEY_ESC || c == 'q') { request_close(); return true; }

    if (c == KEY_UP) {
        if (m_scroll > 0) { --m_scroll; return true; }
        return false;
    }
    if (c == KEY_DOWN) {
        if (m_scroll < max_sc) { ++m_scroll; return true; }
        return false;
    }
    if (c == KEY_PGUP) {
        m_scroll -= vrows;
        if (m_scroll < 0) m_scroll = 0;
        return true;
    }
    if (c == KEY_PGDN) {
        m_scroll += vrows;
        if (m_scroll > max_sc) m_scroll = max_sc;
        return true;
    }
    return false;
}
