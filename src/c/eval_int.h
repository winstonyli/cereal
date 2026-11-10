/* eval_int.h - what eval.c shares with the host library (eval_libc.c): the
 * evaluator's state and its checked access to objects. */
#ifndef CEREAL_EVAL_INT_H
#define CEREAL_EVAL_INT_H

#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/eval.h"
#include "c/symx.h"
#include "idxset.h"

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

/* the evaluator's own interface (eval.h holds what other files use) */
void ev_free(Ev *v);
/* Calls a retained function with integer or pointer arguments. */
EvRes ev_call(Ev *v, uint32_t fsym, const uint64_t *args, size_t n, uint64_t *out);
const char *ev_dup_def(Ev *v);   /* --eval-link: a name defined in two files, or NULL */

typedef struct Obj {
    uint8_t *b, *init;          /* bytes and one init flag per byte: 0 no, 1 yes, 2 a pointer the sweep drew */
    uint32_t *sh;               /* tracking: per byte, an 8-bit expression of the input (0: concrete); NULL: none */
    uint64_t size;
    uint32_t fn;                /* a function's object: its fsym + 1, in the numbering of fc */
    Checker *fc;                /* ... the file that numbering belongs to */
    bool live, ro;
    bool pin;                   /* cached by id for the whole run (host objects): a state restore keeps it */
    bool heap;                  /* made by malloc and friends: free may end it */
    uint8_t kind;               /* OK_*: what the object stands for, for an address constant */
    bool stub;                  /* a static object with no definition here: only its address is usable */
    uint32_t lazy;              /* a sweep blob made on demand (docs/LAZY_BLOBS_DESIGN.md): its index + 1 (0: not one); it grows to what
                                   is accessed and its first read of a byte asks Ev.lazy_fill for the value; the index is its model address
                                   (sw_addr.h) */
    uint32_t ref, fname;       /* OK_STATIC: the symbol ref, and the function it is local to (name id) */
    uint32_t sd1;               /* a static object not yet initialized: its Sd index + 1 */
} Obj;

enum { OK_NONE, OK_STATIC, OK_STR, OK_LABEL };   /* functions are told by Obj.fn */

typedef struct Blk {
    uint32_t *st;
    size_t nst;
    CCase *cases;
    size_t nc;
    uint32_t val, a, b;
    uint8_t term;
    uint64_t seen;              /* the edges any call has taken: a branch's a (bit 0) and b (bit 1);
                                   a switch's cases by index (bits 0..61, later ones share 61), default bit 62 */
} Blk;

/* One step of a tracked call's path: a branch or switch on a symbolic value (blk, the edge taken:
 * a branch's 0 = a, 1 = b; a switch's case index or nc for the default; expr is the condition's
 * slot expression, want whether it was non-zero), or a pin (blk NULL: expr is an equation the
 * path depends on, such as a concretized index). */
typedef struct PathEnt {
    uint32_t expr;
    const Blk *blk;
    uint32_t edge;
    bool want;
} PathEnt;

#define PATH_MAX_ENT 256

static inline unsigned blk_edge_bit(const Blk *b, uint32_t edge)
{
    return b->term == TK_SWTCH ? (edge >= b->nc ? 62 : edge < 61 ? edge : 61) : edge;
}

typedef struct Par {
    uint32_t ref;
    TypeId ty;
} Par;

typedef struct Fn {
    Checker *c;                 /* the file this body, its types and its symbols belong to */
    uint32_t fsym;
    TNode *nodes;
    uint32_t *kids;
    uint8_t *bytes;
    Par *par;
    size_t np;
    Blk *blk;
    size_t nblk;
    LabEnt *lab;                /* its labels: id to block (a computed goto goes to one) */
    size_t nlab;
    uint32_t entry, nreg, ntmp;
    uint32_t fname;             /* the El's function name (identifier id), 0 outside one */
    bool variadic, user;        /* user: defined outside the system headers */
    TypeId ret;
    uint32_t *strobj;           /* per node: its string literal's object + 1 */
} Fn;

/* A static-storage object: a file-scope object (persistent ref) or a block-scope
 * static (local ref, of the function fn).  Created, zeroed and initialized on
 * its first reference; node is its TO_DATA (TN: tentative, zero), found in
 * the elements of Fn fn. */
typedef struct Sd {
    Checker *c;                 /* the file ref belongs to */
    uint32_t ref, node, fn, obj;
    TypeId ty;
    bool local;
    bool stub;                  /* made by a reference: no definition registered (yet) */
    uint32_t alias;             /* --eval-link: this declaration stands for Sd alias - 1 of another file */
} Sd;

#define EV_CALL_OBJS (1u << 20) /* objects one call may make */

struct Ti;

struct Ev {
    Checker *c;
    EvOpts o;
    EvRes res;
    VEC(Obj) obj;               /* object id n is obj.data[n - 1] */
    VEC(Fn) fn;
    VEC(Sd) sd;
    /* --eval-link: one Ev for all files.  Index of the functions, by (file, symbol) and, for external
     * definitions, by name; filled up to fi_done. */
    bool link;
    IdxSet fi_id, fi_nm;
    size_t fi_done;
    IdxSet si_id, si_nm;        /* ... and of the static objects, filled up to si_done */
    size_t si_done;
    const char *dup_def;        /* a name with two external definitions, or NULL */
    long fuel;
    size_t objlim;              /* obj.len at which "object limit" ends the evaluation; 0: none */
    unsigned depth;
    VEC(uint32_t) rec;          /* ev_object: the offsets of address-valued elements, in order */
    uint32_t rec_id;            /* ... of this object */
    bool fold;                  /* constant folding, as gcc does: conversions saturate instead of trapping */
    bool build;                 /* the sweep's state-building phase: the clock may be read (as 0) */
    bool asm_ran;               /* a sweep call ran an asm statement (outputs 0): its result is not compared with gcc */
    VEC(uint32_t) lab;          /* label objects: triples (fn index, label id, object) */
    VEC(uint64_t) dict;         /* the sweep: operands of integer compares and switch cases, while dict_on */
    bool dict_on, dict_use;     /* collecting; drawing from it (sorted, unique) */
    uint32_t stream[3];         /* the objects behind stdout and stderr (pointer, FILE token), 0: not made */
    uint32_t token[3];
    uint32_t errno_obj;         /* the int behind __errno_location, 0: not made */
    uint32_t ctype_obj;         /* the pointer cell behind __ctype_b_loc, 0: not made */
    /* Concolic tracking (docs/CONCOLIC_DESIGN.md): values carry an expression of the input bytes
     * beside their number; 0 is concrete.  Off, every shadow stays 0 and nothing else changes. */
    bool track;
    Sx sx;                      /* the expressions of the current tracked call */
    uint32_t sh;                /* the shadow of the value ev_node last returned */
    const uint32_t *ash;        /* ev_call: a shadow per argument, NULL: all concrete */
    VEC(PathEnt) path;          /* the tracked call's branches and pins, at most PATH_MAX_ENT */
    bool path_full;             /* ... and entries were dropped after that */
    VEC(uint32_t) dom;          /* conditions that hold of every input (the range of a pointer offset the solver may pick), asserted with the path */
    unsigned long nbr;          /* branches and switches run while tracking, symbolic or not */
    unsigned long newseen;      /* edges taken for the first time (Blk.seen), counted over all calls */
    uint32_t ld_obj, f_obj;     /* the object and offset of the last pointer read (ld), and of the one the
                                   latest stop followed (f): where the bad pointer or byte was read from */
    int32_t ld_off, f_off;
    uint32_t sub_a, sub_b;      /* the objects of the pointer subtraction that stopped the call (the sweep retries it aliased) */
    uint64_t sub_sz;            /* the pointee size of that subtraction (Ti.psz of operand 0; 0: none), the size class of the retry hint */
    uint32_t pun_obj;           /* the object whose drawn pointer was read as an integer (the sweep retries it without the pointer into it) */
    char note[112];             /* the sweep's description of that place, for its miss report */
    /* A typed load of uninitialized bytes of a lazy object: draws and stores the value (may make
     * objects, so the caller looks the object up again); false: the load traps as uninitialized. */
    bool (*lazy_fill)(Ev *v, uint32_t id, uint64_t off, const struct Ti *ti);
    /* An untyped read of n bytes of a lazy object: its untouched bytes are zero, a draw whose default is 0 (so a
     * solver can set it). */
    void (*lazy_zero)(Ev *v, uint32_t id, uint64_t off, uint64_t n);
    void *lazy_ud;              /* the sweep's context for lazy_fill */
};

#define EV_LAZY_CAP 65536       /* the most a lazy object grows to (a struct with a big table, as zstd's ZSTD_DDict, is over 8192) */

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
    Checker *up_c;              /* the caller's file, restored when the frame ends */
} Fr;

typedef struct Ti {
    unsigned bytes;
    bool sg, boolean, flt;       /* flt: float or double, the value is its IEEE bit pattern */
    bool ptr;                    /* a pointer: its shadow is its 64-bit word (the offset may be the solver's), see ptr_add_sh */
    TypeId fty;                  /* a pointer to a function: that function type (lazy blobs draw a function for it) */
    unsigned psz;                /* a pointer: the size of its pointee, 0 if it has none (void, a function, incomplete) */
} Ti;

/* ---- failure: the first one wins and ends the evaluation ---- */

void ev_fail(Ev *v, int st, const char *fmt, ...);

#define UNSUP(v, ...) ev_fail(v, EV_UNSUPPORTED, __VA_ARGS__)
#define TRAP(v, ...) ev_fail(v, EV_TRAP, __VA_ARGS__)

/* ---- types and values ---- */

bool ev_tinfo(Ev *v, TypeId t, Ti *ti);
uint64_t ev_canon(const Ti *ti, uint64_t x);
double ev_fget(const Ti *ti, uint64_t x);
uint64_t ev_fput(const Ti *ti, double d);

/* ---- objects: a pointer is an object id (high 32 bits, 0 is null) and a signed offset ---- */

static inline uint64_t mkptr(uint32_t id, int64_t off)
{
    return ((uint64_t)id << 32) | (uint32_t)(int32_t)off;
}
static inline int32_t poff(uint64_t p) { return (int32_t)(uint32_t)p; }
static inline uint32_t pid(uint64_t p) { return (uint32_t)(p >> 32); }

uint32_t ev_new_obj(Ev *v, uint64_t size, bool ro);
uint32_t ev_function_obj(Ev *v, Checker *c, uint32_t fsym);   /* the function's object (one per definition) */
void ev_kill_obj(Ev *v, uint32_t id);
/* The bytes [p, p + n) of a live object, or NULL after a trap. */
Obj *ev_access(Ev *v, uint64_t p, uint64_t n, const char *what);
bool ev_pun_ok(Ev *v, const Obj *o, uint64_t off, uint64_t n);
/* Function fsym: has external linkage (a global symbol), its name. */
bool ev_fn_external(Checker *c, uint32_t fsym);
const char *ev_fn_name(Checker *c, uint32_t fsym);
void ev_copy_n(Ev *v, uint64_t dst, uint64_t src, uint64_t n);
void ev_zero_n(Ev *v, uint64_t dst, uint64_t n);
/* Drops the dead objects at the end of the table (see eval.c). */
void ev_trim(Ev *v);
/* A write of concrete bytes into [off, off + n) of o: their shadows end. */
void ev_unshadow(Obj *o, uint64_t off, uint64_t n);
#define EV_SHADOW_CAP (1u << 24)    /* the most bytes of an object that carries a shadow array (4 bytes of expression per byte) */
bool ev_shadow(Obj *o);
/* An untyped read of [off, off + n) of o (memcpy, the host library): a lazy object's untouched bytes
 * are zero (a draw with the default 0 when it is short, see Ev.lazy_zero), now initialized. */
void ev_lazy_zero(Ev *v, Obj *o, uint64_t off, uint64_t n);

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
