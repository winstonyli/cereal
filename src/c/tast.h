/* tast.h - the typed AST export (docs/TAST_DESIGN.md, docs/TAST_T1_PLAN.md).
 *
 * After the checker has visited a unit, tast_unit elaborates it into an
 * explicit tree in which every conversion is a node, then prints it
 * (--dump-tast).  A construct outside the supported subset makes the whole
 * unit print "unsupported: <reason>" and produce no tree. */
#ifndef CEREAL_TAST_H
#define CEREAL_TAST_H

#include "c/check_int.h"

#define TAST_VERSION 1

/* Elaborates and prints the unit the checker just visited (c->opt.tast). */
void tast_unit(Checker *c);

/* The bytes of object ref initialized by init (a constexpr aggregate): false
 * if chk rejects an element (it reports), or the evaluator cannot build it. */
bool tast_image(Checker *c, uint32_t ref, uint32_t init, bool (*chk)(Checker *, const TIRec *),
                CxImg *out);

#endif
