#ifndef HNEFATAFL_CORE_H
#define HNEFATAFL_CORE_H

/* ── hnefatafl_core — pure-C Tablut 9×9 rules engine + minimax AI ────────────
   Zero kernel dependencies, zero libc, zero allocation: the same file compiles
   freestanding into the kernel (picked up by the Makefile's kernel C glob) AND
   natively on the host for the offline test suite (scratchpad/hn_test.c).

   Rules (Tablut, strong king):
   - 9×9 board; centre (4,4) = throne; the 4 corners are the king's exits.
   - All pieces move like rooks. Only the king may stop on or cross the throne
     and stop on corners; soldiers can neither land on nor pass through them.
   - Throne (when empty) and corners are hostile: they count as an enemy piece
     for custodian captures.
   - Custodian capture: after a move, any enemy SOLDIER orthogonally adjacent
     to the arrival square is removed if the square beyond it holds a friendly
     piece or is hostile. Multiple captures per move; no self-capture (moving
     between two enemies is safe). The king is armed (captures as a defender).
   - King capture: after an ATTACKER move only — the king falls iff all 4 of
     its orthogonal neighbours exist and each is an attacker or the throne.
     (On an edge only 3 neighbours exist → the king cannot be taken there.)
   - Win: king reaches a corner → defenders; king captured → attackers;
     side to move with no legal move loses.
   - Attackers move first.                                                     */

#ifdef __cplusplus
extern "C" {
#endif

enum { HN_EMPTY = 0, HN_ATT = 1, HN_DEF = 2, HN_KING = 3 };
enum { HN_SIDE_ATT = 0, HN_SIDE_DEF = 1 };
enum { HN_NONE = 0, HN_WIN_ATT = 1, HN_WIN_DEF = 2 };

#define HN_MAX_MOVES 320   /* upper bound on legal moves for one side */

typedef struct { unsigned char from, to; } hn_move;   /* square index y*9+x */

/* Set up the initial Tablut position. */
void hn_init(unsigned char b[81]);

/* Generate all legal moves for `side` into out[max]; returns the count. */
int hn_gen_moves(const unsigned char b[81], int side, hn_move* out, int max);

/* Play `m` on the board (assumed legal): moves the piece, removes captured
   soldiers, and removes the king if the move completes his encirclement.
   Returns the number of pieces captured (king counts as one).              */
int hn_apply(unsigned char b[81], hn_move m);

/* HN_NONE while play continues, else HN_WIN_ATT / HN_WIN_DEF.
   `side_to_move` is needed for the no-legal-move loss rule.                */
int hn_winner(const unsigned char b[81], int side_to_move);

/* King's square index, or -1 if he has been captured. */
int hn_king_pos(const unsigned char b[81]);

/* Best move for `side` — negamax + alpha-beta, `depth` plies, node-budgeted
   (deterministic and time-bounded). Board must have at least one legal move
   for `side` (check hn_winner first).                                       */
hn_move hn_ai_move(const unsigned char b[81], int side, int depth);

#ifdef __cplusplus
}
#endif

#endif /* HNEFATAFL_CORE_H */
