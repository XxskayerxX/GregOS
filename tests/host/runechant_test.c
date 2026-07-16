/* Host-side test for the Chant Runique music data (kernel/runechant_data.c).
   Compile:  cc -std=c11 -O2 -Wall -Wextra -I../../include \
                -o /tmp/rct runechant_test.c ../../kernel/runechant_data.c        */
#include <stdio.h>
#include "runechant_data.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("FAIL (l.%d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

int main(void)
{
    /* ── frequency table: 25 chromatic notes, C4..C6 ─────────────────── */
    CHECK(RC_NOTE_COUNT == 25, "25 notes attendues");
    CHECK(RC_NOTE_HZ[9] == 440, "A4 (index 9) doit faire 440 Hz, lu %u", RC_NOTE_HZ[9]);

    for (int i = 1; i < RC_NOTE_COUNT; ++i)
        CHECK(RC_NOTE_HZ[i] > RC_NOTE_HZ[i - 1],
              "table non croissante en %d (%u <= %u)", i, RC_NOTE_HZ[i], RC_NOTE_HZ[i-1]);

    for (int i = 0; i + 12 < RC_NOTE_COUNT; ++i) {
        int d = 2 * (int)RC_NOTE_HZ[i] - (int)RC_NOTE_HZ[i + 12];
        if (d < 0) d = -d;
        CHECK(d <= 2, "octave %d: 2*%u vs %u (delta %d)", i, RC_NOTE_HZ[i], RC_NOTE_HZ[i+12], d);
    }

    /* every keyboard note must be programmable on PIT channel 2 (16-bit divisor) */
    for (int i = 0; i < RC_NOTE_COUNT; ++i)
        CHECK(1193182 / RC_NOTE_HZ[i] < 65536, "diviseur PIT trop grand pour %u Hz", RC_NOTE_HZ[i]);

    /* ── songs ───────────────────────────────────────────────────────── */
    CHECK(RC_SONG_COUNT == 4, "4 melodies attendues");
    for (int s = 0; s < RC_SONG_COUNT; ++s) {
        const rc_song* sg = &RC_SONGS[s];
        CHECK(sg->name && sg->name[0], "melodie %d sans nom", s);
        CHECK(sg->len > 0, "melodie %d vide", s);

        /* names must be CP437-safe: printable ASCII only (no UTF-8 accents) */
        for (const char* p = sg->name; *p; ++p)
            CHECK((unsigned char)*p >= 32 && (unsigned char)*p < 127,
                  "melodie %d: caractere non-ASCII 0x%02X dans le nom", s, (unsigned char)*p);

        unsigned total = 0;
        for (int n = 0; n < sg->len; ++n) {
            unsigned hz = sg->notes[n].hz, dj = sg->notes[n].dur_j;
            CHECK(hz == 0 || (hz >= 131 && hz <= 2093),
                  "melodie %d note %d: %u Hz hors gamme haut-parleur", s, n, hz);
            if (hz) CHECK(1193182 / hz < 65536, "melodie %d note %d: diviseur PIT", s, n);
            CHECK(dj >= 1 && dj <= 300, "melodie %d note %d: duree %u j hors bornes", s, n, dj);
            total += dj;
        }
        CHECK(total >= 100 && total <= 3000,
              "melodie %d: duree totale %u j (%u.%02u s) hors bornes",
              s, total, total / 100, total % 100);
    }

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
