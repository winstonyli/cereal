/* eval_int.h - what eval.c shares with the host library (eval_libc.c): the
 * evaluator's state and its checked access to objects. */
#ifndef CEREAL_EVAL_INT_H
#define CEREAL_EVAL_INT_H

#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/eval.h"

typedef struct Obj {
    uint8_t *b, *init;          /* bytes and one init flag per byte */
    uint64_t size;
    uint32_t fn;                /* a function's object: its fsym + 1 */
    bool live, ro;
    bool heap;                  /* made by malloc and friends: free may end it */
} Obj;

typedef struct Blk {
    uint32_t *st;
    size_t nst;
    CCase *cases;
    size_t nc;
    uint32_t val, a, b;
    uint8_t term;
} Blk;

typedef struct Par {
    uint32_t ref;
    TypeId ty;
} Par;

typedef struct Fn {
    uint32_t fsym;
    TNode *nodes;
    uint32_t *kids;
    uint8_t *bytes;
    Par *par;
    size_t np;
    Blk *blk;
    size_t nblk;
    uint32_t entry, nreg, ntmp;
    bool variadic, user;        /* user: defined outside the system headers */
    TypeId ret;
    uint32_t *strobj;           /* per node: its string literal's object + 1 */
} Fn;

/* A static-storage object: a file-scope object (persistent ref) or a block-scope
 * static (local ref, of the function fn).  Created, zeroed and initialized on
 * its first reference; node is its TO_DATA (TN: tentative, zero), found in
 * the elements of Fn fn. */
typedef struct Sd {
    uint32_t ref, node, fn, obj;
    TypeId ty;
    bool local;
} Sd;

struct Ev {
    Checker *c;
    EvOpts o;
    EvRes res;
    VEC(Obj) obj;               /* object id n is obj.data[n - 1] */
    VEC(Fn) fn;
    VEC(Sd) sd;
    long fuel;
    unsigned depth;
    uint32_t stream[3];         /* the objects behind stdout and stderr (pointer, FILE token), 0: not made */
    uint32_t token[3];
};

typedef struct Loc {
    uint32_t ref, obj;
} Loc;

typedef struct Fr {
    Ev *v;
    Fn *f;
    uint32_t *reg, *tmp;        /* per reg/tmp N: its object, 0: none yet */
    uint64_t va;                /* a variadic function: the pointer to its extra arguments */
    VEC(Loc) loc;
    VEC(uint32_t) own;          /* objects that die with the frame */
    struct Fr *up;
} Fr;

typedef struct Ti {
    unsigned bytes;
    bool sg, boolean, flt;       /* flt: float or double, the value is its IEEE bit pattern */
} Ti;

/* ---- failure: the first one wins and ends the evaluation ---- */

void ev_fail(Ev *v, int st, const char *fmt, ...);

#define UNSUP(v, ...) ev_fail(v, EV_UNSUPPORTED, __VA_ARGS__)
#define TRAP(v, ...) ev_fail(v, EV_TRAP, __VA_ARGS__)

/* ---- types and values ---- */

bool ev_tinfo(Ev *v, TypeId t, Ti *ti);
uint64_t ev_canon(const Ti *ti, uint64_t x);
double ev_fget(const Ti *ti, uint64_t x);

/* ---- objects: a pointer is an object id (high 32 bits, 0 is null) and a signed offset ---- */

static inline uint64_t mkptr(uint32_t id, int64_t off)
{
    return ((uint64_t)id << 32) | (uint32_t)(int32_t)off;
}
static inline int32_t poff(uint64_t p) { return (int32_t)(uint32_t)p; }
static inline uint32_t pid(uint64_t p) { return (uint32_t)(p >> 32); }

uint32_t ev_new_obj(Ev *v, uint64_t size, bool ro);
void ev_kill_obj(Ev *v, uint32_t id);
/* The bytes [p, p + n) of a live object, or NULL after a trap. */
Obj *ev_access(Ev *v, uint64_t p, uint64_t n, const char *what);
void ev_copy_n(Ev *v, uint64_t dst, uint64_t src, uint64_t n);
void ev_zero_n(Ev *v, uint64_t dst, uint64_t n);

/* ---- the host library (eval_libc.c) ---- */

typedef struct HArg {
    uint64_t x;                 /* the slot */
    TypeId ty;                  /* the argument expression's type */
} HArg;

/* A host function: the result slot (the caller canonicalizes it to the call's type). */
typedef uint64_t (*HostFn)(Ev *v, const HArg *a, unsigned na);
HostFn ev_host_fn(const char *name);
/* A host object (stdout, stderr): its object id, or 0 if the name is not one. */
uint32_t ev_host_object(Ev *v, const char *name);

#endif
