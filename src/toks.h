/* toks.h - the token stream of a cell build, without storing it.
 *
 * A build with a cell cache (par.c) covers the TU with cells: runs of
 * plan items [s, e) that preprocess starting fresh at s and end clean at
 * e.  Such a run gives the same tokens whenever it is run again against
 * the same build, so a build keeps only, per cell, how many tokens it
 * gave and their sequence hash; a consumer (the parser) regenerates the
 * tokens of the cells it needs with a TokCursor.  A reused cell's count
 * and hash are the ones stored with it, which is sound because the cell's
 * reads found the same definitions.
 *
 * Token hashes cover what a token *is*: kind, punctuator, spelling and
 * provenance class (TF_ORIGIN_*, TF_PASTED, TF_SYNTH), not its location
 * or the white space around it.  Sequence hashes are polynomial mod
 * 2^61 - 1 (hash.h), so the hash of any run of cells is computed from the
 * cells' own.  See docs/PARSER.md. */
#ifndef CEREAL_TOKS_H
#define CEREAL_TOKS_H

#include "driver.h"
#include "hash.h"
#include "plan.h"
#include "thread.h"

/* Tokens a consumer sees (not phase B's directive markers). */
static inline bool tok_in_stream(const Tok *t)
{
    return t->kind != TK_EOF && t->kind != TK_DIRMARK;
}

#define TOK_HASH_FLAGS (TF_ORIGIN_BODY | TF_ORIGIN_ARG | TF_PASTED | TF_SYNTH)

static inline uint64_t tok_hash(const PP *pp, const Tok *t)
{
    uint64_t seed = (uint64_t)t->kind | (uint64_t)t->punct << 8 |
                    (uint64_t)(t->flags & TOK_HASH_FLAGS) << 16;
    if (t->kind == TK_PUNCT)
        return hash64_mix(seed, 0);
    if (t->kind == TK_IDENT)
        return hash64_mix(seed, ident_by_id(pp->in, t->aux)->digest);
    return hash64(pp_text(pp, t), t->len, seed);
}

/* One cell of a build, in TU order. */
typedef struct TokCell {
    uint32_t s, e;              /* plan items */
    uint32_t ntoks;
    uint64_t hash;              /* sequence hash of its tokens */
    bool reused;                /* from the cache (not run this build) */
} TokCell;

/* A finished cell build's token source: its plan (moved out of par_run)
 * and cells.  Valid while the TU is. */
typedef struct TokRegen {
    TU *tu;
    Plan plan;
    TokCell *cells;
    size_t ncells;
    Mutex m;                    /* guards spare */
    VEC(ScratchCursor) spare;   /* scratch for cursors, reused */
} TokRegen;

void tokregen_init(TokRegen *s);
void tokregen_free(TokRegen *s);
/* Tokens and hash of the whole TU, from the cells' records. */
uint64_t tokregen_hash(const TokRegen *s, uint64_t *ntoks);

/* Regenerates the tokens of plan items [s, e) of a source's build, which
 * must be a run of whole cells.  Cursors are independent: any number may
 * be open at once, on any threads.  Token spellings in scratch space are
 * valid until the cursor is closed, or for as long as the TU if it is
 * closed keeping them (a consumer holding tokens across cells: the
 * parser). */
typedef struct TokCursor {
    TokRegen *src;
    Arena arena;
    DiagEngine diag;
    PP pp;
    size_t end;
    bool unclean;               /* e was not a clean boundary: a bug */
    ScratchMark mark;
} TokCursor;

void tokcur_open(TokCursor *c, TokRegen *src, size_t s, size_t e);
/* The next token and its presentation location (the expansion point of a
 * token from a macro); false at the end. */
bool tokcur_next(TokCursor *c, Tok *t, SrcLoc *exp_loc, SrcLoc *mloc);
void tokcur_close(TokCursor *c, bool keep_spellings);

#endif
