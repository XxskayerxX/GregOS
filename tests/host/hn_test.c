/* Host-side test suite for GregOS hnefatafl_core.c (Tablut 9x9 rules + AI).
   The SAME kernel/hnefatafl_core.c compiles freestanding in the kernel and
   natively here — zero drift between what is tested and what ships.
   Compile (from tests/host/):
     cc -std=c11 -O2 -Wall -Wextra -I../../include \
        -o /tmp/hnt hn_test.c ../../kernel/hnefatafl_core.c && /tmp/hnt          */
#include <stdio.h>
#include <string.h>
#include "hnefatafl_core.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

static int IX(int x, int y) { return y * 9 + x; }

static void clear(unsigned char b[81]) { memset(b, HN_EMPTY, 81); }

/* does the move list contain from->to ? */
static int has_move(const hn_move* mv, int n, int from, int to) {
    for (int i = 0; i < n; ++i)
        if (mv[i].from == from && mv[i].to == to) return 1;
    return 0;
}
/* count moves of one piece */
static int moves_of(const hn_move* mv, int n, int from) {
    int c = 0;
    for (int i = 0; i < n; ++i) if (mv[i].from == from) ++c;
    return c;
}
static int count_piece(const unsigned char b[81], int v) {
    int c = 0;
    for (int i = 0; i < 81; ++i) if (b[i] == v) ++c;
    return c;
}
static int board_valid(const unsigned char b[81]) {
    if (count_piece(b, HN_KING) > 1) return 0;
    static const int restr[5] = { 40, 0, 8, 72, 80 };
    for (int i = 0; i < 5; ++i)
        if (b[restr[i]] == HN_ATT || b[restr[i]] == HN_DEF) return 0;
    return 1;
}

/* xorshift for self-play opening variation */
static unsigned int g_seed = 1;
static unsigned int rnd(void) {
    g_seed ^= g_seed << 13; g_seed ^= g_seed >> 17; g_seed ^= g_seed << 5;
    return g_seed;
}

int main(void) {
    unsigned char b[81];
    hn_move mv[HN_MAX_MOVES];
    int n;

    /* ── 1. initial position ─────────────────────────────────────────── */
    hn_init(b);
    CHECK(count_piece(b, HN_ATT) == 16,  "init: 16 attackers");
    CHECK(count_piece(b, HN_DEF) == 8,   "init: 8 defenders");
    CHECK(count_piece(b, HN_KING) == 1,  "init: 1 king");
    CHECK(b[40] == HN_KING,              "init: king on throne");
    CHECK(b[0] == HN_EMPTY && b[8] == HN_EMPTY && b[72] == HN_EMPTY && b[80] == HN_EMPTY,
          "init: corners empty");
    CHECK(b[IX(4,0)] == HN_ATT && b[IX(4,1)] == HN_ATT, "init: north T");
    CHECK(b[IX(4,2)] == HN_DEF && b[IX(2,4)] == HN_DEF, "init: defender cross");

    /* ── 2. rook movement + restricted squares ───────────────────────── */
    clear(b); b[IX(4,6)] = HN_ATT;
    n = hn_gen_moves(b, HN_SIDE_ATT, mv, HN_MAX_MOVES);
    CHECK(n == 11, "soldier at (4,6): 11 moves (throne blocks up)");
    CHECK(has_move(mv, n, IX(4,6), IX(4,5)),  "soldier reaches (4,5)");
    CHECK(!has_move(mv, n, IX(4,6), IX(4,4)), "soldier cannot land on throne");
    CHECK(!has_move(mv, n, IX(4,6), IX(4,3)), "soldier cannot pass through throne");

    clear(b); b[IX(4,6)] = HN_KING;
    n = hn_gen_moves(b, HN_SIDE_DEF, mv, HN_MAX_MOVES);
    CHECK(has_move(mv, n, IX(4,6), IX(4,4)), "king can land on throne");
    CHECK(has_move(mv, n, IX(4,6), IX(4,2)), "king can pass through throne");

    clear(b); b[IX(0,5)] = HN_KING;
    n = hn_gen_moves(b, HN_SIDE_DEF, mv, HN_MAX_MOVES);
    CHECK(has_move(mv, n, IX(0,5), IX(0,0)), "king can land on corner");
    clear(b); b[IX(0,5)] = HN_ATT;
    n = hn_gen_moves(b, HN_SIDE_ATT, mv, HN_MAX_MOVES);
    CHECK(!has_move(mv, n, IX(0,5), IX(0,0)), "soldier cannot land on corner");

    /* no row wrap at board edge */
    clear(b); b[IX(8,4)] = HN_ATT;
    n = hn_gen_moves(b, HN_SIDE_ATT, mv, HN_MAX_MOVES);
    CHECK(moves_of(mv, n, IX(8,4)) == 9, "edge piece (8,4): 9 moves, no wrap");

    /* blocked by pieces */
    clear(b); b[IX(2,6)] = HN_ATT; b[IX(5,6)] = HN_DEF;
    n = hn_gen_moves(b, HN_SIDE_ATT, mv, HN_MAX_MOVES);
    CHECK(has_move(mv, n, IX(2,6), IX(4,6)),  "moves up to blocker");
    CHECK(!has_move(mv, n, IX(2,6), IX(5,6)), "cannot land on piece");
    CHECK(!has_move(mv, n, IX(2,6), IX(6,6)), "cannot jump over piece");

    /* ── 3. custodian captures ───────────────────────────────────────── */
    clear(b); b[IX(2,2)] = HN_ATT; b[IX(3,2)] = HN_DEF; b[IX(4,5)] = HN_ATT;
    { hn_move m = { (unsigned char)IX(4,5), (unsigned char)IX(4,2) };
      int c = hn_apply(b, m);
      CHECK(c == 1 && b[IX(3,2)] == HN_EMPTY, "simple custodian capture"); }

    clear(b); b[IX(4,3)] = HN_DEF; b[IX(0,2)] = HN_ATT;   /* empty throne assists */
    { hn_move m = { (unsigned char)IX(0,2), (unsigned char)IX(4,2) };
      int c = hn_apply(b, m);
      CHECK(c == 1 && b[IX(4,3)] == HN_EMPTY, "capture against empty throne"); }

    clear(b); b[40] = HN_KING; b[IX(4,3)] = HN_DEF; b[IX(0,2)] = HN_ATT; /* throne occupied by king */
    { hn_move m = { (unsigned char)IX(0,2), (unsigned char)IX(4,2) };
      int c = hn_apply(b, m);
      CHECK(c == 0 && b[IX(4,3)] == HN_DEF, "occupied throne is not hostile"); }

    clear(b); b[IX(1,0)] = HN_DEF; b[IX(2,4)] = HN_ATT;   /* corner assists */
    { hn_move m = { (unsigned char)IX(2,4), (unsigned char)IX(2,0) };
      int c = hn_apply(b, m);
      CHECK(c == 1 && b[IX(1,0)] == HN_EMPTY, "capture against corner"); }

    clear(b);                                              /* double capture */
    b[IX(2,6)] = HN_DEF; b[IX(4,6)] = HN_DEF;
    b[IX(1,6)] = HN_ATT; b[IX(5,6)] = HN_ATT; b[IX(3,0)] = HN_ATT;
    { hn_move m = { (unsigned char)IX(3,0), (unsigned char)IX(3,6) };
      int c = hn_apply(b, m);
      CHECK(c == 2 && b[IX(2,6)] == HN_EMPTY && b[IX(4,6)] == HN_EMPTY, "double capture"); }

    clear(b);                                              /* no suicide */
    b[IX(2,6)] = HN_ATT; b[IX(4,6)] = HN_ATT; b[IX(3,2)] = HN_DEF;
    { hn_move m = { (unsigned char)IX(3,2), (unsigned char)IX(3,6) };
      int c = hn_apply(b, m);
      CHECK(c == 0 && b[IX(3,6)] == HN_DEF, "moving between two enemies is safe"); }

    clear(b);                                              /* the king is armed */
    b[IX(2,2)] = HN_KING; b[IX(2,6)] = HN_ATT; b[IX(2,7)] = HN_DEF;
    { hn_move m = { (unsigned char)IX(2,2), (unsigned char)IX(2,5) };
      int c = hn_apply(b, m);
      CHECK(c == 1 && b[IX(2,6)] == HN_EMPTY, "king participates in captures"); }

    /* defenders capture attackers too */
    clear(b); b[IX(3,6)] = HN_ATT; b[IX(4,6)] = HN_DEF; b[IX(2,2)] = HN_DEF;
    { hn_move m = { (unsigned char)IX(2,2), (unsigned char)IX(2,6) };
      int c = hn_apply(b, m);
      CHECK(c == 1 && b[IX(3,6)] == HN_EMPTY, "defenders capture attackers"); }

    /* ── 4. king capture ─────────────────────────────────────────────── */
    clear(b);                                              /* 4 attackers */
    b[IX(4,6)] = HN_KING; b[IX(3,6)] = HN_ATT; b[IX(5,6)] = HN_ATT; b[IX(4,7)] = HN_ATT;
    b[IX(0,5)] = HN_ATT;
    { hn_move m = { (unsigned char)IX(0,5), (unsigned char)IX(4,5) };
      hn_apply(b, m);
      CHECK(hn_king_pos(b) == -1, "king captured by 4 attackers");
      CHECK(hn_winner(b, HN_SIDE_DEF) == HN_WIN_ATT, "king capture = attacker win"); }

    clear(b);                                              /* 3 + throne */
    b[IX(4,3)] = HN_KING; b[IX(3,3)] = HN_ATT; b[IX(5,3)] = HN_ATT; b[IX(4,0)] = HN_ATT;
    { hn_move m = { (unsigned char)IX(4,0), (unsigned char)IX(4,2) };
      hn_apply(b, m);
      CHECK(hn_king_pos(b) == -1, "king captured by 3 attackers + throne"); }

    clear(b);                                              /* 2 is not enough */
    b[IX(4,6)] = HN_KING; b[IX(3,6)] = HN_ATT; b[IX(5,0)] = HN_ATT;
    { hn_move m = { (unsigned char)IX(5,0), (unsigned char)IX(5,6) };
      hn_apply(b, m);
      CHECK(hn_king_pos(b) == IX(4,6), "king survives 2-side pinch"); }

    clear(b);                                              /* edge: 3 sides only */
    b[IX(4,8)] = HN_KING; b[IX(3,8)] = HN_ATT; b[IX(5,8)] = HN_ATT; b[IX(4,0)] = HN_ATT;
    { hn_move m = { (unsigned char)IX(4,0), (unsigned char)IX(4,7) };
      hn_apply(b, m);
      CHECK(hn_king_pos(b) == IX(4,8), "king on edge survives 3 attackers"); }

    /* defender move never triggers king capture */
    clear(b);
    b[IX(4,6)] = HN_KING; b[IX(3,6)] = HN_ATT; b[IX(5,6)] = HN_ATT; b[IX(4,7)] = HN_ATT;
    b[IX(4,5)] = HN_DEF;  b[IX(0,2)] = HN_DEF;             /* spare defender */
    { hn_move m = { (unsigned char)IX(0,2), (unsigned char)IX(0,3) };
      hn_apply(b, m);
      CHECK(hn_king_pos(b) == IX(4,6), "defender move cannot capture own king"); }

    /* ── 5. winner detection ─────────────────────────────────────────── */
    clear(b); b[0] = HN_KING;
    CHECK(hn_winner(b, HN_SIDE_ATT) == HN_WIN_DEF, "king on corner = defender win");

    clear(b);                                              /* defenders stalemated */
    b[IX(4,6)] = HN_KING;
    b[IX(3,6)] = HN_ATT; b[IX(5,6)] = HN_ATT; b[IX(4,5)] = HN_ATT; b[IX(4,7)] = HN_ATT;
    /* note: king NOT captured (no attacker move applied), but has no move */
    CHECK(hn_winner(b, HN_SIDE_DEF) == HN_WIN_ATT, "side with no legal move loses");

    clear(b);                                              /* attackers stalemated */
    b[IX(1,0)] = HN_ATT; b[IX(2,0)] = HN_DEF; b[IX(1,1)] = HN_DEF; b[IX(4,6)] = HN_KING;
    CHECK(hn_winner(b, HN_SIDE_ATT) == HN_WIN_DEF, "attackers with no move lose");

    hn_init(b);
    CHECK(hn_winner(b, HN_SIDE_ATT) == HN_NONE, "initial position: no winner");

    /* ── 6. AI: mate-in-1 + legality ─────────────────────────────────── */
    clear(b); b[IX(0,4)] = HN_KING; b[IX(7,7)] = HN_ATT;   /* king escapes in 1 */
    { hn_move m = hn_ai_move(b, HN_SIDE_DEF, 3);
      n = hn_gen_moves(b, HN_SIDE_DEF, mv, HN_MAX_MOVES);
      CHECK(has_move(mv, n, m.from, m.to), "AI(def) move is legal");
      hn_apply(b, m);
      CHECK(hn_winner(b, HN_SIDE_ATT) == HN_WIN_DEF, "AI(def) takes the winning escape"); }

    clear(b);                                              /* attackers kill in 1 */
    b[IX(4,6)] = HN_KING; b[IX(3,6)] = HN_ATT; b[IX(5,6)] = HN_ATT; b[IX(4,7)] = HN_ATT;
    b[IX(0,5)] = HN_ATT;  b[IX(8,8 - 8)] = HN_DEF;         /* (8,0)? corner! use (7,1) */
    b[IX(8,0)] = HN_EMPTY; b[IX(7,1)] = HN_DEF;
    { hn_move m = hn_ai_move(b, HN_SIDE_ATT, 3);
      n = hn_gen_moves(b, HN_SIDE_ATT, mv, HN_MAX_MOVES);
      CHECK(has_move(mv, n, m.from, m.to), "AI(att) move is legal");
      hn_apply(b, m);
      CHECK(hn_king_pos(b) == -1, "AI(att) captures the king in 1"); }

    /* AI must BLOCK an immediate escape when it cannot win: king on column 0
       threatens (0,0) (the (0,8) path is already blocked by the attacker at
       (0,6)); the attacker at (5,2) can interpose at (0,2). */
    clear(b); b[IX(0,4)] = HN_KING; b[IX(0,6)] = HN_ATT; b[IX(5,2)] = HN_ATT;
    { hn_move m = hn_ai_move(b, HN_SIDE_ATT, 3);
      hn_apply(b, m);
      /* after the AI move the king must NOT have a win-in-1 anymore… king moves,
         then check: does any defender move reach a corner? */
      hn_move dm[HN_MAX_MOVES];
      int dn = hn_gen_moves(b, HN_SIDE_DEF, dm, HN_MAX_MOVES);
      int king_can_win = 0;
      for (int i = 0; i < dn; ++i) {
          unsigned char nb[81]; memcpy(nb, b, 81);
          hn_apply(nb, dm[i]);
          if (hn_winner(nb, HN_SIDE_ATT) == HN_WIN_DEF) { king_can_win = 1; break; }
      }
      CHECK(!king_can_win, "AI(att) blocks the escape-in-1"); }

    /* ── 7. self-play soak: structural invariants ────────────────────── */
    {
        int ended = 0, long_games = 0;
        for (int g = 0; g < 200; ++g) {
            g_seed = (unsigned)(g * 2654435761u + 12345u); if (!g_seed) g_seed = 7;
            hn_init(b);
            int side = HN_SIDE_ATT, winner = HN_NONE, prev_att = 16, prev_def = 9;
            for (int ply = 0; ply < 400; ++ply) {
                winner = hn_winner(b, side);
                if (winner != HN_NONE) break;
                hn_move m;
                if (ply < 2) {                     /* random opening for variety */
                    n = hn_gen_moves(b, side, mv, HN_MAX_MOVES);
                    m = mv[rnd() % (unsigned)n];
                } else {
                    m = hn_ai_move(b, side, 1);
                }
                hn_apply(b, m);
                int att = count_piece(b, HN_ATT);
                int def = count_piece(b, HN_DEF) + count_piece(b, HN_KING);
                if (att > prev_att || def > prev_def) { printf("FAIL: pieces increased (game %d)\n", g); ++g_fail; break; }
                prev_att = att; prev_def = def;
                if (!board_valid(b)) { printf("FAIL: invalid board (game %d ply %d)\n", g, ply); ++g_fail; break; }
                side = 1 - side;
            }
            if (winner != HN_NONE) ++ended; else ++long_games;
        }
        printf("self-play: %d/200 decisive, %d hit the 400-ply cap\n", ended, long_games);
        CHECK(ended >= 100, "at least half of depth-1 self-play games end decisively");
    }

    /* deeper search soak: depth 2 vs 2, exercises recursion + node budget */
    {
        int bad = 0;
        for (int g = 0; g < 20; ++g) {
            g_seed = (unsigned)(g * 40503u + 977u); if (!g_seed) g_seed = 3;
            hn_init(b);
            int side = HN_SIDE_ATT;
            for (int ply = 0; ply < 120; ++ply) {
                if (hn_winner(b, side) != HN_NONE) break;
                hn_move m;
                if (ply < 2) { n = hn_gen_moves(b, side, mv, HN_MAX_MOVES); m = mv[rnd() % (unsigned)n]; }
                else         { m = hn_ai_move(b, side, 2); }
                n = hn_gen_moves(b, side, mv, HN_MAX_MOVES);
                if (!has_move(mv, n, m.from, m.to)) { ++bad; break; }
                hn_apply(b, m);
                if (!board_valid(b)) { ++bad; break; }
                side = 1 - side;
            }
        }
        CHECK(bad == 0, "depth-2 self-play: every AI move legal, board always valid");
    }

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
