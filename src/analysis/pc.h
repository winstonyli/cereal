/* pc.h - presence conditions of every conditional group of a unit.
 *
 * A symbolic fold of the main file and everything it includes: one BDD per
 * #if/#elif/#else group over the initial state of the macros (defined(X) and
 * thresholds of X's value; see pcexpr.h), with define/undef tracked through
 * the fold, #pragma once as per-file state, and system headers following the
 * real run (pc_trace_attach).  Computed or unresolved includes are skipped and recorded: groups
 * after one are exact outside the condition under which the skipped include
 * is reached (their taint), where the file might define anything. */
#ifndef CEREAL_PC_H
#define CEREAL_PC_H

#include "pcexpr.h"
#include "pcq.h"

typedef struct PcGroup {
    SrcFile *file;
    uint32_t dir;           /* skeleton index of the #if/#elif/#else */
    Bdd pc;                 /* the group is compiled */
    Bdd taint;              /* where an earlier skipped include may have changed things */
    uint64_t inst;          /* the group and the chain of #includes to it */
} PcGroup;

typedef struct PcMacro PcMacro;
typedef struct PcOnce PcOnce;

struct PcUnit {
    Analysis an_own;        /* a copy: the unit outlives the caller's */
    Analysis *an;           /* &an_own */
    PcCtx cx;
    VEC(uint64_t) taken;    /* sorted: the groups the real run took */
    VEC(PcGroup) groups;
    bool over;              /* the BDD outgrew its cap: the fold is partial */
    bool conc;              /* inside a system header */
    Bdd taint;              /* the condition under which a skipped include ran */
    uint64_t chain;         /* hash of the #include directives entered */
    size_t files, sys, unres, computed;
    VEC(const char *) skipped;      /* "<h>", "\"h\"", "computed" */
    /* internals */
    VEC(PcMacro *) macros;
    uint32_t *htab;
    size_t hcap;
    VEC(PcOnce *) once;
};

/* Fold the unit rooted at main, with the trace of a (taken from it).  False
 * when it went over the cap. */
bool pc_fold(PcUnit *u, Analysis *a, SrcFile *main);
void pc_unit_free(PcUnit *u);
void pc_unit_destroy(PcUnit *u);    /* a malloc'd one */
void pc_trace_free(Analysis *a);
/* The truth of an atom for the macros as they are when the main file
 * starts. */
bool pc_host_atom(PcUnit *u, const PcAtom *at);
/* Record which groups the real preprocessor takes: the fold uses it inside
 * system headers and the oracle compares with it.  Single-threaded runs
 * only: call after analysis_attach, before the TU runs. */
void pc_trace_attach(Analysis *a);

/* One conditional group of a file, as the editor asks about it. */
typedef struct PcExplain {
    SrcFile *file;
    uint32_t dir;           /* skeleton index of its #if/#elif/#else */
    SkKind kind;
    uint32_t line;
    bool header;            /* the position is on the directive, not in its text */
    bool active;            /* the real run took it (every inclusion of the file) */
    bool exact;             /* its condition has no opaque atom */
    bool tainted;           /* an earlier skipped include may have changed it */
    int sat;                /* 1, 0 (never), -1 (unknown) */
    char *cond;             /* the condition, as a formula */
    char *text;             /* the directive as written (its first line) */
    PcCube why[3];          /* the cheapest ways to turn an inactive group on */
    size_t nwhy;
} PcExplain;

/* The innermost group around loc of file f (which the fold read).  False:
 * none, or the fold was partial.  One query at a time per unit. */
bool pc_explain(PcUnit *u, SrcFile *f, SrcLoc loc, PcExplain *e);
void pc_explain_free(PcExplain *e);
/* Where the unit defines the macro a requirement is about ("the unit defines
 * `X` at f:3", or that it guards a header); false: nowhere to say. */
bool pc_req_via(const PcUnit *u, const PcReq *q, StrBuf *sb);
/* Markdown, for a hover. */
void pc_explain_render(const PcUnit *u, const PcExplain *e, StrBuf *sb);

enum { PCD_SIZE = 1, PCD_ORACLE = 2, PCD_SKIPPED = 4, PCD_WHY = 8,
    PCD_CHECK = 16 };
/* One "pc:" line per group, then totals, on f (CEREAL_PCDUMP, tests).
 * PCD_SIZE adds BDD sizes and the table size, which depend on the variable
 * order; PCD_ORACLE compares each group's value under the host valuation
 * with the preprocessor's own decision; PCD_CHECK compares the cheapest cube of every satisfiable group with an enumeration of its constrained support (up to 20 variables); PCD_WHY lists, under each group that
 * is off on the host, the cheapest ways to turn it on (pcq.h). */
void pc_dump(PcUnit *u, FILE *f, int flags);

#endif
