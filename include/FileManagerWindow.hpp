#ifndef FILE_MANAGER_WINDOW_HPP
#define FILE_MANAGER_WINDOW_HPP

#include "Window.hpp"
#include "vfs.h"

/* ── FileManagerWindow ─────────────────────────────────────────────────────
   File browser with toolbar: create file/dir, delete, rename (F2).

   Layout (client area):
     y = 0 .. TOOLBAR_H-1        toolbar: [+ Fichier] [+ Dossier] [Supprimer]
     y = TOOLBAR_H .. +INPUT_H-1 inline input bar (only in CREATE/RENAME mode)
     y = list_y() ..             file list with scrollbar                     */

class FileManagerWindow : public Window {
public:
    static constexpr int MAX_ENTRIES = 64;
    static constexpr int ROW_H       = 20;
    static constexpr int TOOLBAR_H   = 30;
    static constexpr int INPUT_H     = 28;
    static constexpr int SB_W        = 14;

    void init_files(int dir_id = 0, int parent_id = -1);

    void draw()             override;
    bool on_event(const Event& e) override;
    bool handle_char(int c) override;

private:
    /* ── layout helpers ─────────────────────────────────────────────── */
    int list_y() const { return client_y() + TOOLBAR_H + (m_mode ? INPUT_H : 0); }
    int list_h() const { return client_h() - TOOLBAR_H - (m_mode ? INPUT_H : 0); }

    /* ── drawing ─────────────────────────────────────────────────────── */
    void draw_entry(int screen_row, int entry_idx, bool selected);
    void draw_scrollbar();
    void draw_toolbar();
    void draw_input_bar();
    void draw_toolbar_btn(int x, int y, int w, int h,
                          const char* label, bool active);

    /* ── actions ─────────────────────────────────────────────────────── */
    void open_selected();
    void delete_selected();
    void commit_input();          /* create or rename from m_input_buf */
    void cancel_input();

    /* ── file list ───────────────────────────────────────────────────── */
    VFSEntry      m_entries[MAX_ENTRIES];
    int           m_count       { 0 };
    int           m_selected    { 0 };
    int           m_scroll      { 0 };
    int           m_dir_id      { 0 };
    int           m_parent_id   { -1 };
    bool          m_has_dotdot  { false };

    /* ── inline input ────────────────────────────────────────────────── */
    static constexpr int MODE_NORMAL      = 0;
    static constexpr int MODE_CREATE_FILE = 1;
    static constexpr int MODE_CREATE_DIR  = 2;
    static constexpr int MODE_RENAME      = 3;

    int  m_mode      { MODE_NORMAL };
    char m_input_buf[VFS_MAX_NAME] {};
    int  m_input_len { 0 };

    /* ── double-click ────────────────────────────────────────────────── */
    unsigned long m_last_click_j { 0 };
    int           m_last_click_r { -1 };
};

extern "C" void open_file_manager(void);

#endif /* FILE_MANAGER_WINDOW_HPP */
