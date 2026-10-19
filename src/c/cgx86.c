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
 * for integer and SSE classes, the rest on the stack.  Not supported yet (the
 * function is then reported in a comment): long double, struct/union/array
 * values, initializer lists, static data and globals, bit-fields, indirect
 * calls, computed goto, conversions between 64-bit unsigned and float.  See
 * docs/TAST_T9_PLAN.md. */
#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/mir.h"
#include "c/tcfg_int.h"

typedef struct Slot {
    uint32_t ref;
    int32_t off;
} Slot;

typedef struct Gx {
    Cfg *L;
    Checker *c;
    MFunc f;
    VEC(Slot) vars;
    int32_t *regoff, *tmpoff;   /* per reg N / tmp N: its slot, 0: none yet */
    int32_t frame;
    int depth;                  /* values waiting on the stack */
    size_t sub_at;              /* the frame-size instruction, patched at the end */
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
        unsup(g, "value of a type outside the integer and float subset");
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

/* The address of an lvalue node, into %rax. */
static void genaddr(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    if (g->why[0])
        return;
    switch (t->op) {
    case TO_SYM: {
        const CSym *s = csym(g->c, t->sym);
        if (!(t->sym & SYM_LOCAL) || s->kind != CS_OBJ || s->sc == SC_STATIC ||
            s->sc == SC_EXTERN) {
            unsup(g, "global or static object");
            return;
        }
        em(g, M_LEA, 8, M(R_BP, var_slot(g, t->sym, 0)), R(R_AX));
        break;
    }
    case TO_REG:
        em(g, M_LEA, 8, M(R_BP, *slot_in(g, &g->regoff, (uint32_t)t->val)), R(R_AX));
        break;
    case TO_TMP:
        em(g, M_LEA, 8, M(R_BP, *slot_in(g, &g->tmpoff, (uint32_t)t->val)), R(R_AX));
        break;
    case TO_DEREF:
        gen(g, kid(g->L, n, 0));
        break;
    case TO_STR: {              /* the literal's bytes go to read-only data */
        MStr s;
        MOp lit = {MK_STR, 0, 0, NULL};
        s.n = t->width;
        s.p = xmalloc(s.n ? s.n : 1);
        memcpy(s.p, g->L->e->bytes.data + t->val, s.n);
        lit.v = (int64_t)g->f.strs.len;
        vec_push(&g->f.strs, s);
        em(g, M_LEA, 8, lit, R(R_AX));
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

static void gen(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    TI ti;
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
        if (!tinfo(g, t->ty, &ti))
            return;
        genaddr(g, kid(g->L, n, 0));
        load_at_rax(g, &ti);
        break;
    case TO_STORE:
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
            if (!from.sg && from.bytes == 8) {
                unsup(g, "unsigned 64-bit integer to float");
                return;
            }
            em(g, M_CVTI2F, ti.bytes, R(R_AX), R(R_X0));
        } else if (from.flt) {
            if (ti.boolean) {
                flt_truth(g, &from);
            } else if (!ti.sg && ti.bytes == 8) {
                unsup(g, "float to unsigned 64-bit integer");
                return;
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
    case TO_CALL: {
        uint32_t j, nstack = 0;
        int ni = 0, nf = 0, pad, *loc;
        TI *ats;
        const CSym *s;
        if (t->pun) {
            unsup(g, "indirect call");
            return;
        }
        s = csym(g->c, t->sym);
        loc = xmalloc((t->nkids + 1) * sizeof *loc);
        ats = xmalloc((t->nkids + 1) * sizeof *ats);
        for (j = 0; j < t->nkids; j++) {
            if (!tinfo(g, nd(g->L, kid(g->L, n, j))->ty, &ats[j])) {
                free(loc);
                free(ats);
                return;
            }
            loc[j] = arg_loc(&ats[j], &ni, &nf);
            nstack += loc[j] < 0;
        }
        /* Stack arguments are pushed first (last to first), then the register
         * ones, which pop off again; the padding goes below the stack
         * arguments so %rsp is 16-aligned at the call. */
        pad = (g->depth + (int)nstack) & 1;
        if (pad) {
            em(g, M_SUB, 8, I(8), R(R_SP));
            g->depth++;
        }
        for (j = t->nkids; j-- > 0;)
            if (loc[j] < 0) {
                gen(g, kid(g->L, n, j));
                pushv(g, &ats[j]);
            }
        for (j = t->nkids; j-- > 0;)
            if (loc[j] >= 0) {
                gen(g, kid(g->L, n, j));
                pushv(g, &ats[j]);
            }
        for (j = 0; j < t->nkids; j++)
            if (loc[j] >= R_X0) {
                pop(g, R_AX);
                em(g, M_MOVQX, 8, R(R_AX), R(loc[j]));
            } else if (loc[j] >= 0) {
                pop(g, loc[j]);
            }
        free(loc);
        free(ats);
        em(g, M_MOV, 4, I(nf), R(R_AX));    /* vector registers used, for variadic callees */
        {
            MOp callee = {MK_SYM, 0, 0, cident(g->c, s->name)};
            em(g, M_CALL, 8, callee, NONE());
        }
        if (nstack + pad) {
            em(g, M_ADD, 8, I(8 * (nstack + pad)), R(R_SP));
            g->depth -= (int)(nstack + pad);
        }
        if (type_ckind(&g->c->tt, t->ty) != TY_VOID) {
            if (!tinfo(g, t->ty, &ti))
                return;
            if (!ti.flt)            /* a float came back in %xmm0 */
                canon(g, &ti);
        }
        break;
    }
    default:
        unsup(g, "expression %d", t->op);
    }
}

/* ---- statements, blocks ------------------------------------------------ */

static void gen_stmt(Gx *g, uint32_t n)
{
    const TNode *t = nd(g->L, n);
    TI ti;
    if (g->why[0])
        return;
    switch (t->op) {
    case TO_EXPRSTMT:
        gen(g, kid(g->L, n, 0));
        break;
    case TO_LOCAL:
        if (t->nkids == 0) {
            var_slot(g, t->sym, 0);
            break;
        }
        if (nd(g->L, kid(g->L, n, 0))->op == TO_ELEM || !tinfo(g, t->ty, &ti)) {
            unsup(g, "initializer list or aggregate local");
            return;
        }
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
            if (!tinfo(g, nd(L, x->val)->ty, &ti))
                return;
            gen(g, x->val);
        }
        em(g, M_LEAVE, 8, NONE(), NONE());
        em(g, M_RET, 8, NONE(), NONE());
        break;
    default:
        unsup(g, "computed goto");
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
        if (!is_real_param(c, i) || !c->cb[i])
            continue;
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
    for (i = 0; i < ps.len && !g.why[0]; i++) {
        TI ti;
        int where;
        uint32_t ref = g.c->cb[ps.data[i]] - 1;
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
    for (i = 0; i < L->npl && !g.why[0]; i++)
        if (L->b.data[ord[i]].reach)
            gen_block(&g, ord, i);
    if (g.why[0]) {
        fprintf(out, "# x86: %s unsupported: %s\n", g.f.name, g.why);
    } else {
        g.f.code.data[g.sub_at].a = I((g.frame + 15) / 16 * 16);
        if (g.c->opt.dump_asm)
            mir_print(&g.f, out);
        if (g.c->opt.emit_obj) {
            if (!g.c->obj)
                g.c->obj = mobj_new();
            if (!mobj_add(g.c->obj, &g.f))
                fprintf(out, "# x86: %s unsupported: no encoding\n", g.f.name);
        }
    }
    for (i = 0; i < g.f.strs.len; i++)
        free(g.f.strs.data[i].p);
    vec_free(&g.f.strs);
    vec_free(&g.f.code);
    vec_free(&g.vars);
    vec_free(&ps);
    free(g.regoff);
    free(g.tmpoff);
}
