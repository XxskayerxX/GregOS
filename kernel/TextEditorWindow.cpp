/* TextEditorWindow — multi-line text editor for GregOS.
   Freestanding: no libc, no exceptions.                                    */

#include "../include/TextEditorWindow.hpp"
#include "../include/Graphics.hpp"
#include "../include/WindowManager.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/vfs.h"

/* ── itoa helper ──────────────────────────────────────────────────────────── */

static int tew_itoa(int n, char* buf)
{
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    char tmp[12]; int ti = 0;
    while (n > 0) { tmp[ti++] = '0' + n % 10; n /= 10; }
    for (int k = 0; k < ti; ++k) buf[k] = tmp[ti - 1 - k];
    buf[ti] = '\0';
    return ti;
}

static int tew_len(const char* s) { int n=0; while(s[n]) ++n; return n; }

/* ── init_editor ─────────────────────────────────────────────────────────── */

void TextEditorWindow::init_editor(int entry_id)
{
    m_vfs_id    = entry_id;
    m_modified  = false;
    m_cursor    = 0;
    m_scroll_row = 0;
    m_scroll_col = 0;

    int len = vfs_read_file(entry_id, m_text, MAX_TEXT);
    m_len = (len >= 0) ? len : 0;
    m_text[m_len] = '\0';
}

/* ── row/col helpers ─────────────────────────────────────────────────────── */

int TextEditorWindow::total_rows() const
{
    int r = 1;
    for (int i = 0; i < m_len; ++i)
        if (m_text[i] == '\n') ++r;
    return r;
}

int TextEditorWindow::row_of(int pos) const
{
    int r = 0;
    for (int i = 0; i < pos && i < m_len; ++i)
        if (m_text[i] == '\n') ++r;
    return r;
}

int TextEditorWindow::col_of(int pos) const
{
    int c = 0;
    for (int i = 0; i < pos && i < m_len; ++i) {
        if (m_text[i] == '\n') c = 0;
        else ++c;
    }
    return c;
}

int TextEditorWindow::row_start(int row) const
{
    int r = 0;
    for (int i = 0; i < m_len; ++i) {
        if (r == row) return i;
        if (m_text[i] == '\n') ++r;
    }
    return m_len;
}

int TextEditorWindow::row_end(int row) const
{
    int s = row_start(row);
    int i = s;
    while (i < m_len && m_text[i] != '\n') ++i;
    return i;
}

int TextEditorWindow::row_len(int row) const
{
    return row_end(row) - row_start(row);
}

int TextEditorWindow::visible_rows() const
{
    int h = client_h() - STATUS_H - PAD_T;
    return (h > 0) ? h / LH : 1;
}

int TextEditorWindow::visible_cols() const
{
    int w = client_w() - PAD_L - PAD_R;
    return (w > 0) ? w / CH : 1;
}

void TextEditorWindow::ensure_cursor_visible()
{
    int crow = row_of(m_cursor);
    int ccol = col_of(m_cursor);
    int vrows = visible_rows();
    int vcols = visible_cols();

    if (crow < m_scroll_row) m_scroll_row = crow;
    if (crow >= m_scroll_row + vrows) m_scroll_row = crow - vrows + 1;
    if (m_scroll_row < 0) m_scroll_row = 0;

    if (ccol < m_scroll_col) m_scroll_col = ccol;
    if (ccol >= m_scroll_col + vcols) m_scroll_col = ccol - vcols + 1;
    if (m_scroll_col < 0) m_scroll_col = 0;
}

/* ── mutation ─────────────────────────────────────────────────────────────── */

void TextEditorWindow::insert_char(char c)
{
    if (m_len >= MAX_TEXT - 1) return;
    for (int i = m_len; i > m_cursor; --i)
        m_text[i] = m_text[i - 1];
    m_text[m_cursor] = c;
    ++m_len;
    ++m_cursor;
    m_text[m_len] = '\0';
    m_modified = true;
}

void TextEditorWindow::delete_before()
{
    if (m_cursor == 0) return;
    for (int i = m_cursor - 1; i < m_len - 1; ++i)
        m_text[i] = m_text[i + 1];
    --m_len;
    --m_cursor;
    m_text[m_len] = '\0';
    m_modified = true;
}

void TextEditorWindow::delete_after()
{
    if (m_cursor >= m_len) return;
    for (int i = m_cursor; i < m_len - 1; ++i)
        m_text[i] = m_text[i + 1];
    --m_len;
    m_text[m_len] = '\0';
    m_modified = true;
}

/* ── save ─────────────────────────────────────────────────────────────────── */

void TextEditorWindow::save()
{
    if (m_vfs_id < 0) return;
    vfs_write_file(m_vfs_id, m_text, m_len);
    m_modified = false;
}

/* ── draw ─────────────────────────────────────────────────────────────────── */

void TextEditorWindow::draw_gutter(int screen_row, int logical_row)
{
    Graphics& g = Graphics::instance();
    int x = client_x();
    int y = client_y() + PAD_T + screen_row * LH;
    g.fill_rect(x, y, PAD_L - 2, LH, 0x1C1C2Cu);

    char nbuf[8];
    tew_itoa(logical_row + 1, nbuf);
    int nw = tew_len(nbuf) * CH;
    g.draw_str(x + PAD_L - 2 - nw - 2, y + (LH - 8) / 2, nbuf, 0x5A5A8Au, 0x1C1C2Cu);
}

void TextEditorWindow::draw_status_bar()
{
    Graphics& g   = Graphics::instance();
    int sy        = client_y() + client_h() - STATUS_H;
    int sx        = client_x();
    int sw        = client_w();

    g.fill_rect(sx, sy, sw, STATUS_H, 0x1A1A2Au);
    g.draw_hline(sx, sy, sw, 0x3A3A5Au);

    int crow = row_of(m_cursor) + 1;
    int ccol = col_of(m_cursor) + 1;

    char buf[64]; int bp = 0;
    /* "Ln 12  Col 4  | [modified]" */
    buf[bp++]='L'; buf[bp++]='n'; buf[bp++]=' ';
    int lnw = tew_itoa(crow, buf + bp); bp += lnw;
    buf[bp++]=' '; buf[bp++]=' ';
    buf[bp++]='C'; buf[bp++]='o'; buf[bp++]='l'; buf[bp++]=' ';
    int cw = tew_itoa(ccol, buf + bp); bp += cw;
    if (m_modified) {
        buf[bp++]=' '; buf[bp++]=' '; buf[bp++]='[';
        buf[bp++]='m'; buf[bp++]='o'; buf[bp++]='d'; buf[bp++]='i';
        buf[bp++]='f'; buf[bp++]='i'; buf[bp++]='e'; buf[bp++]='d';
        buf[bp++]=']';
    }
    buf[bp] = '\0';
    g.draw_str(sx + 6, sy + (STATUS_H - 8) / 2, buf, 0xAAAACCu,
               (unsigned int)0x1A1A2A);

    /* Right: "Ctrl+S: save  Esc: quit" */
    const char* hint = m_modified ? "Ctrl+S: save  Esc: quit" : "Esc: quit";
    int hw = tew_len(hint) * CH;
    g.draw_str(sx + sw - hw - 6, sy + (STATUS_H - 8) / 2, hint,
               0x6666AAu, (unsigned int)0x1A1A2A);
}

extern "C" volatile unsigned long jiffies;

void TextEditorWindow::draw()
{
    Window::draw();
    Graphics& g  = Graphics::instance();
    int cx0      = client_x();
    int cy0      = client_y() + PAD_T;
    int vrows    = visible_rows();
    int vcols    = visible_cols();

    /* Client background */
    g.fill_rect(client_x(), client_y(), client_w(), client_h() - STATUS_H, 0x12121Eu);

    int crow = row_of(m_cursor);
    int ccol = col_of(m_cursor);

    /* Render each visible row */
    for (int sr = 0; sr < vrows; ++sr) {
        int lr = m_scroll_row + sr;
        int py = cy0 + sr * LH;

        draw_gutter(sr, lr);

        /* Highlight current line */
        if (lr == crow)
            g.fill_rect(cx0 + PAD_L, py, client_w() - PAD_L - PAD_R, LH, 0x1A1A30u);

        int rs = row_start(lr);
        int re = row_end(lr);
        int rl = re - rs;   /* chars on this logical row */

        /* Render characters, clipped to horizontal scroll */
        char linebuf[256];
        int li = 0;
        for (int ci = m_scroll_col; ci < m_scroll_col + vcols && ci < rl; ++ci)
            linebuf[li++] = m_text[rs + ci];
        linebuf[li] = '\0';

        if (li > 0)
            g.draw_str(cx0 + PAD_L, py + (LH - 8) / 2, linebuf, 0xDDDDEEu,
                       (unsigned int)(lr == crow ? 0x1A1A30u : 0x12121Eu));

        /* Cursor */
        if (lr == crow) {
            int vis_col = ccol - m_scroll_col;
            if (vis_col >= 0 && vis_col <= vcols) {
                bool blink = (jiffies / 50) % 2 == 0;
                if (blink) {
                    int curx = cx0 + PAD_L + vis_col * CH;
                    g.fill_rect(curx, py + 1, 2, LH - 2, 0xAAAAFFu);
                }
            }
        }

        if (lr >= total_rows() - 1) break;
    }

    /* Gutter separator */
    g.draw_vline(cx0 + PAD_L - 1, client_y(), client_h() - STATUS_H, 0x2A2A4Au);

    draw_status_bar();
}

/* ── handle_char ─────────────────────────────────────────────────────────── */

bool TextEditorWindow::handle_char(int c)
{
    if (c == KEY_CTRL_S) { save(); ensure_cursor_visible(); return true; }
    if (c == KEY_ESC) { request_close(); return true; }

    int crow   = row_of(m_cursor);
    int ccol   = col_of(m_cursor);
    int tr     = total_rows();

    if (c == KEY_UP) {
        if (crow > 0) {
            int rs = row_start(crow - 1);
            int rl = row_len(crow - 1);
            int nc = ccol < rl ? ccol : rl;
            m_cursor = rs + nc;
        }
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_DOWN) {
        if (crow < tr - 1) {
            int rs = row_start(crow + 1);
            int rl = row_len(crow + 1);
            int nc = ccol < rl ? ccol : rl;
            m_cursor = rs + nc;
        }
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_LEFT) {
        if (m_cursor > 0) --m_cursor;
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_RIGHT) {
        if (m_cursor < m_len) ++m_cursor;
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_HOME) {
        m_cursor = row_start(crow);
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_END) {
        m_cursor = row_end(crow);
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_PGUP) {
        int vr = visible_rows();
        crow = (crow >= vr) ? crow - vr : 0;
        int rs = row_start(crow);
        int rl = row_len(crow);
        int nc = ccol < rl ? ccol : rl;
        m_cursor = rs + nc;
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_PGDN) {
        int vr = visible_rows();
        crow = (crow + vr < tr) ? crow + vr : tr - 1;
        int rs = row_start(crow);
        int rl = row_len(crow);
        int nc = ccol < rl ? ccol : rl;
        m_cursor = rs + nc;
        ensure_cursor_visible(); return true;
    }
    if (c == '\b' || c == 127) {
        delete_before();
        ensure_cursor_visible(); return true;
    }
    if (c == KEY_DELETE) {
        delete_after();
        ensure_cursor_visible(); return true;
    }
    if (c == '\n' || c == '\r') {
        insert_char('\n');
        ensure_cursor_visible(); return true;
    }
    if (c >= 32 && c < 127) {
        insert_char((char)c);
        ensure_cursor_visible(); return true;
    }
    return false;
}

/* ── open_text_editor ────────────────────────────────────────────────────── */

extern "C" void open_text_editor(int vfs_id)
{
    /* Derive a title: read VFS entry name isn't exposed, use generic name  */
    auto w = Greg::make_ref<TextEditorWindow>();
    int sw = gfx_width(), sh = gfx_height();
    int ww = 680, wh = 460;
    int wx = (sw - ww) / 2, wy = (sh - wh) / 2;
    if (wx < 0) wx = 0;
    if (wy < 0) wy = 0;
    w->setup(wx, wy, ww, wh, "Text Editor", 0x12121Eu);
    w->init_editor(vfs_id);
    w->set_focused(true);
    WindowManager::instance().add_window(Greg::move(w));
}
