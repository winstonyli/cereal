/* diag.h - diagnostics engine. */
#ifndef CEREAL_DIAG_H
#define CEREAL_DIAG_H

#include "common.h"
#include "srcmgr.h"

typedef enum {
    DL_IGNORED,
    DL_NOTE,
    DL_REMARK,
    DL_WARNING,
    DL_ERROR,
    DL_FATAL
} DiagLevel;

/* How a diagnostic merges with the parser's diagnostics of the same unit
 * (diag_merge_from).  gcc checks as it parses, so by default the two
 * lists merge by location. */
typedef enum DiagOrd {
    ORD_NORMAL,
    ORD_LATE,   /* checker: after every parser diagnostic (K&R checks, scope
                   close, finish_struct after a missing ';') */
    ORD_CUT,    /* checker: after the first parser diagnostic at or past its
                   location (a call cut short by a syntax error) */
    ORD_EARLY,  /* checker: before a parser diagnostic at its location */
    ORD_TIE,    /* parser: checker diagnostics at its location come first */
    ORD_EOF     /* parser: after every checker diagnostic (unclosed body) */
} DiagOrd;

/* docs/C_DESIGN.md section 3.  A fix-it is a real source range [begin, end)
 * of one line of one user file (begin == end: an insertion); diag_fixit is
 * the only way to add one. */
typedef enum { FIX_SAFE, FIX_MAYBE, FIX_INFO } FixApplic;

typedef struct FixIt {
    SrcLoc begin, end;
    const char *text;        /* arena; "" deletes */
    uint16_t owner;          /* 0: the diagnostic itself; k: notes.data[k-1] */
    uint8_t app;             /* FixApplic */
} FixIt;

typedef struct DiagNote {
    SrcLoc loc;
    const char *msg;
    bool nocol;              /* a line-only location, as the warning's */
    bool nofix;              /* a fix-it for this note was refused: no more */
    bool expn;               /* an "in expansion of macro" note */
    const char *hdr;         /* a note suggesting the #include of this header */
} DiagNote;

typedef struct Diagnostic {
    DiagLevel level;
    const char *id;          /* warning option name, "" for hard errors */
    SrcLoc loc;
    SrcLoc oloc;             /* where it merges with the parser's: loc, or
                                the invocation of the macro whose
                                replacement list holds loc */
    SrcRange range;          /* optional highlight (end == 0 if none) */
    const char *msg;
    VEC(DiagNote) notes;
    SrcLoc *inc_chain;       /* #include locations, innermost first */
    int ninc;
    uint8_t ord;             /* DiagOrd: where it merges with the parser's */
    bool nocol;              /* printed as file:line: with no snippet */
    uint32_t vcol;           /* nonzero: the header column, byte-based */
    bool promoted;           /* a warning made an error by -Werror[=X]:
                                shown as [-Werror=X] like gcc */
    VEC(FixIt) fixits;       /* in the order added, notes' included */
    bool nofix;              /* a fix-it of the diagnostic itself was refused */
    uint32_t key;           /* plan item at the time (parallel ordering) */
    uint8_t once;            /* nonzero: report at most once per (once, loc);
                                a parallel merge drops later duplicates */
} Diagnostic;

typedef enum { DIAG_FMT_TEXT, DIAG_FMT_JSON } DiagFormat;

/* -W configuration: built once from the command line, then read-only. */
typedef struct DiagConfig DiagConfig;
DiagConfig *diag_config_new(void);
void diag_config_free(DiagConfig *c);
DiagConfig *diag_config_clone(const DiagConfig *c);   /* NULL: the defaults */
/* "foo", "no-foo", "error", "group", "all", "everything"; false if unknown */
bool diag_config_apply(DiagConfig *c, const char *flag);
bool diag_config_werror(const DiagConfig *c);
bool diag_config_pedantic(const DiagConfig *c);

typedef struct DiagEngine {
    Arena *arena;
    SrcMgr *sm;
    VEC(Diagnostic *) all;
    int nerrors, nwarnings;
    bool werror;
    bool pedantic;
    bool pedantic_errors;
    bool no_warnings;        /* -w */
    bool fatal_errors;       /* -Wfatal-errors: stop after the first error */
    uint32_t vcol_next;      /* the next report's column is this (a spliced line's cleaned offset), 0 = its own */
    bool nocol_next;         /* the next report prints no column (gcc: a line-only location) */
    VEC(const char *) hdr_seen; /* headers a missing-#include note suggested */
    bool fixit_lines;        /* -fdiagnostics-parseable-fixits */
    bool gen_patch;          /* -fdiagnostics-generate-patch */
    bool track0;             /* -ftrack-macro-expansion=0: report macro
                                tokens at the expansion point */
    bool show_system;        /* report warnings located in system headers */
    bool immediate;          /* print as reported (text format) */
    bool color;
    DiagFormat format;
    FILE *out;
    /* Supplies the current include chain (innermost first). */
    void (*include_chain)(void *ctx, SrcLoc **locs, int *n);
    void *include_chain_ctx;
    int max_errors;
    const DiagConfig *cfg;   /* NULL: defaults */
    uint32_t key;            /* current plan item (set by the preprocessor) */
    /* option lookup cache keyed by the id string's address (ids are
     * literals); a hit is verified by name */
    const char *idc_key[512];
    int16_t idc_val[512];
    /* option -> level+1 under the config generation memo_gen (0: unknown) */
    uint32_t memo_gen;
    uint8_t memo[512];
    uint8_t ord;             /* DiagOrd of new diagnostics */
} DiagEngine;

/* cexpr.c: gcc's known-headers table */
const char *std_header(const char *name);
/* PRId64, SCNu8, ...: the format macros gcc offers after a string literal */
bool inttypes_fmt_macro(const char *name);
/* A note suggesting the #include of hdr ("<stdio.h>").  gcc puts the first
 * note per header where the #include would go, with the fix-it that adds it
 * (owned by the note), and later ones at loc. */
SrcLoc diag_header_note(DiagEngine *d, Diagnostic *dg, SrcLoc loc,
                        const char *hdr, const char *fmt, ...);
/* -fdiagnostics-generate-patch: the TU's fix-its as one patch on d->out */
void diag_generate_patch(DiagEngine *d);

/* The one way to add a fix-it (owner 0: dg itself; k: notes.data[k-1]).
 * False, with the owner's fix-its dropped and no more accepted, unless
 * [b, e) lies on one line of one user file and text has a newline only as
 * its last character, in an insertion at column 1 (libcpp's rich_location).
 * A fix-it that starts where the owner's last ended is merged into it. */
bool diag_fixit(DiagEngine *d, Diagnostic *dg, int owner, SrcLoc b, SrcLoc e,
                const char *text, FixApplic app);
/* One edit of a fix-it group, in the offsets of its file. */
typedef struct DiagEdit {
    SrcFile *file;
    uint32_t begin, end;
    const char *text;
    uint16_t owner;          /* as FixIt.owner */
    uint8_t app;             /* FixApplic */
} DiagEdit;
typedef VEC(DiagEdit) DiagEditVec;

/* The fix-its of d as one atomic group, appended to out: what `cereal fix`
 * and the language server offer for d.  False, with out unchanged, if d
 * offers none: a fix-it was refused (nofix), one lies outside the user
 * files, or the edits change nothing (gcc offers "%u" for a "%u").  A note's
 * fix-it that overlaps one of d's own is left out, being another way to the
 * same end (xor: "0x2" for "1 <<"). */
bool diag_fix_group(SrcMgr *sm, const Diagnostic *d, DiagEditVec *out);
/* Writes text as gcc escapes it in a fix-it line: \\ \" \t \n, the other
 * bytes below 0x20 and from 0x7f as \ooo. */
void fixit_escape(FILE *o, const char *s, size_t n);

/* Set the order of new diagnostics, returning the previous one. */
static inline DiagOrd diag_ord(DiagEngine *d, DiagOrd o)
{
    DiagOrd old = (DiagOrd)d->ord;
    d->ord = (uint8_t)o;
    return old;
}

/* Mark the diagnostic just reported. */
static inline void diag_mark_last(DiagEngine *d, DiagOrd o)
{
    if (d->all.len)
        d->all.data[d->all.len - 1]->ord = (uint8_t)o;
}

/* Warning option registry.  An option is enabled by an explicit -W flag,
 * else by an umbrella flag in `by` (gcc's EnabledBy), else by default.
 * A name ending in '=' is a gcc level option (-Wshift-overflow=): it is
 * spelled -Wname or -Wname=N (0: off) and printed as [-Wname=]. */
#define DO_ALL          0x01    /* -Wall */
#define DO_EXTRA        0x02    /* -Wextra */
#define DO_PEDANTIC     0x04    /* -pedantic, -Wpedantic */
#define DO_UNUSED       0x08    /* -Wunused (itself enabled by -Wall) */
#define DO_IMPLICIT     0x10    /* -Wimplicit (itself enabled by -Wall) */
#define DO_UNUSED_EXTRA 0x20    /* -Wunused and -Wextra together */

typedef struct DiagOption {
    const char *name;
    const char *group;       /* hygiene | cond | include | pp | pedantic | c */
    DiagLevel level;         /* severity when enabled */
    bool on;                 /* enabled by default */
    uint8_t by;              /* DO_* umbrella flags that enable it */
    const char *help;
} DiagOption;

void diag_init(DiagEngine *d, Arena *a, SrcMgr *sm);
/* A worker engine with the unit engine's options. */
void diag_init_worker(DiagEngine *d, Arena *a, SrcMgr *sm, const DiagEngine *tu);
void diag_free(DiagEngine *d);

DiagLevel diag_level_for(DiagEngine *d, const char *id, DiagLevel requested);
bool diag_enabled(DiagEngine *d, const char *id);
/* Which options d reports (a byte per registry entry, malloc'd), and the
 * question for one id against such a map (hard errors, ids outside the
 * registry: yes).  For comparing translation units with different flags. */
uint8_t *diag_enabled_map(DiagEngine *d);
bool diag_map_enabled(const uint8_t *map, const char *id);
/* -Wno-error=id (or #pragma GCC diagnostic warning): -Werror leaves it a warning */
bool diag_noerror(DiagEngine *d, const char *id);
/* 1: enabled; 0: disabled by a flag (-Wno-X, or -Wno-all for an option
 * -Wall enables); -1: disabled by default.  gcc emits some pedwarns
 * untagged when the option is merely at its default (warn_return_type
 * == -1) and not at all when it is turned off. */
int diag_option_state(DiagEngine *d, const char *id);
/* true when -Wid itself was given on the command line */
bool diag_option_explicit(DiagEngine *d, const char *id);
bool diag_option_requested(DiagEngine *d, const char *id);
/* N of -Wid=N as given on the command line, else dflt */
int diag_option_level(DiagEngine *d, const char *id, int dflt);
uint64_t diag_option_size(DiagEngine *d, const char *id);

Diagnostic *diag_report(DiagEngine *d, DiagLevel lvl, const char *id,
                        SrcLoc loc, const char *fmt, ...);
bool diag_hidden_in_system_header(DiagEngine *d, SrcLoc loc);
Diagnostic *diag_vreport(DiagEngine *d, DiagLevel lvl, const char *id,
                         SrcLoc loc, const char *fmt, va_list ap);
void diag_note(DiagEngine *d, Diagnostic *dg, SrcLoc loc, const char *fmt, ...);
void diag_note_nocol(DiagEngine *d, Diagnostic *dg, SrcLoc loc, const char *fmt, ...);
void diag_set_range(Diagnostic *dg, SrcLoc b, SrcLoc e);
/* Print a diagnostic now (used when immediate is false, e.g. after sort). */
void diag_print(DiagEngine *d, Diagnostic *dg);
void diag_flush(DiagEngine *d);   /* print everything not yet printed */
void diag_print_json(DiagEngine *d, FILE *out);
void diag_list_options(FILE *out);
void diag_merge_from(DiagEngine *d, size_t from, size_t mid);

const DiagOption *diag_find_option(const char *name);
/* Display width of a code point as gcc counts it (libcpp wcwidth). */
int wc_width(uint32_t cp);

/* One "in expansion of macro" note: the macro and where it was invoked. */
typedef struct MacroNote {
    const char *name;
    uint32_t len;
    SrcLoc loc;
} MacroNote;

/* Notes the macros a token spelled at loc (invoked at exp) was expanded
 * through, innermost first.  body: a replacement-list token, for which chain
 * (if any) gives the macros; else, or if it gives none, the name written at
 * exp. */
typedef size_t (*MacroChainFn)(void *ctx, SrcLoc spelled, SrcLoc exp,
                               MacroNote *out, size_t max);
void diag_expansion_notes(DiagEngine *d, Diagnostic *dg, const SrcMgr *sm,
                          SrcLoc loc, SrcLoc exp, bool body,
                          MacroChainFn chain, void *ctx);

#endif
