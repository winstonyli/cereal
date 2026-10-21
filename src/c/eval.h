/* eval.h - the evaluator: runs lowered functions (El + Cfg) over byte-addressed
 * objects.  docs/EVAL_DESIGN.md. */
#ifndef CEREAL_EVAL_H
#define CEREAL_EVAL_H

#include "c/tcfg_int.h"

typedef struct Ev Ev;

typedef struct EvOpts {
    long fuel;                  /* node evaluations before "step limit" */
    unsigned depth;             /* call depth limit */
    bool wrap;                  /* signed overflow wraps (gcc -fwrapv) instead of trapping */
    FILE *out, *err;            /* the program's stdout and stderr; NULL: output is dropped */
} EvOpts;

enum { EV_OK, EV_UNSUPPORTED, EV_TRAP, EV_FUEL, EV_EXIT };

typedef struct EvRes {
    int status;                 /* EV_* */
    int code;                   /* EV_EXIT: the argument of exit */
    char why[96];               /* not OK: the reason */
} EvRes;

Ev *ev_new(Checker *c, const EvOpts *o);
void ev_free(Ev *v);
/* Copies a lowered function in; the caller keeps (and frees) e and g. */
void ev_add_func(Ev *v, El *e, Cfg *g);
/* Copies in a unit of file-scope objects (its TO_DATA nodes), and registers an
 * object defined by a tentative definition (no initializer, zero). */
void ev_add_data(Ev *v, El *e);
void ev_add_tentative(Ev *v, uint32_t ref);
/* Calls a retained function with integer or pointer arguments. */
EvRes ev_call(Ev *v, uint32_t fsym, const uint64_t *args, size_t n, uint64_t *out);

/* The checker's hooks (`cereal run`): keep each function as it is lowered, and
 * after the last unit run `main` (c->opt.eval_argv are its arguments), the
 * program's stdout going to c->opt.tast; the exit status is stored through
 * c->opt.eval_status, and a stop is reported on stderr. */
void eval_keep(Checker *c, El *e, Cfg *g);
void eval_data(Checker *c, El *e);
void eval_tentative(Checker *c, uint32_t ref);
void eval_main(Checker *c);
/* --eval-sweep: for each scalar function of the main file, print its signature
 * and N seeded argument vectors with the evaluator's result (tests/eval_sweep.sh). */
void eval_sweep(Checker *c);
void eval_release(Checker *c);

#endif
