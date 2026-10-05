# Design notes

Options considered and not taken, kept so a later session can pick one up
without redoing the survey.

## Spilling `Node.aux`

`Node{tag:u8, flags:u8, aux:u16, tok:u32, size:u32}` is 12 bytes; `aux` holds
small per-kind data (a union/enum flag, the asm section count, ...). Two kinds
used to need more than 16 bits and saturated at 65535:

- `N_STRING`: the number of concatenated pieces (gcc.dg/concat2.c has 100000).
  Token offsets computed from it (`last_tok`) went wrong too.
- `N_ERROR`: the number of skipped tokens (read only by the AST dump).

**Taken (Round 117):** string pieces are not stored. A string node starts at
`tok` and covers the run of `TK_STRING` tokens, so `node_pieces(c, i)`
(check_int.h) counts them. `N_ERROR` keeps its saturating count.

**Not taken: a spill table, as in rustc's `Span`.** Keep `aux:u16`; the value
`0xFFFF` means "the real value is in a side table". rustc does the same for a
span that does not fit inline (an index into an interner).

- Table: per unit, a `(node index, u32 value)` vector. Nodes are emitted in
  post-order and spill as they are emitted, so it is already sorted by node
  index; lookup is a binary search, and it is empty for nearly every unit.
- Reads go through `node_aux(c, i)`: `a = n->aux; return a == 0xFFFF ?
  spill_lookup(c, i) : a;`. Direct `->aux` reads must be converted; today
  most are for kinds that cannot spill.
- Writes: `set_aux(p, v)` stores `0xFFFF` and pushes `(i, v)` when `v >=
  0xFFFF`.
- Cost: no node growth, a branch per read of a spillable kind, and a table
  that must be cleared with the unit's arena (see PARALLEL.md for the
  per-thread unit lifetime).
- Use it if a future node kind needs unbounded inline data that cannot be
  derived from the tokens. Strings were derivable, so it was not needed.

**Rejected: widening `aux` to `u32`.** `Node` becomes 16 bytes (+33% of every
unit's node array) for two rare cases.
