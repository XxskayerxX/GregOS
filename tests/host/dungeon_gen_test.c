/* Offline connectivity test for DungeonWindow::gen_level().
   Mirrors the kernel generator (rooms + L-corridors) and flood-fills from the
   player start to prove the stairs are ALWAYS reachable.
   ⚠️ The generator here is a PORT of kernel/DungeonWindow.cpp::gen_level —
   if you change the kernel generator, update this copy to match.
   Compile:  cc -std=c11 -O2 -Wall -Wextra -o /tmp/dgt dungeon_gen_test.c && /tmp/dgt */
#include <stdio.h>
#include <string.h>

enum { COLS = 48, ROWS = 26 };
enum { WALL = 0, FLOOR = 1, STAIRS = 3 };

static unsigned int g_seed;
static unsigned int rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}
static int rnd_range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(rnd() % (unsigned)(hi - lo + 1));
}

static unsigned char tile[ROWS * COLS];

/* Faithful port of gen_level's terrain step (floors 1..4: stairs present). */
static void gen_level(int *pcx, int *pcy, int *scx, int *scy) {
    for (int i = 0; i < ROWS * COLS; ++i) tile[i] = WALL;
    int nrooms = rnd_range(8, 12);
    int prev_cx = 0, prev_cy = 0, first_cx = 0, first_cy = 0, last_cx = 0, last_cy = 0;
    for (int r = 0; r < nrooms; ++r) {
        int rw = rnd_range(4, 8), rh = rnd_range(4, 6);
        int rx = rnd_range(1, COLS - rw - 2), ry = rnd_range(1, ROWS - rh - 2);
        for (int y = ry; y < ry + rh; ++y)
            for (int x = rx; x < rx + rw; ++x) tile[y * COLS + x] = FLOOR;
        int cx = rx + rw / 2, cy = ry + rh / 2;
        if (r == 0) { first_cx = cx; first_cy = cy; }
        else {
            int x0 = cx < prev_cx ? cx : prev_cx, x1 = cx > prev_cx ? cx : prev_cx;
            for (int x = x0; x <= x1; ++x) tile[prev_cy * COLS + x] = FLOOR;
            int y0 = cy < prev_cy ? cy : prev_cy, y1 = cy > prev_cy ? cy : prev_cy;
            for (int y = y0; y <= y1; ++y) tile[y * COLS + cx] = FLOOR;
        }
        prev_cx = cx; prev_cy = cy; last_cx = cx; last_cy = cy;
    }
    tile[last_cy * COLS + last_cx] = STAIRS;
    *pcx = first_cx; *pcy = first_cy; *scx = last_cx; *scy = last_cy;
}

static unsigned char vis[ROWS * COLS];
static int stack_x[ROWS * COLS], stack_y[ROWS * COLS];
static int reachable(int px, int py, int tx, int ty) {
    memset(vis, 0, sizeof vis);
    int sp = 0; stack_x[sp] = px; stack_y[sp] = py; ++sp; vis[py * COLS + px] = 1;
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    while (sp) {
        --sp; int x = stack_x[sp], y = stack_y[sp];
        for (int k = 0; k < 4; ++k) {
            int nx = x + dx[k], ny = y + dy[k];
            if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) continue;
            int idx = ny * COLS + nx;
            if (vis[idx] || tile[idx] == WALL) continue;
            vis[idx] = 1; stack_x[sp] = nx; stack_y[sp] = ny; ++sp;
        }
    }
    return vis[ty * COLS + tx];
}

int main(void) {
    int fails = 0, N = 2000;
    for (int s = 1; s <= N; ++s) {
        g_seed = (unsigned)s * 2654435761u + 1u;
        if (g_seed == 0) g_seed = 0x1234567u;
        int px, py, sx, sy;
        gen_level(&px, &py, &sx, &sy);
        if (tile[py * COLS + px] == WALL) { printf("FAIL seed %d: player on WALL\n", s); ++fails; continue; }
        if (!reachable(px, py, sx, sy))   { printf("FAIL seed %d: stairs unreachable\n", s); ++fails; }
    }
    if (fails == 0) printf("OK %d/%d : escalier toujours atteignable, heros jamais dans un mur\n", N, N);
    else            printf("FAILURES: %d/%d\n", fails, N);
    return fails ? 1 : 0;
}
