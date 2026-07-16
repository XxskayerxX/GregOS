/* png — from-scratch PNG decoder (zlib/DEFLATE included) for GregOS.
   Pure C, no libc, no allocation, every access bounds-checked: hostile input
   must yield 0, never a crash. See include/png_dec.h for the contract.       */

#include "../include/png_dec.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define PNG_MAX_SIDE   2048
#define PNG_MAX_PIXELS 1500000
#define PNG_MAX_IDAT   64
#define PNG_MAX_BITS   15

/* ── bit reader over the (possibly split) IDAT byte stream ───────────────── */
typedef struct {
    const u8* seg[PNG_MAX_IDAT];
    int       seg_len[PNG_MAX_IDAT];
    int       nseg, cur, pos;
    u32       bitbuf;
    int       bitcnt;
    int       err;
} bitrd;

static int br_byte(bitrd* b)
{
    while (b->cur < b->nseg && b->pos >= b->seg_len[b->cur]) { ++b->cur; b->pos = 0; }
    if (b->cur >= b->nseg) { b->err = 1; return 0; }
    return b->seg[b->cur][b->pos++];
}

static u32 br_bits(bitrd* b, int n)
{
    while (b->bitcnt < n) {
        b->bitbuf |= (u32)br_byte(b) << b->bitcnt;
        b->bitcnt += 8;
    }
    u32 v = b->bitbuf & ((1u << n) - 1u);
    b->bitbuf >>= n;
    b->bitcnt  -= n;
    return v;
}

static void br_align(bitrd* b) { b->bitbuf = 0; b->bitcnt = 0; }

/* ── canonical Huffman ───────────────────────────────────────────────────── */
typedef struct {
    u16 count[PNG_MAX_BITS + 1];   /* codes per bit length     */
    u16 sym[288];                  /* symbols, canonical order */
} huff;

static int huff_build(huff* h, const u8* lens, int n)
{
    int offs[PNG_MAX_BITS + 1];
    for (int i = 0; i <= PNG_MAX_BITS; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;            /* empty table  */
    h->count[0] = 0;

    int left = 1;                              /* over-subscription check */
    for (int len = 1; len <= PNG_MAX_BITS; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < PNG_MAX_BITS; ++len)
        offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; ++i)
        if (lens[i]) h->sym[offs[lens[i]]++] = (u16)i;
    return 0;
}

static int huff_decode(bitrd* b, const huff* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= PNG_MAX_BITS; ++len) {
        code |= (int)br_bits(b, 1);
        int cnt = h->count[len];
        if (code - first < cnt) return h->sym[index + (code - first)];
        index += cnt;
        first  = (first + cnt) << 1;
        code <<= 1;
        if (b->err) return -1;
    }
    return -1;
}

/* ── inflate (RFC 1951) into out[0..out_len) ─────────────────────────────── */
static const u16 LEN_BASE[29]  = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,
                                   43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,
                                   4,4,4,4,5,5,5,5,0 };
static const u16 DST_BASE[30]  = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
                                   257,385,513,769,1025,1537,2049,3073,4097,
                                   6145,8193,12289,16385,24577 };
static const u8  DST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,
                                   9,9,10,10,11,11,12,12,13,13 };
static const u8  CLC_ORDER[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

static huff s_lit, s_dst;                     /* static: keep thread stacks small */
static u8   s_lens[288 + 32];

static int inflate_block_huff(bitrd* b, u8* out, int out_len, int* wpos,
                              const huff* lit, const huff* dst)
{
    int w = *wpos;
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0 || b->err) return -1;
        if (sym < 256) {
            if (w >= out_len) return -1;
            out[w++] = (u8)sym;
        } else if (sym == 256) {
            break;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len  = LEN_BASE[sym] + (int)br_bits(b, LEN_EXTRA[sym]);
            int dsym = huff_decode(b, dst);
            if (dsym < 0 || dsym >= 30 || b->err) return -1;
            int dist = DST_BASE[dsym] + (int)br_bits(b, DST_EXTRA[dsym]);
            if (dist <= 0 || dist > w) return -1;
            if (w + len > out_len) return -1;
            for (int i = 0; i < len; ++i, ++w) out[w] = out[w - dist];
        }
        if (b->err) return -1;
    }
    *wpos = w;
    return 0;
}

static int inflate_all(bitrd* b, u8* out, int out_len)
{
    int w = 0, final = 0;
    while (!final) {
        final     = (int)br_bits(b, 1);
        int btype = (int)br_bits(b, 2);
        if (b->err) return -1;

        if (btype == 0) {                                 /* stored */
            br_align(b);
            int len  = br_byte(b) | (br_byte(b) << 8);
            int nlen = br_byte(b) | (br_byte(b) << 8);
            if (b->err || len != (nlen ^ 0xFFFF)) return -1;
            if (w + len > out_len) return -1;
            for (int i = 0; i < len; ++i) out[w++] = (u8)br_byte(b);
            if (b->err) return -1;
        } else if (btype == 1) {                          /* fixed Huffman */
            for (int i = 0;   i < 144; ++i) s_lens[i] = 8;
            for (int i = 144; i < 256; ++i) s_lens[i] = 9;
            for (int i = 256; i < 280; ++i) s_lens[i] = 7;
            for (int i = 280; i < 288; ++i) s_lens[i] = 8;
            if (huff_build(&s_lit, s_lens, 288)) return -1;
            for (int i = 0; i < 32; ++i) s_lens[i] = 5;
            if (huff_build(&s_dst, s_lens, 32)) return -1;
            if (inflate_block_huff(b, out, out_len, &w, &s_lit, &s_dst)) return -1;
        } else if (btype == 2) {                          /* dynamic Huffman */
            int hlit  = (int)br_bits(b, 5) + 257;
            int hdist = (int)br_bits(b, 5) + 1;
            int hclen = (int)br_bits(b, 4) + 4;
            if (b->err || hlit > 288 || hdist > 32) return -1;

            u8 clc[19];
            for (int i = 0; i < 19; ++i) clc[i] = 0;
            for (int i = 0; i < hclen; ++i) clc[CLC_ORDER[i]] = (u8)br_bits(b, 3);
            huff hcl;
            if (b->err || huff_build(&hcl, clc, 19)) return -1;

            int n = 0;
            while (n < hlit + hdist) {
                int sym = huff_decode(b, &hcl);
                if (sym < 0 || b->err) return -1;
                if (sym < 16)       { s_lens[n++] = (u8)sym; }
                else if (sym == 16) {
                    if (n == 0) return -1;
                    int rep = 3 + (int)br_bits(b, 2);
                    u8  v   = s_lens[n - 1];
                    if (n + rep > hlit + hdist) return -1;
                    while (rep--) s_lens[n++] = v;
                } else if (sym == 17) {
                    int rep = 3 + (int)br_bits(b, 3);
                    if (n + rep > hlit + hdist) return -1;
                    while (rep--) s_lens[n++] = 0;
                } else {
                    int rep = 11 + (int)br_bits(b, 7);
                    if (n + rep > hlit + hdist) return -1;
                    while (rep--) s_lens[n++] = 0;
                }
            }
            if (s_lens[256] == 0) return -1;              /* no end-of-block */
            if (huff_build(&s_lit, s_lens, hlit)) return -1;
            if (huff_build(&s_dst, s_lens + hlit, hdist)) return -1;
            if (inflate_block_huff(b, out, out_len, &w, &s_lit, &s_dst)) return -1;
        } else {
            return -1;
        }
    }
    return w;
}

/* ── PNG chunk walking ───────────────────────────────────────────────────── */
static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16)
                                    | ((u32)p[2] << 8)  |  (u32)p[3]; }

typedef struct {
    int w, h, depth, ctype, interlace;
    int channels;
    const u8* plte;      int plte_n;      /* palette entries (RGB triples) */
    const u8* trns;      int trns_n;      /* palette alpha                 */
    bitrd idat;
} png_info;

static int png_parse(const u8* d, int len, png_info* pi)
{
    static const u8 SIG[8] = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    if (len < 8 + 25) return 0;
    for (int i = 0; i < 8; ++i) if (d[i] != SIG[i]) return 0;

    pi->plte = 0; pi->plte_n = 0; pi->trns = 0; pi->trns_n = 0;
    pi->idat.nseg = 0; pi->idat.cur = 0; pi->idat.pos = 0;
    pi->idat.bitbuf = 0; pi->idat.bitcnt = 0; pi->idat.err = 0;
    int have_ihdr = 0;

    int off = 8;
    while (off + 12 <= len) {
        u32 clen = be32(d + off);
        if (clen > 0x7FFFFFFFu - 12u) return 0;
        if ((int)clen > len - off - 12) return 0;   /* overflow-safe: off+12<=len here */
        const u8* typ = d + off + 4;
        const u8* pay = d + off + 8;

        if (typ[0]=='I' && typ[1]=='H' && typ[2]=='D' && typ[3]=='R') {
            if (clen != 13) return 0;
            pi->w = (int)be32(pay);
            pi->h = (int)be32(pay + 4);
            pi->depth = pay[8]; pi->ctype = pay[9]; pi->interlace = pay[12];
            if (pay[10] != 0 || pay[11] != 0) return 0;   /* method/filter */
            have_ihdr = 1;
        } else if (typ[0]=='P' && typ[1]=='L' && typ[2]=='T' && typ[3]=='E') {
            pi->plte = pay; pi->plte_n = (int)clen / 3;
            if (pi->plte_n > 256) return 0;
        } else if (typ[0]=='t' && typ[1]=='R' && typ[2]=='N' && typ[3]=='S') {
            pi->trns = pay; pi->trns_n = (int)clen;
        } else if (typ[0]=='I' && typ[1]=='D' && typ[2]=='A' && typ[3]=='T') {
            if (pi->idat.nseg >= PNG_MAX_IDAT) return 0;
            pi->idat.seg[pi->idat.nseg]     = pay;
            pi->idat.seg_len[pi->idat.nseg] = (int)clen;
            pi->idat.nseg++;
        } else if (typ[0]=='I' && typ[1]=='E' && typ[2]=='N' && typ[3]=='D') {
            break;
        }
        off += 12 + (int)clen;
    }
    if (!have_ihdr || pi->idat.nseg == 0) return 0;

    if (pi->depth != 8 || pi->interlace != 0) return 0;
    switch (pi->ctype) {
        case 0: pi->channels = 1; break;
        case 2: pi->channels = 3; break;
        case 3: pi->channels = 1; if (!pi->plte) return 0; break;
        case 4: pi->channels = 2; break;
        case 6: pi->channels = 4; break;
        default: return 0;
    }
    if (pi->w < 1 || pi->h < 1 || pi->w > PNG_MAX_SIDE || pi->h > PNG_MAX_SIDE)
        return 0;
    if ((long)pi->w * pi->h > PNG_MAX_PIXELS) return 0;
    return 1;
}

/* ── public: dimensions ──────────────────────────────────────────────────── */
int png_dims(const unsigned char* png, int len, int* w, int* h)
{
    png_info pi;
    if (!png || len <= 0 || !png_parse(png, len, &pi)) return 0;
    *w = pi.w; *h = pi.h;
    return 1;
}

int png_scratch_size(int w, int h)
{
    return (w * 4 + 1) * h + 16;
}

/* ── Paeth predictor ─────────────────────────────────────────────────────── */
static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

/* ── public: full decode ─────────────────────────────────────────────────── */
int png_decode(const unsigned char* png, int len,
               unsigned char* scratch, int scratch_len,
               unsigned int* out_xrgb, unsigned int bg_rgb)
{
    png_info pi;
    if (!png || !scratch || !out_xrgb) return 0;
    if (!png_parse(png, len, &pi)) return 0;

    int stride  = pi.w * pi.channels;
    int raw_len = (stride + 1) * pi.h;
    if (raw_len + 8 > scratch_len) return 0;

    /* zlib header (RFC 1950) */
    bitrd* b = &pi.idat;
    int cmf = br_byte(b), flg = br_byte(b);
    if (b->err) return 0;
    if ((cmf & 0x0F) != 8) return 0;               /* method: deflate    */
    if (((cmf << 8) | flg) % 31 != 0) return 0;    /* header checksum    */
    if (flg & 0x20) return 0;                      /* FDICT: unsupported */

    int got = inflate_all(b, scratch, raw_len);
    if (got != raw_len) return 0;

    /* adler32 over the decompressed stream (trailer bytes after the data) */
    {
        u32 s1 = 1, s2 = 0;
        for (int i = 0; i < raw_len; ++i) {
            s1 += scratch[i];      if (s1 >= 65521u) s1 -= 65521u;
            s2 += s1;              s2 %= 65521u;
        }
        br_align(b);
        u32 a = 0;
        for (int i = 0; i < 4; ++i) a = (a << 8) | (u32)br_byte(b);
        if (b->err || a != ((s2 << 16) | s1)) return 0;
    }

    /* de-filter scanlines in place (each row: 1 filter byte + stride bytes) */
    int bpp = pi.channels;
    for (int y = 0; y < pi.h; ++y) {
        u8* row  = scratch + y * (stride + 1) + 1;
        u8* prev = (y > 0) ? scratch + (y - 1) * (stride + 1) + 1 : 0;
        int f    = row[-1];
        switch (f) {
            case 0: break;
            case 1: for (int x = bpp; x < stride; ++x) row[x] = (u8)(row[x] + row[x - bpp]); break;
            case 2: if (prev) for (int x = 0; x < stride; ++x) row[x] = (u8)(row[x] + prev[x]); break;
            case 3:
                for (int x = 0; x < stride; ++x) {
                    int a = (x >= bpp) ? row[x - bpp] : 0;
                    int u = prev ? prev[x] : 0;
                    row[x] = (u8)(row[x] + ((a + u) >> 1));
                }
                break;
            case 4:
                for (int x = 0; x < stride; ++x) {
                    int a = (x >= bpp) ? row[x - bpp] : 0;
                    int u = prev ? prev[x] : 0;
                    int c = (prev && x >= bpp) ? prev[x - bpp] : 0;
                    row[x] = (u8)(row[x] + paeth(a, u, c));
                }
                break;
            default: return 0;
        }
    }

    /* convert to XRGB, compositing alpha over bg_rgb:
       out = (a*src + (255-a)*bg + 127) / 255                                */
    int bgr = (int)((bg_rgb >> 16) & 0xFF);
    int bgg = (int)((bg_rgb >> 8)  & 0xFF);
    int bgb = (int)( bg_rgb        & 0xFF);
    for (int y = 0; y < pi.h; ++y) {
        const u8* row = scratch + y * (stride + 1) + 1;
        unsigned int* out = out_xrgb + (long)y * pi.w;
        for (int x = 0; x < pi.w; ++x) {
            int r, g, bl, a = 255;
            switch (pi.ctype) {
                case 0:  r = g = bl = row[x]; break;
                case 2:  r = row[x*3]; g = row[x*3+1]; bl = row[x*3+2]; break;
                case 3: {
                    int idx = row[x];
                    if (idx >= pi.plte_n) return 0;
                    r  = pi.plte[idx*3]; g = pi.plte[idx*3+1]; bl = pi.plte[idx*3+2];
                    if (pi.trns && idx < pi.trns_n) a = pi.trns[idx];
                    break;
                }
                case 4:  r = g = bl = row[x*2]; a = row[x*2+1]; break;
                default: r = row[x*4]; g = row[x*4+1]; bl = row[x*4+2]; a = row[x*4+3]; break;
            }
            if (a != 255) {
                r  = (a * r  + (255 - a) * bgr + 127) / 255;
                g  = (a * g  + (255 - a) * bgg + 127) / 255;
                bl = (a * bl + (255 - a) * bgb + 127) / 255;
            }
            out[x] = ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
        }
    }
    return 1;
}
