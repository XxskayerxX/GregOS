#ifndef PAINT_WINDOW_HPP
#define PAINT_WINDOW_HPP

#include "Window.hpp"

/* PaintWindow — "GregPaint" raster paint app.
   Tools: pencil, line, rect, filled rect, circle, flood-fill, eraser.
   Canvas is a kmalloc'd XRGB32 buffer blitted via Graphics::draw_image.     */

class PaintWindow : public Window {
public:
    ~PaintWindow();
    void draw()                   override;
    bool on_event(const Event& e) override;
    bool handle_char(int c)       override;
    void on_removed()             override;

    /* Called by the factory after setup() to allocate the canvas. */
    void init_canvas();

    static constexpr int CANVAS_W = 504;
    static constexpr int CANVAS_H = 356;
    static constexpr int TOOLBAR_W = 44;   /* left tool column */
    static constexpr int TOPBAR_H  = 26;   /* clear / save / open / quit row */
    static constexpr int PALETTE_H = 26;   /* bottom colour row */

    /* Top-bar button layout (x offset + width, relative to client_x()).
       Shared by draw() and on_event() so hit-tests never drift.          */
    static constexpr int TB_CLR_X  =   3, TB_CLR_W  = 66;   /* [Effacer] */
    static constexpr int TB_SAVE_X =  72, TB_SAVE_W = 58;   /* [Sauver]  */
    static constexpr int TB_OPEN_X = 133, TB_OPEN_W = 58;   /* [Ouvrir]  */
    static constexpr int TB_QUIT_X = 194, TB_QUIT_W = 44;   /* [Quit]    */

    enum Tool { T_PENCIL, T_LINE, T_RECT, T_RECTF, T_CIRCLE, T_FILL, T_ERASER, T_COUNT };

private:
    unsigned int* m_canvas { nullptr };
    unsigned int  m_color  { 0x000000u };
    int  m_tool { T_PENCIL };
    int  m_size { 2 };

    bool m_dragging { false };
    int  m_sx { 0 }, m_sy { 0 };   /* drag start (canvas coords)   */
    int  m_cx { 0 }, m_cy { 0 };   /* drag current (canvas coords) */
    int  m_lx { 0 }, m_ly { 0 };   /* last pencil point            */
    int  m_clear_arm { 0 };        /* two-click clear confirmation */
    char m_status[40] {};          /* transient message shown in top bar */

    int cvx() const { return client_x() + TOOLBAR_W; }
    int cvy() const { return client_y() + TOPBAR_H; }

    void put_c(int x, int y, unsigned int col);
    void brush(int x, int y, unsigned int col);
    void line_c(int x0, int y0, int x1, int y1, unsigned int col, int th);
    void rect_c(int x0, int y0, int x1, int y1, unsigned int col, bool fill);
    void circle_c(int x0, int y0, int x1, int y1, unsigned int col);
    void flood(int x, int y, unsigned int col);
    void clear_canvas(unsigned int col);

    /* Persistence: RLE ".gpi" images in the VFS (see PaintWindow.cpp). */
    void save_canvas();
    void load_canvas();
    void set_status(const char* s);   /* bounded copy into m_status */

    void draw_toolbar();
    void draw_palette();
    void draw_preview();   /* elastic shape overlay while dragging */
    int  tool_at(int mx, int my) const;   /* returns Tool or -1  */
    int  size_at(int mx, int my) const;   /* returns 1/3/5 or -1 */
    int  palette_at(int mx, int my) const;/* colour index or -1  */
};

extern "C" void open_paint_window(void);

#endif /* PAINT_WINDOW_HPP */
