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
#include "plan.h"
#include "macrotab.h"

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
    /* Versions: the macro is live at version v iff def_seq < v <= undef_seq
     * (undef_seq is UINT32_MAX while live).  A version is the number of
     * define/undef events before a point in the TU. */
    uint32_t def_seq, undef_seq;
    struct Macro *prev;      /* previous definition of the same name */
    struct Macro *alias_of;  /* pop_macro re-instatement of this definition */
    struct SrcFile *file;
    uint32_t expansions;     /* atomic */
    uint32_t cond_refs;      /* atomic */
    void *user;
} Macro;

static inline bool macro_live_at(const Macro *m, uint32_t v)
{
    return m->def_seq < v && v <= m->undef_seq;
}

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
    uint32_t seq;            /* macro version when the name was looked up */
    uint32_t seq_item;       /* phase B: the plan item that set that version */
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
    bool self_loc;           /* argument pre-expansion: a token spelled in
                                the source is its own expansion point (GCC:
                                __LINE__ in a multi-line argument list) */
    bool root_obj;           /* the outermost macro is object-like: then
                                __LINE__ is its call site's line (GCC) */
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
    const LineAdj *adj;      /* #line history (NULL: none) */
    bool system;             /* system header (as of this point) */
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
    bool fatal_missing_include; /* -E: stop the TU there, as GCC does */
    LexOptions lex;
    const char *date_str, *time_str;  /* set by pp_options_finish */
    char date_buf[32], time_buf[16];
} PPOptions;

/* GCC assertions: #assert pred(answer).  Directive state only (phase A). */
typedef struct Assertion {
    struct Assertion *next;
    struct Ident *pred;
    const char *answer;      /* tokens joined by single spaces */
} Assertion;

typedef struct MacroStackEnt {
    struct MacroStackEnt *next;
    struct Ident *name;
    Macro *macro;
} MacroStackEnt;

typedef enum { TRACK_NONE, TRACK_EXPANSIONS } TrackLevel;

typedef enum {
    PPM_FULL,       /* the reference engine: directives and text together */
    PPM_PHASE_A,    /* directives only; text recorded as plan segments */
    PPM_PLAN        /* phase B: text from a plan, directives as markers */
} PPMode;

/* Called in PPM_PLAN whenever the top-level reader reaches the start of a
 * segment item; returns false to stop the preprocessor there. */
typedef bool (*BoundaryFn)(void *ctx, size_t item, bool clean);

/* Ident.kw values for directive names */
enum {
    KW_NONE, KW_IF, KW_IFDEF, KW_IFNDEF, KW_ELIF, KW_ELSE, KW_ENDIF,
    KW_DEFINE, KW_UNDEF, KW_INCLUDE, KW_INCLUDE_NEXT, KW_LINE, KW_ERROR,
    KW_WARNING, KW_PRAGMA, KW_IDENT, KW_SCCS, KW_ASSERT, KW_UNASSERT
};
typedef enum { SRC_LEXER, SRC_CONTEXT, SRC_BARRIER } TokSrc;

typedef struct PP {
    Arena *arena;
    Interner *in;
    MacroTab *mt;            /* this build's macro state (shared by workers) */
    bool mt_owned;
    SrcMgr *sm;
    DiagEngine *diag;
    PPOptions *opt;
    TrackLevel track;

    Lexer lex;               /* current file */
    Tok pending;             /* one-token pushback for the lexer stream */
    bool has_pending;
    bool pending_unread;     /* pending was returned once (pp_unread) */
    bool dir_poison;         /* read_line: report poisoned identifiers */
    VEC(Context) ctx;
    TokPool pool;
    ScratchCursor scratch;   /* this thread's scratch chunk */
    TokBuf line;             /* current directive line */

    IncludeFrame *inc;
    int include_depth;
    CondFrame *cond;
    bool in_directive;
    bool in_if_expr;
    bool collecting_args;    /* arg pre-expansion: defer _Pragma */
    bool carry_space;
    PPMode mode;
    Plan *plan;              /* PHASE_A: being built; PLAN: being read */
    PlanFrame *pframe;       /* PHASE_A: current frame snapshot */
    size_t dir_item;         /* PHASE_A: DIR item of the running directive */
    size_t plan_pos;         /* PLAN: next item */
    bool seg_active;         /* PLAN: lexer is inside plan_pos - 1 */
    bool reading_top;        /* PLAN: pp_next's own read (for clean points) */
    bool diverged;           /* PLAN: hit something only FULL mode can do */
    bool halted;             /* a fatal error (or cancellation) ended the TU */
    const uint32_t *cancel;  /* atomic flag: nonzero stops at the next token */
    BoundaryFn on_boundary;
    void *boundary_ctx;
    size_t plan_stop;        /* PLAN: end before this item (0: none) ... */
    bool plan_stop_clean;    /* ... where nothing was in flight */
    bool versioned;          /* phase B: look macros up by version */
    uint32_t version;
    uint32_t version_item;   /* phase B: the plan item that set version */
    struct CellReads *reads; /* phase B: recording the read set (cell.h) */
    bool check_versions;     /* debug: cross-check versioned lookups */

    /* provenance of the last token read (see pp_read_raw) */
    SrcLoc tok_exp_loc;
    uint32_t tok_exp_id, tok_root;
    bool tok_root_obj;
    bool subst_root_obj;     /* root_obj of the expansion being substituted */
    /* ... and of the last token returned by pp_next */
    SrcLoc out_exp_loc;
    uint32_t out_root;

    VEC(Macro *) macros;
    VEC(Expansion *) expansions;
    VEC(PPListener) listeners;
    VEC(const char *) search;
    size_t first_angle, first_system;

    MacroStackEnt *pushed;
    Assertion *asserts;
    uint32_t seq;
    uint32_t counter;
    SrcFile *main_file;
    SrcFile *builtin_file;
    VEC(SrcLoc) chain_buf;
    StrBuf sb;               /* scratch string building */
    StrBuf predef;           /* <command line>: predefines, -D, -U, -include */

    struct Ident *id_defined, *id_va_args, *id_pragma;
    const char *const *host_attrs;
    const char *const *host_builtins;
} PP;

/* Once per process, before any TU: fixes __DATE__ / __TIME__ (honouring
 * SOURCE_DATE_EPOCH) so concurrent TUs agree and nothing is shared. */
void pp_options_finish(PPOptions *opt);
void pp_init(PP *pp, Arena *a, Interner *in, SrcMgr *sm, DiagEngine *d,
             PPOptions *opt);
void pp_free(PP *pp);
void pp_add_listener(PP *pp, PPListener l);

void pp_define_builtin_text(PP *pp, const char *name, const char *text);
void pp_cmdline_define(PP *pp, const char *def);
void pp_cmdline_undef(PP *pp, const char *name);
void pp_cmdline_include(PP *pp, const char *path);

bool pp_enter_main(PP *pp, const char *path);
/* Phase A: run the whole TU in directives-only mode, filling `plan`. */
bool pp_run_phase_a(PP *pp, Plan *plan);
/* A phase-B worker sharing main's interner, sources and options (macros
 * are read through versioned lookups; no builtins are created). */
void pp_init_worker(PP *w, const PP *main, Arena *a, DiagEngine *d);
/* Phase B: position a worker at a plan item. */
void pp_plan_start(PP *pp, Plan *plan, size_t item);
/* Next fully macro-expanded token; false at the end of the TU. */
bool pp_next(PP *pp, Tok *out);

/* The definition of id in effect at the current point. */
static inline Macro *macro_at_version(const MacroTab *mt, const Ident *id,
                                      uint32_t v)
{
    Macro *m;
    for (m = mt_hist(mt, id); m; m = m->prev)
        if (m->def_seq < v)
            return v <= m->undef_seq ? m : NULL;
    return NULL;
}

void pp_version_mismatch(const PP *pp, const Ident *id);
/* Make m (m->name) the newest definition and the one in effect. */
void pp_install_macro(PP *pp, Macro *m);

/* Parallel runs: the plan item an event belongs to (see ParClient). */
static inline uint32_t pp_event_key(const PP *pp)
{
    return pp->diag->key;
}

struct CellReads;
void cell_reads_note(struct CellReads *r, uint32_t ident, uint32_t item);
void cell_reads_line(struct CellReads *r, uint32_t item);

static inline Macro *pp_macro(const PP *pp, const Ident *id)
{
    if (pp->versioned) {
        if (pp->reads)
            cell_reads_note(pp->reads, id->id, pp->version_item);
        return macro_at_version(pp->mt, id, pp->version);
    }
    if (pp->check_versions &&
        macro_at_version(pp->mt, id, pp->seq) != mt_cur(pp->mt, id))
        pp_version_mismatch(pp, id);
    return mt_cur(pp->mt, id);
}

/* Is id poisoned at the current point? */
static inline bool pp_poisoned(const PP *pp, const Ident *id)
{
    uint32_t ps = mt_slot(pp->mt, id->id)->poison_seq;
    return ps && (pp->versioned ? pp->version : pp->seq) >= ps;
}

/* Is m being expanded (its context live) in this preprocessor? */
/* Disabling is by name, as in GCC: a macro redefined inside its own
 * argument list stays disabled while its (old) body is rescanned. */
static inline bool pp_macro_disabled(const PP *pp, const Macro *m)
{
    size_t i;
    for (i = pp->ctx.len; i-- > 0;)
        if (pp->ctx.data[i].macro && pp->ctx.data[i].macro->name == m->name)
            return true;
    return false;
}

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
void pp_plan_apply_dir(PP *pp, uint32_t item);
bool pp_cross_file_end(PP *pp);
void pp_macro_ref(PP *pp, const Tok *name, RefKind kind);
bool pp_try_expand(PP *pp, Tok *name, TokSrc src);
void pp_expand_into(PP *pp, TokSpan in, TokBuf *out);
bool pp_eval_if(PP *pp, TokSpan expr, bool *ok);
void pp_do_pragma(PP *pp, TokSpan toks, SrcLoc loc);
void pp_assert_str(PP *pp, const char *pred, const char *answer);
/* Parse `pred [(answer)]` at s.t[*i]; need_answer for #assert.  On success
 * *answer is NULL when there was none.  Reports errors; *i is advanced past
 * what was consumed either way. */
bool pp_parse_assertion(PP *pp, TokSpan s, uint32_t *i, bool need_answer,
                        SrcLoc at, struct Ident **pred, const char **answer);
bool pp_assertion_holds(PP *pp, struct Ident *pred, const char *answer);
void pp_emit_line(PP *pp, const char *text, size_t len, SrcLoc loc);
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
