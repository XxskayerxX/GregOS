#ifndef BMP_HPP
#define BMP_HPP

/* ── bmp_decode ───────────────────────────────────────────────────────────
   Decodes an uncompressed 24-bit or 32-bit BMP from an in-memory buffer.
   On success: allocates out_pixels via kmalloc (rows top-down, XRGB32),
   sets *out_w / *out_h, and returns true.
   No libc, no exceptions. kmalloc is a bump allocator — never freed.     */

bool bmp_decode(const unsigned char* data, unsigned int size,
                unsigned int* out_w, unsigned int* out_h,
                unsigned int** out_pixels);

#endif /* BMP_HPP */
