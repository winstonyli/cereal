/* skel.h - per-file directive skeleton.
 *
 * A static view of one file's directives, independent of any configuration:
 * it includes directives inside inactive groups, which the preprocessor
 * never evaluates.  Used by the configuration-space and include analyses. */
#ifndef CEREAL_SKEL_H
#define CEREAL_SKEL_H

#include "common.h"
#include "diag.h"
#include "lex.h"
#include "srcmgr.h"

typedef enum {
    SK_IF,
    SK_IFDEF,
    SK_IFNDEF,
    SK_ELIF,
    SK_ELSE,
    SK_ENDIF,
    SK_DEFINE,
    SK_UNDEF,
    SK_INCLUDE,
    SK_OTHER
} SkKind;

typedef struct SkDirective {
    SkKind kind;
    SrcLoc hash_loc;
    SrcLoc kw_loc;
    SrcLoc end_loc;
    TokSpan toks;           /* tokens after the keyword (arena copy) */
    struct Ident *name;     /* ifdef/ifndef/define/undef operand */
    const char *comment;    /* trailing comment text (for #else/#endif) */
    int depth;              /* conditional nesting depth (0 = top level) */
    int opener;             /* for elif/else/endif: index of the #if */
} SkDirective;

typedef struct Skeleton {
    SrcFile *file;
    VEC(SkDirective) dirs;
    size_t ntokens;          /* non-directive tokens */
    size_t ntokens_toplevel; /* ... outside every conditional */
    /* include-guard shape: #ifndef G / #define D ... #endif covering all */
    struct Ident *guard_ifndef;
    struct Ident *guard_define;
    int guard_ifndef_index;
    bool guard_covers_file;
} Skeleton;

Skeleton *skel_get(Arena *a, SrcMgr *sm, Interner *in, LexOptions lo,
                   SrcFile *f);

#endif
