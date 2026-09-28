/* ppout.h - `-E` output.
 *
 * Printing a token is split into a *transition* (newlines, linemarker,
 * separating space: a pure function of PrintState and the token) and the
 * token's text.  After any positioned token the state depends only on that
 * token, which is what lets parallel output be stitched byte-identically:
 * the merge re-renders one transition per stitch point. */
#ifndef CEREAL_PPOUT_H
#define CEREAL_PPOUT_H

#include "pp.h"

typedef struct PrintState {
    SrcFile *file;           /* file of the output cursor */
    uint32_t line;           /* presumed line of the output cursor */
    const char *name;        /* presumed file name ... */
    int32_t delta;           /* ... and #line offset of the last marker */
    bool at_bol;
    bool have_prev;
    Tok prev;
    int pending_flag;        /* 1 entered / 2 returned since the last token */
} PrintState;

/* Everything the transition needs about a token, resolved at print time. */
typedef struct PrintTok {
    Tok t;
    SrcFile *f;              /* NULL: no position; state keeps its place */
    uint32_t line;           /* presumed line */
    const char *name;        /* presumed file name (for linemarkers) */
    int32_t delta;           /* #line offset in effect */
    bool system;             /* system header at that point */
} PrintTok;

typedef struct OutSink {
    char *buf;
    size_t len, cap;
    FILE *fp;                /* NULL: keep everything in memory */
} OutSink;

void sink_put(OutSink *o, const char *s, size_t n);
void sink_flush(OutSink *o);
void sink_free(OutSink *o);

/* Per-boundary record kept by parallel workers (see par.c).  Other
 * workers read a record once it is published; has_first is published with
 * a release store after first and first_body are written. */
typedef struct BoundRec {
    size_t item;             /* plan item (a segment) */
    bool clean;              /* no expansion in flight at the boundary */
    uint32_t has_first;      /* a token was printed after the boundary */
    PrintTok first;          /* ... this one */
    size_t first_body;       /* output offset after first's transition */
    bool events_pre;         /* file enter/exit events before first */
    int pre_last;            /* ... the last of them */
} BoundRec;

typedef struct Printer {
    PP *pp;
    bool linemarkers;
    PrintState st;
    LineCursor lc;
    SrcFile *last_f;
    OutSink *out;
    BoundRec *open;          /* parallel: record of the current span */
} Printer;

void printer_init(Printer *p, PP *pp, OutSink *out, bool linemarkers);
void printer_token(Printer *p, const Tok *t);
void printer_finish(Printer *p);
void print_transition(const SrcMgr *sm, const Interner *in, bool linemarkers,
                      PrintState *st, const PrintTok *pt, OutSink *o);

void pp_write_output(PP *pp, FILE *out, bool linemarkers);

#endif
