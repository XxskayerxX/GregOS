#ifndef TEXT_EDITOR_WINDOW_HPP
#define TEXT_EDITOR_WINDOW_HPP

#include "Window.hpp"

/* ── TextEditorWindow ──────────────────────────────────────────────────────
   Multi-line text editor backed by a VFS file.

   Layout:
     title bar (Window)
     client area:
       y = 0 .. client_h()-STATUS_H-1   editing area
       y = client_h()-STATUS_H .. end   status bar: "Ln col | [modified]"

   Cursor model: flat index into m_text[MAX_TEXT].
   Ctrl+S saves.  Esc / close button quits (prompts if modified).           */

class TextEditorWindow : public Window {
public:
    static constexpr int MAX_TEXT  = 4096;
    static constexpr int LH        = 16;   /* line height px               */
    static constexpr int CH        = 8;    /* char width px (fixed font)   */
    static constexpr int STATUS_H  = 18;   /* status bar height            */
    static constexpr int PAD_L     = 36;   /* left gutter (line numbers)   */
    static constexpr int PAD_R     = 4;
    static constexpr int PAD_T     = 4;

    /* Open file entry_id.  Call after setup().                             */
    void init_editor(int entry_id);

    void draw()             override;
    bool handle_char(int c) override;

private:
    /* ── text buffer ─────────────────────────────────────────────────────── */
    char m_text[MAX_TEXT];
    int  m_len      { 0 };
    int  m_cursor   { 0 };   /* byte index of insertion point               */
    bool m_modified { false };

    /* ── VFS ──────────────────────────────────────────────────────────────── */
    int  m_vfs_id   { -1 };

    /* ── scroll ───────────────────────────────────────────────────────────── */
    int  m_scroll_row { 0 };
    int  m_scroll_col { 0 };

    /* ── helpers ──────────────────────────────────────────────────────────── */
    int  total_rows()           const;
    int  row_of(int pos)        const;
    int  col_of(int pos)        const;
    int  row_start(int row)     const;
    int  row_end(int row)       const;   /* exclusive, points past '\n' or at m_len */
    int  row_len(int row)       const;   /* chars in row, excl. '\n'                */
    int  visible_rows()         const;
    int  visible_cols()         const;
    void ensure_cursor_visible();
    void insert_char(char c);
    void delete_before();        /* Backspace */
    void delete_after();         /* Delete key */
    void save();
    void draw_status_bar();
    void draw_gutter(int screen_row, int logical_row);
};

extern "C" void open_text_editor(int vfs_id);

#endif /* TEXT_EDITOR_WINDOW_HPP */
