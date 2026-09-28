/* pp.h - translation phase 4: the preprocessor (C99 6.10).
 *
 * Streaming design:
 *   - file tokens come straight from a Lexer, one at a time;
 *   - each macro expansion pushes a Context whose tokens live in a pooled
 *     buffer, recycled when the context is exhausted (memory is bounded by
 *     the deepest expansion, not by file size);
 *   - recursion is prevented the GCC/Clang way: a macro is disabled while
 *     its context is live, and identifiers read while their macro is
 *     disabled are painted TF_NOEXPAND for good. */
#ifndef CEREAL_PP_H
#define CEREAL_PP_H

#include "common.h"
#include "diag.h"
#include "lex.h"
#include "srcmgr.h"
#include "token.h"

#define NO_EXP UINT32_MAX

typedef enum {
    BUILTIN_NONE,
    BUILTIN_FILE,
    BUILTIN_LINE,
    BUILTIN_DATE,
    BUILTIN_TIME,
    BUILTIN_COUNTER,
    BUILTIN_INCLUDE_LEVEL,
    BUILTIN_BASE_FILE,
    BUILTIN_HAS_INCLUDE,
    BUILTIN_HAS_INCLUDE_NEXT,
    BUILTIN_HAS_ATTRIBUTE,
    BUILTIN_HAS_BUILTIN,
    BUILTIN_HAS_C_ATTRIBUTE,
    BUILTIN_HAS_CPP_ATTRIBUTE,
    BUILTIN_PRAGMA_OP        /* _Pragma */
} BuiltinKind;

typedef struct Macro {
    struct Ident *name;
    uint32_t id;             /* dense, 0-based, in definition order */
    bool funclike;
    bool variadic;           /* last param is __VA_ARGS__ (or GNU named) */
    bool gnu_named_variadic;
    bool predefined;         /* from <built-in> or <command line> */
    bool disabled;           /* its expansion context is live */
    bool has_ops;            /* body contains # or ## */
    BuiltinKind builtin;
    int nparams;
    struct Ident **params;
    SrcLoc *param_locs;
    uint8_t *param_raw;      /* param used next to # / ## */
    uint8_t *param_expanded; /* param used as a plain operand */
    Tok *body;               /* replacement list; params flagged TF_PARAM */
    uint32_t body_len;
    SrcLoc hash_loc, name_loc, end_loc;
    SrcLoc undef_loc;        /* 0 while live */
    uint32_t def_seq, undef_seq;
    struct Macro *prev;      /* previous definition of the same name */
    struct SrcFile *file;
    uint32_t expansions;
    uint32_t cond_refs;
    void *user;
} Macro;

/* One macro invocation (recorded only when tracking is on). */
typedef struct Expansion {
    uint32_t id;
    uint32_t parent;         /* expansion the name token came from, or NO_EXP */
    uint32_t root;           /* outermost expansion */
    uint16_t depth;
    uint16_t name_flags;     /* TF_ORIGIN_* / TF_PASTED of the name token */
    bool in_directive;
    Macro *macro;
    SrcLoc name_loc;         /* spelling loc of the name token */
    SrcLoc end_loc;          /* end of ')' for function-like */
} Expansion;

typedef enum {
    COND_IF,
    COND_IFDEF,
    COND_IFNDEF,
    COND_ELIF,
    COND_ELSE,
    COND_ENDIF
} CondKind;

typedef struct CondEvent {
    CondKind kind;
    SrcLoc hash_loc, kw_loc, end_loc;
    TokSpan expr;            /* raw (unexpanded) condition tokens */
    bool evaluated;
    bool value;
    bool taken;
} CondEvent;

typedef enum {
    REF_EXPANSION,
    REF_IFDEF,
    REF_DEFINED,
    REF_UNDEF,
    REF_PRAGMA,
    REF_IF_VALUE             /* undefined identifier evaluated as 0 in #if */
} RefKind;

typedef enum {
    INC_OK,
    INC_SKIPPED_GUARD,
    INC_SKIPPED_ONCE,
    INC_NOT_FOUND
} IncludeResult;

typedef struct IncludeEvent {
    SrcLoc hash_loc, name_loc, name_end;
    const char *spelled;
    bool angled, next, macro_expanded;
    struct SrcFile *file;
    struct SrcFile *from;
    IncludeResult result;
} IncludeEvent;

/* Views passed to listeners (TokSpan, Tok*) are valid only during the
 * callback; Tok values themselves may be copied and kept. */
typedef struct PPListener {
    void *ctx;
    void (*file_enter)(void *ctx, SrcFile *f, const IncludeEvent *via);
    void (*file_exit)(void *ctx, SrcFile *f);
    void (*include)(void *ctx, const IncludeEvent *ev);
    void (*define)(void *ctx, Macro *m, Macro *replaced);
    void (*undef)(void *ctx, struct Ident *id, Macro *m, SrcLoc hash_loc,
                  SrcLoc name_loc);
    void (*expand)(void *ctx, const Expansion *e, const TokSpan *args,
                   int nargs);
    void (*macro_ref)(void *ctx, struct Ident *id, Macro *m, const Tok *tok,
                      RefKind kind);
    void (*cond)(void *ctx, const CondEvent *ev);
    void (*skipped)(void *ctx, SrcLoc begin, SrcLoc end);
    void (*pragma)(void *ctx, SrcLoc loc, TokSpan toks);
    void (*checkpoint)(void *ctx, SrcLoc loc, uint32_t seq);
} PPListener;

/* ---- token buffers -------------------------------------------------- */

typedef struct TokBuf {
    Tok *t;
    uint32_t len, cap;
} TokBuf;

#define POOL_CLASSES 26
typedef struct TokPool {
    VEC(Tok *) free[POOL_CLASSES];
} TokPool;

typedef struct Context {
    const Tok *toks;
    uint32_t pos, end;
    TokBuf owned;            /* returned to the pool when popped */
    Macro *macro;            /* disabled while live */
    uint32_t exp_id, root_id;
    SrcLoc exp_loc;          /* outermost call site (for -E, __LINE__) */
    SrcLoc name_loc;         /* for "in expansion of" notes */
    bool barrier;            /* sub-stream end: yields EOF, never popped */
} Context;

typedef struct CondFrame {
    struct CondFrame *prev;
    CondKind kind;
    SrcLoc if_loc;
    bool taken_any, active, parent_active, seen_else;
    int include_depth;
} CondFrame;

typedef struct IncludeFrame {
    struct IncludeFrame *prev;
    SrcFile *file;
    Lexer saved_lex;         /* includer's lexer */
    Tok saved_pending;
    bool saved_has_pending;
    SrcLoc include_loc;
    int dir_index;
    CondFrame *cond_base;
    enum { G_START, G_IN_GUARD, G_AFTER, G_INVALID } guard_state;
    struct Ident *guard_candidate;
    CondFrame *guard_frame;
    const char *presumed_name;
    int32_t line_delta;
    uint32_t line_adj_from;
} IncludeFrame;

typedef struct PPOptions {
    VEC(const char *) quote_dirs;
    VEC(const char *) angle_dirs;
    VEC(const char *) system_dirs;
    bool nostdinc;
    bool no_predefs;
    bool pedantic;
    bool gnu_extensions;
    bool gnu_mode;
    LexOptions lex;
    const char *date_str, *time_str;
} PPOptions;

typedef struct MacroStackEnt {
    struct MacroStackEnt *next;
    struct Ident *name;
    Macro *macro;
} MacroStackEnt;

typedef enum { TRACK_NONE, TRACK_EXPANSIONS } TrackLevel;

/* Ident.kw values for directive names */
enum {
    KW_NONE, KW_IF, KW_IFDEF, KW_IFNDEF, KW_ELIF, KW_ELSE, KW_ENDIF,
    KW_DEFINE, KW_UNDEF, KW_INCLUDE, KW_INCLUDE_NEXT, KW_LINE, KW_ERROR,
    KW_WARNING, KW_PRAGMA, KW_IDENT, KW_SCCS
};
typedef enum { SRC_LEXER, SRC_CONTEXT, SRC_BARRIER } TokSrc;

typedef struct PP {
    Arena *arena;
    Interner *in;
    SrcMgr *sm;
    DiagEngine *diag;
    PPOptions *opt;
    TrackLevel track;

    Lexer lex;               /* current file */
    Tok pending;             /* one-token pushback for the lexer stream */
    bool has_pending;
    VEC(Context) ctx;
    TokPool pool;
    TokBuf line;             /* current directive line */

    IncludeFrame *inc;
    int include_depth;
    CondFrame *cond;
    bool in_directive;
    bool in_if_expr;
    bool collecting_args;    /* arg pre-expansion: defer _Pragma */
    bool carry_space;

    /* provenance of the last token read (see pp_read_raw) */
    SrcLoc tok_exp_loc;
    uint32_t tok_exp_id, tok_root;
    /* ... and of the last token returned by pp_next */
    SrcLoc out_exp_loc;
    uint32_t out_root;

    VEC(Macro *) macros;
    VEC(Expansion *) expansions;
    VEC(PPListener) listeners;
    VEC(const char *) search;
    size_t first_angle, first_system;

    MacroStackEnt *pushed;
    uint32_t seq;
    uint32_t counter;
    SrcFile *main_file;
    SrcFile *builtin_file;
    VEC(SrcLoc) chain_buf;
    StrBuf sb;               /* scratch string building */

    struct Ident *id_defined, *id_va_args, *id_pragma;
    const char *const *host_attrs;
    const char *const *host_builtins;
} PP;

void pp_init(PP *pp, Arena *a, Interner *in, SrcMgr *sm, DiagEngine *d,
             PPOptions *opt);
void pp_free(PP *pp);
void pp_add_listener(PP *pp, PPListener l);

void pp_define_builtin_text(PP *pp, const char *name, const char *text);
void pp_cmdline_define(PP *pp, const char *def);
void pp_cmdline_undef(PP *pp, const char *name);
void pp_cmdline_include(PP *pp, const char *path);

bool pp_enter_main(PP *pp, const char *path);
/* Next fully macro-expanded token; false at the end of the TU. */
bool pp_next(PP *pp, Tok *out);

static inline const char *pp_text(const PP *pp, const Tok *t)
{
    return tok_text_raw(pp->sm, pp->in, t);
}
static inline Ident *pp_ident(const PP *pp, const Tok *t)
{
    return tok_ident(pp->in, t);
}

/* helpers shared with printers and analyzers */
void pp_add_expansion_notes(PP *pp, Diagnostic *d);
Diagnostic *pp_error_at(PP *pp, const Tok *t, const char *fmt, ...);
Diagnostic *pp_warn_at(PP *pp, const Tok *t, const char *id,
                       const char *fmt, ...);
const char *macro_kind_str(const Macro *m);
char *macro_signature(Arena *a, const Macro *m);
char *macro_body_str(PP *pp, const Macro *m);
char *tokens_str(PP *pp, TokSpan s);
uint32_t pp_presumed_line(PP *pp, SrcLoc loc);
const char *pp_presumed_name(PP *pp, SrcFile *f);

/* ---- internal (pp.c / ppexpand.c / ppexpr.c) ------------------------ */
void tokbuf_init(PP *pp, TokBuf *b, uint32_t mincap);
void tokbuf_grow(PP *pp, TokBuf *b);
static inline void tokbuf_push(PP *pp, TokBuf *b, Tok t)
{
    if (b->len == b->cap)
        tokbuf_grow(pp, b);
    b->t[b->len++] = t;
}
void tokbuf_release(PP *pp, TokBuf *b);
void pp_push_context(PP *pp, Context c);
TokSrc pp_read_raw(PP *pp, Tok *t);
void pp_unread(PP *pp, const Tok *t, TokSrc src);
void pp_directive(PP *pp, const Tok *hash);
void pp_macro_ref(PP *pp, const Tok *name, RefKind kind);
bool pp_try_expand(PP *pp, Tok *name, TokSrc src);
void pp_expand_into(PP *pp, TokSpan in, TokBuf *out);
bool pp_eval_if(PP *pp, TokSpan expr, bool *ok);
void pp_do_pragma(PP *pp, TokSpan toks, SrcLoc loc);
SrcFile *pp_find_include(PP *pp, const char *name, bool angled, bool next,
                         int *dir_index);
Tok pp_make_token(PP *pp, TokKind k, const char *text, size_t n, SrcLoc loc,
                  uint16_t flags);

#define PP_EMIT(pp, fn, ...)                                              \
    do {                                                                  \
        size_t li_;                                                       \
        for (li_ = 0; li_ < (pp)->listeners.len; li_++)                   \
            if ((pp)->listeners.data[li_].fn)                             \
                (pp)->listeners.data[li_].fn((pp)->listeners.data[li_].ctx,\
                                             __VA_ARGS__);                \
    } while (0)

#endif
