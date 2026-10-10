/* fixptok.h - the parser and checker helpers through which producers add
 * fix-its (docs/C_DESIGN.md section 3.2).  A token that is part of a macro
 * expansion (an argument included) poisons the owner, as gcc refuses a
 * fix-it on a location inside a macro map. */
#ifndef CEREAL_FIXPTOK_H
#define CEREAL_FIXPTOK_H

#include "ast.h"
#include "../diag.h"
#include "../lex.h"

/* The tokens first..last (the same token for one). */
bool fix_replace_toks(DiagEngine *d, Diagnostic *dg, int owner,
                      const SrcMgr *sm, const Interner *in, const PTok *first,
                      const PTok *last, const char *text, FixApplic app);
bool fix_insert_before(DiagEngine *d, Diagnostic *dg, int owner,
                       const PTok *t, const char *text, FixApplic app);
bool fix_insert_after(DiagEngine *d, Diagnostic *dg, int owner,
                      const SrcMgr *sm, const Interner *in, const PTok *t,
                      const char *text, FixApplic app);

#endif
