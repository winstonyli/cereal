/* pc.h - presence conditions of every conditional group of a unit.
 *
 * A symbolic fold of the main file and everything it includes: one BDD per
 * #if/#elif/#else group over the initial state of the macros (defined(X) and
 * thresholds of X's value; see pcexpr.h), with define/undef tracked through
 * the fold, #pragma once as per-file state, and system headers evaluated
 * under the host valuation (the macros as they are when the main file
 * starts).  Computed or unresolved includes are skipped and recorded: groups
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
    size_t files, sys, unres, computed;
    /* internals */
    VEC(PcMacro *) macros;
    uint32_t *htab;
    size_t hcap;
    VEC(PcOnce *) once;
} PcUnit;

/* Fold the unit rooted at main.  False when it went over the cap. */
bool pc_fold(PcUnit *u, Analysis *a, SrcFile *main);
void pc_unit_free(PcUnit *u);
/* The macros as they are when the main file starts: the truth of an atom. */
bool pc_host_atom(PcUnit *u, const PcAtom *at);
/* One "pc:" line per group, then totals, on f (CEREAL_PCDUMP, tests);
 * verbose adds BDD sizes, which depend on the variable order. */
void pc_dump(PcUnit *u, FILE *f, bool verbose);

#endif
