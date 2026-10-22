/* tcfg.c - the typed AST lowered to a control-flow graph (docs/TAST_T8_PLAN.md,
 * stage T8a).
 *
 * The structured tree of one function (tast.c) is cut into basic blocks.  A
 * block is a run of the tree's own statement nodes (local, exprstmt, data)
 * followed by exactly one terminator: jmp, br, swtch, ijmp or ret.  Statements
 * (if, loops, switch, break, continue, goto, label, return) become blocks and
 * edges; expressions are not touched, so one that hides control flow (&&, ||,
 * ?:, statement expressions) stays inside its block until stage T8b.  Nothing
 * here changes the structured form. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/tcfg_int.h"
#include "c/eval.h"

static void lunsup(Cfg *L, const char *fmt, ...)
{
    if (!L->why[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(L->why, sizeof L->why, fmt, ap);
        va_end(ap);
    }
}

static uint32_t newb(Cfg *L)
{
    CBlk k;
    memset(&k, 0, sizeof k);
    k.val = k.a = k.b = k.pos = TN;
    vec_push(&L->b, k);
    return (uint32_t)L->b.len - 1;
}

static void place(Cfg *L, uint32_t b)
{
    L->b.data[b].pos = L->npl++;
    L->cur = b;
}

/* Code after a terminator is dead but kept: it goes to a block nothing jumps
 * to (reported unreachable). */
static uint32_t open_cur(Cfg *L)
{
    if (L->cur == TN)
        place(L, newb(L));
    return L->cur;
}

static void ends(Cfg *L, int term, uint32_t val, uint32_t a, uint32_t b)
{
    CBlk *k = &L->b.data[open_cur(L)];
    k->term = (uint8_t)term;
    k->val = val;
    k->a = a;
    k->b = b;
    if (term == TK_JMP || term == TK_BR)
        L->b.data[a].used = true;
    if (term == TK_BR)
        L->b.data[b].used = true;
    L->cur = TN;
}

/* Leave the current block (if it is still open) for `to`. */
static void jump(Cfg *L, uint32_t to)
{
    if (L->cur != TN)
        ends(L, TK_JMP, TN, to, TN);
}

/* Carry on in `to`, a join or an exit, which exists only if an edge led to it
 * (otherwise what follows is dead and the function may simply end). */
static void join(Cfg *L, uint32_t to)
{
    if (L->b.data[to].used)
        place(L, to);
    else
        L->cur = TN;
}

/* ... and carry on in `to`. */
static void fall(Cfg *L, uint32_t to)
{
    jump(L, to);
    place(L, to);
}

static uint32_t lab_blk(Cfg *L, uint32_t id)
{
    size_t i;
    LabEnt le;
    for (i = 0; i < L->labs.len; i++)
        if (L->labs.data[i].id == id)
            return L->labs.data[i].blk;
    le.id = id;
    le.blk = newb(L);
    vec_push(&L->labs, le);
    return le.blk;
}

static void push_tgt(Cfg *L, uint32_t brk, uint32_t cont, uint32_t sw)
{
    Tgt g;
    g.brk = brk, g.cont = cont, g.sw = sw;
    vec_push(&L->tg, g);
}

/* The innermost switch's dispatch block, TN: not inside one. */
static uint32_t cur_switch(Cfg *L)
{
    size_t i;
    for (i = L->tg.len; i-- > 0;)
        if (L->tg.data[i].sw != TN)
            return L->tg.data[i].sw;
    return TN;
}

static void lower(Cfg *L, uint32_t n);

/* ---- expressions: removing the control flow they hide (T8b) --------------- */

static bool is_lv_op(int op)
{
    return op == TO_SYM || op == TO_TMP || op == TO_COMPLIT || op == TO_DEREF ||
           op == TO_FIELD || op == TO_STR || op == TO_REG;
}

/* A new node like `proto` with the given kids. */
static uint32_t clone(Cfg *L, TNode proto, const uint32_t *k, uint32_t nk)
{
    uint32_t j;
    proto.kid0 = (uint32_t)L->e->kids.len;
    proto.nkids = nk;
    for (j = 0; j < nk; j++)
        vec_push(&L->e->kids, k[j]);
    vec_push(&L->e->nodes, proto);
    return (uint32_t)L->e->nodes.len - 1;
}

static uint32_t newn(Cfg *L, int op, TypeId ty, SrcLoc loc, int64_t val, const uint32_t *k,
                     uint32_t nk)
{
    TNode t;
    memset(&t, 0, sizeof t);
    t.op = (uint8_t)op;
    t.ty = ty;
    t.loc = loc;
    t.val = val;
    return clone(L, t, k, nk);
}

static uint32_t reg_node(Cfg *L, uint32_t r, TypeId ty, SrcLoc loc)
{
    return newn(L, TO_REG, ty, loc, r, NULL, 0);
}

static uint32_t reg_load(Cfg *L, uint32_t r, TypeId ty, SrcLoc loc)
{
    uint32_t k = reg_node(L, r, ty, loc);
    return newn(L, TO_LOAD, ty, loc, 0, &k, 1);
}

/* Statement `exprstmt v` in the current block. */
static void emit(Cfg *L, uint32_t v)
{
    uint32_t s = newn(L, TO_EXPRSTMT, 0, nd(L, v)->loc, 0, &v, 1);
    vec_push(&L->b.data[open_cur(L)].st, s);
    L->adj++;
}

/* r := v, as a statement the lowering added. */
static void reg_set(Cfg *L, uint32_t r, TypeId ty, uint32_t v)
{
    SrcLoc loc = nd(L, v)->loc;
    uint32_t ka[2], s;
    ka[0] = reg_node(L, r, ty, loc);
    ka[1] = v;
    s = newn(L, TO_STORE, ty, loc, 0, ka, 2);
    emit(L, s);
}

/* A value that stays valid across a later split: constants as they are, the
 * rest saved in a register. */
static uint32_t spill_val(Cfg *L, uint32_t v)
{
    TNode t = *nd(L, v);
    uint32_t r;
    if (t.op == TO_CONST || t.op == TO_FCONST || t.op == TO_LABADDR)
        return v;
    if (t.op == TO_ADDR) {
        int ko = nd(L, kid(L, v, 0))->op;
        if (ko == TO_SYM || ko == TO_STR)
            return v;
    }
    r = L->nreg++;
    reg_set(L, r, t.ty, v);
    return reg_load(L, r, t.ty, t.loc);
}

/* The same for an lvalue: its address is what must survive. */
static uint32_t spill_lv(Cfg *L, uint32_t v)
{
    TNode t = *nd(L, v);
    uint32_t k, a, r;
    TypeId pt;
    switch (t.op) {
    case TO_SYM: case TO_TMP: case TO_STR: case TO_REG:
        return v;
    case TO_COMPLIT:
        if (t.pun)
            return v;
        break;
    case TO_DEREF:
        k = spill_val(L, kid(L, v, 0));
        return k == kid(L, v, 0) ? v : clone(L, t, &k, 1);
    case TO_FIELD:
        k = spill_lv(L, kid(L, v, 0));
        return k == kid(L, v, 0) ? v : clone(L, t, &k, 1);
    default:
        break;
    }
    pt = type_ptr(&L->e->c->tt, t.ty);
    a = newn(L, TO_ADDR, pt, t.loc, 0, &v, 1);
    r = L->nreg++;
    reg_set(L, r, pt, a);
    k = reg_load(L, r, pt, t.loc);
    return newn(L, TO_DEREF, t.ty, t.loc, 0, &k, 1);
}

static uint32_t lx(Cfg *L, uint32_t n);

/* The same operator over lowered kids.  A kid evaluated before one that
 * splits is spilled first, so it survives the split. */
static uint32_t rebuild(Cfg *L, uint32_t n)
{
    TNode t = *nd(L, n);
    uint32_t nk = t.nkids, last = 0, i, *kk, id;
    bool any = false;
    kk = xmalloc((nk ? nk : 1) * sizeof *kk);
    for (i = 0; i < nk; i++) {
        uint32_t k = kid(L, n, i);
        if (nd(L, k)->op == TO_ELEM)
            k = kid(L, k, 0);
        if (k < L->n0 && L->hs[k])
            last = i + 1;
    }
    for (i = 0; i < nk; i++) {
        uint32_t k = kid(L, n, i), v;
        if (nd(L, k)->op == TO_ELEM) {          /* an initializer element */
            TNode et = *nd(L, k);
            uint32_t in = kid(L, k, 0);
            v = lx(L, in);
            if (i + 1 < last)
                v = spill_val(L, v);
            kk[i] = v == in ? k : clone(L, et, &v, 1);
        } else {
            v = lx(L, k);
            if (i + 1 < last)
                v = is_lv_op(nd(L, v)->op) ? spill_lv(L, v) : spill_val(L, v);
            kk[i] = v;
        }
        any |= kk[i] != k;
    }
    id = any ? clone(L, t, kk, nk) : n;
    free(kk);
    return id;
}

/* Branch to bt if the value of n is nonzero, else to bf; the current block
 * ends.  && and || become branches, so a condition needs no register. */
static void lcond(Cfg *L, uint32_t n, uint32_t bt, uint32_t bf)
{
    TNode t = *nd(L, n);
    uint32_t m, mb;
    if (n < L->n0 && L->hs[n]) {
        switch (t.op) {
        case TO_LOGAND: case TO_LOGOR:
            m = newb(L);
            if (t.op == TO_LOGAND)
                lcond(L, kid(L, n, 0), m, bf);
            else
                lcond(L, kid(L, n, 0), bt, m);
            place(L, m);
            lcond(L, kid(L, n, 1), bt, bf);
            return;
        case TO_UNOP:
            if (t.pun == P_BANG) {
                lcond(L, kid(L, n, 0), bf, bt);
                return;
            }
            break;
        case TO_COND:
            m = newb(L);
            mb = newb(L);
            lcond(L, kid(L, n, 0), m, mb);
            place(L, m);
            lcond(L, kid(L, n, 1), bt, bf);
            place(L, mb);
            lcond(L, kid(L, n, 2), bt, bf);
            return;
        default:
            break;
        }
    }
    ends(L, TK_BR, lx(L, n), bt, bf);
}

/* The lowered form of expression n: a value (or lvalue) node that is
 * split-free, with whatever had to run first emitted into the current block;
 * TN for a void expression. */
static uint32_t lx(Cfg *L, uint32_t n)
{
    TNode t;
    uint32_t r, bj, v, w, j;
    if (n >= L->n0 || !L->hs[n])
        return n;
    t = *nd(L, n);
    switch (t.op) {
    case TO_LOGAND: case TO_LOGOR: {
        uint32_t bt = newb(L), bf = newb(L);
        bj = newb(L);
        r = L->nreg++;
        lcond(L, n, bt, bf);
        place(L, bt);
        reg_set(L, r, t.ty, newn(L, TO_CONST, t.ty, t.loc, 1, NULL, 0));
        jump(L, bj);
        place(L, bf);
        reg_set(L, r, t.ty, newn(L, TO_CONST, t.ty, t.loc, 0, NULL, 0));
        jump(L, bj);
        place(L, bj);
        return reg_load(L, r, t.ty, t.loc);
    }
    case TO_COND: {
        uint32_t ba = newb(L), bb = newb(L);
        bool isv = type_is_void(&L->e->c->tt, t.ty);
        bj = newb(L);
        r = L->nreg++;
        lcond(L, kid(L, n, 0), ba, bb);
        for (j = 1; j <= 2; j++) {
            place(L, j == 1 ? ba : bb);
            v = lx(L, kid(L, n, j));
            if (isv) {
                if (v != TN)
                    emit(L, v);
            } else {
                reg_set(L, r, t.ty, v);
            }
            jump(L, bj);
        }
        place(L, bj);
        return isv ? TN : reg_load(L, r, t.ty, t.loc);
    }
    case TO_ELVIS: {
        uint32_t bb = newb(L);
        bj = newb(L);
        r = L->nreg++;
        v = lx(L, kid(L, n, 0));
        reg_set(L, r, t.ty, v);
        ends(L, TK_BR, reg_load(L, r, t.ty, t.loc), bj, bb);
        place(L, bb);
        reg_set(L, r, t.ty, lx(L, kid(L, n, 1)));
        jump(L, bj);
        place(L, bj);
        return reg_load(L, r, t.ty, t.loc);
    }
    case TO_COMMA:
        v = lx(L, kid(L, n, 0));
        if (v != TN) {
            emit(L, v);                     /* the left side is a statement now */
        }
        return lx(L, kid(L, n, 1));
    case TO_STMTEXPR:
        w = TN;
        for (j = 0; j < t.nkids; j++) {
            if (j + 1 == t.nkids && !type_is_void(&L->e->c->tt, t.ty))
                w = lx(L, kid(L, n, j));
            else
                lower(L, kid(L, n, j));
        }
        return w;
    default:
        return rebuild(L, n);
    }
}

static void lower(Cfg *L, uint32_t n)
{
    TNode tv = *nd(L, n);
    const TNode *t = &tv;
    uint32_t j, x, y, z, h, sw;
    if (L->why[0])
        return;
    switch (t->op) {
    case TO_BLOCK:
        for (j = 0; j < t->nkids; j++)
            lower(L, kid(L, n, j));
        break;
    case TO_NOP:
        break;
    case TO_EXPRSTMT:
        x = lx(L, kid(L, n, 0));
        if (x == TN) {
            L->adj--;                       /* a void ?: left nothing to run */
            break;
        }
        if (x != kid(L, n, 0))
            n = clone(L, tv, &x, 1);
        vec_push(&L->b.data[open_cur(L)].st, n);
        break;
    case TO_LOCAL:
        n = rebuild(L, n);
        vec_push(&L->b.data[open_cur(L)].st, n);
        break;
    case TO_DATA: case TO_ASM:
        vec_push(&L->b.data[open_cur(L)].st, n);
        break;
    case TO_IF: {
        bool els = t->nkids > 2;
        x = newb(L);                        /* then */
        y = els ? newb(L) : TN;             /* else */
        z = newb(L);                        /* join */
        lcond(L, kid(L, n, 0), x, els ? y : z);
        place(L, x);
        lower(L, kid(L, n, 1));
        jump(L, z);
        if (els) {
            place(L, y);
            lower(L, kid(L, n, 2));
            jump(L, z);
        }
        join(L, z);
        break;
    }
    case TO_WHILE:
        h = newb(L);                        /* test */
        x = newb(L);                        /* body */
        z = newb(L);                        /* exit */
        fall(L, h);
        lcond(L, kid(L, n, 0), x, z);
        place(L, x);
        push_tgt(L, z, h, TN);
        lower(L, kid(L, n, 1));
        L->tg.len--;
        jump(L, h);
        join(L, z);
        break;
    case TO_DO:
        x = newb(L);                        /* body */
        h = newb(L);                        /* test */
        z = newb(L);                        /* exit */
        fall(L, x);
        push_tgt(L, z, h, TN);
        lower(L, kid(L, n, 0));
        L->tg.len--;
        fall(L, h);
        lcond(L, kid(L, n, 1), x, z);
        join(L, z);
        break;
    case TO_FOR: {
        /* kids: init, test (nop: none), step (nop: none), body.  With no test
         * the body block is the loop head; with no step, continue goes to the
         * head. */
        bool test = nd(L, kid(L, n, 1))->op != TO_NOP;
        bool step = nd(L, kid(L, n, 2))->op != TO_NOP;
        lower(L, kid(L, n, 0));
        x = newb(L);                        /* body */
        h = test ? newb(L) : x;             /* head: the test, or the body */
        y = step ? newb(L) : h;             /* step: where continue goes */
        z = newb(L);                        /* exit */
        fall(L, h);
        if (test) {
            lcond(L, kid(L, n, 1), x, z);
            place(L, x);
        }
        push_tgt(L, z, y, TN);
        lower(L, kid(L, n, 3));
        L->tg.len--;
        if (step) {
            fall(L, y);
            lower(L, kid(L, n, 2));
        }
        jump(L, h);
        join(L, z);
        break;
    }
    case TO_SWITCH:
        x = newb(L);                        /* exit */
        y = lx(L, kid(L, n, 0));
        sw = open_cur(L);
        ends(L, TK_SWTCH, y, TN, TN);
        push_tgt(L, x, L->tg.len ? L->tg.data[L->tg.len - 1].cont : TN, sw);
        lower(L, kid(L, n, 1));
        L->tg.len--;
        jump(L, x);
        if (L->b.data[sw].a == TN) {
            L->b.data[sw].a = x;            /* no default: out */
            L->b.data[x].used = true;
        }
        join(L, x);
        break;
    case TO_CASE: case TO_DEFAULT:
        sw = cur_switch(L);
        if (sw == TN) {
            lunsup(L, "case outside a switch");
            break;
        }
        x = newb(L);
        fall(L, x);
        if (t->op == TO_CASE) {
            CCase cc;
            cc.v = t->val, cc.to = x;
            vec_push(&L->b.data[sw].cases, cc);
        } else {
            L->b.data[sw].a = x;
        }
        lower(L, kid(L, n, 0));
        break;
    case TO_BREAK:
    case TO_CONTINUE: {
        size_t i;
        for (i = L->tg.len; i-- > 0;) {
            Tgt *g = &L->tg.data[i];
            uint32_t to = t->op == TO_BREAK ? g->brk : g->cont;
            if (to != TN) {
                ends(L, TK_JMP, TN, to, TN);
                return;
            }
        }
        lunsup(L, "%s outside a loop", t->op == TO_BREAK ? "break" : "continue");
        break;
    }
    case TO_GOTO:
        ends(L, TK_JMP, TN, lab_blk(L, (uint32_t)t->val), TN);
        break;
    case TO_LABEL:
        x = lab_blk(L, (uint32_t)t->val);
        if (L->b.data[x].pos != TN) {
            lunsup(L, "label defined twice");
            break;
        }
        fall(L, x);
        lower(L, kid(L, n, 0));
        break;
    case TO_IGOTO:
        x = lx(L, kid(L, n, 0));
        ends(L, TK_IJMP, x, TN, TN);
        break;
    case TO_RETURN:
        x = t->nkids ? lx(L, kid(L, n, 0)) : TN;
        ends(L, TK_RET, x, TN, TN);
        break;
    default:
        lunsup(L, "statement op %d", t->op);
        break;
    }
}

/* ---- printing and checking ------------------------------------------------ */

/* The nodes of a subtree in post order: kids first, each node one dump line. */
static void put_tree(Cfg *L, FILE *out, uint32_t n)
{
    uint32_t j;
    for (j = 0; j < nd(L, n)->nkids; j++)
        put_tree(L, out, kid(L, n, j));
    tast_print_node(L->e, out, n);
}

/* The labels whose address is taken: where an ijmp may go. */
static void ijmp_targets(Cfg *L, U32Vec *out)
{
    size_t i, k;
    for (i = 0; i < L->e->nodes.len; i++) {
        if (L->e->nodes.data[i].op != TO_LABADDR)
            continue;
        for (k = 0; k < L->labs.len; k++)
            if (L->labs.data[k].id == (uint32_t)L->e->nodes.data[i].val &&
                L->b.data[L->labs.data[k].blk].pos != TN) {
                size_t m;
                for (m = 0; m < out->len; m++)
                    if (out->data[m] == L->labs.data[k].blk)
                        break;
                if (m == out->len)
                    vec_push(out, L->labs.data[k].blk);
            }
    }
}

/* Marks the blocks reachable from `entry` (an explicit work list: a function
 * can have thousands of blocks in a chain). */
static void mark(Cfg *L, uint32_t entry, const U32Vec *ij)
{
    U32Vec w = {0};
    vec_push(&w, entry);
    while (w.len) {
        uint32_t b = vec_pop(&w);
        CBlk *k = &L->b.data[b];
        size_t i;
        if (k->reach)
            continue;
        k->reach = true;
        switch (k->term) {
        case TK_JMP:
            vec_push(&w, k->a);
            break;
        case TK_BR:
            vec_push(&w, k->a);
            vec_push(&w, k->b);
            break;
        case TK_SWTCH:
            vec_push(&w, k->a);
            for (i = 0; i < k->cases.len; i++)
                vec_push(&w, k->cases.data[i].to);
            break;
        case TK_IJMP:
            for (i = 0; i < ij->len; i++)
                vec_push(&w, ij->data[i]);
            break;
        default:
            break;
        }
    }
    vec_free(&w);
}

/* The statement nodes of the structured tree: the graph must hold each one
 * (a rebuilt one replaces it), plus the stores the lowering added. */
static size_t struct_stmts(Cfg *L, uint32_t n)
{
    const TNode *t = nd(L, n);
    size_t c = t->op == TO_LOCAL || t->op == TO_EXPRSTMT || t->op == TO_DATA || t->op == TO_ASM;
    uint32_t j;
    for (j = 0; j < t->nkids; j++)
        c += struct_stmts(L, kid(L, n, j));
    return c;
}

static void seen_tree(Cfg *L, uint32_t n, uint8_t *seen, unsigned *dup, unsigned *hid)
{
    uint32_t j;
    int op = nd(L, n)->op;
    if (seen[n]++)
        ++*dup;
    if (op == TO_LOGAND || op == TO_LOGOR || op == TO_COND || op == TO_ELVIS ||
        op == TO_STMTEXPR)
        ++*hid;
    for (j = 0; j < nd(L, n)->nkids; j++)
        seen_tree(L, kid(L, n, j), seen, dup, hid);
}

/* Where block x may go next. */
static void succs(const Cfg *L, const CBlk *x, const U32Vec *ij, U32Vec *out)
{
    size_t k;
    out->len = 0;
    if (x->term == TK_JMP || x->term == TK_BR)
        vec_push(out, x->a);
    if (x->term == TK_BR)
        vec_push(out, x->b);
    if (x->term == TK_SWTCH) {
        vec_push(out, x->a);
        for (k = 0; k < x->cases.len; k++)
            vec_push(out, x->cases.data[k].to);
    }
    if (x->term == TK_IJMP)
        for (k = 0; k < ij->len; k++)
            vec_push(out, ij->data[k]);
    (void)L;
}

typedef struct RegWalk {
    Cfg *L;
    FILE *out;
    uint8_t *def;           /* per register: set on every path so far */
    bool report;
    uint32_t blk;
    TypeId *ty;             /* per register: its type once seen */
    uint8_t *tyset;
} RegWalk;

static void cbad(Cfg *L, FILE *out, uint32_t b, const char *fmt, ...);

static void reg_walk(RegWalk *w, uint32_t n)
{
    Cfg *L = w->L;
    TNode t = *nd(L, n);
    uint32_t j, r;
    if ((t.op == TO_LOAD || t.op == TO_STORE) && nd(L, kid(L, n, 0))->op == TO_REG) {
        const TNode *rg = nd(L, kid(L, n, 0));
        r = (uint32_t)rg->val;
        if (t.op == TO_STORE)
            reg_walk(w, kid(L, n, 1));
        if (w->report) {
            if (r >= L->nreg)
                cbad(L, w->out, w->blk, "reg %u does not exist", r);
            else {
                if (!w->tyset[r])
                    w->ty[r] = rg->ty, w->tyset[r] = 1;
                else if (w->ty[r] != rg->ty)
                    cbad(L, w->out, w->blk, "reg %u has two types", r);
                if (t.op == TO_LOAD && !w->def[r])
                    cbad(L, w->out, w->blk, "reg %u read before it is set on every path", r);
            }
        }
        if (t.op == TO_STORE && r < L->nreg)
            w->def[r] = 1;
        return;
    }
    if (t.op == TO_REG) {
        if (w->report)
            cbad(L, w->out, w->blk, "reg outside a load or a store");
        return;
    }
    for (j = 0; j < t.nkids; j++)
        reg_walk(w, kid(L, n, j));
}

static void reg_block(RegWalk *w, const CBlk *x)
{
    size_t k;
    for (k = 0; k < x->st.len; k++)
        reg_walk(w, x->st.data[k]);
    if (x->val != TN)
        reg_walk(w, x->val);
}

/* Every load of a register has a store on every path to it (a must-defined
 * dataflow over the reachable blocks). */
static void check_regs(Cfg *L, FILE *out, const uint32_t *ord, const U32Vec *ij)
{
    size_t nb = L->npl, nr = L->nreg, i, k;
    uint8_t *in, *outs;
    RegWalk w;
    U32Vec su = {0};
    bool changed;
    if (!nr)
        return;
    in = xmalloc(nb * nr);
    outs = xmalloc(nb * nr);
    memset(in, 1, nb * nr);
    memset(in, 0, nr);                      /* the entry: nothing is set */
    memset(&w, 0, sizeof w);
    w.L = L;
    w.out = out;
    w.ty = xcalloc(nr, sizeof *w.ty);
    w.tyset = xcalloc(nr, 1);
    w.def = xmalloc(nr);
    do {
        changed = false;
        for (i = 0; i < nb; i++) {
            const CBlk *x = &L->b.data[ord[i]];
            if (!x->reach)
                continue;
            memcpy(w.def, in + i * nr, nr);
            reg_block(&w, x);
            memcpy(outs + i * nr, w.def, nr);
            succs(L, x, ij, &su);
            for (k = 0; k < su.len; k++) {
                size_t s = L->b.data[su.data[k]].pos, r;
                if (s == 0)
                    continue;
                for (r = 0; r < nr; r++)
                    if (in[s * nr + r] && !w.def[r]) {
                        in[s * nr + r] = 0;
                        changed = true;
                    }
            }
        }
    } while (changed);
    w.report = true;
    for (i = 0; i < nb; i++) {
        const CBlk *x = &L->b.data[ord[i]];
        if (!x->reach)
            continue;
        w.blk = ord[i];
        memcpy(w.def, in + i * nr, nr);
        reg_block(&w, x);
    }
    free(in);
    free(outs);
    free(w.ty);
    free(w.tyset);
    free(w.def);
    vec_free(&su);
}

static void cbad(Cfg *L, FILE *out, uint32_t b, const char *fmt, ...)
{
    va_list ap;
    if (L->e->c->opt.tast_bad)
        ++*L->e->c->opt.tast_bad;
    fprintf(out, "verify-cfg: B%u: ", L->b.data[b].pos);
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
}

/* The placed blocks in printed (placement) order. */
static uint32_t *by_pos(Cfg *L)
{
    uint32_t *o = xmalloc((L->npl ? L->npl : 1) * sizeof *o), i;
    for (i = 0; i < L->b.len; i++)
        if (L->b.data[i].pos != TN)
            o[L->b.data[i].pos] = i;
    return o;
}

static void check(Cfg *L, FILE *out, uint32_t root, const uint32_t *ord, const U32Vec *ij)
{
    size_t i, k, stmts = 0;
    unsigned dup = 0, hid = 0;
    uint8_t *seen = xcalloc(L->e->nodes.len ? L->e->nodes.len : 1, 1);
    for (i = 0; i < L->npl; i++) {
        uint32_t b = ord[i];
        CBlk *x = &L->b.data[b];
        uint32_t tg[2], nt = 0;
        for (k = 0; k < x->st.len; k++)
            seen_tree(L, x->st.data[k], seen, &dup, &hid);
        stmts += x->st.len;
        if (x->val != TN)
            seen_tree(L, x->val, seen, &dup, &hid);
        if (x->term == TK_NONE)
            cbad(L, out, b, "no terminator");
        if (x->term == TK_JMP || x->term == TK_BR)
            tg[nt++] = x->a;
        if (x->term == TK_BR)
            tg[nt++] = x->b;
        if (x->term == TK_SWTCH) {
            tg[nt++] = x->a;
            for (k = 0; k < x->cases.len; k++) {
                size_t m;
                for (m = 0; m < k; m++)
                    if (x->cases.data[m].v == x->cases.data[k].v)
                        cbad(L, out, b, "case %lld twice", (long long)x->cases.data[k].v);
                if (x->cases.data[k].to >= L->b.len ||
                    L->b.data[x->cases.data[k].to].pos == TN)
                    cbad(L, out, b, "case target is not a block");
            }
        }
        for (k = 0; k < nt; k++)
            if (tg[k] >= L->b.len || L->b.data[tg[k]].pos == TN)
                cbad(L, out, b, "target is not a block");
        if ((x->term == TK_BR || x->term == TK_SWTCH || x->term == TK_IJMP) && x->val == TN)
            cbad(L, out, b, "terminator without a value");
        if (x->term == TK_RET && x->val != TN && type_is_void(&L->e->c->tt, L->e->ret))
            cbad(L, out, b, "value returned from a void function");
    }
    if (dup)
        cbad(L, out, ord[0], "%u node(s) placed twice", dup);
    if (hid)
        cbad(L, out, ord[0], "%u node(s) with control flow hidden in an expression", hid);
    if ((long)stmts - L->adj != (long)struct_stmts(L, root))
        cbad(L, out, ord[0], "%zu statements in the graph (%ld added), %zu in the tree", stmts,
             L->adj, struct_stmts(L, root));
    check_regs(L, out, ord, ij);
    free(seen);
}

void tast_cfg(El *e, uint32_t root, FILE *out)
{
    Cfg L;
    size_t i, k, nun = 0;
    bool asm_only = CHK_ASM(e->c->opt) && !e->c->opt.dump_cfg;
    uint32_t *ord;
    U32Vec ij = {0};
    memset(&L, 0, sizeof L);
    L.e = e;
    L.cur = TN;
    L.n0 = (uint32_t)e->nodes.len;
    L.hs = xcalloc(L.n0 ? L.n0 : 1, 1);
    for (i = 0; i < L.n0; i++) {            /* where control flow hides */
        const TNode *t = &e->nodes.data[i];
        L.hs[i] = t->op == TO_LOGAND || t->op == TO_LOGOR || t->op == TO_COND ||
                  t->op == TO_ELVIS || t->op == TO_STMTEXPR;
        for (k = 0; k < t->nkids; k++)
            L.hs[i] |= L.hs[e->kids.data[t->kid0 + k]];
    }
    place(&L, newb(&L));                    /* the entry */
    lower(&L, root);
    if (!L.why[0] && L.cur != TN)
        ends(&L, TK_RET, TN, TN, TN);       /* falling off the end */
    for (i = 0; i < L.b.len && !L.why[0]; i++)
        if (L.b.data[i].pos == TN && L.b.data[i].used)
            lunsup(&L, "label never defined");
    if (L.why[0]) {
        fprintf(out, "%scfg unsupported: %s\n", asm_only ? "# " : "", L.why);
    } else {
        ord = by_pos(&L);
        ijmp_targets(&L, &ij);
        mark(&L, ord[0], &ij);
        if (CHK_EVAL(e->c->opt))
            eval_keep(e->c, e, &L);
        if (CHK_ASM(e->c->opt)) {
            if (!CHK_EVAL(e->c->opt))
                eval_statics(e->c, e);      /* the block-scope statics ev_object images */
            x86_func(&L, ord, out);
        }
        for (i = 0; i < L.npl && e->c->opt.dump_cfg; i++) {
            const CBlk *x = &L.b.data[ord[i]];
            fprintf(out, "B%zu:%s\n", i, x->reach ? "" : " unreachable");
            nun += !x->reach;
            for (k = 0; k < x->st.len; k++)
                put_tree(&L, out, x->st.data[k]);
            if (x->val != TN)
                put_tree(&L, out, x->val);
            switch (x->term) {
            case TK_JMP:
                fprintf(out, "  jmp B%u\n", L.b.data[x->a].pos);
                break;
            case TK_BR:
                fprintf(out, "  br #%u B%u B%u\n", x->val, L.b.data[x->a].pos,
                        L.b.data[x->b].pos);
                break;
            case TK_SWTCH:
                fprintf(out, "  swtch #%u default B%u\n", x->val, L.b.data[x->a].pos);
                for (k = 0; k < x->cases.len; k++)
                    fprintf(out, "    case %lld B%u\n", (long long)x->cases.data[k].v,
                            L.b.data[x->cases.data[k].to].pos);
                break;
            case TK_IJMP:
                fprintf(out, "  ijmp #%u", x->val);
                for (k = 0; k < ij.len; k++)
                    fprintf(out, " B%u", L.b.data[ij.data[k]].pos);
                fputc('\n', out);
                break;
            case TK_RET:
                if (x->val != TN)
                    fprintf(out, "  ret #%u\n", x->val);
                else
                    fputs("  ret\n", out);
                break;
            default:
                fputs("  (no terminator)\n", out);
                break;
            }
        }
        if (e->c->opt.dump_cfg)
            fprintf(out, "cfg: %u blocks, %zu unreachable\n", L.npl, nun);
        if (e->c->opt.verify_cfg)
            check(&L, out, root, ord, &ij);
        free(ord);
    }
    for (i = 0; i < L.b.len; i++) {
        vec_free(&L.b.data[i].st);
        vec_free(&L.b.data[i].cases);
    }
    free(L.hs);
    vec_free(&L.b);
    vec_free(&L.labs);
    vec_free(&L.tg);
    vec_free(&ij);
}
