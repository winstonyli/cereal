/* cgx86.c - a function's control-flow graph as x86-64 (System V) machine
 * instructions.
 *
 * The simplest correct scheme: every variable, register (reg N) and temporary
 * lives in a stack slot; an expression is evaluated into %rax, a pending left
 * operand waits on the stack.  Every integer or pointer value in %rax is
 * canonical (sign- or zero-extended from its type's width to 64 bits), so
 * 64-bit arithmetic followed by a re-canonicalisation gives the type's own
 * wrap-around.  A float or double value lives in %xmm0 instead (a waiting
 * left operand goes through %rax onto the stack); arguments follow System V
 * for integer and SSE classes, the rest on the stack.  Globals and statics
 * are reached %rip-relative and defined by cgdata.c; string literals are
 * anonymous read-only objects.  Not supported yet (the function is then
 * reported in a comment): long double, bit-fields, computed goto, conversions
 * between 64-bit unsigned and float.  Aggregates are addresses (see
 * "aggregates"); passing and returning them follows System V.  See
 * docs/TAST_T9_PLAN.md. */
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/cgx86.h"
#include "c/tcfg_int.h"

typedef struct Slot {
    uint32_t ref;
    int32_t off;
} Slot;

typedef struct AggCls {
    uint64_t size;
    int np;                     /* eightbytes */
    bool mem;
    char c[2];
} AggCls;

typedef struct Gx {
    Cfg *L;
    Checker *c;
    MFunc f;
    VEC(Slot) vars;
    VEC(MData) lits;            /* string literals, read-only data */
    int32_t *regoff, *tmpoff;   /* per reg N / tmp N: its slot, 0: none yet */
    int32_t frame;
    int depth;                  /* values waiting on the stack */
    size_t sub_at;              /* the frame-size instruction, patched at the end */
    AggCls rc;                  /* the function's own result class, when an aggregate */
    bool retagg;
    int32_t retptr;             /* where a memory-class result goes */
    bool variadic;
    int32_t save;               /* the register save area of a variadic function */
    int va_gp, va_fp, va_stk;   /* va_start: offsets past the named arguments, rbp offset */
    uint32_t nlab;              /* next label past the blocks' */
    char why[96];
} Gx;

static const uint8_t arg_regs[6] = {R_DI, R_SI, R_DX, R_CX, R_R8, R_R9};

static void unsup(Gx *g, const char *fmt, ...)
{
    if (!g->why[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g->why, sizeof g->why, fmt, ap);
        va_end(ap);
    }
}

static MOp R(int r) { MOp o = {MK_REG, (uint8_t)r, 0, NULL}; return o; }
static MOp I(int64_t v) { MOp o = {MK_IMM, 0, v, NULL}; return o; }
static MOp M(int r, int64_t d) { MOp o = {MK_MEM, (uint8_t)r, d, NULL}; return o; }
static MOp LB(int64_t n) { MOp o = {MK_LAB, 0, n, NULL}; return o; }
static MOp NONE(void) { MOp o = {MK_NONE, 0, 0, NULL}; return o; }

static void em(Gx *g, int op, int w, MOp a, MOp b)
{
    MInst m;
    memset(&m, 0, sizeof m);
    m.op = (uint8_t)op;
    m.w = (uint8_t)w;
    m.a = a;
    m.b = b;
    vec_push(&g->f.code, m);
}

static void em_cc(Gx *g, int op, int cc, MOp a)
{
    MInst m;
    memset(&m, 0, sizeof m);
    m.op = (uint8_t)op;
    m.cc = (uint8_t)cc;
    m.a = a;
    vec_push(&g->f.code, m);
}

/* ---- types ------------------------------------------------------------- */

typedef struct TI {
    int bytes;
    bool sg;
    bool boolean;
    bool flt;                   /* float or double: the value lives in %xmm0 */
} TI;

/* An integer, enum, pointer, float or double type; anything else is outside
 * the subset. */
static bool tinfo(Gx *g, TypeId t, TI *ti)
{
    TypeTable *tt = &g->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok;
    uint64_t sz;
    memset(ti, 0, sizeof *ti);
    if (k == TY_PTR) {
        ti->bytes = 8;
        return true;
    }
    if (k == TY_FLOAT || k == TY_DOUBLE) {
        ti->bytes = k == TY_FLOAT ? 4 : 8;
        ti->sg = true;
        ti->flt = true;
        return true;
    }
    if (!((k >= TY_BOOL && k <= TY_ULLONG) || k == TY_ENUM)) {
        unsup(g, "value of a type outside the integer and float subset k%d", (int)k);
        return false;
    }
    sz = type_size(tt, t, &ok);
    ti->bytes = (int)sz;
    ti->sg = type_is_signed(tt, t);
    ti->boolean = k == TY_BOOL;
    if (!ok || (sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
        unsup(g, "integer of %d bytes", (int)sz);
        return false;
    }
    return true;
}

/* %rax holds some 64-bit value: make it canonical for a type of this kind. */
static void canon(Gx *g, const TI *ti)
{
    if (ti->boolean) {
        em(g, M_TEST, 8, R(R_AX), R(R_AX));
        em_cc(g, M_SETCC, CC_NE, R(R_AX));
        em(g, M_MOVZX, 1, R(R_AX), R(R_AX));
    } else if (ti->bytes < 8) {
        em(g, ti->sg ? M_MOVSX : M_MOVZX, ti->bytes, R(R_AX), R(R_AX));
    }
}

static void push(Gx *g)
{
    em(g, M_PUSH, 8, R(R_AX), NONE());
    g->depth++;
}

static void pop(Gx *g, int r)
{
    em(g, M_POP, 8, R(r), NONE());
    g->depth--;
}

/* The value (in %rax, or %xmm0 for a float) waits on the stack / comes back
 * into the same register. */
static void pushv(Gx *g, const TI *ti)
{
    if (ti->flt)
        em(g, M_MOVQX, 8, R(R_X0), R(R_AX));
    push(g);
}

static void popv(Gx *g, const TI *ti)
{
    pop(g, R_AX);
    if (ti->flt)
        em(g, M_MOVQX, 8, R(R_AX), R(R_X0));
}

/* A float value in %xmm0 becomes 0 or 1 in %rax: nonzero or NaN is true. */
static void flt_truth(Gx *g, const TI *ti)
{
    em(g, M_XOR, 4, R(R_AX), R(R_AX));
    em(g, M_MOVQX, 8, R(R_AX), R(R_X0 + 1));
    em(g, M_FCMP, ti->bytes, R(R_X0 + 1), R(R_X0));
    em_cc(g, M_SETCC, CC_NE, R(R_AX));
    em_cc(g, M_SETCC, CC_P, R(R_CX));
    em(g, M_OR, 1, R(R_CX), R(R_AX));
    em(g, M_MOVZX, 1, R(R_AX), R(R_AX));
}

/* Where a call's or function's next argument goes: a register (R_* or
 * R_X0 + n), or -1 for the stack.  Counters are per class. */
static int arg_loc(const TI *ti, int *ni, int *nf)
{
    if (ti->flt)
        return *nf < 8 ? R_X0 + (*nf)++ : -1;
    return *ni < 6 ? arg_regs[(*ni)++] : -1;
}

/* ---- slots ------------------------------------------------------------- */

static int32_t alloc(Gx *g, uint64_t size, unsigned align)
{
    uint64_t a = align ? align : 1;
    g->frame = (int32_t)(((uint64_t)g->frame + size + a - 1) / a * a);
    return -g->frame;
}

static int32_t var_slot(Gx *g, uint32_t ref, int32_t forced_size)
{
    TypeTable *tt = &g->c->tt;
    size_t i;
    Slot s;
    for (i = 0; i < g->vars.len; i++)
        if (g->vars.data[i].ref == ref)
            return g->vars.data[i].off;
    {
        TypeId t = csym(g->c, ref)->ty;
        bool ok = false;
        uint64_t sz = forced_size ? (uint64_t)forced_size : type_size(tt, t, &ok);
        s.ref = ref;
        s.off = alloc(g, sz ? sz : 1, forced_size ? 8 : type_align(tt, t));
    }
    vec_push(&g->vars, s);
    return s.off;
}

static int32_t *slot_in(Gx *g, int32_t **tab, uint32_t n)
{
    if (!(*tab)[n])
        (*tab)[n] = alloc(g, 8, 8);
    return &(*tab)[n];
}

/* ---- expressions ------------------------------------------------------- */

static void gen(Gx *g, uint32_t n);
static void gen_call(Gx *g, uint32_t n, const char *name);
static void gen_intrin(Gx *g, uint32_t n);
static void gen_va(Gx *g, uint32_t n);
static bool is_agg(Gx *g, TypeId t, uint64_t *size);
static void init_list(Gx *g, uint32_t n, int32_t base);

/* The address of an lvalue node, into %rax. */
static void genaddr(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    if (g->why[0])
        return;
    switch (t->op) {
    case TO_SYM: {
        const CSym *s = csym(g->c, t->sym);
        if ((t->sym & SYM_LOCAL) && s->kind == CS_OBJ && s->sc != SC_STATIC &&
            s->sc != SC_EXTERN) {
            em(g, M_LEA, 8, M(R_BP, var_slot(g, t->sym, 0)), R(R_AX));
        } else if (s->kind == CS_OBJ || s->kind == CS_FUNC) {   /* %rip-relative */
            /* a symbol this unit only declares may live in another object: its address comes from the GOT */
            bool got = s->sc != SC_STATIC && !(s->flags & CSF_DEFINED) &&
                       (s->kind == CS_FUNC || s->sc == SC_EXTERN);
            MOp o = {MK_SYM, got, 0, x86_symname(g->c, g->L->e->fname, t->sym)};
            em(g, got ? M_MOV : M_LEA, 8, o, R(R_AX));
        } else {
            unsup(g, "symbol of kind %d", s->kind);
        }
        break;
    }
    case TO_REG:
    case TO_TMP: {              /* an 8-byte slot: no room for an aggregate */
        uint64_t sz;
        if (is_agg(g, t->ty, &sz)) {
            unsup(g, "aggregate in a register or temporary");
            return;
        }
        em(g, M_LEA, 8, M(R_BP, *slot_in(g, t->op == TO_REG ? &g->regoff : &g->tmpoff,
                                          (uint32_t)t->val)), R(R_AX));
        break;
    }
    case TO_DEREF:
        gen(g, kid(g->L, n, 0));
        break;
    case TO_STR: {              /* the literal's bytes go to read-only data */
        MData lit;
        MOp o = {MK_SYM, 0, 0, NULL};
        char nm[128];
        memset(&lit, 0, sizeof lit);
        snprintf(nm, sizeof nm, ".LC%s_%zu", g->f.name, g->lits.len);
        lit.name = xmalloc(strlen(nm) + 1);
        strcpy(lit.name, nm);
        lit.sec = MD_RODATA;
        lit.align = 1;
        lit.size = t->width;
        lit.bytes = xmalloc(lit.size ? lit.size : 1);
        memcpy(lit.bytes, g->L->e->bytes.data + t->val, lit.size);
        o.name = lit.name;
        vec_push(&g->lits, lit);
        em(g, M_LEA, 8, o, R(R_AX));
        break;
    }
    case TO_FIELD:
        if (t->width) {
            unsup(g, "bit-field");
            return;
        }
        genaddr(g, kid(g->L, n, 0));
        if (t->val)
            em(g, M_ADD, 8, I(t->val), R(R_AX));
        break;
    case TO_COMPLIT: {          /* an object made here, in a slot of its own */
        bool ok = false;
        uint64_t sz = type_size(&g->c->tt, t->ty, &ok);
        int32_t base;
        if (t->pun || !ok) {
            unsup(g, "static compound literal in a function");
            return;
        }
        base = alloc(g, sz ? sz : 1, type_align(&g->c->tt, t->ty));
        init_list(g, n, base);
        em(g, M_LEA, 8, M(R_BP, base), R(R_AX));
        break;
    }
    default:
        unsup(g, "lvalue %d", t->op);
    }
}

/* Load a value of type ti from the address in %rax into %rax. */
static void load_at_rax(Gx *g, const TI *ti)
{
    if (ti->flt)
        em(g, M_FMOV, ti->bytes, M(R_AX, 0), R(R_X0));
    else if (ti->bytes == 8)
        em(g, M_MOV, 8, M(R_AX, 0), R(R_AX));
    else if (ti->bytes == 4 && !ti->sg)
        em(g, M_MOV, 4, M(R_AX, 0), R(R_AX));
    else
        em(g, ti->sg ? M_MOVSX : M_MOVZX, ti->bytes, M(R_AX, 0), R(R_AX));
}

/* Store %rax to the address in %rcx, width of ti. */
static void store_rcx(Gx *g, const TI *ti)
{
    if (ti->flt)
        em(g, M_FMOV, ti->bytes, R(R_X0), M(R_CX, 0));
    else
        em(g, M_MOV, ti->bytes, R(R_AX), M(R_CX, 0));
}

/* ---- aggregates ---------------------------------------------------------
 * A struct, union or array value is its address: evaluating such a node
 * leaves in %rax the address of memory that holds it (a load is the lvalue's
 * address, a store copies and yields the destination). */

static bool is_agg(Gx *g, TypeId t, uint64_t *size)
{
    TypeTable *tt = &g->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok = false;
    uint64_t sz;
    if (k != TY_STRUCT && k != TY_UNION && k != TY_ARRAY)
        return false;
    sz = type_size(tt, t, &ok);
    if (!ok)
        unsup(g, "aggregate of unknown size");
    *size = sz;
    return true;
}

/* Copy n bytes from (%rax) to (%rcx); %rax ends as the destination. */
static void copy_bytes(Gx *g, uint64_t n)
{
    if (n > 64) {
        em(g, M_MOV, 8, R(R_CX), R(R_DX));
        em(g, M_MOV, 8, R(R_AX), R(R_SI));
        em(g, M_MOV, 8, R(R_CX), R(R_DI));
        em(g, M_MOVABS, 8, I((int64_t)n), R(R_CX));
        em(g, M_REPMOVSB, 0, NONE(), NONE());
        em(g, M_MOV, 8, R(R_DX), R(R_AX));
    } else {
        uint64_t off = 0;
        int w;
        for (; off < n; off += (uint64_t)w) {
            w = n - off >= 8 ? 8 : n - off >= 4 ? 4 : n - off >= 2 ? 2 : 1;
            em(g, M_MOV, w, M(R_AX, (int64_t)off), R(R_DX));
            em(g, M_MOV, w, R(R_DX), M(R_CX, (int64_t)off));
        }
        em(g, M_MOV, 8, R(R_CX), R(R_AX));
    }
}

/* Zero n bytes of the frame at rbp+off; clobbers %rax, %rcx, %rdx, %rdi. */
static void zero_frame(Gx *g, int32_t off, uint64_t n)
{
    if (n > 64) {
        em(g, M_LEA, 8, M(R_BP, off), R(R_DI));
        em(g, M_XOR, 4, R(R_AX), R(R_AX));
        em(g, M_MOVABS, 8, I((int64_t)n), R(R_CX));
        em(g, M_REPSTOSB, 0, NONE(), NONE());
    } else {
        uint64_t at = 0;
        int w;
        em(g, M_XOR, 4, R(R_DX), R(R_DX));
        for (; at < n; at += (uint64_t)w) {
            w = n - at >= 8 ? 8 : n - at >= 4 ? 4 : n - at >= 2 ? 2 : 1;
            em(g, M_MOV, w, R(R_DX), M(R_BP, off + (int64_t)at));
        }
    }
}

/* A local's initializer list: the object is zeroed, then each element is
 * stored in source order. */
/* The elements of node n (a local or a compound literal) zeroed into the frame at base. */
static void init_list(Gx *g, uint32_t n, int32_t base)
{
    const TNode *t = nd(g->L, n);
    uint64_t size = 0;
    uint32_t i;
    bool ok = false;
    size = type_size(&g->c->tt, t->ty, &ok);
    if (!ok) {
        unsup(g, "initializer list for a type of unknown size");
        return;
    }
    zero_frame(g, base, size);
    for (i = 0; i < t->nkids && !g->why[0]; i++) {
        uint32_t k = kid(g->L, n, i), v = kid(g->L, k, 0);
        const TNode *e = nd(g->L, k);
        uint64_t esz = 0;
        bool agg;
        TI ti;
        if (e->width) {
            unsup(g, "bit-field initializer");
            return;
        }
        agg = is_agg(g, e->ty, &esz);
        if (agg) {                      /* a string cut to its array, or a struct value */
            const TNode *s = nd(g->L, v);
            if (s->op == TO_STR && s->width < esz)
                esz = s->width;
            if (s->op == TO_STR)
                genaddr(g, v);
            else
                gen(g, v);
        } else {
            if (!tinfo(g, e->ty, &ti))
                return;
            gen(g, v);
        }
        em(g, M_LEA, 8, M(R_BP, base + (int32_t)e->val), R(R_CX));
        if (agg)
            copy_bytes(g, esz);
        else
            store_rcx(g, &ti);
    }
}

static void local_list(Gx *g, uint32_t n)
{
    init_list(g, n, var_slot(g, nd(g->L, n)->sym, 0));
}

static int cc_of(int p, bool sg)
{
    switch (p) {
    case P_LT: return sg ? CC_L : CC_B;
    case P_GT: return sg ? CC_G : CC_A;
    case P_LE: return sg ? CC_LE : CC_BE;
    case P_GE: return sg ? CC_GE : CC_AE;
    case P_EQEQ: return CC_E;
    default: return CC_NE;
    }
}

static void gen_binary(Gx *g, uint32_t n)
{
    TI lt;
    if (!tinfo(g, nd(g->L, kid(g->L, n, 0))->ty, &lt))
        return;
    gen(g, kid(g->L, n, 0));
    pushv(g, &lt);
    gen(g, kid(g->L, n, 1));
    if (lt.flt)                 /* right in %xmm1, left back in %xmm0 */
        em(g, M_MOVAPS, 8, R(R_X0), R(R_X0 + 1));
    else
        em(g, M_MOV, 8, R(R_AX), R(R_CX));
    popv(g, &lt);
}

/* ---- System V classes of an aggregate ------------------------------------
 * Up to 16 bytes it travels as eightbytes, each INTEGER ('i') or SSE ('s');
 * anything larger (or packed) goes in memory. */


static void cls_mark(AggCls *a, uint64_t off, char k)
{
    if (off / 8 < 2 && a->c[off / 8] != 'i')
        a->c[off / 8] = k;
}

static bool cls_walk(Gx *g, TypeId t, uint64_t base, AggCls *a)
{
    TypeTable *tt = &g->c->tt;
    TypeId ct = type_canon(tt, t);
    TypeKind k = type_ckind(tt, ct);
    TI ti;
    if (k == TY_STRUCT || k == TY_UNION) {
        const Record *r = type_record(tt, ct);
        uint32_t i;
        if (r->flags & RF_PACKED) {
            a->mem = true;
            return true;
        }
        for (i = 0; i < r->nfields; i++) {
            const Field *f = &tt->fields.data[r->fields + i];
            if (f->flags & FF_PACKED) {
                a->mem = true;
                return true;
            }
            if (f->flags & FF_BITFIELD)
                cls_mark(a, base + f->off_bits / 8, 'i');
            else if (!cls_walk(g, f->ty, base + f->off_bits / 8, a))
                return false;
        }
        return true;
    }
    if (k == TY_ARRAY) {
        bool ok = false;
        uint64_t n = type_ent(tt, ct)->n, i, esz = type_size(tt, type_base(tt, ct), &ok);
        for (i = 0; ok && i < n && base + i * esz < a->size; i++)
            if (!cls_walk(g, type_base(tt, ct), base + i * esz, a))
                return false;
        return true;
    }
    if (!tinfo(g, ct, &ti))
        return false;
    cls_mark(a, base, ti.flt ? 's' : 'i');
    return true;
}

static int piece_w(const AggCls *a, int p);

static bool agg_class(Gx *g, TypeId t, AggCls *a)
{
    bool ok = false;
    int p;
    memset(a, 0, sizeof *a);
    a->size = type_size(&g->c->tt, t, &ok);
    a->np = (int)((a->size + 7) / 8);
    a->mem = a->size > 16;
    if (!a->mem && !cls_walk(g, t, 0, a))
        return false;
    for (p = 0; p < 2; p++)
        if (!a->c[p])
            a->c[p] = 'i';
    if (!a->mem)
        for (p = 0; p < a->np; p++) {
            int w = piece_w(a, p);
            if (w != 8 && w != 4 && w != 2 && w != 1) {
                unsup(g, "aggregate of %d bytes in registers", (int)a->size);
                return false;
            }
        }
    return true;
}

static int piece_w(const AggCls *a, int p)
{
    uint64_t w = a->size - 8 * (uint64_t)p;
    return w > 8 ? 8 : (int)w;
}

/* A zero-extended piece of w bytes at off(base) into register dst. */
static void load_piece(Gx *g, int w, int64_t off, int base, int dst)
{
    em(g, w >= 4 ? M_MOV : M_MOVZX, w, M(base, off), R(dst));
}

/* A call: name is the symbol, or NULL for a call through the pointer in kid 0. */
typedef struct Arg {
    uint32_t node;              /* what is evaluated: the argument, or what a transparent union wraps */
    TI ti;
    AggCls ac;
    bool agg;
    int np;                     /* registers used; 0: on the stack */
    int loc[2];
    uint32_t words;             /* stack words when on the stack */
} Arg;

static void gen_call(Gx *g, uint32_t n, const char *name)
{
    uint32_t j, b0 = name ? 0 : 1, nstack = 0;
    int ni = 0, nf = 0, pad, p;
    Arg *as;
    const TNode *t = nd(g->L, n);
    AggCls rc;
    uint64_t sz;
    bool ragg = is_agg(g, t->ty, &sz);
    int32_t rtmp = 0;
    TI ti;
    if (g->why[0])
        return;
    if (ragg) {
        if (!agg_class(g, t->ty, &rc))
            return;
        rtmp = alloc(g, rc.size ? rc.size : 1, 16);
        if (rc.mem)
            ni = 1;                         /* %rdi carries the result's address */
    }
    as = xcalloc(t->nkids + 1, sizeof *as);
    for (j = b0; j < t->nkids; j++) {
        Arg *a = &as[j];
        TypeId at;
        a->node = kid(g->L, n, j);
        if (nd(g->L, a->node)->op == TO_CAST) {   /* (transparent union)p passes as p */
            TypeTable *tt = &g->c->tt;
            TypeId ut = type_canon(tt, nd(g->L, a->node)->ty);
            if (type_ckind(tt, ut) == TY_UNION && (type_record(tt, ut)->flags & RF_TRANSPARENT))
                a->node = kid(g->L, a->node, 0);
        }
        at = nd(g->L, a->node)->ty;
        a->agg = is_agg(g, at, &sz);
        if (g->why[0])
            goto out;
        if (!a->agg) {
            if (!tinfo(g, at, &a->ti))
                goto out;
            a->loc[0] = arg_loc(&a->ti, &ni, &nf);
            a->np = a->loc[0] >= 0;
            a->words = !a->np;
        } else {
            if (!agg_class(g, at, &a->ac))
                goto out;
            if (!a->ac.mem) {
                int need_i = 0, need_f = 0;
                for (p = 0; p < a->ac.np; p++)
                    if (a->ac.c[p] == 'i')
                        need_i++;
                    else
                        need_f++;
                if (ni + need_i <= 6 && nf + need_f <= 8) {
                    a->np = a->ac.np;
                    for (p = 0; p < a->np; p++)
                        a->loc[p] = a->ac.c[p] == 'i' ? arg_regs[ni++] : R_X0 + nf++;
                }
            }
            if (!a->np)
                a->words = (uint32_t)((a->ac.size + 7) / 8);
        }
        nstack += a->words;
    }
    /* Stack arguments are pushed first (last to first), then the register
     * ones, which pop off again; the padding goes below the stack
     * arguments so %rsp is 16-aligned at the call. */
    pad = (g->depth + (int)nstack) & 1;
    if (pad) {
        em(g, M_SUB, 8, I(8), R(R_SP));
        g->depth++;
    }
    for (j = t->nkids; j-- > b0;)
        if (!as[j].np && as[j].words) {
            gen(g, as[j].node);
            if (as[j].agg) {
                em(g, M_SUB, 8, I(8 * (int64_t)as[j].words), R(R_SP));
                g->depth += (int)as[j].words;
                em(g, M_MOV, 8, R(R_SP), R(R_CX));
                copy_bytes(g, as[j].ac.size);
            } else {
                pushv(g, &as[j].ti);
            }
        }
    for (j = t->nkids; j-- > b0;)
        if (as[j].np) {
            gen(g, as[j].node);
            if (as[j].agg) {
                for (p = as[j].np; p-- > 0;) {
                    load_piece(g, piece_w(&as[j].ac, p), 8 * p, R_AX, R_CX);
                    em(g, M_PUSH, 8, R(R_CX), NONE());
                    g->depth++;
                }
            } else {
                pushv(g, &as[j].ti);
            }
        }
    if (b0) {               /* the callee's address, after the arguments are safe on the stack */
        gen(g, kid(g->L, n, 0));
        em(g, M_MOV, 8, R(R_AX), R(R_R11));
    }
    for (j = b0; j < t->nkids; j++)
        for (p = 0; p < as[j].np; p++) {
            if (as[j].loc[p] >= R_X0) {
                pop(g, R_AX);
                em(g, M_MOVQX, 8, R(R_AX), R(as[j].loc[p]));
            } else {
                pop(g, as[j].loc[p]);
            }
        }
    if (ragg && rc.mem)
        em(g, M_LEA, 8, M(R_BP, rtmp), R(R_DI));
    em(g, M_MOV, 4, I(nf), R(R_AX));    /* vector registers used, for variadic callees */
    {
        MOp callee = b0 ? R(R_R11) : (MOp){MK_SYM, 0, 0, name};
        em(g, M_CALL, 8, callee, NONE());
    }
    if (nstack + pad) {
        em(g, M_ADD, 8, I(8 * (nstack + pad)), R(R_SP));
        g->depth -= (int)(nstack + pad);
    }
    if (ragg) {
        if (!rc.mem) {              /* %rax, %rdx, %xmm0, %xmm1 into the result's slot */
            int ii = 0, ss = 0;
            em(g, M_LEA, 8, M(R_BP, rtmp), R(R_CX));
            for (p = 0; p < rc.np; p++) {
                int w = piece_w(&rc, p);
                if (rc.c[p] == 'i')
                    em(g, M_MOV, w, R(ii++ ? R_DX : R_AX), M(R_CX, 8 * p));
                else
                    em(g, M_FMOV, w == 4 ? 4 : 8, R(R_X0 + ss++), M(R_CX, 8 * p));
            }
            em(g, M_LEA, 8, M(R_BP, rtmp), R(R_AX));
        }
    } else if (type_ckind(&g->c->tt, t->ty) != TY_VOID) {
        if (!tinfo(g, t->ty, &ti))
            goto out;
        if (!ti.flt)            /* a float came back in %xmm0 */
            canon(g, &ti);
    }
out:
    free(as);
}

/* The aggregate value at (%rax) leaves a function: into the caller's memory
 * (the address saved at entry), or in %rax, %rdx, %xmm0, %xmm1. */
static void ret_agg(Gx *g, const AggCls *rc, int32_t retptr)
{
    if (rc->mem) {
        em(g, M_MOV, 8, M(R_BP, retptr), R(R_CX));
        copy_bytes(g, rc->size);
    } else {
        int ii = 0, ss = 0, p;
        em(g, M_MOV, 8, R(R_AX), R(R_CX));
        for (p = 0; p < rc->np; p++) {
            int w = piece_w(rc, p);
            if (rc->c[p] == 'i')
                load_piece(g, w, 8 * p, R_CX, ii++ ? R_DX : R_AX);
            else
                em(g, M_FMOV, w == 4 ? 4 : 8, M(R_CX, 8 * p), R(R_X0 + ss++));
        }
    }
}

static void gen(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    TI ti;
    uint64_t asz;
    if (g->why[0])
        return;
    switch (t->op) {
    case TO_CONST:
        if (!tinfo(g, t->ty, &ti))
            return;
        em(g, M_MOVABS, 8, I(t->val), R(R_AX));
        break;
    case TO_FCONST: {           /* the bytes, little-endian, through %rax */
        uint64_t bits = 0;
        uint32_t b;
        if (!tinfo(g, t->ty, &ti))
            return;
        for (b = t->width; b-- > 0;)
            bits = bits << 8 | g->L->e->bytes.data[t->val + b];
        em(g, M_MOVABS, 8, I((int64_t)bits), R(R_AX));
        em(g, M_MOVQX, ti.bytes, R(R_AX), R(R_X0));
        break;
    }
    case TO_LOAD:
        if (is_agg(g, t->ty, &asz)) {
            genaddr(g, kid(g->L, n, 0));
            break;
        }
        if (!tinfo(g, t->ty, &ti))
            return;
        genaddr(g, kid(g->L, n, 0));
        load_at_rax(g, &ti);
        break;
    case TO_STORE:
        if (is_agg(g, t->ty, &asz)) {
            genaddr(g, kid(g->L, n, 0));
            push(g);
            gen(g, kid(g->L, n, 1));
            pop(g, R_CX);
            copy_bytes(g, asz);
            break;
        }
        if (!tinfo(g, t->ty, &ti))
            return;
        genaddr(g, kid(g->L, n, 0));
        push(g);
        gen(g, kid(g->L, n, 1));
        pop(g, R_CX);
        store_rcx(g, &ti);
        if (!ti.flt)
            canon(g, &ti);
        break;
    case TO_POST: {             /* the value before the store it wraps */
        uint32_t s = kid(g->L, n, 0);
        if (nd(g->L, s)->op != TO_STORE || !tinfo(g, nd(g->L, s)->ty, &ti))
            return;
        genaddr(g, kid(g->L, s, 0));
        load_at_rax(g, &ti);
        pushv(g, &ti);
        gen(g, s);
        popv(g, &ti);
        break;
    }
    case TO_CAST: {
        TI from;
        gen(g, kid(g->L, n, 0));
        if (type_ckind(&g->c->tt, t->ty) == TY_VOID)
            break;
        if (!tinfo(g, t->ty, &ti) || !tinfo(g, nd(g->L, kid(g->L, n, 0))->ty, &from))
            return;
        if (from.flt && ti.flt) {
            if (from.bytes != ti.bytes)
                em(g, M_CVTF2F, from.bytes, R(R_X0), R(R_X0));
        } else if (ti.flt) {
            if (!from.sg && from.bytes == 8) {  /* at 2^63 and up: halve, keep the low bit, convert, double */
                int big = g->nlab++, done = g->nlab++;
                em(g, M_CMP, 8, I(0), R(R_AX));
                em_cc(g, M_JCC, CC_L, LB(big));
                em(g, M_CVTI2F, ti.bytes, R(R_AX), R(R_X0));
                em(g, M_JMP, 8, LB(done), NONE());
                em(g, M_LABEL, 8, LB(big), NONE());
                em(g, M_MOV, 8, R(R_AX), R(R_DX));
                em(g, M_AND, 8, I(1), R(R_DX));
                em(g, M_MOV, 8, I(1), R(R_CX));
                em(g, M_SHR, 8, NONE(), R(R_AX));
                em(g, M_OR, 8, R(R_DX), R(R_AX));
                em(g, M_CVTI2F, ti.bytes, R(R_AX), R(R_X0));
                em(g, M_FADD, ti.bytes, R(R_X0), R(R_X0));
                em(g, M_LABEL, 8, LB(done), NONE());
            } else {
                em(g, M_CVTI2F, ti.bytes, R(R_AX), R(R_X0));
            }
        } else if (from.flt) {
            if (ti.boolean) {
                flt_truth(g, &from);
            } else if (!ti.sg && ti.bytes == 8) {       /* at 2^63 and up: subtract it, convert, set the top bit */
                int big = g->nlab++, done = g->nlab++;
                em(g, M_MOVABS, 8, I(from.bytes == 4 ? 0x5f000000LL : 0x43e0000000000000LL), R(R_AX));
                em(g, M_MOVQX, from.bytes, R(R_AX), R(R_X0 + 1));
                em(g, M_FCMP, from.bytes, R(R_X0 + 1), R(R_X0));
                em_cc(g, M_JCC, CC_AE, LB(big));
                em(g, M_CVTF2I, from.bytes, R(R_X0), R(R_AX));
                em(g, M_JMP, 8, LB(done), NONE());
                em(g, M_LABEL, 8, LB(big), NONE());
                em(g, M_FSUB, from.bytes, R(R_X0 + 1), R(R_X0));
                em(g, M_CVTF2I, from.bytes, R(R_X0), R(R_AX));
                em(g, M_MOVABS, 8, I(INT64_MIN), R(R_CX));
                em(g, M_XOR, 8, R(R_CX), R(R_AX));
                em(g, M_LABEL, 8, LB(done), NONE());
            } else {
                em(g, M_CVTF2I, from.bytes, R(R_X0), R(R_AX));
                canon(g, &ti);
            }
        } else {
            canon(g, &ti);
        }
        break;
    }
    case TO_UNOP:
        gen(g, kid(g->L, n, 0));
        if (!tinfo(g, t->ty, &ti))
            return;
        if (t->pun == P_BANG) {
            TI ot;              /* the operand's own class decides how to test it */
            if (!tinfo(g, nd(g->L, kid(g->L, n, 0))->ty, &ot))
                return;
            if (ot.flt) {
                flt_truth(g, &ot);
                em(g, M_XOR, 8, I(1), R(R_AX));
                break;
            }
        }
        if (ti.flt) {
            if (t->pun == P_MINUS) {    /* flip the sign bit through %rax */
                em(g, M_MOVQX, ti.bytes, R(R_X0), R(R_AX));
                em(g, M_MOVABS, 8, I(ti.bytes == 4 ? 0x80000000LL : INT64_MIN), R(R_CX));
                em(g, M_XOR, 8, R(R_CX), R(R_AX));
                em(g, M_MOVQX, ti.bytes, R(R_AX), R(R_X0));
            } else if (t->pun != P_PLUS) {
                unsup(g, "unary operator on a float");
            }
            break;
        }
        if (t->pun == P_MINUS)
            em(g, M_NEG, 8, R(R_AX), NONE());
        else if (t->pun == P_TILDE)
            em(g, M_NOT, 8, R(R_AX), NONE());
        else if (t->pun == P_BANG) {
            em(g, M_TEST, 8, R(R_AX), R(R_AX));
            em_cc(g, M_SETCC, CC_E, R(R_AX));
            em(g, M_MOVZX, 1, R(R_AX), R(R_AX));
        } else if (t->pun != P_PLUS) {
            unsup(g, "unary operator");
            return;
        }
        canon(g, &ti);
        break;
    case TO_BINOP: {
        gen_binary(g, n);
        if (!tinfo(g, t->ty, &ti))
            return;
        if (ti.flt) {
            int op = t->pun == P_PLUS ? M_FADD : t->pun == P_MINUS ? M_FSUB :
                     t->pun == P_STAR ? M_FMUL : t->pun == P_SLASH ? M_FDIV : -1;
            if (op < 0)
                unsup(g, "binary operator on floats");
            else
                em(g, op, ti.bytes, R(R_X0 + 1), R(R_X0));
            break;
        }
        switch (t->pun) {
        case P_PLUS: em(g, M_ADD, 8, R(R_CX), R(R_AX)); break;
        case P_MINUS: em(g, M_SUB, 8, R(R_CX), R(R_AX)); break;
        case P_STAR: em(g, M_IMUL, 8, R(R_CX), R(R_AX)); break;
        case P_AMP: em(g, M_AND, 8, R(R_CX), R(R_AX)); break;
        case P_PIPE: em(g, M_OR, 8, R(R_CX), R(R_AX)); break;
        case P_CARET: em(g, M_XOR, 8, R(R_CX), R(R_AX)); break;
        case P_SHL: em(g, M_SHL, 8, NONE(), R(R_AX)); break;
        case P_SHR: em(g, ti.sg ? M_SAR : M_SHR, 8, NONE(), R(R_AX)); break;
        case P_SLASH: case P_PERCENT:
            if (ti.sg) {
                em(g, M_CQO, 8, NONE(), NONE());
                em(g, M_IDIV, 8, R(R_CX), NONE());
            } else {
                em(g, M_XOR, 4, R(R_DX), R(R_DX));
                em(g, M_DIV, 8, R(R_CX), NONE());
            }
            if (t->pun == P_PERCENT)
                em(g, M_MOV, 8, R(R_DX), R(R_AX));
            break;
        default:
            unsup(g, "binary operator");
            return;
        }
        canon(g, &ti);
        break;
    }
    case TO_CMP: {
        TI ot;
        if (!tinfo(g, nd(g->L, kid(g->L, n, 0))->ty, &ot))
            return;
        gen_binary(g, n);
        if (ot.flt) {           /* left in %xmm0, right in %xmm1 */
            int p = t->pun;
            if (p == P_LT || p == P_LE)     /* x < y is y > x: compare the other way */
                em(g, M_FCMP, ot.bytes, R(R_X0), R(R_X0 + 1));
            else
                em(g, M_FCMP, ot.bytes, R(R_X0 + 1), R(R_X0));
            if (p == P_EQEQ || p == P_NE) {     /* unordered sets ZF too: test PF */
                em_cc(g, M_SETCC, p == P_EQEQ ? CC_E : CC_NE, R(R_AX));
                em_cc(g, M_SETCC, p == P_EQEQ ? CC_NP : CC_P, R(R_CX));
                em(g, p == P_EQEQ ? M_AND : M_OR, 1, R(R_CX), R(R_AX));
            } else {
                em_cc(g, M_SETCC, p == P_LT || p == P_GT ? CC_A : CC_AE, R(R_AX));
            }
        } else {
            em(g, M_CMP, 8, R(R_CX), R(R_AX));
            em_cc(g, M_SETCC, cc_of(t->pun, ot.sg), R(R_AX));
        }
        em(g, M_MOVZX, 1, R(R_AX), R(R_AX));
        break;
    }
    case TO_COMMA:
        gen(g, kid(g->L, n, 0));
        gen(g, kid(g->L, n, 1));
        break;
    case TO_PTRADD:
        gen(g, kid(g->L, n, 0));
        push(g);
        gen(g, kid(g->L, n, 1));
        em(g, M_IMUL, 8, I(t->val), R(R_AX));
        pop(g, R_CX);
        em(g, M_ADD, 8, R(R_CX), R(R_AX));
        break;
    case TO_PTRDIFF:
        gen_binary(g, n);
        em(g, M_SUB, 8, R(R_CX), R(R_AX));
        em(g, M_CQO, 8, NONE(), NONE());
        em(g, M_MOV, 8, I(t->val), R(R_CX));
        em(g, M_IDIV, 8, R(R_CX), NONE());
        break;
    case TO_ADDR:
        genaddr(g, kid(g->L, n, 0));
        break;
    case TO_CALL:
        gen_call(g, n, t->pun ? NULL : cident(g->c, csym(g->c, t->sym)->name));
        break;
    case TO_INTRIN:
        gen_intrin(g, n);
        break;
    case TO_VA:
        gen_va(g, n);
        break;
    case TO_LABADDR: {          /* a label's address: %rip-relative */
        size_t q;
        for (q = 0; q < g->L->labs.len; q++)
            if (g->L->labs.data[q].id == (uint32_t)t->val) {
                em(g, M_LEA, 8, LB(g->L->b.data[g->L->labs.data[q].blk].pos), R(R_AX));
                return;
            }
        unsup(g, "address of an unknown label");
        break;
    }
    default:
        unsup(g, "expression %d", t->op);
    }
}

/* va_start / va_end / va_copy / va_arg on the System V va_list (a 24-byte
 * record: gp_offset, fp_offset, overflow_arg_area, reg_save_area).  A
 * variadic function saves all argument registers at entry. */
static void gen_va(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    uint64_t sz = 0;
    TI ti;
    AggCls ac = {0};
    bool agg, flt = false;
    uint32_t over, done;
    int64_t adv = 8;
    switch (t->pun) {
    case 1:                     /* start */
        if (!g->variadic) {
            unsup(g, "va_start outside a variadic function");
            return;
        }
        gen(g, kid(g->L, n, 0));
        em(g, M_MOV, 4, I(g->va_gp), R(R_CX));
        em(g, M_MOV, 4, R(R_CX), M(R_AX, 0));
        em(g, M_MOV, 4, I(g->va_fp), R(R_CX));
        em(g, M_MOV, 4, R(R_CX), M(R_AX, 4));
        em(g, M_LEA, 8, M(R_BP, g->va_stk), R(R_CX));
        em(g, M_MOV, 8, R(R_CX), M(R_AX, 8));
        em(g, M_LEA, 8, M(R_BP, g->save), R(R_CX));
        em(g, M_MOV, 8, R(R_CX), M(R_AX, 16));
        return;
    case 2:                     /* end */
        gen(g, kid(g->L, n, 0));
        return;
    case 3:                     /* copy */
        gen(g, kid(g->L, n, 0));
        push(g);
        gen(g, kid(g->L, n, 1));
        pop(g, R_CX);
        copy_bytes(g, 24);
        return;
    default:
        break;
    }
    agg = is_agg(g, t->ty, &sz);
    if (agg) {
        if (!agg_class(g, t->ty, &ac))
            return;
        adv = (int64_t)((sz + 7) / 8 * 8);
    } else {
        if (!tinfo(g, t->ty, &ti))
            return;
        flt = ti.flt;
    }
    gen(g, kid(g->L, n, 0));    /* %rax: the va_list; the argument's address ends in %rdx */
    over = g->nlab++;
    done = g->nlab++;
    if (!agg) {
        em(g, M_MOV, 4, M(R_AX, flt ? 4 : 0), R(R_CX));
        em(g, M_CMP, 4, I(flt ? 160 : 40), R(R_CX));
        em_cc(g, M_JCC, CC_A, LB(over));
        em(g, M_MOV, 8, M(R_AX, 16), R(R_DX));
        em(g, M_ADD, 8, R(R_CX), R(R_DX));
        em(g, M_ADD, 4, I(flt ? 16 : 8), R(R_CX));
        em(g, M_MOV, 4, R(R_CX), M(R_AX, flt ? 4 : 0));
        em(g, M_JMP, 8, LB(done), NONE());
        em(g, M_LABEL, 8, LB(over), NONE());
    } else if (!ac.mem) {       /* the eightbytes come from the register save area, into a slot of ours */
        int ng = 0, nf = 0, p;
        int32_t tmp;
        for (p = 0; p < ac.np; p++)
            if (ac.c[p] == 'i')
                ng++;
            else
                nf++;
        tmp = alloc(g, (uint64_t)ac.np * 8, 8);
        if (ng) {
            em(g, M_CMP, 4, I(48 - 8 * ng), M(R_AX, 0));
            em_cc(g, M_JCC, CC_A, LB(over));
        }
        if (nf) {
            em(g, M_CMP, 4, I(176 - 16 * nf), M(R_AX, 4));
            em_cc(g, M_JCC, CC_A, LB(over));
        }
        for (p = 0; p < ac.np; p++) {
            bool gp = ac.c[p] == 'i';
            em(g, M_MOV, 4, M(R_AX, gp ? 0 : 4), R(R_CX));
            em(g, M_MOV, 8, M(R_AX, 16), R(R_DX));
            em(g, M_ADD, 8, R(R_CX), R(R_DX));
            em(g, M_MOV, 8, M(R_DX, 0), R(R_DX));
            em(g, M_MOV, 8, R(R_DX), M(R_BP, tmp + 8 * p));
            em(g, M_ADD, 4, I(gp ? 8 : 16), R(R_CX));
            em(g, M_MOV, 4, R(R_CX), M(R_AX, gp ? 0 : 4));
        }
        em(g, M_LEA, 8, M(R_BP, tmp), R(R_DX));
        em(g, M_JMP, 8, LB(done), NONE());
        em(g, M_LABEL, 8, LB(over), NONE());
    }
    em(g, M_MOV, 8, M(R_AX, 8), R(R_DX));
    em(g, M_LEA, 8, M(R_DX, adv), R(R_CX));
    em(g, M_MOV, 8, R(R_CX), M(R_AX, 8));
    em(g, M_LABEL, 8, LB(done), NONE());
    if (agg)
        em(g, M_MOV, 8, R(R_DX), R(R_AX));
    else if (flt)
        em(g, M_FMOV, ti.bytes, M(R_DX, 0), R(R_X0));
    else {
        em(g, M_MOV, 8, R(R_DX), R(R_AX));
        load_at_rax(g, &ti);
    }
}

/* Built-ins: values (__builtin_expect, infinity), libc calls, and the
 * __atomic_ operations on objects of 1, 2, 4 or 8 bytes (the order argument
 * is ignored: loads are plain moves, stores and exchanges are xchg). */
static bool atomic_width(Gx *g, uint32_t ptr, TI *raw)
{
    TypeTable *tt = &g->c->tt;
    TypeId pt = type_canon(tt, nd(g->L, ptr)->ty);
    bool ok = false;
    uint64_t sz;
    memset(raw, 0, sizeof *raw);
    if (type_ckind(tt, pt) != TY_PTR) {
        unsup(g, "atomic operand is not a pointer");
        return false;
    }
    sz = type_size(tt, type_base(tt, pt), &ok);
    if (!ok || (sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
        unsup(g, "atomic object of %d bytes", (int)sz);
        return false;
    }
    raw->bytes = (int)sz;
    return true;
}

static void gen_intrin(Gx *g, uint32_t n)
{
    static const char *const libc[] = {"memset", "memcpy", "memmove", "memcmp", "strlen", "strcmp",
                                       "strncmp", "strcpy", "strncpy", "strcat", "strncat",
                                       "strchr", "strrchr", "strstr", "abs", "labs", "llabs"};
    const TNode *t = nd(g->L, n);
    const char *nm = cident(g->c, t->sym);
    TI raw, ti;
    size_t i;
    if (t->width == 2) {
        const char *op = strncmp(nm, "__atomic_", 9) ? "" : nm + 9;
        if (!strcmp(op, "load") || !strcmp(op, "store") || !strcmp(op, "exchange")) {
            bool ex = !strcmp(op, "exchange"), ld = !strcmp(op, "load");
            if (!atomic_width(g, kid(g->L, n, 0), &raw))
                return;
            if (ex || ld) {             /* where the old value goes */
                gen(g, kid(g->L, n, ex ? 2 : 1));
                push(g);
            }
            gen(g, kid(g->L, n, 0));
            if (ld) {
                load_at_rax(g, &raw);
                pop(g, R_CX);
                store_rcx(g, &raw);
                return;
            }
            push(g);
            gen(g, kid(g->L, n, 1));
            load_at_rax(g, &raw);
            pop(g, R_CX);
            em(g, M_XCHG, raw.bytes, R(R_AX), M(R_CX, 0));
            if (ex) {
                pop(g, R_CX);
                store_rcx(g, &raw);
            }
        } else if (!strcmp(op, "fetch_add") || !strcmp(op, "fetch_sub")) {
            if (!atomic_width(g, kid(g->L, n, 0), &raw) || !tinfo(g, t->ty, &ti))
                return;
            gen(g, kid(g->L, n, 0));
            push(g);
            gen(g, kid(g->L, n, 1));
            if (op[6] == 's')
                em(g, M_NEG, 8, R(R_AX), NONE());
            pop(g, R_CX);
            em(g, M_XADD, raw.bytes, R(R_AX), M(R_CX, 0));
            canon(g, &ti);
        } else if (!strcmp(op, "compare_exchange") || !strcmp(op, "compare_exchange_n")) {
            bool by_ptr = op[16] == 0;    /* the desired value comes by address */
            int ok = g->nlab++;
            if (!atomic_width(g, kid(g->L, n, 0), &raw))
                return;
            gen(g, kid(g->L, n, 1));    /* where the expected value is, and the old one goes on failure */
            push(g);
            gen(g, kid(g->L, n, 0));
            push(g);
            gen(g, kid(g->L, n, 2));
            if (by_ptr)
                load_at_rax(g, &raw);
            em(g, M_MOV, 8, R(R_AX), R(R_DX));
            pop(g, R_CX);
            pop(g, R_SI);
            em(g, M_MOV, raw.bytes, M(R_SI, 0), R(R_AX));
            em(g, M_CMPXCHG, raw.bytes, R(R_DX), M(R_CX, 0));
            em_cc(g, M_JCC, CC_E, LB(ok));
            em(g, M_MOV, raw.bytes, R(R_AX), M(R_SI, 0));
            em(g, M_LABEL, 8, LB(ok), NONE());
            em_cc(g, M_SETCC, CC_E, R(R_AX));
            em(g, M_MOVZX, 1, R(R_AX), R(R_AX));
        } else {
            unsup(g, "atomic built-in %s", nm);
        }
        return;
    }
    if (strncmp(nm, "__builtin_", 10)) {
        unsup(g, "built-in %s", nm);
        return;
    }
    nm += 10;
    if (!strcmp(nm, "expect") && nd(g->L, kid(g->L, n, 1))->op == TO_CONST) {
        gen(g, kid(g->L, n, 0));
        return;
    }
    if (!strcmp(nm, "huge_val") || !strcmp(nm, "inf") || !strcmp(nm, "huge_valf") ||
        !strcmp(nm, "inff")) {
        bool f = nm[strlen(nm) - 1] == 'f';
        em(g, M_MOVABS, 8, I(f ? 0x7f800000LL : 0x7ff0000000000000LL), R(R_AX));
        em(g, M_MOVQX, f ? 4 : 8, R(R_AX), R(R_X0));
        return;
    }
    for (i = 0; i < sizeof libc / sizeof *libc; i++)
        if (!strcmp(nm, libc[i])) {
            gen_call(g, n, libc[i]);
            return;
        }
    unsup(g, "built-in %s", nm);
}

/* ---- statements, blocks ------------------------------------------------ */

static void gen_stmt(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    TI ti;
    uint64_t asz;
    if (g->why[0])
        return;
    switch (t->op) {
    case TO_EXPRSTMT:
        gen(g, kid(g->L, n, 0));
        break;
    case TO_ASM: {              /* no assembler: the instructions without operands this back end knows */
        static const struct { const char *text; int n; int64_t bytes; } known[] = {
            {"rep; nop", 2, 0x90f3}, {"pause", 2, 0x90f3}, {"nop", 1, 0x90},
            {"mfence", 3, 0xf0ae0f}, {"lfence", 3, 0xe8ae0f}, {"sfence", 3, 0xf8ae0f}};
        const TNode *s = nd(g->L, kid(g->L, n, 0));
        char txt[64];
        size_t len = s->width ? s->width - 1 : 0, b = 0, e = len, q;
        const uint8_t *p = g->L->e->bytes.data + s->val;
        while (b < e && isspace(p[b]))
            b++;
        while (e > b && isspace(p[e - 1]))
            e--;
        if (e == b)
            break;              /* "": a compiler barrier */
        snprintf(txt, sizeof txt, "%.*s", (int)(e - b < 60 ? e - b : 60), (const char *)p + b);
        for (q = 0; q < sizeof known / sizeof *known; q++)
            if (!strcmp(txt, known[q].text)) {
                MOp o = {MK_SYM, 0, known[q].bytes, known[q].text};
                em(g, M_BYTES, known[q].n, o, NONE());
                return;
            }
        unsup(g, "asm template \"%s\"", txt);
        break;
    }
    case TO_DATA: {             /* a block-scope static: defined in the object, no code */
        char why[96];
        if (!x86_data_node(g->L->e, n, why, sizeof why))
            unsup(g, "static object: %s", why);
        break;
    }
    case TO_LOCAL:
        if (t->nkids == 0) {
            var_slot(g, t->sym, 0);
            break;
        }
        if (nd(g->L, kid(g->L, n, 0))->op == TO_ELEM) {
            local_list(g, n);
            break;
        }
        if (is_agg(g, t->ty, &asz)) {   /* a struct or union copied in */
            gen(g, kid(g->L, n, 0));
            em(g, M_LEA, 8, M(R_BP, var_slot(g, t->sym, 0)), R(R_CX));
            copy_bytes(g, asz);
            break;
        }
        if (!tinfo(g, t->ty, &ti))
            return;
        gen(g, kid(g->L, n, 0));
        em(g, M_LEA, 8, M(R_BP, var_slot(g, t->sym, 0)), R(R_CX));
        store_rcx(g, &ti);
        break;
    default:
        unsup(g, "statement %d", t->op);
    }
}

static void jump_to(Gx *g, uint32_t pos, uint32_t cur)
{
    if (pos != cur + 1)
        em(g, M_JMP, 8, LB(pos), NONE());
}

static void gen_block(Gx *g, const uint32_t *ord, uint32_t i)
{
    Cfg *L = g->L;
    const CBlk *x = &L->b.data[ord[i]];
    size_t k;
    TI ti;
    em(g, M_LABEL, 8, LB(i), NONE());
    for (k = 0; k < x->st.len; k++) {
        gen_stmt(g, x->st.data[k]);
        if (g->depth)
            unsup(g, "unbalanced stack");
    }
    switch (x->term) {
    case TK_JMP:
        jump_to(g, L->b.data[x->a].pos, i);
        break;
    case TK_BR: {
        uint32_t pa = L->b.data[x->a].pos, pb = L->b.data[x->b].pos;
        if (!tinfo(g, nd(L, x->val)->ty, &ti))
            return;
        gen(g, x->val);
        if (ti.flt)
            flt_truth(g, &ti);
        em(g, M_TEST, 8, R(R_AX), R(R_AX));
        if (pa == i + 1) {
            em_cc(g, M_JCC, CC_E, LB(pb));
        } else {
            em_cc(g, M_JCC, CC_NE, LB(pa));
            jump_to(g, pb, i);
        }
        break;
    }
    case TK_SWTCH:
        gen(g, x->val);
        for (k = 0; k < x->cases.len; k++) {
            int64_t v = x->cases.data[k].v;
            if (v >= INT32_MIN && v <= INT32_MAX) {
                em(g, M_CMP, 8, I(v), R(R_AX));
            } else {
                em(g, M_MOVABS, 8, I(v), R(R_CX));
                em(g, M_CMP, 8, R(R_CX), R(R_AX));
            }
            em_cc(g, M_JCC, CC_E, LB(L->b.data[x->cases.data[k].to].pos));
        }
        jump_to(g, L->b.data[x->a].pos, i);
        break;
    case TK_RET:
        if (x->val != TN) {
            uint64_t rsz;
            if (is_agg(g, nd(L, x->val)->ty, &rsz)) {
                gen(g, x->val);
                ret_agg(g, &g->rc, g->retptr);
            } else {
                if (!tinfo(g, nd(L, x->val)->ty, &ti))
                    return;
                gen(g, x->val);
            }
        }
        em(g, M_LEAVE, 8, NONE(), NONE());
        em(g, M_RET, 8, NONE(), NONE());
        break;
    case TK_IJMP:
        gen(g, x->val);
        em(g, M_JMP, 8, R(R_AX), NONE());
        break;
    default:
        unsup(g, "block terminator %d", x->term);
    }
}

/* The parameters of the function being generated: the N_PARAM nodes of its
 * declarator, outermost only, in order. */
static void params(Gx *g, uint32_t root, U32Vec *out)
{
    Checker *c = g->c;
    uint32_t nk = nkids(c, root, NULL, 0), *k, j, i, body = NO_NODE;
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, root, k, nk);
    for (j = 0; j < nk; j++)
        if (ntag(c, k[j]) == N_COMPOUND)
            body = k[j];
    free(k);
    if (body == NO_NODE)
        return;
    for (i = cfirst(c, root); i < cfirst(c, body); i++) {
        if (!is_real_param(c, i) || !c->cb[i] || type_ckind(&c->tt, c->ty[i]) == TY_VOID)
            continue;           /* not f(void) */
        while (out->len && out->data[out->len - 1] >= cfirst(c, i))
            out->len--;         /* a parameter of a parameter's own type */
        vec_push(out, i);
    }
}

void x86_func(Cfg *L, const uint32_t *ord, FILE *out)
{
    Gx g;
    uint32_t i, root = L->e->c->nn - 1;
    int ni = 0, nf = 0, ns = 0;
    U32Vec ps = {0};
    const CSym *fs = csym(L->e->c, L->e->fsym);
    memset(&g, 0, sizeof g);
    g.L = L;
    g.c = L->e->c;
    g.f.name = cident(g.c, fs->name);
    g.f.global = fs->linkage == LK_EXTERNAL && fs->sc != SC_STATIC;
    g.regoff = xcalloc(L->nreg ? L->nreg : 1, sizeof *g.regoff);
    g.tmpoff = xcalloc(L->e->ntmp ? L->e->ntmp : 1, sizeof *g.tmpoff);
    em(&g, M_PUSH, 8, R(R_BP), NONE());
    em(&g, M_MOV, 8, R(R_SP), R(R_BP));
    g.sub_at = g.f.code.len;
    em(&g, M_SUB, 8, I(0), R(R_SP));
    params(&g, root, &ps);
    {
        uint64_t rsz;
        TypeTable *tt = &g.c->tt;
        TypeId rt = type_base(tt, type_canon(tt, fs->ty));
        g.retagg = is_agg(&g, rt, &rsz);
        if (g.retagg && agg_class(&g, rt, &g.rc) && g.rc.mem) {
            g.retptr = alloc(&g, 8, 8);         /* the caller's buffer, in %rdi */
            em(&g, M_MOV, 8, R(R_DI), M(R_BP, g.retptr));
            ni = 1;
        }
    }
    g.nlab = L->npl;
    g.variadic = (type_ent(&g.c->tt, type_canon(&g.c->tt, fs->ty))->flags & TF_VARIADIC) != 0;
    if (g.variadic) {                   /* every argument register, for va_arg */
        g.save = alloc(&g, 176, 16);
        for (i = 0; i < 6; i++)
            em(&g, M_MOV, 8, R(arg_regs[i]), M(R_BP, g.save + 8 * (int64_t)i));
        for (i = 0; i < 8; i++)
            em(&g, M_FMOV, 8, R(R_X0 + (int)i), M(R_BP, g.save + 48 + 16 * (int64_t)i));
    }
    for (i = 0; i < ps.len && !g.why[0]; i++) {
        TI ti;
        int where;
        uint32_t ref = g.c->cb[ps.data[i]] - 1;
        uint64_t psz;
        if (is_agg(&g, csym(g.c, ref)->ty, &psz)) {
            AggCls pc;
            int32_t slot, p, need_i = 0, need_f = 0;
            if (!agg_class(&g, csym(g.c, ref)->ty, &pc))
                break;
            slot = var_slot(&g, ref, 0);
            for (p = 0; !pc.mem && p < pc.np; p++)
                if (pc.c[p] == 'i')
                    need_i++;
                else
                    need_f++;
            if (!pc.mem && ni + need_i <= 6 && nf + need_f <= 8) {
                for (p = 0; p < pc.np; p++)
                    if (pc.c[p] == 'i')
                        em(&g, M_MOV, piece_w(&pc, p), R(arg_regs[ni++]), M(R_BP, slot + 8 * p));
                    else
                        em(&g, M_FMOV, piece_w(&pc, p) == 4 ? 4 : 8, R(R_X0 + nf++),
                           M(R_BP, slot + 8 * p));
            } else {
                em(&g, M_LEA, 8, M(R_BP, 16 + 8 * (int64_t)ns), R(R_AX));
                em(&g, M_LEA, 8, M(R_BP, slot), R(R_CX));
                copy_bytes(&g, pc.size);
                ns += (int)((pc.size + 7) / 8);
            }
            continue;
        }
        if (!tinfo(&g, csym(g.c, ref)->ty, &ti))
            break;
        where = arg_loc(&ti, &ni, &nf);
        if (where >= R_X0) {
            em(&g, M_FMOV, ti.bytes, R(where), M(R_BP, var_slot(&g, ref, 8)));
        } else if (where >= 0) {
            em(&g, M_MOV, 8, R(where), M(R_BP, var_slot(&g, ref, 8)));
        } else {                /* above the saved %rbp and return address */
            em(&g, M_MOV, 8, M(R_BP, 16 + 8 * (int64_t)ns++), R(R_AX));
            em(&g, M_MOV, 8, R(R_AX), M(R_BP, var_slot(&g, ref, 8)));
        }
    }
    g.va_gp = 8 * ni;
    g.va_fp = 48 + 16 * nf;
    g.va_stk = 16 + 8 * ns;
    for (i = 0; i < L->npl && !g.why[0]; i++)
        if (L->b.data[ord[i]].reach)
            gen_block(&g, ord, i);
    if (g.why[0]) {
        fprintf(out, "# x86: %s unsupported: %s\n", g.f.name, g.why);
    } else {
        g.f.code.data[g.sub_at].a = I((g.frame + 15) / 16 * 16);
        for (i = 0; i < L->e->nodes.len; i++)   /* labels whose address is taken, for static data */
            if (L->e->nodes.data[i].op == TO_LABADDR)
                break;
        if (i < L->e->nodes.len) {
            size_t q;
            for (q = 0; q < L->labs.len; q++) {
                const CBlk *lb = &L->b.data[L->labs.data[q].blk];
                MPub pb;
                char nm[160];
                if (lb->pos == TN || !lb->reach)
                    continue;
                snprintf(nm, sizeof nm, ".L%s_a%u", g.f.name, L->labs.data[q].id);
                pb.name = xmalloc(strlen(nm) + 1);
                strcpy(pb.name, nm);
                pb.lab = lb->pos;
                vec_push(&g.f.pubs, pb);
            }
        }
        for (i = 0; i < g.lits.len; i++)
            x86_emit_data(g.c, &g.lits.data[i]);
        if (g.c->opt.dump_asm)
            mir_print(&g.f, out);
        if (g.c->opt.emit_obj && !mobj_add(x86_get(g.c)->obj, &g.f))
            fprintf(out, "# x86: %s unsupported: no encoding\n", g.f.name);
    }
    for (i = 0; i < g.lits.len; i++)
        mdata_free(&g.lits.data[i]);
    vec_free(&g.lits);
    for (i = 0; i < g.f.pubs.len; i++)
        free(g.f.pubs.data[i].name);
    vec_free(&g.f.pubs);
    vec_free(&g.f.code);
    vec_free(&g.vars);
    vec_free(&ps);
    free(g.regoff);
    free(g.tmpoff);
}
