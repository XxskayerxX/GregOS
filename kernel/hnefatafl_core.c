/* hnefatafl_core — pure-C Tablut 9×9 rules engine + negamax AI for GregOS.
   Freestanding: no libc, no allocation, no floats, no randomness. The same
   file compiles into the kernel and on the host (test suite).               */

#include "../include/hnefatafl_core.h"

#define THRONE 40

static int is_corner(int i)     { return i == 0 || i == 8 || i == 72 || i == 80; }
static int is_restricted(int i) { return i == THRONE || is_corner(i); }

/* side of a piece value (HN_ATT / HN_DEF / HN_KING) */
static int side_of(int v) { return v == HN_ATT ? HN_SIDE_ATT : HN_SIDE_DEF; }

/* ── initial position ──────────────────────────────────────────────────────── */
void hn_init(unsigned char b[81])
{
    for (int i = 0; i < 81; ++i) b[i] = HN_EMPTY;

    /* attackers: T-groups at the middle of each edge */
    static const unsigned char att[16] = {
        3, 4, 5, 13,                    /* north: (3,0)(4,0)(5,0)(4,1)   */
        75, 76, 77, 67,                 /* south: (3,8)(4,8)(5,8)(4,7)   */
        27, 36, 45, 37,                 /* west:  (0,3)(0,4)(0,5)(1,4)   */
        35, 44, 53, 43,                 /* east:  (8,3)(8,4)(8,5)(7,4)   */
    };
    /* defenders: orthogonal cross around the throne */
    static const unsigned char def[8] = {
        22, 31, 49, 58,                 /* (4,2)(4,3)(4,5)(4,6)          */
        38, 39, 41, 42,                 /* (2,4)(3,4)(5,4)(6,4)          */
    };
    for (int i = 0; i < 16; ++i) b[att[i]] = HN_ATT;
    for (int i = 0; i < 8;  ++i) b[def[i]] = HN_DEF;
    b[THRONE] = HN_KING;
}

/* ── move generation ───────────────────────────────────────────────────────── */
int hn_gen_moves(const unsigned char b[81], int side, hn_move* out, int max)
{
    static const int DX[4] = { 1, -1, 0, 0 };
    static const int DY[4] = { 0, 0, 1, -1 };
    int n = 0;

    for (int from = 0; from < 81; ++from) {
        int v = b[from];
        if (v == HN_EMPTY || side_of(v) != side) continue;
        int king = (v == HN_KING);
        int fx = from % 9, fy = from / 9;

        for (int d = 0; d < 4; ++d) {
            int x = fx, y = fy;
            for (;;) {
                x += DX[d]; y += DY[d];
                if (x < 0 || x > 8 || y < 0 || y > 8) break;
                int to = y * 9 + x;
                if (b[to] != HN_EMPTY) break;
                if (!king && is_restricted(to)) break;   /* no landing, no crossing */
                if (n < max) { out[n].from = (unsigned char)from;
                               out[n].to   = (unsigned char)to; ++n; }
                else return n;
            }
        }
    }
    return n;
}

/* ── apply + captures ──────────────────────────────────────────────────────── */

/* far square f supports a capture by `side` if it holds a friendly piece or is
   a hostile square (corner, or the throne while empty). */
static int assists(const unsigned char b[81], int f, int side)
{
    if (is_corner(f)) return 1;
    if (f == THRONE && b[f] == HN_EMPTY) return 1;
    int v = b[f];
    return v != HN_EMPTY && side_of(v) == side;
}

int hn_apply(unsigned char b[81], hn_move m)
{
    static const int DX[4] = { 1, -1, 0, 0 };
    static const int DY[4] = { 0, 0, 1, -1 };

    int mover = b[m.from];
    int side  = side_of(mover);
    b[m.to]   = (unsigned char)mover;
    b[m.from] = HN_EMPTY;

    int captured = 0;
    int tx = m.to % 9, ty = m.to / 9;

    /* custodian captures of enemy SOLDIERS around the arrival square */
    int enemy_soldier = (side == HN_SIDE_ATT) ? HN_DEF : HN_ATT;
    for (int d = 0; d < 4; ++d) {
        int nx = tx + DX[d],     ny = ty + DY[d];
        int fx = tx + 2 * DX[d], fy = ty + 2 * DY[d];
        if (nx < 0 || nx > 8 || ny < 0 || ny > 8) continue;
        if (fx < 0 || fx > 8 || fy < 0 || fy > 8) continue;
        int ni = ny * 9 + nx;
        if (b[ni] != enemy_soldier) continue;
        if (assists(b, fy * 9 + fx, side)) { b[ni] = HN_EMPTY; ++captured; }
    }

    /* king encirclement — only an attacker move can complete it */
    if (side == HN_SIDE_ATT) {
        int k = hn_king_pos(b);
        if (k >= 0) {
            int kx = k % 9, ky = k / 9, hostile = 0, sides = 0;
            for (int d = 0; d < 4; ++d) {
                int nx = kx + DX[d], ny = ky + DY[d];
                if (nx < 0 || nx > 8 || ny < 0 || ny > 8) continue;
                ++sides;
                int ni = ny * 9 + nx;
                if (b[ni] == HN_ATT || ni == THRONE) ++hostile;
            }
            if (sides == 4 && hostile == 4) { b[k] = HN_EMPTY; ++captured; }
        }
    }
    return captured;
}

/* ── end of game ───────────────────────────────────────────────────────────── */
int hn_king_pos(const unsigned char b[81])
{
    for (int i = 0; i < 81; ++i)
        if (b[i] == HN_KING) return i;
    return -1;
}

int hn_winner(const unsigned char b[81], int side_to_move)
{
    int k = hn_king_pos(b);
    if (k < 0)         return HN_WIN_ATT;
    if (is_corner(k))  return HN_WIN_DEF;

    hn_move mv[1];
    if (hn_gen_moves(b, side_to_move, mv, 1) == 0)
        return side_to_move == HN_SIDE_ATT ? HN_WIN_DEF : HN_WIN_ATT;
    return HN_NONE;
}

/* ── evaluation (positive = good for the attackers) ────────────────────────── */
static int hn_eval(const unsigned char b[81])
{
    int natt = 0, ndef = 0, k = -1;
    for (int i = 0; i < 81; ++i) {
        if      (b[i] == HN_ATT)  ++natt;
        else if (b[i] == HN_DEF)  ++ndef;
        else if (b[i] == HN_KING) k = i;
    }
    if (k < 0)        return  10000;
    if (is_corner(k)) return -10000;

    int score = 16 * natt - 24 * ndef;

    /* king's distance to his nearest corner (Manhattan) — far is good for att */
    int kx = k % 9, ky = k / 9;
    int dx0 = kx, dx8 = 8 - kx, dy0 = ky, dy8 = 8 - ky;
    int d = dx0 + dy0;
    if (dx8 + dy0 < d) d = dx8 + dy0;
    if (dx0 + dy8 < d) d = dx0 + dy8;
    if (dx8 + dy8 < d) d = dx8 + dy8;
    score += 6 * d;

    /* open rook lines from the king straight into a corner: escape threats */
    static const int DX[4] = { 1, -1, 0, 0 };
    static const int DY[4] = { 0, 0, 1, -1 };
    int paths = 0;
    for (int dir = 0; dir < 4; ++dir) {
        int x = kx, y = ky;
        for (;;) {
            x += DX[dir]; y += DY[dir];
            if (x < 0 || x > 8 || y < 0 || y > 8) break;
            int i = y * 9 + x;
            if (b[i] != HN_EMPTY) break;
            if (is_corner(i)) { ++paths; break; }
        }
    }
    score -= 120 * paths;

    /* pressure: attackers (and the hostile throne) around the king */
    for (int dir = 0; dir < 4; ++dir) {
        int x = kx + DX[dir], y = ky + DY[dir];
        if (x < 0 || x > 8 || y < 0 || y > 8) continue;
        int i = y * 9 + x;
        if      (b[i] == HN_ATT) score += 25;
        else if (i == THRONE)    score += 10;
    }
    return score;
}

/* ── negamax + alpha-beta, node-budgeted ───────────────────────────────────── */
static long g_nodes;
#define HN_NODE_BUDGET 25000L   /* ≈5 ms host, ≈0.1-0.3 s under QEMU TCG — far
                                   above a typical depth-3 tree (~4-10k nodes) */

static int negamax(const unsigned char b[81], int side, int depth,
                   int alpha, int beta)
{
    ++g_nodes;

    int k = hn_king_pos(b);
    if (k < 0)        return side == HN_SIDE_ATT ?  10000 + depth : -(10000 + depth);
    if (is_corner(k)) return side == HN_SIDE_ATT ? -(10000 + depth) : 10000 + depth;

    if (depth <= 0 || g_nodes > HN_NODE_BUDGET) {
        int e = hn_eval(b);
        return side == HN_SIDE_ATT ? e : -e;
    }

    hn_move mv[HN_MAX_MOVES];
    int n = hn_gen_moves(b, side, mv, HN_MAX_MOVES);
    if (n == 0) return -(10000 + depth);          /* no move → current side loses */

    int best = -32000;
    for (int i = 0; i < n; ++i) {
        unsigned char nb[81];
        for (int j = 0; j < 81; ++j) nb[j] = b[j];
        hn_apply(nb, mv[i]);
        int s = -negamax(nb, 1 - side, depth - 1, -beta, -alpha);
        if (s > best)  best = s;
        if (best > alpha) alpha = best;
        if (alpha >= beta) break;
    }
    return best;
}

hn_move hn_ai_move(const unsigned char b[81], int side, int depth)
{
    hn_move mv[HN_MAX_MOVES];
    int n = hn_gen_moves(b, side, mv, HN_MAX_MOVES);
    hn_move best_m = { 0, 0 };
    if (n == 0) return best_m;
    best_m = mv[0];

    g_nodes = 0;
    int best = -32000, alpha = -32000, beta = 32000;
    for (int i = 0; i < n; ++i) {
        /* budget exhausted: children would return optimistic 1-ply statics that
           could displace a fully-searched best — keep what we have instead    */
        if (g_nodes > HN_NODE_BUDGET) break;
        unsigned char nb[81];
        for (int j = 0; j < 81; ++j) nb[j] = b[j];
        hn_apply(nb, mv[i]);
        int s = -negamax(nb, 1 - side, depth - 1, -beta, -alpha);
        if (s > best) { best = s; best_m = mv[i]; }
        if (best > alpha) alpha = best;
    }
    return best_m;
}
