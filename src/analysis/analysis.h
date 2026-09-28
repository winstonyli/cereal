/* analysis.h - preprocessor static analyses. */
#ifndef CEREAL_ANALYSIS_H
#define CEREAL_ANALYSIS_H

#include "../parclient.h"
#include "../pp.h"
#include "../skel.h"

typedef struct HygieneState HygieneState;
typedef struct CondState CondState;
typedef struct IncludeState IncludeState;

typedef struct Analysis {
    PP *pp;
    Arena *arena;
    DiagEngine *diag;
    SrcMgr *sm;
    Interner *in;
    HygieneState *hyg;
    CondState *cond;
    IncludeState *inc;
} Analysis;

void analysis_attach(Analysis *a, PP *pp);
void analysis_finish(Analysis *a);   /* end-of-TU checks */
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
void cond_attach(Analysis *a);
void cond_finish(Analysis *a);
void include_attach(Analysis *a);
void include_finish(Analysis *a);
void *include_fork(Analysis *a, Analysis *w);
void include_join(Analysis *a, Analysis *w, uint32_t from, uint32_t to);
void include_release(Analysis *w);

#endif
