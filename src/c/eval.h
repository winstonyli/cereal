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
    uint64_t maxobj;            /* the most bytes of one object (a bigger request traps); 0: 1 GiB */
    FILE *out, *err;            /* the program's stdout and stderr; NULL: output is dropped */
} EvOpts;

/* The sweep's bound on one object: the largest real object seen in the Lua sweep is 16 MB (its stack), and the solver picks
 * sizes of 61 MB to 1 GB for the same allocations; such an object is written whole by the code that fills it (docs/STOPPED_SEEDS_SPIKE.md, Memory). */
#define EV_SWEEP_OBJ ((uint64_t)32 << 20)

enum { EV_OK, EV_UNSUPPORTED, EV_TRAP, EV_FUEL, EV_EXIT };

typedef struct EvRes {
    int status;                 /* EV_* */
    int code;                   /* EV_EXIT: the argument of exit */
    char why[96];               /* not OK: the reason */
} EvRes;

void ev_free(Ev *v);
/* Copies a lowered function in; the caller keeps (and frees) e and g. */
/* Copies in a unit of file-scope objects (its TO_DATA nodes), and registers an
 * object defined by a tentative definition (no initializer, zero). */
void ev_add_data(Ev *v, El *e);
/* Calls a retained function with integer or pointer arguments. */
EvRes ev_call(Ev *v, uint32_t fsym, const uint64_t *args, size_t n, uint64_t *out);

/* The image of a static object, for the code generator: its bytes (an address
 * element left zero) and one relocation per address element, in element order.
 * El e is the unit the TO_DATA node n is in (registered by eval_statics). */
enum { EV_R_SYM, EV_R_STR, EV_R_LABEL };
typedef struct EvReloc {
    uint32_t off;
    int kind;
    int64_t addend;
    uint32_t ref, fname;        /* SYM: the symbol; LABEL: its id in ref, the function in fname */
    uint8_t *lit;               /* STR: the literal's bytes (owned) */
    uint64_t litsize;
} EvReloc;
typedef struct EvImage {
    uint8_t *bytes;             /* NULL: all zero */
    uint64_t size;
    EvReloc *rel;
    size_t nrel;
    char why[96];               /* ev_object false: the reason */
} EvImage;
bool ev_object(Ev *v, El *e, uint32_t n, EvImage *img);
void ev_image_free(EvImage *img);

/* The checker's hooks (CHK_EVAL): keep each function and object as it is lowered. */
void eval_keep(Checker *c, El *e, Cfg *g);
void eval_tentative(Checker *c, uint32_t ref);
/* Registers the unit's static objects for ev_object (no-op without any). */
void eval_statics(Checker *c, El *e);
/* The checker's evaluator, made on first use. */
Ev *ev_of(Checker *c);
const char *ev_dup_def(Ev *v);   /* --eval-link: a name defined in two files, or NULL */
/* --eval-link: the files of one run, checked one after the other with one Ev, then swept together. */
struct EvLink {
    Ev *ev;
    VEC(Checker *) cs;          /* the files whose checkers are kept, in order */
    VEC(void *) srcs;           /* what the caller keeps for each (its translation unit) */
};
/* Sweep the files, free their checkers and the shared Ev; the exit status.  The caller frees srcs. */
int evlink_end(struct EvLink *lk);
/* After the last unit: `cereal run` runs main (c->opt.eval_argv are its arguments,
 * the program's stdout goes to c->opt.tast, the exit status through
 * c->opt.eval_status, a stop is reported on stderr); --eval-sweep prints each scalar
 * function's signature and N seeded argument vectors with the result (tests/eval_sweep.sh). */
void eval_finish(Checker *c);
void eval_release(Checker *c);

#endif
