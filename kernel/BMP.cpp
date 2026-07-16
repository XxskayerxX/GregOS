/* BMP.cpp — freestanding uncompressed BMP decoder for GregOS.
   Supports 24-bit and 32-bit BMPs only. Rows are flipped to top-down.
   Output pixels are XRGB32 stored in kmalloc-allocated memory.           */

#include "../include/BMP.hpp"

extern "C" void* kmalloc(unsigned int);

/* Little-endian readers from a byte pointer */
static inline unsigned int le16(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static inline unsigned int le32(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8)
         | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

bool bmp_decode(const unsigned char* data, unsigned int size,
                unsigned int* out_w, unsigned int* out_h,
                unsigned int** out_pixels)
{
    if (!data || size < 54) return false;
    if (data[0] != 'B' || data[1] != 'M') return false;

    unsigned int pix_off = le32(data + 10);
    unsigned int width   = le32(data + 18);
    unsigned int height  = le32(data + 22);
    unsigned int bpp     = le16(data + 28);
    unsigned int compr   = le32(data + 30);

    if (compr != 0)                  return false; /* no RLE */
    if (bpp != 24 && bpp != 32)     return false;
    if (width == 0 || height == 0)  return false;
    if (width > 1024 || height > 1024) return false; /* sanity cap */
    if (pix_off >= size)             return false;

    unsigned int bytes_pp  = bpp >> 3;                    /* 3 or 4 */
    unsigned int row_bytes = (width * bytes_pp + 3u) & ~3u; /* 4-byte pad */

    if (pix_off + row_bytes * height > size) return false;

    unsigned int n = width * height;
    unsigned int* pixels = (unsigned int*)kmalloc(n * sizeof(unsigned int));
    if (!pixels) return false;

    for (unsigned int row = 0; row < height; row++) {
        /* BMP rows are stored bottom-up; flip to top-down */
        const unsigned char* src = data + pix_off + (height - 1u - row) * row_bytes;
        unsigned int*        dst = pixels + row * width;
        for (unsigned int col = 0; col < width; col++) {
            const unsigned char* px = src + col * bytes_pp;
            unsigned int b = px[0], g = px[1], r = px[2]; /* BGR in file */
            dst[col] = (r << 16) | (g << 8) | b;
        }
    }

    *out_w      = width;
    *out_h      = height;
    *out_pixels = pixels;
    return true;
}
