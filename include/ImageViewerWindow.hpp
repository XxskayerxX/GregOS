#ifndef IMAGE_VIEWER_WINDOW_HPP
#define IMAGE_VIEWER_WINDOW_HPP

#include "Window.hpp"

/* ── ImageViewerWindow ────────────────────────────────────────────────────────
   Displays a decoded BMP image centred in the client area on a grey background.
   Created by FileManagerWindow when the user opens a *.bmp file.
   Keyboard: ESC / 'q' → close.                                             */

class ImageViewerWindow : public Window {
public:
    ImageViewerWindow() = default;

    /* Decode raw BMP bytes and store the pixel buffer.
       Returns false if the data is not a valid supported BMP.              */
    bool init_image(const unsigned char* data, int size);

    void draw() override;
    bool handle_char(int c) override;

private:
    unsigned int* m_pixels { nullptr };
    int           m_img_w  { 0 };
    int           m_img_h  { 0 };
};

#endif /* IMAGE_VIEWER_WINDOW_HPP */
