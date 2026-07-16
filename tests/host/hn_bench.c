/* AI think-time benchmark for hnefatafl_core (depth-3, realistic sequence).
   Compile (from tests/host/):
     cc -std=c11 -O2 -Wall -Wextra -I../../include \
        -o /tmp/hnb hn_bench.c ../../kernel/hnefatafl_core.c && /tmp/hnb         */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <time.h>
#include "hnefatafl_core.h"
int main(void) {
    unsigned char b[81];
    hn_init(b);
    /* simulate a realistic middlegame sequence: 8 plies of depth-3 replies */
    int side = HN_SIDE_ATT;
    double worst = 0;
    for (int ply = 0; ply < 8; ++ply) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        hn_move m = hn_ai_move(b, side, 3);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (ms > worst) worst = ms;
        printf("ply %d side %d: move %d->%d  %.1f ms\n", ply, side, m.from, m.to, ms);
        hn_apply(b, m);
        side = 1 - side;
    }
    printf("worst: %.1f ms host -> ~%.1f s in QEMU TCG at 50x\n", worst, worst * 50 / 1000);
    return 0;
}
