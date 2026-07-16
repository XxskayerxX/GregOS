/* runechant_data — frequency table and the realm's melodies for GregOS.
   Pure C, no libc: same file builds freestanding (kernel) and native (tests). */

#include "../include/runechant_data.h"

/* Chromatic C4..C6, integer Hz (equal temperament, rounded).
   Index 9 = A4 = 440 Hz.                                                     */
const unsigned short RC_NOTE_HZ[RC_NOTE_COUNT] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,   /* C4..B4 */
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988,   /* C5..B5 */
    1047,                                                          /* C6     */
};

/* Piano geometry as semitone offsets — used by drawing AND hit-testing. */
const unsigned char RC_WHITE_SEMI[RC_NWHITE] = {
    0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24
};
const unsigned char RC_BLACK_SEMI[RC_NBLACK] = {
    1, 3, 6, 8, 10, 13, 15, 18, 20, 22
};

/* ── Les melodies du royaume (hz, duree en jiffies ; hz 0 = silence) ─────── */

static const rc_note S_COR[] = {          /* corne de guerre, grave et lente  */
    { 196, 50 }, { 0, 6 }, { 196, 50 }, { 0, 6 }, { 131, 120 }, { 0, 10 },
    { 196, 35 }, { 220, 35 }, { 196, 80 },
};
static const rc_note S_FANFARE[] = {      /* le tresor est a vous             */
    { 523, 12 }, { 659, 12 }, { 784, 12 }, { 1047, 40 },
    { 784, 12 }, { 1047, 90 },
};
static const rc_note S_MARCHE[] = {       /* le donjon vous a engloutis       */
    { 262, 45 }, { 0, 8 }, { 262, 28 }, { 262, 45 }, { 0, 8 },
    { 311, 30 }, { 294, 28 }, { 294, 45 }, { 262, 28 }, { 262, 45 },
    { 247, 28 }, { 262, 90 },
};
static const rc_note S_DRAKKAR[] = {      /* le chant du dragon, mineur       */
    { 440, 30 }, { 523, 30 }, { 659, 45 }, { 0, 6 },
    { 659, 30 }, { 587, 30 }, { 523, 45 }, { 0, 6 },
    { 440, 60 }, { 415, 30 }, { 440, 90 },
};

#define RC_LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

const rc_song RC_SONGS[RC_SONG_COUNT] = {
    { "Cor de Guerre",       S_COR,     RC_LEN(S_COR)     },
    { "Fanfare de Victoire", S_FANFARE, RC_LEN(S_FANFARE) },
    { "Marche Funebre",      S_MARCHE,  RC_LEN(S_MARCHE)  },
    { "Chant du Drakkar",    S_DRAKKAR, RC_LEN(S_DRAKKAR) },
};
