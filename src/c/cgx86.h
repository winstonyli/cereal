/* cgx86.h - what the x86-64 back end's files share: the per-checker state
 * (the object being built, the data defined so far) and the data emitter
 * (cgdata.c).  See docs/TAST_T9_PLAN.md, back end. */
#ifndef CEREAL_CGX86_H
#define CEREAL_CGX86_H

#include "c/mir.h"
#include "c/tast_int.h"

typedef struct X86 {
    MObj *obj;                  /* --emit-obj: the object being built */
    VEC(char *) names;          /* mangled linker names handed out (freed at the end) */
    VEC(char *) defined;        /* the data objects defined so far */
    VEC(uint32_t) tents;        /* tentative definitions (symbol refs), settled at the end */
    uint32_t nlits;             /* anonymous read-only objects made for data */
} X86;

/* The state of a checker, made on first use. */
X86 *x86_get(Checker *c);

/* The linker name of a function or object.  A block-scope static gets the
 * function's name (fname, an identifier id) and its own number in front.
 * Valid until x86_finish. */
const char *x86_symname(Checker *c, uint32_t fname, uint32_t ref);

/* Prints and/or encodes a data object (per the checker's options); the
 * caller still owns d. */
void x86_emit_data(Checker *c, const MData *d);

/* Defines the static object of data node n of e (cgdata.c); false, with
 * the reason in why, when its initializer is outside what the evaluator
 * does (the object is then not defined). */
bool x86_data_node(El *e, uint32_t n, char *why, size_t nwhy);

/* A file-scope object declared without an initializer: defined at the end
 * (zeros) unless a data node defines it. */
void x86_tentative(Checker *c, uint32_t ref);

/* Settles tentative definitions, writes the object file, frees the state
 * (checker_free). */
void x86_finish(Checker *c);

#endif
