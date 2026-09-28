/* pp.h - translation phase 4: the preprocessor (C99 6.10). */
#ifndef CEREAL_PP_H
#define CEREAL_PP_H

#include "common.h"
#include "diag.h"
#include "lex.h"
#include "srcmgr.h"
#include "token.h"

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
    bool gnu_named_variadic; /* `args...` */
    bool predefined;         /* from <built-in> or <command line> */
    BuiltinKind builtin;
    int nparams;
    struct Ident **params;
    SrcLoc *param_locs;
    Token *body;             /* replacement list (TK_EOF terminated) */
    int body_len;
    SrcLoc hash_loc;         /* '#' of the #define */
    SrcLoc name_loc;
    SrcLoc end_loc;          /* end of the directive line */
    SrcLoc undef_loc;        /* 0 while live */
    uint32_t def_seq;        /* event sequence numbers (see Index) */
    uint32_t undef_seq;
    struct Macro *prev;      /* previous definition of the same name */
    struct SrcFile *file;
    /* usage statistics, maintained by the preprocessor */
    uint32_t expansions;
    uint32_t cond_refs;
    void *user;              /* analyzer scratch */
} Macro;

/* One macro invocation. */
typedef struct Expansion {
    uint32_t id;
    Macro *macro;
    SrcLoc name_loc;         /* spelling loc of the name token */
    Prov *name_prov;         /* provenance of the name token (parent) */
    SrcLoc end_loc;          /* spelling loc of ')' for function-like */
    Prov *end_prov;
    bool in_directive;       /* inside #if / #include */
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
    SrcLoc hash_loc;         /* '#' */
    SrcLoc kw_loc;           /* directive name */
    SrcLoc end_loc;
    Token *expr;             /* raw (unexpanded) tokens of the condition */
    bool evaluated;          /* false if in a skipped region / after taken */
    bool value;              /* result when evaluated */
    bool taken;              /* this group is the active one */
} CondEvent;

typedef enum {
    REF_EXPANSION,
    REF_IFDEF,               /* #ifdef / #ifndef */
    REF_DEFINED,             /* defined X / defined(X) in #if/#elif */
    REF_UNDEF,
    REF_PRAGMA               /* push_macro/pop_macro/poison */
} RefKind;

typedef enum {
    INC_OK,
    INC_SKIPPED_GUARD,       /* multiple-include optimization */
    INC_SKIPPED_ONCE,        /* #pragma once */
    INC_NOT_FOUND
} IncludeResult;

typedef struct IncludeEvent {
    SrcLoc hash_loc;
    SrcLoc name_loc;         /* first token of the header name */
    SrcLoc name_end;
    const char *spelled;     /* as written, without delimiters */
    bool angled;
    bool next;               /* #include_next */
    bool macro_expanded;     /* computed include */
    SrcFile *file;           /* resolved file (NULL if not found) */
    SrcFile *from;           /* including file */
    IncludeResult result;
} IncludeEvent;

/* Everything downstream observes the preprocessor through listeners. */
typedef struct PPListener {
    void *ctx;
    void (*file_enter)(void *ctx, SrcFile *f, const IncludeEvent *via);
    void (*file_exit)(void *ctx, SrcFile *f);
    void (*include)(void *ctx, const IncludeEvent *ev);
    void (*define)(void *ctx, Macro *m, Macro *replaced);
    void (*undef)(void *ctx, struct Ident *id, Macro *m, SrcLoc hash_loc,
                  SrcLoc name_loc);
    /* args: pre-expansion argument token lists (EOF-terminated); nargs may
     * exceed nparams by one for variadic macros */
    void (*expand)(void *ctx, Expansion *e, Token **args, int nargs);
    void (*macro_ref)(void *ctx, struct Ident *id, Macro *m, const Token *tok,
                      RefKind kind);
    void (*cond)(void *ctx, const CondEvent *ev);
    void (*skipped)(void *ctx, SrcLoc begin, SrcLoc end);
    void (*pragma)(void *ctx, SrcLoc loc, Token *toks);
    void (*paste)(void *ctx, Expansion *e, const Token *lhs, const Token *rhs,
                  const Token *result, bool valid);
    /* checkpoint: a directive that may change macro state ended at `loc` */
    void (*checkpoint)(void *ctx, SrcLoc loc, uint32_t seq);
} PPListener;

typedef struct CondFrame {
    struct CondFrame *prev;
    CondKind kind;           /* last directive seen: IF*, ELIF, ELSE */
    SrcLoc if_loc;
    bool taken_any;          /* some group already taken */
    bool active;             /* current group is being processed */
    bool parent_active;
    bool seen_else;
    int include_depth;
} CondFrame;

typedef struct IncludeFrame {
    struct IncludeFrame *prev;
    SrcFile *file;
    Token *rest;             /* tokens of this file not yet consumed */
    SrcLoc include_loc;      /* '#include' in the parent (0 for main) */
    int dir_index;           /* search-path index the file was found at */
    CondFrame *cond_base;
    /* multiple-include guard detection */
    enum { G_START, G_IN_GUARD, G_AFTER, G_INVALID } guard_state;
    struct Ident *guard_candidate;
    CondFrame *guard_frame;
    const char *presumed_name;
    int32_t line_delta;      /* #line adjustment */
    uint32_t line_adj_from;  /* line from which the delta applies */
} IncludeFrame;

typedef struct PPOptions {
    VEC(const char *) quote_dirs;   /* -iquote */
    VEC(const char *) angle_dirs;   /* -I */
    VEC(const char *) system_dirs;  /* -isystem + host */
    bool nostdinc;
    bool no_predefs;                /* -undef */
    bool pedantic;
    bool gnu_extensions;            /* default true (host headers need them) */
    bool gnu_mode;                  /* -std=gnu99: GNU semantics where they differ */
    bool track_bodies;              /* keep provenance for LSP */
    LexOptions lex;
    const char *date_str, *time_str;
} PPOptions;

typedef struct MacroStackEnt {
    struct MacroStackEnt *next;
    struct Ident *name;
    Macro *macro;
} MacroStackEnt;

typedef struct PP {
    Arena *arena;
    Interner *in;
    SrcMgr *sm;
    DiagEngine *diag;
    PPOptions *opt;

    IncludeFrame *inc;
    int include_depth;
    CondFrame *cond;
    Token *cur;                     /* current token stream */
    bool in_directive;
    bool in_if_expr;
    bool collecting_args;

    VEC(Macro *) macros;            /* every definition ever made */
    VEC(Expansion *) expansions;
    VEC(PPListener) listeners;
    VEC(const char *) search;       /* all include dirs, in order */
    size_t first_angle, first_system;

    MacroStackEnt *pushed;          /* #pragma push_macro */
    uint32_t seq;                   /* define/undef event counter */
    uint32_t counter;               /* __COUNTER__ */
    uint32_t next_exp_id;
    SrcFile *main_file;
    SrcFile *builtin_file;
    Token *pending_pragmas;         /* _Pragma results for -E output */
    VEC(SrcLoc) chain_buf;

    struct Ident *id_defined, *id_va_args, *id_has_include,
        *id_has_include_next, *id_pragma, *id_once;
    const char *const *host_attrs;
    const char *const *host_builtins;
} PP;

void pp_init(PP *pp, Arena *a, Interner *in, SrcMgr *sm, DiagEngine *d,
             PPOptions *opt);
void pp_free(PP *pp);
void pp_add_listener(PP *pp, PPListener l);

/* Queue predefined macros (before pp_enter_main). */
void pp_define_builtin_text(PP *pp, const char *name, const char *text);
/* -D / -U handling: "NAME", "NAME=VALUE". */
void pp_cmdline_define(PP *pp, const char *def);
void pp_cmdline_undef(PP *pp, const char *name);
void pp_cmdline_include(PP *pp, const char *path);

bool pp_enter_main(PP *pp, const char *path);
/* Next fully macro-expanded token; TK_EOF at end of translation unit. */
Token *pp_next(PP *pp);

/* Helpers shared by pp*.c, the printer and analyzers. */
SrcLoc pp_expansion_loc(const Token *t);  /* outermost call site */
SrcLoc prov_expansion_loc(const Prov *p, SrcLoc fallback);
void pp_add_expansion_notes(PP *pp, Diagnostic *d, const Token *t);
Diagnostic *pp_error_at(PP *pp, const Token *t, const char *fmt, ...);
Diagnostic *pp_warn_at(PP *pp, const Token *t, const char *id,
                       const char *fmt, ...);
const char *macro_kind_str(const Macro *m);
char *macro_signature(Arena *a, const Macro *m);   /* NAME(a, b) */
char *macro_body_str(Arena *a, const Macro *m);
char *tokens_str(Arena *a, const Token *t, const Token *end);
bool macro_is_guard_like(const Macro *m);

/* internal (ppexpand.c / ppexpr.c) */
void pp_directive(PP *pp, Token *hash);
void pp_macro_ref(PP *pp, Token *name, RefKind kind);
bool pp_try_expand(PP *pp, Token *tok);
Token *pp_copy_token(PP *pp, const Token *t);
Token *pp_new_eof(PP *pp, SrcLoc loc);
Token *pp_next_raw(PP *pp);
bool pp_eval_if(PP *pp, Token *expr, SrcLoc loc, bool *ok);
Token *pp_read_line(PP *pp);         /* rest of directive line (EOF-term.) */
Token *pp_expand_list(PP *pp, Token *list); /* fully expand a line */
void pp_do_pragma(PP *pp, Token *toks, SrcLoc loc);
Token *pp_destringize_pragma(PP *pp, Token *str);
Token *pp_builtin_expand(PP *pp, Macro *m, Token *tok, Expansion *e);
uint32_t pp_presumed_line(PP *pp, SrcLoc loc);
char *pp_search_include(PP *pp, const char *name, bool angled, bool next,
                        int *dir_index);
Hideset *hs_add(PP *pp, Hideset *hs, Macro *m);
bool hs_contains(Hideset *hs, Macro *m);
Hideset *hs_union(PP *pp, Hideset *a, Hideset *b);
Hideset *hs_intersect(PP *pp, Hideset *a, Hideset *b);

#define PP_EMIT(pp, fn, ...)                                              \
    do {                                                                  \
        size_t li_;                                                       \
        for (li_ = 0; li_ < (pp)->listeners.len; li_++)                   \
            if ((pp)->listeners.data[li_].fn)                             \
                (pp)->listeners.data[li_].fn((pp)->listeners.data[li_].ctx,\
                                             __VA_ARGS__);                \
    } while (0)

#endif
