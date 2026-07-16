#ifndef BROWSER_WINDOW_HPP
#define BROWSER_WINDOW_HPP

#include "Window.hpp"

/* ── BrowserWindow — "GregNet" native web browser ──────────────────────────
   Minimal fenetred HTTP/1.0 browser for GregOS.

   Layout (client area):
     y = 0 .. TOOLBAR_H-1                 toolbar: [<][>][O] address [GO]
     y = TOOLBAR_H .. h-STATUS_H-1        HTML render viewport (+ scrollbar)
     y = h-STATUS_H .. h-1                status bar

   Networking is done exclusively through the C API in include/net.h.
   HTTPS is NOT supported (the stack has no TLS). Internal pages are served
   from the greg:// pseudo-scheme (greg://home, greg://about).             */

class BrowserWindow : public Window {
public:
    void init_browser();
    /* Public entry for the factory: navigate immediately to a URL. */
    void open_url(const char* url) { navigate(url, true); }

    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;
    void on_removed()             override;

    /* ── layout / geometry constants ─────────────────────────────────── */
    static constexpr int TOOLBAR_H = 28;
    static constexpr int STATUS_H  = 18;
    static constexpr int SB_W      = 14;   /* scrollbar width               */
    static constexpr int PAD       = 6;    /* content inner padding         */
    static constexpr int LH        = 18;   /* line height (font 16 + lead)  */
    static constexpr int CW        = 8;    /* char width (monospace 8x16)   */
    static constexpr int PARA_GAP  = 9;    /* extra gap between blocks      */
    static constexpr int LIST_IND  = 22;   /* list indent (px)              */
    static constexpr int QUOTE_IND = 20;   /* blockquote indent (px)        */

    /* ── capacities (fixed buffers — never overflow) ─────────────────── */
    static constexpr int TEXT_CAP  = 96 * 1024;  /* decoded visible text    */
    static constexpr int MAX_FRAGS = 4000;       /* positioned text runs    */
    static constexpr int MAX_LINKS = 256;        /* clickable link targets  */
    static constexpr int LINK_MAX  = 256;        /* max chars per link URL  */
    static constexpr int HIST_MAX  = 16;         /* back/forward stack      */
    static constexpr int URL_MAX   = 512;        /* URL buffer size         */

    /* A positioned run of same-styled text (content coordinates). */
    struct Frag {
        int          x, y, w;   /* x/y relative to content origin, w in px */
        int          off, len;  /* slice into the text arena               */
        unsigned int color;
        unsigned char flags;    /* FL_BOLD | FL_UL | FL_HR                  */
        short         link;     /* index into m_links[], or -1             */
    };

private:
    /* ── toolbar helper ──────────────────────────────────────────────── */
    struct TbRects {
        int back_x, fwd_x, rel_x, btn_w;
        int fld_x, fld_w;
        int go_x, go_w;
        int y, h;
    };
    TbRects tb_rects() const;

    /* ── rendering ───────────────────────────────────────────────────── */
    void draw_toolbar();
    void draw_status_bar();
    void draw_content();
    void draw_scrollbar();
    void draw_btn(int x, int y, int w, int h, const char* label, bool enabled);

    int  render_h()  const;
    int  content_w() const;   /* usable text width in pixels               */
    int  content_top() const; /* screen y of first text line               */
    int  max_scroll() const;

    /* ── HTML layout engine ──────────────────────────────────────────── */
    void layout(const char* src);          /* parse + word-wrap into frags */
    void reset_layout();
    void add_frag(int x, int y, int w, int off, int len,
                  unsigned int color, unsigned char flags, short link);
    void put_run(const char* s, int len);  /* emit raw glyphs at pen       */
    void emit_word(const char* w, int wl); /* flow-mode word with wrapping */
    void flush_word();
    void flush_frag();
    void recompute_pen();
    void recompute_indent();
    void line_break();
    void ensure_block(bool gap);
    void add_hr();
    void handle_tag(const char* name, bool closing,
                    const char* attrs, int attrlen);

    /* ── navigation ──────────────────────────────────────────────────── */
    void navigate(const char* url, bool push);
    void go_back();
    void go_forward();
    void push_history(const char* url);
    void free_body();
    void set_error_page(const char* title, const char* line1,
                        const char* line2, const char* line3);

    /* ── input ───────────────────────────────────────────────────────── */
    void handle_toolbar_click(int mx, int my);
    void handle_scroll_click(int my);
    int  link_at(int mx, int my) const;
    void update_hover(int mx, int my);
    void activate_link(int idx);
    void scroll_by(int dy);

    /* ── state: fetched document ─────────────────────────────────────── */
    char*        m_body       { nullptr };   /* kmalloc'd HTTP body         */
    const char*  m_last_src   { nullptr };   /* source used for last layout */
    int          m_layout_w   { 0 };         /* content_w() at layout time  */

    /* ── text arena + display list ───────────────────────────────────── */
    char  m_text[TEXT_CAP];
    int   m_text_len { 0 };
    Frag  m_frags[MAX_FRAGS];
    int   m_frag_count { 0 };
    int   m_content_h  { 0 };

    /* ── link table ──────────────────────────────────────────────────── */
    char  m_links[MAX_LINKS][LINK_MAX];
    int   m_link_count { 0 };

    /* ── parser pen / style state ────────────────────────────────────── */
    int   m_pen_x { 0 }, m_pen_y { 0 };
    int   m_indent { 0 };
    int   m_frag_x { 0 }, m_frag_start { 0 };
    bool  m_at_line_start { true };
    bool  m_any_content   { false };
    bool  m_gap_done      { false };
    bool  m_pre           { false };
    bool  m_in_head       { false };
    bool  m_in_title      { false };
    int   m_bold_depth    { 0 };
    int   m_dim_depth     { 0 };
    int   m_list_depth    { 0 };
    int   m_quote_depth   { 0 };
    int   m_heading       { 0 };
    int   m_cur_link      { -1 };
    /* captured style for the frag currently being accumulated */
    unsigned int  m_pen_color { 0 };
    unsigned char m_pen_flags { 0 };
    short m_pen_link { -1 };
    /* pending word buffer */
    char  m_word[LINK_MAX];
    int   m_word_len { 0 };

    /* ── navigation state ────────────────────────────────────────────── */
    char  m_url[URL_MAX]   {};
    char  m_addr[URL_MAX]  {};
    int   m_addr_len       { 0 };
    bool  m_addr_focus     { false };
    char  m_title[128]     {};
    char  m_status[192]    {};
    char  m_hover[URL_MAX] {};
    char  m_errbuf[1024]   {};
    int   m_scroll         { 0 };

    /* ── history ─────────────────────────────────────────────────────── */
    char  m_hist[HIST_MAX][URL_MAX];
    int   m_hist_count { 0 };
    int   m_hist_pos   { -1 };
};

extern "C" void open_browser_window(void);

#endif /* BROWSER_WINDOW_HPP */
