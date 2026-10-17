/* pcexpr.h - presence conditions of #if expressions, as BDDs.
 *
 * An expression becomes a BDD over atoms, created in order of first use:
 *   PA_DEF   defined(X) at the start of the unit (I(X))
 *   PA_GE    the initial value of X, when defined, is >= c
 *   PA_LEAF  leaf c of an axis of the family table (pcfam.h): the OS, the compiler
 *   PA_HAS   a feature test (__has_include, __has_attribute, ...) keyed by its
 *            canonical text: a fact about the compiler and sysroot, exact
 *            but unknown to the fold, and never flipped by a query
 *   PA_TEXT  an opaque condition (a function-like call, arithmetic on a
 *            macro, a comparison of two macros), keyed by canonical text
 * The environment says what the macros mean at the point being evaluated:
 * whether X is defined now and which values it can hold under which
 * conditions.  The default environment is the start of the unit (nothing
 * defined or redefined yet), which is what the lint checks use.  An undefined
 * X is 0 (C99).  GE(hi) implies GE(lo); that clause is added per query over
 * the atoms of its support (pc_sat).
 *
 * Evaluation: an integer expression is a list of cases, a value under a
 * condition (a number, the initial value of a macro plus a number, or an
 * opaque text); every operator is a product over the cases of its operands
 * (pcexpr.c). */
#ifndef CEREAL_PCEXPR_H
#define CEREAL_PCEXPR_H

#include "analysis.h"
#include "bdd.h"

typedef struct PcNode PcNode;

typedef enum { PA_DEF, PA_GE, PA_TEXT, PA_LEAF, PA_HAS } PcAKind;

/* What changes an atom, cheapest first: an overlay, the language field (the
 * standard, GNU or ISO mode), the target (OS, compiler, data model). */
typedef enum { PC_CFG, PC_LANG, PC_TARGET } PcClass;

typedef struct PcAtom {
    PcAKind kind;
    Ident *id;              /* PA_DEF, PA_GE */
    intmax_t c;             /* PA_GE */
    const char *text;       /* PA_TEXT, PA_HAS; PA_LEAF: the axis ("os", "cc") */
    PcClass cls;            /* PA_LEAF: PC_LANG or PC_TARGET; else PC_CFG */
    bool derived;           /* a macro the family table (pcfam.h) follows from its leaf */
} PcAtom;

/* a fact no query flips: it keeps the value of the concrete run */
static inline bool pc_atom_pinned(const PcAtom *a)
{
    return a->kind == PA_TEXT || a->kind == PA_HAS;
}

/* One possible value of a defined macro: it holds under `cond`.  `known`:
 * the number n.  Otherwise the value the macro had at the start of the unit
 * (opq NULL), or the value a #define gave it: the expression expr (if the
 * body is one), named by opq where nothing more can be said. */
/* A function-like macro: parameters and replacement tokens, instantiated at
 * each call (pcexpr.c). */
typedef struct PcFn {
    Ident **params;
    uint32_t np;            /* named parameters */
    bool va;                /* a trailing ... (__VA_ARGS__) */
    TokSpan body;
} PcFn;

typedef struct PcEntry {
    Bdd cond;
    bool known;
    intmax_t n;
    const char *opq;
    PcNode *expr;           /* its body, evaluated where the macro is used */
    const PcFn *fn;         /* function-like: instantiated at each call */
} PcEntry;

typedef VEC(PcEntry) PcEntries;

struct PcCtx;
typedef struct PcEnv {
    void *ctx;
    Bdd (*defined)(void *ctx, struct PcCtx *cx, Ident *id);
    void (*values)(void *ctx, struct PcCtx *cx, Ident *id, PcEntries *out);
} PcEnv;

/* The atoms of one macro (or one opaque text) share a block of consecutive
 * BDD variables, blocks in order of first use: a macro's atoms stay together
 * without knowing them in advance. */
typedef struct PcBlock PcBlock;

typedef struct PcCtx {
    Analysis *an;
    BddMgr m;
    PcEnv env;              /* {0}: the start of the unit */
    int depth;              /* macro bodies being evaluated */
    VEC(PcBlock *) blocks;
    uint32_t *htab;         /* head blocks by key: block index + 1 */
    size_t hcap;
    struct { Bdd f, g; } cons[256]; /* pc_constrain by f (BDD_FALSE: empty) */
} PcCtx;

void pc_init(PcCtx *cx, Analysis *an, size_t node_cap);
void pc_free(PcCtx *cx);
Bdd pc_atom(PcCtx *cx, PcAKind kind, Ident *id, intmax_t c, const char *text);
/* The atom behind BDD variable var, or NULL. */
const PcAtom *pc_atom_at(const PcCtx *cx, uint32_t var);
/* The value of a PPNUM token (decimal, octal, hex, with suffixes); *uns
 * (if given): it is unsigned. */
bool pc_ppnum(Analysis *an, const Tok *t, intmax_t *out, bool *uns);
/* NULL when the tokens are not an expression this module models */
PcNode *pc_parse(PcCtx *cx, TokSpan toks);
/* The condition of an #if/#elif/#ifdef/#ifndef/#elifdef/#elifndef (NULL: none
 * or not modeled). */
PcNode *pc_branch_cond(PcCtx *cx, const SkDirective *d);
PcNode *pc_defined_node(PcCtx *cx, Ident *id, bool negate);
/* The value of a constant expression (a macro body without macros). */
bool pc_const_value(PcCtx *cx, TokSpan toks, intmax_t *out);
bool pc_has_vars(const PcNode *n);
/* The condition under which n is nonzero. */
Bdd pc_truth(PcCtx *cx, PcNode *n);
/* What a host knows of the macros named in an opaque atom's key. */
typedef struct PcKeyHost {
    void *ctx;
    /* The value of macro name (0 when undefined); false: not a number. */
    bool (*value)(void *ctx, const char *name, size_t n, intmax_t *v);
    bool (*defined)(void *ctx, const char *name, size_t n);
} PcKeyHost;
/* The value of an opaque atom's key under the host; false: it names a call
 * or a macro the host cannot reduce to a number. */
bool pc_key_eval(const char *key, const PcKeyHost *h, intmax_t *out);
/* f with every clause a query needs: monotone thresholds, family table. */
Bdd pc_constrain(PcCtx *cx, Bdd f);
/* The macro or text a BDD variable belongs to (one value for all of its
 * blocks), and its name; UINT32_MAX for a variable without an atom. */
uint32_t pc_var_owner(const PcCtx *cx, uint32_t var);
const char *pc_owner_name(const PcCtx *cx, uint32_t owner);
/* 1 satisfiable, 0 not, -1 unknown (the BDD outgrew its cap). */
int pc_sat(PcCtx *cx, Bdd f);

#endif
