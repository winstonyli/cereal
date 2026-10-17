/* analysis.h - preprocessor static analyses. */
#ifndef CEREAL_ANALYSIS_H
#define CEREAL_ANALYSIS_H

#include "../parclient.h"
#include "../fixtok.h"
#include "../pp.h"
#include "../skel.h"

typedef struct HygieneState HygieneState;
typedef struct CondState CondState;
typedef struct IncludeState IncludeState;
typedef struct PcTrace PcTrace;
typedef struct PcUnit PcUnit;

typedef struct Analysis {
    PP *pp;
    Arena *arena;
    DiagEngine *diag;
    SrcMgr *sm;
    Interner *in;
    HygieneState *hyg;
    CondState *cond;
    IncludeState *inc;
    PcTrace *pct;           /* the groups the run took (pc.h); NULL: no fold */
    PcUnit *pcu;            /* the fold, kept for the caller (analysis_keep_pc) */
    bool keep_pc;
} Analysis;

void analysis_attach(Analysis *a, PP *pp);
void analysis_finish(Analysis *a);   /* end-of-TU checks */
/* Fold the unit's presence conditions at analysis_finish and keep the result
 * (call after analysis_attach): analysis_take_pc hands it over; free it with
 * pc_unit_destroy.  Without this the fold runs only for CEREAL_PCDUMP. */
void analysis_keep_pc(Analysis *a);
PcUnit *analysis_take_pc(Analysis *a);
/* Only the fold, with none of the checks of analysis_attach (the index job's
 * --cover): attach before the run, analysis_fold after it; free the result with
 * pc_unit_destroy.  NULL: the unit has no main file. */
void analysis_attach_pc(Analysis *a, PP *pp);
PcUnit *analysis_fold(Analysis *a);
/* Free the state of an analysis that will not be finished (a parallel run
 * that fell back). */
void analysis_discard(Analysis *a);
void cond_discard(Analysis *a);
void include_discard(Analysis *a);
/* Take part in a parallel run (call after analysis_attach). */
ParClient analysis_par_client(Analysis *a);

/* helpers shared by the analyzers */
bool an_user_loc(Analysis *a, SrcLoc loc);          /* not system/virtual */
bool an_user_file(const SrcFile *f);
Skeleton *an_skeleton(Analysis *a, SrcFile *f);
bool is_c_keyword(const char *s, size_t n);

void hygiene_attach(Analysis *a);
void hygiene_finish(Analysis *a);
void *hygiene_fork(Analysis *a, Analysis *w);
void hygiene_release(Analysis *w);
void hygiene_decode(Analysis *w);   /* state of a reused cell: none */
void cond_attach(Analysis *a);
void cond_finish(Analysis *a);
void include_attach(Analysis *a);
void include_finish(Analysis *a);
void *include_fork(Analysis *a, Analysis *w);
void include_join(Analysis *a, Analysis *w, uint32_t from, uint32_t to);
void include_release(Analysis *w);
struct CellEnc;
struct CellDec;
void *include_encode(Analysis *w, uint32_t from, uint32_t to,
                     struct CellEnc *e);
void include_decode(Analysis *w, const void *blob, const struct CellDec *d);

#endif
