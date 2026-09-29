/* index.h - the LSP-facing model of a translation unit's macros.
 *
 * Built as a PP listener while the TU is preprocessed; answers the queries
 * an editor needs so that macros get the same features as variables and
 * functions: definition, references, rename, hover, completion, document
 * symbols, semantic tokens, folding and inactive regions. */
#ifndef CEREAL_INDEX_H
#define CEREAL_INDEX_H

#include "mgraph.h"
#include "parclient.h"
#include "pp.h"

enum {
    IREF_IN_BODY  = 1 << 0,   /* spelled inside a #define replacement list */
    IREF_PASTED   = 1 << 1,   /* name synthesized by ##: not renamable */
    IREF_FROM_ARG = 1 << 2,   /* spelled in a macro argument */
    IREF_STATIC   = 1 << 3,   /* textual occurrence in a body, unresolved */
    IREF_SYSTEM   = 1 << 4    /* in a system header */
};

typedef struct IdxRef {
    struct Ident *name;
    Macro *macro;             /* NULL for undefined names / static refs */
    SrcLoc loc;
    uint32_t len;
    RefKind kind;
    unsigned flags;
    Expansion *exp;
    /* cells mode (query results): the expansion's place in sequential
     * order, and where the ref lives (cell index + 1, 0: phase A) */
    uint64_t order;
    uint32_t cell, cexp;
} IdxRef;

typedef struct IdxParamRef {
    Macro *macro;
    int param;
    SrcLoc loc;
    uint32_t len;
} IdxParamRef;

typedef struct IdxExp {
    Expansion *e;
    Expansion *root;          /* outermost (file-level) expansion */
    int depth;
    char **args;              /* raw argument text */
    int nargs;
    StrBuf text;              /* root only: final expanded text */
    VEC(struct Ident *) arg_names; /* identifiers spelled in the arguments */
    uint32_t key;             /* parallel runs: plan item of the event */
} IdxExp;

/* Parallel runs: an expansion joined from a worker, awaiting renumbering. */
typedef struct IdxJoined {
    IdxExp *x;
    Expansion *parent, *root;
} IdxJoined;

typedef struct IdxCheckpoint {
    SrcLoc loc;
    uint32_t seq;
} IdxCheckpoint;

typedef struct IdxInclusion {
    SrcFile *file;
    VEC(IdxCheckpoint) cps;
} IdxInclusion;

typedef struct IdxInclude {
    SrcFile *from, *to;
    SrcLoc hash_loc, name_loc, name_end;
    const char *spelled;
    IncludeResult result;
} IdxInclude;

typedef struct IdxBlock {
    SrcLoc begin, end;        /* #if .. #endif */
} IdxBlock;

/* Cells mode: a cell of the build, taken as it is (see index_par_client). */
typedef struct IdxCell {
    struct Cell *cell;        /* a reference */
    const void *blob;         /* the index's part */
    uint32_t s;               /* its first plan item */
    uint32_t nreads;
    Macro **rmacro;           /* what its reads found in this build */
} IdxCell;

typedef struct Index {
    PP *pp;
    Arena *arena;
    SrcMgr *sm;
    VEC(IdxRef) refs;
    VEC(IdxParamRef) params;
    VEC(IdxExp *) exps;       /* indexed by Expansion id */
    VEC(IdxInclusion *) inclusions;
    VEC(IdxInclusion *) stack;
    VEC(IdxInclude) includes;
    VEC(SrcRange) inactive;
    VEC(IdxBlock) blocks;
    VEC(SrcLoc) open_blocks;
    /* parallel runs: a worker's slice, filtered and sorted by prepare */
    VEC(IdxJoined) prep_kept;
    VEC(IdxRef) prep_refs;
    VEC(struct Index *) runs;  /* main: joined workers, for the ref merge */
    int8_t refs_by_id;        /* refs in expansion id order (0: yes, as far
                                 as recorded; -1: no) */
    bool sorted;
    uint32_t max_len;         /* longest ref (after sorting) */
    /* Cells mode: phase A's records are here as usual, the text's stay in
     * the build's cells and queries walk them (absolute positions from the
     * plan's items).  Global expansion ids and ref order are never made:
     * index_dump_json and index_check_graph need a materialized index. */
    bool want_cells;          /* set before a parallel run with a cache */
    bool cells_mode;
    VEC(IdxCell) cells;
    PlanItem *items;
    size_t nitems;
    uint32_t *item_cell;      /* item -> index in cells */
    struct IdxSpan *spans;    /* lazily: item text ranges by start */
    size_t nspans;
    uint32_t *mr_start;       /* lazily: by Macro.id, into mr */
    struct IdxMacRead *mr;    /* (cell, read) pairs that found a definition */
} Index;

typedef enum { TGT_NONE, TGT_MACRO, TGT_PARAM, TGT_INCLUDE } TargetKind;

typedef struct IdxTarget {
    TargetKind kind;
    struct Ident *name;
    Macro *macros[16];        /* candidate definitions (usually one) */
    int nmacros;
    int param;                /* TGT_PARAM */
    SrcFile *file;            /* TGT_INCLUDE */
    SrcRange range;           /* extent of the symbol under the cursor */
    IdxExp *top;              /* file-level expansion at the cursor, if any */
} IdxTarget;

void index_init(Index *ix, PP *pp);
void index_free(Index *ix);
/* Drain the preprocessor, recording expansion results. */
void index_run(Index *ix);
/* Take part in a parallel run instead of index_run (after index_init);
 * the result is the one a sequential run gives. */
ParClient index_par_client(Index *ix);

IdxTarget index_resolve(Index *ix, SrcLoc loc);
/* The macro version in effect at loc (end of TU if never reached). */
uint32_t index_seq_at(Index *ix, SrcLoc loc);
/* Macros live at loc (for completion), in definition order. */
size_t index_visible(Index *ix, SrcLoc loc, Macro ***out);
/* All references to the target (definitions first). */
size_t index_references(Index *ix, const IdxTarget *t, IdxRef **out);
/* Every ref located in f, in index order. */
size_t index_file_refs(Index *ix, const SrcFile *f, IdxRef **out);
/* ... located in [b, e] (inside one file): only the cells and definitions
 * meeting the range are read. */
size_t index_range_refs(Index *ix, SrcLoc b, SrcLoc e, IdxRef **out);

/* Call hierarchy (LSP incoming/outgoing calls) for a definition.  Static
 * edges come from the macro graph: a callee is any definition of a name
 * in the body that is live while the caller is; a caller is any
 * definition naming the callee while it is live.  Observed edges come
 * from the expansions actually performed; calls through a name that
 * arrived as an argument belong to whoever spelled the argument. */
typedef struct IdxCall {
    struct Ident *name;       /* as spelled (callees) */
    Macro *macro;             /* NULL: the name is never a macro then */
    unsigned observed;        /* expansions seen along this edge */
    bool pasted;              /* only via a name formed by ## */
} IdxCall;

size_t index_callees(Index *ix, const MacroGraph *g, Macro *m, IdxCall **out);
size_t index_callers(Index *ix, const MacroGraph *g, Macro *m, IdxCall **out);

/* Soundness check of the macro graph against what was expanded: every
 * expansion under a file-level invocation must be in the static closure
 * of the invocation's name and of the identifiers in the arguments of its
 * tree, at the versions involved; a name formed by ## only when that
 * closure is open.  Prints violations; returns their number. */
size_t index_check_graph(Index *ix, const MacroGraph *g, FILE *out,
                         size_t *checked);

SrcFile *index_find_file(Index *ix, const char *path);
/* Cells mode: where the cells' memory goes and how much of it repeats
 * (for sizing hash-consing). */
void index_cell_stats(Index *ix, FILE *out);
void index_dump_json(Index *ix, FILE *out, bool all);

#endif
