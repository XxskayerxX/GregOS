#ifndef PNG_DEC_H
#define PNG_DEC_H

/* ── png_dec — from-scratch PNG decoder for GregOS ───────────────────────────
   Pure C, zero libc, zero allocation (caller provides all buffers): the same
   file compiles freestanding into the kernel and natively on the host, where
   tests/host/png_test.c checks it pixel-perfect against Pillow references.

   Scope: 8-bit, non-interlaced PNG — colour types 0 (grey), 2 (RGB),
   3 (palette, incl. tRNS), 4 (grey+alpha), 6 (RGBA). zlib/DEFLATE inflate
   (stored + fixed + dynamic Huffman) implemented in full, adler32 verified.
   16-bit and Adam7-interlaced files are REJECTED cleanly (return 0).

   The engine has no alpha blending, so alpha is composited here, at decode
   time, over a caller-supplied uniform background colour:
       out = (a·src + (255−a)·bg + 127) / 255   per channel (integer).
   Output is XRGB dwords, row-major.                                          */

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the header only. Returns 1 and fills the dimensions if the file looks
   like a PNG this decoder accepts (8-bit, non-interlaced, sane bounded size:
   1..2048 per side and ≤ 1.5 Mpixel), else 0.                               */
int png_dims(const unsigned char* png, int len, int* w, int* h);

/* Scratch bytes needed by png_decode for a w×h image (filtered scanlines). */
int png_scratch_size(int w, int h);

/* Decode. `out_xrgb` must hold w*h dwords (dims from png_dims), `scratch`
   at least png_scratch_size(w,h) bytes. `bg_rgb` is the page background the
   alpha is composited over. Returns 1 on success, 0 on any error — never
   reads or writes out of bounds on hostile input.                           */
int png_decode(const unsigned char* png, int len,
               unsigned char* scratch, int scratch_len,
               unsigned int* out_xrgb, unsigned int bg_rgb);

#ifdef __cplusplus
}
#endif

#endif /* PNG_DEC_H */
