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
#include "c/tast_int.h"

enum { TK_NONE, TK_JMP, TK_BR, TK_SWTCH, TK_IJMP, TK_RET };

typedef struct CCase {
    int64_t v;
    uint32_t to;
} CCase;

typedef struct CBlk {
    U32Vec st;              /* the statement nodes, in order */
    VEC(CCase) cases;       /* swtch: the case values and their blocks */
    uint32_t val;           /* br, swtch, ijmp, ret: the value node, TN: none */
    uint32_t a, b;          /* jmp: a; br: a if true, else b; swtch: a = default */
    uint32_t pos;           /* placement order (the printed number), TN: unplaced */
    uint8_t term;
    bool used;              /* some edge leads here */
    bool reach;
} CBlk;

typedef struct LabEnt {
    uint32_t id, blk;
} LabEnt;

typedef struct Tgt {
    uint32_t brk, cont, sw; /* sw: the switch's dispatch block, TN: a loop */
} Tgt;

typedef struct Cfg {
    El *e;
    VEC(CBlk) b;
    VEC(LabEnt) labs;
    VEC(Tgt) tg;
    uint32_t cur;           /* the block being filled, TN: none (dead code) */
    uint32_t npl;
    char why[96];
} Cfg;

static void lunsup(Cfg *L, const char *fmt, ...)
{
    if (!L->why[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(L->why, sizeof L->why, fmt, ap);
        va_end(ap);
    }
}

static const TNode *nd(Cfg *L, uint32_t n)
{
    return &L->e->nodes.data[n];
}

static uint32_t kid(Cfg *L, uint32_t n, uint32_t j)
{
    return L->e->kids.data[nd(L, n)->kid0 + j];
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

static void lower(Cfg *L, uint32_t n)
{
    const TNode *t = nd(L, n);
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
    case TO_LOCAL: case TO_EXPRSTMT: case TO_DATA:
        vec_push(&L->b.data[open_cur(L)].st, n);
        break;
    case TO_IF: {
        bool els = t->nkids > 2;
        x = newb(L);                        /* then */
        y = els ? newb(L) : TN;             /* else */
        z = newb(L);                        /* join */
        ends(L, TK_BR, kid(L, n, 0), x, els ? y : z);
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
        ends(L, TK_BR, kid(L, n, 0), x, z);
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
        ends(L, TK_BR, kid(L, n, 1), x, z);
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
            ends(L, TK_BR, kid(L, n, 1), x, z);
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
        sw = open_cur(L);
        ends(L, TK_SWTCH, kid(L, n, 0), TN, TN);
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
        ends(L, TK_IJMP, kid(L, n, 0), TN, TN);
        break;
    case TO_RETURN:
        ends(L, TK_RET, t->nkids ? kid(L, n, 0) : TN, TN, TN);
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

/* The statements of the structured tree that the graph must hold. */
static size_t struct_stmts(Cfg *L, uint32_t n)
{
    const TNode *t = nd(L, n);
    size_t c = 0;
    uint32_t j;
    switch (t->op) {
    case TO_LOCAL: case TO_EXPRSTMT: case TO_DATA:
        return 1;
    case TO_BLOCK:
        for (j = 0; j < t->nkids; j++)
            c += struct_stmts(L, kid(L, n, j));
        return c;
    case TO_IF:
        c = struct_stmts(L, kid(L, n, 1));
        return t->nkids > 2 ? c + struct_stmts(L, kid(L, n, 2)) : c;
    case TO_WHILE:
        return struct_stmts(L, kid(L, n, 1));
    case TO_DO:
        return struct_stmts(L, kid(L, n, 0));
    case TO_FOR:
        return struct_stmts(L, kid(L, n, 0)) + struct_stmts(L, kid(L, n, 2)) +
               struct_stmts(L, kid(L, n, 3));
    case TO_SWITCH:
        return struct_stmts(L, kid(L, n, 1));
    case TO_CASE: case TO_DEFAULT: case TO_LABEL:
        return struct_stmts(L, kid(L, n, 0));
    default:
        return 0;
    }
}

static void seen_tree(Cfg *L, uint32_t n, uint8_t *seen, unsigned *dup)
{
    uint32_t j;
    if (seen[n]++)
        ++*dup;
    for (j = 0; j < nd(L, n)->nkids; j++)
        seen_tree(L, kid(L, n, j), seen, dup);
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

static void check(Cfg *L, FILE *out, uint32_t root, const uint32_t *ord)
{
    size_t i, k, stmts = 0;
    unsigned dup = 0;
    uint8_t *seen = xcalloc(L->e->nodes.len ? L->e->nodes.len : 1, 1);
    for (i = 0; i < L->npl; i++) {
        uint32_t b = ord[i];
        CBlk *x = &L->b.data[b];
        uint32_t tg[2], nt = 0;
        for (k = 0; k < x->st.len; k++)
            seen_tree(L, x->st.data[k], seen, &dup);
        stmts += x->st.len;
        if (x->val != TN)
            seen_tree(L, x->val, seen, &dup);
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
    if (stmts != struct_stmts(L, root))
        cbad(L, out, ord[0], "%zu statements in the graph, %zu in the tree", stmts,
             struct_stmts(L, root));
    free(seen);
}

void tast_cfg(El *e, uint32_t root, FILE *out)
{
    Cfg L;
    size_t i, k, nun = 0;
    uint32_t *ord;
    U32Vec ij = {0};
    memset(&L, 0, sizeof L);
    L.e = e;
    L.cur = TN;
    place(&L, newb(&L));                    /* the entry */
    lower(&L, root);
    if (!L.why[0] && L.cur != TN)
        ends(&L, TK_RET, TN, TN, TN);       /* falling off the end */
    for (i = 0; i < L.b.len && !L.why[0]; i++)
        if (L.b.data[i].pos == TN && L.b.data[i].used)
            lunsup(&L, "label never defined");
    if (L.why[0]) {
        fprintf(out, "cfg unsupported: %s\n", L.why);
    } else {
        ord = by_pos(&L);
        ijmp_targets(&L, &ij);
        mark(&L, ord[0], &ij);
        for (i = 0; i < L.npl; i++) {
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
        fprintf(out, "cfg: %u blocks, %zu unreachable\n", L.npl, nun);
        if (e->c->opt.verify_cfg)
            check(&L, out, root, ord);
        free(ord);
    }
    for (i = 0; i < L.b.len; i++) {
        vec_free(&L.b.data[i].st);
        vec_free(&L.b.data[i].cases);
    }
    vec_free(&L.b);
    vec_free(&L.labs);
    vec_free(&L.tg);
    vec_free(&ij);
}
