#ifndef WINDOW_HPP
#define WINDOW_HPP

#include "Widget.hpp"
#include "Theme.hpp"
#include "Greg/Greg.h"
#include "GUI/Widget.hpp"

/* ── Window: SerenityOS-style titled container ───────────────────────────────
   Geometry (vertical layout, from top):
     y + 0 .. y+1          : outer 3D bevel top (2px)
     y + 2 .. y+TITLE_H-1  : title bar gradient (TITLE_H-2 px)
     y + TITLE_H            : client area begins
     y + h - BORDER_W .. h-1: bottom bevel                                  */

class Window : public Widget, public Greg::RefCounted<Window> {
public:
    static constexpr int TITLE_H  = 22;  /* 2px border + 18px gradient + 2px inner */
    static constexpr int BORDER_W = 2;   /* outer bevel width */
    static constexpr int CLOSE_W  = 16;  /* close button size (px, square) */
    static constexpr int RESIZE_W = 6;   /* resize handle zone width (px) */
    static constexpr int MIN_W    = 160; /* minimum window width */
    static constexpr int MIN_H    = 80;  /* minimum window height */

    /* Resize edge identifiers */
    static constexpr int RESIZE_NONE = 0;
    static constexpr int RESIZE_S    = 1;
    static constexpr int RESIZE_E    = 2;
    static constexpr int RESIZE_W_E  = 3;  /* W edge (W clashes with BORDER_W) */
    static constexpr int RESIZE_SE   = 4;
    static constexpr int RESIZE_SW   = 5;

    Window() : Widget(0, 0, 0, 0),
               bg_(Theme::WIN_BG_PURE), focused_(false),
               close_req_(false), dragging_(false),
               drag_ox_(0), drag_oy_(0),
               resizing_(false), resize_edge_(RESIZE_NONE),
               resize_ox_(0), resize_oy_(0),
               resize_x0_(0), resize_y0_(0),
               resize_w0_(0), resize_h0_(0),
               maximized_(false),
               restore_x_(0), restore_y_(0), restore_w_(0), restore_h_(0),
               last_title_click_j_(0),
               last_title_click_x_(0), last_title_click_y_(0) {}

    void setup(int x, int y, int w, int h,
               const Greg::String& title, unsigned int bg = Theme::WIN_BG_PURE);
    void setup(int x, int y, int w, int h,
               const char* title, unsigned int bg = Theme::WIN_BG_PURE);

    void draw() override;
    bool on_event(const Event& e) override;

    const Greg::String& title() const { return title_; }

    /* Client area geometry — inside bevel and below title bar */
    int client_x() const { return _x + BORDER_W; }
    int client_y() const { return _y + TITLE_H; }
    int client_w() const { return _w - 2 * BORDER_W; }
    int client_h() const { return _h - TITLE_H - BORDER_W; }

    bool focused()       const { return focused_; }
    void set_focused(bool f)   { focused_ = f; }

    /* Maximize/restore + minimize (title-bar buttons & double-click).
       maximize saves the current geometry, restore puts it back.
       minimize just hides the window (taskbar button restores it).  */
    bool maximized()     const { return maximized_; }
    void toggle_maximize();
    void minimize();

    bool close_requested() const { return close_req_; }
    void reset_close()           { close_req_ = false; }
    void request_close()         { close_req_ = true; }

    /* Called by WindowManager just before the window is removed.
       Subclasses (e.g. TerminalWindow) override to release resources.   */
    virtual void on_removed() {}

    /* Offer a processed keycode to this window (from wm_handle_key bridge).
       Base implementation forwards to the focused child widget (if any).
       Subclasses override to handle their own keys (and may call super). */
    virtual bool handle_char(int c);

    /* ── LibGUI child widgets ─────────────────────────────────────────── */
    void add_widget(Greg::RefPtr<GUI::Widget> w);

protected:
    unsigned int bg_;

private:
    Greg::String title_;
    bool         focused_, close_req_, dragging_;
    int          drag_ox_, drag_oy_;
    bool         resizing_;
    int          resize_edge_;
    int          resize_ox_, resize_oy_;
    int          resize_x0_, resize_y0_, resize_w0_, resize_h0_;

    /* Maximize state — restore_* holds the pre-maximize geometry. */
    bool          maximized_;
    int           restore_x_, restore_y_, restore_w_, restore_h_;

    /* Title-bar double-click tracking (jiffies + position of last click). */
    unsigned long last_title_click_j_;
    int           last_title_click_x_, last_title_click_y_;

    /* Returns one of the RESIZE_* constants based on mouse position. */
    int edge_at(int mx, int my) const;

    /* Title-bar button geometry — shared by draw() and on_event() so their
       hit-tests never drift.  Buttons are CLOSE_W squares laid out from the
       right: [X] close, [ ] maximize, [ _ ] minimize.                      */
    int title_btn_y() const { return _y + BORDER_W + (TITLE_H - BORDER_W - CLOSE_W) / 2; }
    int close_btn_x() const { return _x + _w - BORDER_W - 2 - CLOSE_W; }
    int max_btn_x()   const { return close_btn_x() - (CLOSE_W + 2); }
    int min_btn_x()   const { return max_btn_x()   - (CLOSE_W + 2); }

    Greg::Vector<Greg::RefPtr<GUI::Widget>> m_widgets;
};

#endif /* WINDOW_HPP */
