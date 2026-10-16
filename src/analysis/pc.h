/* pc.h - presence conditions of every conditional group of a unit.
 *
 * A symbolic fold of the main file and everything it includes: one BDD per
 * #if/#elif/#else group over the initial state of the macros (defined(X) and
 * thresholds of X's value; see pcexpr.h), with define/undef tracked through
 * the fold, #pragma once as per-file state, and system headers following the
 * real run (pc_trace_attach).  Computed or unresolved includes are skipped and recorded: groups
 * after one hold on the assumption that the skipped file defines nothing
 * that matters. */
#ifndef CEREAL_PC_H
#define CEREAL_PC_H

#include "pcexpr.h"

typedef struct PcGroup {
    SrcFile *file;
    uint32_t dir;           /* skeleton index of the #if/#elif/#else */
    Bdd pc;                 /* the group is compiled */
    uint8_t taint;          /* PC_T_*: skipped includes before it */
    uint64_t inst;          /* the group and the chain of #includes to it */
} PcGroup;

enum { PC_T_COMPUTED = 1, PC_T_QUOTED = 2, PC_T_ANGLED = 4 };

typedef struct PcMacro PcMacro;
typedef struct PcOnce PcOnce;

typedef struct PcUnit {
    Analysis *an;
    PcCtx cx;
    VEC(PcGroup) groups;
    bool over;              /* the BDD outgrew its cap: the fold is partial */
    bool conc;              /* inside a system header */
    uint8_t taint;
    uint64_t chain;         /* hash of the #include directives entered */
    size_t files, sys, unres, computed;
    VEC(const char *) skipped;      /* "<h>", "\"h\"", "computed" */
    /* internals */
    VEC(PcMacro *) macros;
    uint32_t *htab;
    size_t hcap;
    VEC(PcOnce *) once;
} PcUnit;

/* Fold the unit rooted at main.  False when it went over the cap. */
bool pc_fold(PcUnit *u, Analysis *a, SrcFile *main);
void pc_unit_free(PcUnit *u);
/* The truth of an atom for the macros as they are when the main file
 * starts. */
bool pc_host_atom(PcUnit *u, const PcAtom *at);
/* Record which groups the real preprocessor takes: the fold uses it inside
 * system headers and the oracle compares with it.  Single-threaded runs
 * only: call after analysis_attach, before the TU runs. */
void pc_trace_attach(Analysis *a);

enum { PCD_SIZE = 1, PCD_ORACLE = 2, PCD_SKIPPED = 4 };
/* One "pc:" line per group, then totals, on f (CEREAL_PCDUMP, tests).
 * PCD_SIZE adds BDD sizes and the table size, which depend on the variable
 * order; PCD_ORACLE compares each group's value under the host valuation
 * with the preprocessor's own decision. */
void pc_dump(PcUnit *u, FILE *f, int flags);

#endif
