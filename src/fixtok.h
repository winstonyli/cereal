/* fixtok.h - the lint helper through which producers add fix-its
 * (docs/C_DESIGN.md section 3.2); the parser and checker use c/fixptok.h.
 * A fix-it is never written into the text a macro expansion produced. */
#ifndef CEREAL_FIXTOK_H
#define CEREAL_FIXTOK_H

#include "diag.h"
#include "lex.h"

/* A token read from a file or a #define as written (not one an expansion
 * produced).  Refused tokens poison the owner, as diag_fixit does. */
bool fix_tok_replace(DiagEngine *d, Diagnostic *dg, int owner,
                     const SrcMgr *sm, const Interner *in, const Tok *t,
                     const char *text, FixApplic app);

/* Inserts text before t (after: after it); the same token rules. */
bool fix_tok_insert(DiagEngine *d, Diagnostic *dg, int owner,
                    const SrcMgr *sm, const Interner *in, const Tok *t,
                    bool after, const char *text, FixApplic app);

#endif
