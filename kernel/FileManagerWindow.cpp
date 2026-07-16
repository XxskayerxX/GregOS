/* FileManagerWindow — graphical file browser with toolbar.
   Supports: open (text/image), create file/dir, delete, rename (F2).
   Freestanding: no libc.                                                   */

#include "../include/FileManagerWindow.hpp"
#include "../include/TextViewerWindow.hpp"
#include "../include/TextEditorWindow.hpp"
#include "../include/ImageViewerWindow.hpp"
#include "../include/WindowManager.hpp"
#include "../include/Graphics.hpp"
#include "../include/Theme.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"
#include "../include/tty.h"

extern "C" volatile unsigned long jiffies;

/* ── init_files ──────────────────────────────────────────────────────────── */

void FileManagerWindow::init_files(int dir_id, int parent_id)
{
    m_dir_id     = dir_id;
    m_parent_id  = parent_id;
    m_has_dotdot = (parent_id >= 0);
    m_selected   = 0;
    m_scroll     = 0;
    m_last_click_r = -1;

    int base = 0;
    if (m_has_dotdot) {
        VFSEntry& dd = m_entries[0];
        dd.type = VFS_TYPE_DIR; dd.id = parent_id; dd.parent_id = -1;
        dd.size = 0;
        dd.name[0]='.'; dd.name[1]='.'; dd.name[2]='\0';
        base = 1;
    }
    m_count = base + vfs_list_dir(dir_id, m_entries + base, MAX_ENTRIES - base);
}

/* ── draw helpers ────────────────────────────────────────────────────────── */

static int sfm_len(const char* s) { int n=0; while(s[n]) ++n; return n; }

void FileManagerWindow::draw_toolbar_btn(int x, int y, int w, int h,
                                         const char* label, bool active)
{
    Graphics& g = Graphics::instance();
    unsigned int bg = active ? Theme::AMBER_DEEP : Theme::BTN_FACE;
    unsigned int fg = active ? Theme::AMBER_HI   : Theme::AMBER;
    g.fill_rect(x, y, w, h, bg);
    g.draw_hline(x,     y,     w, active ? Theme::GOLD_DIM : Theme::BEVEL_OUTER_LT);
    g.draw_vline(x,     y,     h, active ? Theme::GOLD_DIM : Theme::BEVEL_OUTER_LT);
    g.draw_hline(x,     y+h-1, w, active ? Theme::GOUFFRE  : Theme::BEVEL_OUTER_DK);
    g.draw_vline(x+w-1, y,     h, active ? Theme::GOUFFRE  : Theme::BEVEL_OUTER_DK);
    int llen = sfm_len(label);
    g.draw_str(x + (w - llen*8)/2, y + (h-8)/2, label, fg, GFX_TRANSPARENT);
}

void FileManagerWindow::draw_toolbar()
{
    Graphics& g = Graphics::instance();
    int tx = client_x(), ty = client_y(), tw = client_w();

    g.fill_rect(tx, ty, tw, TOOLBAR_H, Theme::WIN_BG);
    g.draw_hline(tx, ty + TOOLBAR_H - 1, tw, Theme::BEVEL_OUTER_DK);

    bool cf = (m_mode == MODE_CREATE_FILE);
    bool cd = (m_mode == MODE_CREATE_DIR);
    bool rn = (m_mode == MODE_RENAME);

    draw_toolbar_btn(tx+4,   ty+4, 88, 22, "+ Fichier", cf);
    draw_toolbar_btn(tx+96,  ty+4, 88, 22, "+ Dossier", cd);
    draw_toolbar_btn(tx+188, ty+4, 88, 22, "Renommer",  rn);
    draw_toolbar_btn(tx+280, ty+4, 88, 22, "Supprimer", false);

    /* Path label */
    const char* path = m_has_dotdot ? "/.." : "/";
    g.draw_str(tx + tw - sfm_len(path)*8 - 8, ty + 7, path, 0x666666u, GFX_TRANSPARENT);
}

void FileManagerWindow::draw_input_bar()
{
    if (!m_mode) return;
    Graphics& g = Graphics::instance();
    int bx = client_x(), by = client_y() + TOOLBAR_H, bw = client_w();

    g.fill_rect(bx, by, bw, INPUT_H, 0x1A1A2Au);
    g.draw_hline(bx, by + INPUT_H - 1, bw, Theme::BEVEL_OUTER_DK);

    const char* prompt =
        (m_mode == MODE_CREATE_FILE) ? " Nouveau fichier: " :
        (m_mode == MODE_CREATE_DIR)  ? " Nouveau dossier: " :
                                       " Renommer: ";
    int pl = sfm_len(prompt);
    g.draw_str(bx + 2, by + (INPUT_H-8)/2, prompt, 0x8888CCu, GFX_TRANSPARENT);

    /* Input field */
    int fx = bx + 2 + pl*8, fy = by + 3;
    int fw = bw - pl*8 - 100, fh = INPUT_H - 6;
    g.fill_rect(fx, fy, fw, fh, 0x10101Au);
    g.draw_rect(fx, fy, fw, fh, 0x444466u);
    g.draw_str(fx + 4, fy + (fh-8)/2, m_input_buf, 0xDDDDDDu, GFX_TRANSPARENT);
    /* Cursor */
    if ((jiffies / 50) % 2 == 0) {
        int cx = fx + 4 + m_input_len * 8;
        g.draw_vline(cx, fy + 2, fh - 4, 0xCCCCCCu);
    }

    /* Hint */
    const char* hint = "\x1C OK  Esc annuler";
    g.draw_str(bx + bw - sfm_len(hint)*8 - 8, by + (INPUT_H-8)/2,
               hint, Theme::ASH, GFX_TRANSPARENT);
}

void FileManagerWindow::draw_entry(int screen_row, int entry_idx, bool selected)
{
    Graphics& g = Graphics::instance();
    int ry = list_y() + screen_row * ROW_H;
    int rx = client_x(), rw = client_w() - SB_W;

    unsigned int bg = selected ? Theme::AMBER_DEEP : Theme::WIN_BG_PURE;
    unsigned int fg = selected ? Theme::AMBER_HI
        : (m_entries[entry_idx].type == VFS_TYPE_DIR ? Theme::FG_FOLDER : Theme::FG_PRIMARY);

    g.fill_rect(rx, ry, rw, ROW_H, bg);
    const char* icon = (m_entries[entry_idx].type == VFS_TYPE_DIR) ? "[D] " : "[F] ";
    g.draw_str(rx + 4,  ry + (ROW_H-16)/2, icon,                fg, GFX_TRANSPARENT);
    g.draw_str(rx + 36, ry + (ROW_H-16)/2, m_entries[entry_idx].name, fg, GFX_TRANSPARENT);

    /* File size for regular files */
    if (!selected && m_entries[entry_idx].type == VFS_TYPE_FILE && m_entries[entry_idx].size > 0) {
        char sbuf[16]; int sv = m_entries[entry_idx].size, si = 0;
        if (sv == 0) { sbuf[si++]='0'; }
        else { char t[12]; int ti=0; while(sv>0){t[ti++]='0'+sv%10;sv/=10;} for(int k=ti-1;k>=0;--k) sbuf[si++]=t[k]; }
        sbuf[si++]='B'; sbuf[si]='\0';
        g.draw_str(rx + rw - si*8 - 4, ry + (ROW_H-16)/2, sbuf, Theme::ASH, GFX_TRANSPARENT);
    }
}

void FileManagerWindow::draw_scrollbar()
{
    int vis = list_h() / ROW_H;
    if (m_count <= vis) return;
    Graphics& g = Graphics::instance();
    int sb_x = client_x() + client_w() - SB_W;
    int sb_y = list_y(), sb_h = list_h();

    g.fill_rect(sb_x, sb_y, SB_W, sb_h, Theme::WIN_BG);
    g.draw_vline(sb_x, sb_y, sb_h, Theme::BEVEL_INNER_DK);

    int th = sb_h * vis / m_count; if (th < 12) th = 12;
    int ty2 = sb_y + (sb_h - th) * m_scroll / (m_count - vis + 1);
    g.fill_rect(sb_x+2, ty2, SB_W-2, th, Theme::BTN_FACE);
    g.draw_hline(sb_x+2, ty2,      SB_W-2, Theme::BEVEL_OUTER_LT);
    g.draw_vline(sb_x+2, ty2,      th,     Theme::BEVEL_OUTER_LT);
    g.draw_hline(sb_x+2, ty2+th-1, SB_W-2, Theme::BEVEL_OUTER_DK);
    g.draw_vline(sb_x+SB_W-1, ty2, th,     Theme::BEVEL_OUTER_DK);
}

/* ── draw ────────────────────────────────────────────────────────────────── */

void FileManagerWindow::draw()
{
    Window::draw();
    Graphics& g = Graphics::instance();
    int vis = list_h() / ROW_H;

    if (m_selected - m_scroll >= vis) m_scroll = m_selected - vis + 1;
    if (m_selected < m_scroll)        m_scroll = m_selected;
    if (m_scroll < 0) m_scroll = 0;

    /* File list background */
    g.fill_rect(client_x(), list_y(), client_w(), list_h(), Theme::WIN_BG_PURE);

    for (int i = m_scroll; i < m_count && i < m_scroll + vis; ++i)
        draw_entry(i - m_scroll, i, i == m_selected);

    draw_scrollbar();
    g.draw_vline(client_x() + client_w() - SB_W, list_y(), list_h(), Theme::BEVEL_INNER_DK);

    draw_toolbar();
    if (m_mode) draw_input_bar();
}

/* ── actions ─────────────────────────────────────────────────────────────── */

void FileManagerWindow::open_selected()
{
    if (m_selected < 0 || m_selected >= m_count) return;
    VFSEntry& e = m_entries[m_selected];

    if (e.type == VFS_TYPE_DIR) {
        if (m_has_dotdot && m_selected == 0)
            init_files(m_parent_id, -1);
        else
            init_files(e.id, m_dir_id);
        return;
    }

    char content[TextViewerWindow::MAX_TEXT];
    int  len = vfs_read_file(e.id, content, TextViewerWindow::MAX_TEXT);
    if (len < 0) return;

    int tx = _x + 30, ty2 = _y + 30;
    int sw = gfx_width(), sh = gfx_height();
    if (tx + 600 > sw) tx = sw - 600;
    if (ty2 + 420 > sh) ty2 = sh - 420;
    if (tx < 0) tx = 0;
    if (ty2 < 0) ty2 = 0;

    int nlen = sfm_len(e.name);
    bool is_bmp = nlen >= 4
        && e.name[nlen-4]=='.'
        && (e.name[nlen-3]=='b'||e.name[nlen-3]=='B')
        && (e.name[nlen-2]=='m'||e.name[nlen-2]=='M')
        && (e.name[nlen-1]=='p'||e.name[nlen-1]=='P');

    if (is_bmp) {
        auto iv = Greg::make_ref<ImageViewerWindow>();
        iv->setup(tx, ty2, 360, 300, e.name, Theme::WIN_BG);
        iv->init_image((const unsigned char*)content, len);
        iv->set_focused(true); set_focused(false);
        WindowManager::instance().add_window(Greg::move(iv));
    } else {
        /* Open in editor; size/position computed above */
        int ew = 680, eh = 460;
        int ex = tx, ey = ty2;
        if (ex + ew > gfx_width())  ex = gfx_width()  - ew;
        if (ey + eh > gfx_height()) ey = gfx_height() - eh;
        if (ex < 0) ex = 0;
        if (ey < 0) ey = 0;
        auto ed = Greg::make_ref<TextEditorWindow>();
        ed->setup(ex, ey, ew, eh, e.name, 0x12121Eu);
        ed->init_editor(e.id);
        ed->set_focused(true); set_focused(false);
        WindowManager::instance().add_window(Greg::move(ed));
    }
}

void FileManagerWindow::delete_selected()
{
    if (m_selected < 0 || m_selected >= m_count) return;
    if (m_has_dotdot && m_selected == 0) return;   /* can't delete ".." */
    vfs_delete(m_entries[m_selected].id);
    init_files(m_dir_id, m_parent_id);
    if (m_selected >= m_count) m_selected = m_count - 1;
    if (m_selected < 0) m_selected = 0;
}

void FileManagerWindow::commit_input()
{
    if (m_input_len == 0) { cancel_input(); return; }

    if (m_mode == MODE_CREATE_FILE)
        vfs_create_file(m_input_buf, m_dir_id);
    else if (m_mode == MODE_CREATE_DIR)
        vfs_create_dir(m_input_buf, m_dir_id);
    else if (m_mode == MODE_RENAME && m_selected >= 0 && m_selected < m_count)
        vfs_rename(m_entries[m_selected].id, m_input_buf);

    cancel_input();
    init_files(m_dir_id, m_parent_id);
}

void FileManagerWindow::cancel_input()
{
    m_mode = MODE_NORMAL;
    m_input_buf[0] = '\0';
    m_input_len = 0;
}

/* ── on_event ────────────────────────────────────────────────────────────── */

bool FileManagerWindow::on_event(const Event& e)
{
    if (e.type == EVT_MOUSE_BUTTON) {
        bool pressed = (e.mouse.buttons & 0x01) != 0;
        int  mx = e.mouse.x, my = e.mouse.y;

        if (!pressed)        return Window::on_event(e);
        if (!hit_test(mx,my)) return false;
        if (my < client_y()) return Window::on_event(e);

        /* ── Toolbar clicks ───────────────────────────── */
        if (my < client_y() + TOOLBAR_H) {
            int tx = client_x(), ty = client_y();
            /* [+ Fichier] */
            if (mx >= tx+4 && mx < tx+92) {
                m_mode = (m_mode == MODE_CREATE_FILE) ? MODE_NORMAL : MODE_CREATE_FILE;
                m_input_buf[0]='\0'; m_input_len=0;
                return true;
            }
            /* [+ Dossier] */
            if (mx >= tx+96 && mx < tx+184) {
                m_mode = (m_mode == MODE_CREATE_DIR) ? MODE_NORMAL : MODE_CREATE_DIR;
                m_input_buf[0]='\0'; m_input_len=0;
                return true;
            }
            /* [Renommer] */
            if (mx >= tx+188 && mx < tx+276) {
                if (m_selected >= 0 && m_selected < m_count &&
                    !(m_has_dotdot && m_selected == 0)) {
                    m_mode = (m_mode == MODE_RENAME) ? MODE_NORMAL : MODE_RENAME;
                    /* Pre-fill with current name */
                    m_input_len = 0;
                    const char* cur = m_entries[m_selected].name;
                    while (*cur && m_input_len < (int)sizeof(m_input_buf)-1)
                        m_input_buf[m_input_len++] = *cur++;
                    m_input_buf[m_input_len] = '\0';
                } else {
                    cancel_input();
                }
                (void)ty;
                return true;
            }
            /* [Supprimer] */
            if (mx >= tx+280 && mx < tx+368) {
                cancel_input();
                delete_selected();
                return true;
            }
            return true;
        }

        /* ── Input bar click (cancel if clicked outside field) ─── */
        if (m_mode && my < client_y() + TOOLBAR_H + INPUT_H) {
            return true;  /* absorb — keyboard still handles it */
        }

        /* ── File list click ──────────────────────────── */
        int rw = client_w() - SB_W;
        if (mx < client_x() + rw) {
            int row = (my - list_y()) / ROW_H + m_scroll;
            if (row >= 0 && row < m_count) {
                if (row == m_last_click_r && jiffies - m_last_click_j < 50u) {
                    m_last_click_r = -1;
                    open_selected();
                } else {
                    m_selected     = row;
                    m_last_click_r = row;
                    m_last_click_j = jiffies;
                }
            }
        }
        return true;
    }

    if (e.type == EVT_MOUSE_MOVE) return Window::on_event(e);
    return false;
}

/* ── handle_char ─────────────────────────────────────────────────────────── */

bool FileManagerWindow::handle_char(int c)
{
    /* Input mode: intercept all keys */
    if (m_mode != MODE_NORMAL) {
        if (c == '\r' || c == '\n')       { commit_input(); return true; }
        if (c == KEY_ESC)                 { cancel_input(); return true; }
        if ((c == '\b' || c == 127) && m_input_len > 0) {
            m_input_buf[--m_input_len] = '\0'; return true;
        }
        if (c >= 32 && c < 127 && m_input_len < (int)sizeof(m_input_buf)-1) {
            m_input_buf[m_input_len++] = (char)c;
            m_input_buf[m_input_len]   = '\0';
            return true;
        }
        return true;   /* consume everything else too */
    }

    /* Normal navigation */
    if (c == KEY_UP   && m_selected > 0)           { --m_selected; return true; }
    if (c == KEY_DOWN && m_selected < m_count - 1) { ++m_selected; return true; }
    if (c == KEY_PGUP) { m_selected -= 8; if (m_selected<0) m_selected=0; return true; }
    if (c == KEY_PGDN) { m_selected += 8; if (m_selected>=m_count) m_selected=m_count-1; return true; }
    if (c == KEY_HOME) { m_selected = 0;           return true; }
    if (c == KEY_END)  { m_selected = m_count - 1; return true; }
    if (c == '\n' || c == '\r')                    { open_selected(); return true; }
    if (c == KEY_DELETE)                           { delete_selected(); return true; }
    if (c == KEY_ESC || c == 'q')                  { request_close();  return true; }

    /* F2 → rename (scancode 0x3C = F2, but handle_char gets KEY codes;
       use 'r' as a fallback keyboard shortcut for rename)               */
    if (c == 'r' || c == 'R') {
        if (m_selected >= 0 && m_selected < m_count &&
            !(m_has_dotdot && m_selected == 0)) {
            m_mode = MODE_RENAME;
            m_input_len = 0;
            const char* cur = m_entries[m_selected].name;
            while (*cur && m_input_len < (int)sizeof(m_input_buf)-1)
                m_input_buf[m_input_len++] = *cur++;
            m_input_buf[m_input_len] = '\0';
        }
        return true;
    }

    if (c == 'n' || c == 'N') {
        m_mode = MODE_CREATE_FILE;
        m_input_buf[0] = '\0'; m_input_len = 0;
        return true;
    }
    if (c == 'd' || c == 'D') {
        m_mode = MODE_CREATE_DIR;
        m_input_buf[0] = '\0'; m_input_len = 0;
        return true;
    }

    return false;
}

/* ── factory ─────────────────────────────────────────────────────────────── */

extern "C" void open_file_manager(void)
{
    auto fm = Greg::make_ref<FileManagerWindow>();
    fm->setup(50, 50, 480, 370, "Gestionnaire de Fichiers", Theme::WIN_BG);
    fm->init_files(0, -1);
    WindowManager::instance().add_window(Greg::move(fm));
}
