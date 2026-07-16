/* GUI::GraphWidget — ring-buffer area chart.
   Freestanding: no libc, no exceptions.                                      */

#include "../../include/GUI/GraphWidget.hpp"
#include "../../include/Graphics.hpp"
#include "../../include/Theme.hpp"

namespace GUI {

void GraphWidget::push_value(int val)
{
    if (val < 0) val = 0;
    m_buf[m_head] = val;
    m_head = (m_head + 1) % BUF_SIZE;
    if (m_count < BUF_SIZE) ++m_count;
    if (val > m_max) m_max = val;
    if (m_max < 1)   m_max = 1;   /* never allow div-by-zero in draw() */
}

void GraphWidget::draw(Graphics& g, int wx, int wy)
{
    /* ── ISOLATION: draw only the dark frame + grid lines; skip bars ──
       Re-enable bars once boot is confirmed stable by removing this block */
    {
        int ax = wx + m_x, ay = wy + m_y;
        /* Sunken bevel */
        g.draw_hline(ax, ay, m_w, Theme::BEVEL_OUTER_DK);
        g.draw_vline(ax, ay, m_h, Theme::BEVEL_OUTER_DK);
        g.draw_hline(ax, ay + m_h - 1, m_w, Theme::BEVEL_OUTER_LT);
        g.draw_vline(ax + m_w - 1, ay, m_h, Theme::BEVEL_OUTER_LT);
        /* Dark interior */
        if (m_w > 4 && m_h > 4)
            g.fill_rect(ax + 2, ay + 2, m_w - 4, m_h - 4, 0x080810u);
        return;  /* skip all bar-drawing and divisions */
    }

    int ax = wx + m_x;
    int ay = wy + m_y;

    /* Sunken 3D bevel — 2 lines, matching draw_bevel(raised=false) */
    g.draw_hline(ax,           ay,           m_w, Theme::BEVEL_OUTER_DK);
    g.draw_vline(ax,           ay,           m_h, Theme::BEVEL_OUTER_DK);
    g.draw_hline(ax,           ay + m_h - 1, m_w, Theme::BEVEL_OUTER_LT);
    g.draw_vline(ax + m_w - 1, ay,           m_h, Theme::BEVEL_OUTER_LT);
    if (m_w > 2 && m_h > 2) {
        g.draw_hline(ax + 1,       ay + 1,       m_w - 2, Theme::BEVEL_INNER_DK);
        g.draw_vline(ax + 1,       ay + 1,       m_h - 2, Theme::BEVEL_INNER_DK);
        g.draw_hline(ax + 1,       ay + m_h - 2, m_w - 2, Theme::BEVEL_INNER_LT);
        g.draw_vline(ax + m_w - 2, ay + 1,       m_h - 2, Theme::BEVEL_INNER_LT);
    }

    /* Dark interior */
    int ix = ax + 2, iy = ay + 2;
    int iw = m_w - 4, ih = m_h - 4;
    if (iw < 1 || ih < 1) return;
    g.fill_rect(ix, iy, iw, ih, 0x080810u);

    if (m_count == 0 || m_max < 1) return;   /* nothing to draw or bad state */

    /* Dim grid lines at 50 % and 75 % height */
    g.draw_hline(ix, iy + ih / 2, iw, 0x18241Au);
    g.draw_hline(ix, iy + ih / 4, iw, 0x18241Au);

    /* Each sample is one bar of width = iw / BUF_SIZE */
    int bar_w = iw / BUF_SIZE;
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < m_count; ++i) {
        /* oldest → left, newest → right */
        int si  = (m_head - m_count + i + BUF_SIZE * 2) % BUF_SIZE;
        int val = m_buf[si];
        int bh  = m_max > 0 ? (val * ih) / m_max : 0;
        if (bh < 1 && val > 0) bh = 1;
        if (bh > ih) bh = ih;

        int bx = ix + i * bar_w;
        int by = iy + ih - bh;
        if (bh == 0) continue;   /* skip zero-height bars */

        int pct = m_max > 0 ? (val * 100) / m_max : 0;
        unsigned int col = pct < 50 ? 0x00DD55u
                         : pct < 80 ? 0xFFCC00u
                                    : 0xFF4400u;
        g.fill_rect(bx, by, bar_w, bh, col);
    }
}

} /* namespace GUI */
