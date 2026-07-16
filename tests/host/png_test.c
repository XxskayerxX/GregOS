/* Host-side test for the GregOS PNG decoder (kernel/png.c) against
   pixel-perfect references produced by Pillow (gen_png_fixtures.py).
   Compile:  cc -std=c11 -O2 -Wall -Wextra -I../../include \
                -o /tmp/pngt png_test.c ../../kernel/png.c                      */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "png_dec.h"

#define TEST_BG 0x336699u          /* must match gen_png_fixtures.py BG */

static int g_pass = 0, g_fail = 0;

static unsigned char* slurp(const char* path, long* out_len)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* buf = malloc(n);
    if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f); *out_len = n;
    return buf;
}

static void expect_ok(const char* name)
{
    char path[256]; long plen, rlen;
    snprintf(path, sizeof path, "png_fixtures/%s.png", name);
    unsigned char* png = slurp(path, &plen);
    snprintf(path, sizeof path, "png_fixtures/%s.ref", name);
    unsigned char* ref = slurp(path, &rlen);
    if (!png || !ref) { printf("FAIL %s: fixture introuvable\n", name); ++g_fail; return; }

    int w = 0, h = 0;
    if (!png_dims(png, (int)plen, &w, &h)) {
        printf("FAIL %s: png_dims a refuse le fichier\n", name); ++g_fail;
        free(png); free(ref); return;
    }
    int rw, rh;
    memcpy(&rw, ref, 4); memcpy(&rh, ref + 4, 4);
    if (w != rw || h != rh) {
        printf("FAIL %s: dims %dx%d != ref %dx%d\n", name, w, h, rw, rh); ++g_fail;
        free(png); free(ref); return;
    }

    int scratch_len = png_scratch_size(w, h);
    unsigned char* scratch = malloc(scratch_len);
    unsigned int* out = malloc((size_t)w * h * 4);
    int ok = png_decode(png, (int)plen, scratch, scratch_len, out, TEST_BG);
    if (!ok) {
        printf("FAIL %s: png_decode a echoue\n", name); ++g_fail;
        goto done;
    }
    {
        const unsigned int* expect = (const unsigned int*)(ref + 8);
        long bad = -1;
        for (long i = 0; i < (long)w * h; ++i)
            if ((out[i] & 0xFFFFFFu) != (expect[i] & 0xFFFFFFu)) { bad = i; break; }
        if (bad >= 0) {
            printf("FAIL %s: pixel %ld (x=%ld y=%ld) = %06X, attendu %06X\n",
                   name, bad, bad % w, bad / w,
                   out[bad] & 0xFFFFFF, expect[bad] & 0xFFFFFF);
            ++g_fail;
        } else { ++g_pass; printf("OK   %s (%dx%d, pixel-perfect)\n", name, w, h); }
    }
done:
    free(png); free(ref); free(scratch); free(out);
}

static void expect_reject(const char* name)
{
    char path[256]; long plen;
    snprintf(path, sizeof path, "png_fixtures/%s.png", name);
    unsigned char* png = slurp(path, &plen);
    if (!png) { printf("FAIL %s: fixture introuvable\n", name); ++g_fail; return; }
    int w, h;
    int dims_ok = png_dims(png, (int)plen, &w, &h);
    if (dims_ok) {
        int sl = png_scratch_size(w, h);
        unsigned char* scratch = malloc(sl);
        unsigned int* out = malloc((size_t)w * h * 4);
        int ok = png_decode(png, (int)plen, scratch, sl, out, TEST_BG);
        free(scratch); free(out);
        if (ok) { printf("FAIL %s: aurait du etre rejete\n", name); ++g_fail; free(png); return; }
    }
    ++g_pass; printf("OK   %s (rejete proprement)\n", name);
    free(png);
}

/* Regression: a chunk length near INT_MAX must be rejected, not wrapped.
   With off=33 and clen=0x7FFFFFF3 the old check `off+12+(int)clen > len`
   overflowed signed int to a negative value → bounds check bypassed →
   `off += 12+clen` wrapped off hugely negative → OOB read next iteration. */
static void expect_hostile_chunklen(void)
{
    unsigned char d[45];
    memset(d, 0, sizeof d);
    const unsigned char sig[8] = { 0x89,'P','N','G','\r','\n',0x1A,'\n' };
    memcpy(d, sig, 8);
    /* IHDR: len=13, valid 8x8 truecolor */
    d[8]=0;d[9]=0;d[10]=0;d[11]=13;
    d[12]='I';d[13]='H';d[14]='D';d[15]='R';
    d[16]=0;d[17]=0;d[18]=0;d[19]=8;      /* width  = 8 */
    d[20]=0;d[21]=0;d[22]=0;d[23]=8;      /* height = 8 */
    d[24]=8;  d[25]=2; d[26]=0; d[27]=0; d[28]=0;   /* depth/ctype/comp/filt/interlace */
    /* CRC (unchecked) at 29..32 */
    /* malicious chunk at off=33: len=0x7FFFFFF3, type=IDAT */
    d[33]=0x7F;d[34]=0xFF;d[35]=0xFF;d[36]=0xF3;
    d[37]='I';d[38]='D';d[39]='A';d[40]='T';
    int w = 0, h = 0;
    int dims_ok = png_dims(d, (int)sizeof d, &w, &h);
    if (dims_ok) {          /* must NOT parse into a walk that reads OOB */
        printf("FAIL hostile_chunklen: png_dims a accepte une longueur geante\n");
        ++g_fail; return;
    }
    ++g_pass; printf("OK   hostile_chunklen (rejet sans overflow/OOB)\n");
}

int main(void)
{
    expect_ok("rgb_3x2");
    expect_ok("gradient_120x80");
    expect_ok("rgba_64x64");
    expect_ok("palette_48x48");
    expect_ok("gray_32x32");
    expect_ok("graya_32x32");
    expect_ok("noise_200x150");
    expect_ok("stored_24x24");
    expect_reject("interlaced_32x32");
    expect_reject("depth16_4x4");
    expect_hostile_chunklen();

    /* robustesse : troncatures et corruptions ne doivent jamais crasher */
    {
        long plen;
        unsigned char* png = slurp("png_fixtures/gradient_120x80.png", &plen);
        int w, h, crashes = 0;
        for (int cut = 0; cut < (int)plen; cut += 7) {
            if (png_dims(png, cut, &w, &h)) {
                int sl = png_scratch_size(w, h);
                unsigned char* scratch = malloc(sl);
                unsigned int* out = malloc((size_t)w * h * 4);
                png_decode(png, cut, scratch, sl, out, TEST_BG);   /* résultat libre, crash interdit */
                free(scratch); free(out);
            }
        }
        for (int i = 0; i < 500; ++i) {                    /* corruption d'octets */
            unsigned char* mut = malloc(plen);
            memcpy(mut, png, plen);
            mut[(i * 2654435761u) % plen] ^= (unsigned char)(i * 37 + 1);
            if (png_dims(mut, (int)plen, &w, &h) && w <= 4096 && h <= 4096) {
                int sl = png_scratch_size(w, h);
                unsigned char* scratch = malloc(sl);
                unsigned int* out = malloc((size_t)w * h * 4);
                png_decode(mut, (int)plen, scratch, sl, out, TEST_BG);
                free(scratch); free(out);
            }
            free(mut);
        }
        free(png);
        (void)crashes;
        ++g_pass; printf("OK   fuzz troncature+corruption (aucun crash)\n");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
