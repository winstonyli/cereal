/* pcfam.h - the family table (docs/CONFIG_DESIGN.md 3.3): the OS and compiler
 * a unit is built for, as one-hot leaves, and what each leaf says about the
 * predefined macros, and the data model (pointer and long size) that fixes
 * the limits of the types.  Leaves are atoms (PA_LEAF) of one block per axis; a
 * leaf is changed by the target, not by a macro overlay, so the cost model
 * weighs them above every config change.  A condition that mentions a table
 * macro is conjoined with the table's clauses before it is solved, so it
 * cannot ask for `_MSC_VER` on Linux. */
#ifndef CEREAL_PCFAM_H
#define CEREAL_PCFAM_H

#include "pcexpr.h"

/* macro name is determined (wholly or partly) by a leaf */
bool pc_fam_macro(const char *name);
/* The macro's value is always below this (a digit of a version number such as
 * __GNUC_MINOR__, so that major * 100 + minor reads as two digits); 0: no
 * bound is known. */
intmax_t pc_fam_range(const char *name);
/* f with the table's clauses for the macros and leaves in its support */
Bdd pc_family(PcCtx *cx, Bdd f);
/* what changes the leaves of the named axis: the language field or the target */
PcClass pc_axis_class(const char *axis);
/* "os = linux" */
const char *pc_leaf_name(const PcAtom *at);
/* the axis of a leaf, to index per-axis state */
int pc_leaf_axis(const PcAtom *at);
/* Append the values a table macro has at the start of the unit, where it is
 * defined (def): per leaf, or one for all.  False: the table does not value
 * it. */
bool pc_fam_values(PcCtx *cx, Ident *id, Bdd def, PcEntries *out);
/* Is this leaf the host's, judged from which predefined macros it defines. */
bool pc_leaf_host(const PcAtom *at, const PcKeyHost *h);

#endif
