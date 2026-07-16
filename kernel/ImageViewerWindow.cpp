/* ImageViewerWindow — displays a decoded BMP centred in its client area.
   Freestanding: no libc, no exceptions.                                   */

#include "../include/ImageViewerWindow.hpp"
#include "../include/BMP.hpp"
#include "../include/Graphics.hpp"
#include "../include/gfx.h"
#include "../include/keyboard.h"

bool ImageViewerWindow::init_image(const unsigned char* data, int size)
{
    unsigned int w, h;
    unsigned int* px;
    if (!bmp_decode(data, (unsigned int)size, &w, &h, &px)) return false;
    m_pixels = px;
    m_img_w  = (int)w;
    m_img_h  = (int)h;
    return true;
}

void ImageViewerWindow::draw()
{
    Window::draw();

    Graphics& g = Graphics::instance();
    g.fill_rect(client_x(), client_y(), client_w(), client_h(), 0x404040u);

    if (!m_pixels) {
        g.draw_str(client_x() + 8, client_y() + 8,
                   "BMP decode error", 0xFF4444u, GFX_TRANSPARENT);
        return;
    }

    /* Centre image; clamp so it never overflows the client area */
    int dw = (m_img_w < client_w()) ? m_img_w : client_w();
    int dh = (m_img_h < client_h()) ? m_img_h : client_h();
    int ox = client_x() + (client_w() - dw) / 2;
    int oy = client_y() + (client_h() - dh) / 2;

    g.draw_image(ox, oy, dw, dh, m_pixels);
}

/* ── handle_char ─────────────────────────────────────────────────────── */

bool ImageViewerWindow::handle_char(int c)
{
    if (c == KEY_ESC || c == 'q') { request_close(); return true; }
    return false;
}
