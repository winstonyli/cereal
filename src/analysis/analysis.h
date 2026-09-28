/* analysis.h - preprocessor static analyses. */
#ifndef CEREAL_ANALYSIS_H
#define CEREAL_ANALYSIS_H

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

/* helpers shared by the analyzers */
bool an_user_loc(Analysis *a, SrcLoc loc);          /* not system/virtual */
bool an_user_file(const SrcFile *f);
Skeleton *an_skeleton(Analysis *a, SrcFile *f);
bool is_c_keyword(const char *s, size_t n);

void hygiene_attach(Analysis *a);
void hygiene_finish(Analysis *a);
void cond_attach(Analysis *a);
void cond_finish(Analysis *a);
void include_attach(Analysis *a);
void include_finish(Analysis *a);

#endif
