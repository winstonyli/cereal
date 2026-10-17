/* pcfam.h - the family table (docs/CONFIG_DESIGN.md 3.3): the OS and compiler
 * a unit is built for, as one-hot leaves, and what each leaf says about the
 * predefined macros.  Leaves are atoms (PA_LEAF) of one block per axis; a
 * leaf is changed by the target, not by a macro overlay, so the cost model
 * weighs them above every config change.  A condition that mentions a table
 * macro is conjoined with the table's clauses before it is solved, so it
 * cannot ask for `_MSC_VER` on Linux. */
#ifndef CEREAL_PCFAM_H
#define CEREAL_PCFAM_H

#include "pcexpr.h"

/* macro name is determined (wholly or partly) by a leaf */
bool pc_fam_macro(const char *name);
/* f with the table's clauses for the macros and leaves in its support */
Bdd pc_family(PcCtx *cx, Bdd f);
/* "os = linux" */
const char *pc_leaf_name(const PcAtom *at);
/* Is this leaf the host's, judged from which predefined macros it defines. */
bool pc_leaf_host(const PcAtom *at, const PcKeyHost *h);

#endif
