#ifndef RUNECHANT_DATA_H
#define RUNECHANT_DATA_H

/* ── runechant_data — music data for the « Chant Runique » app ───────────────
   Pure C, zero kernel dependencies: compiled freestanding into the kernel
   (picked up by the Makefile's kernel C glob) AND natively on the host by
   tests/host/runechant_test.c. Frequencies are integer Hz (equal temperament,
   rounded), durations are jiffies (100 Hz PIT ticks). hz == 0 means silence. */

#ifdef __cplusplus
extern "C" {
#endif

#define RC_NOTE_COUNT 25   /* chromatic C4..C6 (two octaves inclusive) */
#define RC_NWHITE     15   /* white piano keys across the two octaves  */
#define RC_NBLACK     10   /* black piano keys                         */
#define RC_SONG_COUNT 4

extern const unsigned short RC_NOTE_HZ[RC_NOTE_COUNT];

/* Semitone offsets (0..24) of the white and black keys, left to right —
   shared by the renderer AND the mouse hit-test so they can never drift.  */
extern const unsigned char RC_WHITE_SEMI[RC_NWHITE];
extern const unsigned char RC_BLACK_SEMI[RC_NBLACK];

typedef struct { unsigned short hz, dur_j; } rc_note;
typedef struct { const char* name; const rc_note* notes; int len; } rc_song;

extern const rc_song RC_SONGS[RC_SONG_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* RUNECHANT_DATA_H */
