/* cseqpt.c - -Wsequence-point: a port of c-family/c-common.cc verify_tree
 * (split from cexpr.c).  The shared readers are cexpr_int.h's. */
#include "c/cexpr_int.h"

#include <string.h>

/* ---- -Wsequence-point (a port of c-family/c-common.cc verify_tree) -------- */

/* A reference to an object: read when writer is NO_NODE, else written by
 * the node writer (an assignment or an increment). */
typedef struct TL {
    struct TL *next;
    uint32_t expr, writer;
} TL;

typedef struct SaveEnt {
    struct SaveEnt *next;
    uint32_t node;
    TL *before, *after;
} SaveEnt;

typedef struct {
    Checker *c;
    TL **pool;                  /* every allocated entry, freed at the end */
    size_t npool, cappool;
    TL *warned;
    SaveEnt *saves;
    uint32_t *saved;            /* nodes that gcc wraps in a SAVE_EXPR */
    uint32_t skip;
    size_t nsaved, capsaved;
    uint32_t *lnode;            /* locations gcc's fold moved to a comma */
    SrcLoc *lloc;
    size_t nloc, caploc;
} SeqCtx;

static TL *sq_new(SeqCtx *s, TL *next, uint32_t t, uint32_t writer)
{
    TL *l = malloc(sizeof *l);
    if (s->npool == s->cappool) {
        s->cappool = s->cappool ? s->cappool * 2 : 64;
        s->pool = realloc(s->pool, s->cappool * sizeof *s->pool);
    }
    s->pool[s->npool++] = l;
    l->next = next;
    l->expr = t;
    l->writer = writer;
    return l;
}

static bool sq_side(Checker *c, uint32_t x);

static uint32_t sq_strip(Checker *c, uint32_t x)
{
    while (x != NO_NODE && ntag(c, x) == N_PAREN)
        x = first_child(c, x);
    return x;
}

/* Whether the trees of x and y are the same object expression
 * (operand_equal_p): no side effects, same structure. */
static bool sq_eq(Checker *c, uint32_t x, uint32_t y)
{
    x = sq_strip(c, x);
    y = sq_strip(c, y);
    if (x == y)
        return x != NO_NODE;
    return x != NO_NODE && y != NO_NODE && opeq(c, x, y);
}

static bool sq_same(Checker *c, uint32_t x, uint32_t y)
{
    return x == y || (x != NO_NODE && y != NO_NODE && sq_eq(c, x, y));
}

/* warning_candidate_p */
static bool sq_cand(Checker *c, uint32_t x)
{
    switch (ntag(c, x)) {
    case N_IDENT: case N_MEMBER_EXPR: case N_INDEX:
        break;
    case N_UNARY:
        if (npunct(c, x) != P_STAR)
            return false;
        break;
    default:
        return false;
    }
    return (c->ef[x] & EF_LVALUE) && !is_func(c, c->ty[x]) &&
           !is_array(c, c->ty[x]) &&
           !is_void(c, c->ty[x]) && !is_err(c, c->ty[x]);
}

static void sq_add(SeqCtx *s, TL **to, TL *add, uint32_t exclude, bool copy)
{
    while (add) {
        TL *next = add->next;
        if (!copy)
            add->next = *to;
        if (exclude == NO_NODE || !sq_same(s->c, add->writer, exclude))
            *to = copy ? sq_new(s, *to, add->expr, add->writer) : add;
        add = next;
    }
}

static void sq_merge(SeqCtx *s, TL **to, TL *add, bool copy)
{
    TL **end = to;
    while (*end)
        end = &(*end)->next;
    while (add) {
        bool found = false;
        TL *t2, *next = add->next;
        for (t2 = *to; t2; t2 = t2->next)
            if (sq_same(s->c, t2->expr, add->expr)) {
                found = true;
                if (t2->writer == NO_NODE)
                    t2->writer = add->writer;
            }
        if (!found) {
            *end = copy ? sq_new(s, NULL, add->expr, add->writer) : add;
            end = &(*end)->next;
            *end = NULL;
        }
        add = next;
    }
}

static SrcLoc sq_loc(SeqCtx *s, uint32_t n)
{
    for (size_t k = s->nloc; k-- > 0;)
        if (s->lnode[k] == n)
            return s->lloc[k];
    return cnode_loc(s->c, n);
}

static void sq_setloc(SeqCtx *s, uint32_t n, SrcLoc loc)
{
    if (s->nloc == s->caploc) {
        s->caploc = s->caploc ? s->caploc * 2 : 8;
        s->lnode = realloc(s->lnode, s->caploc * sizeof *s->lnode);
        s->lloc = realloc(s->lloc, s->caploc * sizeof *s->lloc);
    }
    s->lnode[s->nloc] = n;
    s->lloc[s->nloc++] = loc;
}

static void sq_collide1(SeqCtx *s, uint32_t written, uint32_t writer, TL *list,
                        bool only_writes)
{
    TL *t;
    for (t = s->warned; t; t = t->next)
        if (sq_same(s->c, t->expr, written))
            return;
    for (; list; list = list->next)
        if (sq_same(s->c, list->expr, written) &&
            !sq_same(s->c, list->writer, writer) &&
            (!only_writes || list->writer != NO_NODE)) {
            s->warned = sq_new(s, s->warned, written, NO_NODE);
            cwarn(s->c, sq_loc(s, writer), "sequence-point",
                  "operation on '%s' may be undefined",
                  estr(s->c, list->expr));
        }
}

static void sq_collide(SeqCtx *s, TL *list)
{
    TL *t;
    for (t = list; t; t = t->next)
        if (t->writer != NO_NODE)
            sq_collide1(s, t->expr, t->writer, list, false);
}

static void sq_verify(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno,
                      uint32_t writer);
static bool sq_truth_wraps(Checker *c, uint32_t e);

/* an operand that gcc truth-converts to `x != 0` first: one more level */
static void sq_verify_t(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno)
{
    if (sq_truth_wraps(s->c, x)) {
        TL *tb = NULL, *tn = NULL;
        sq_verify(s, x, &tb, &tn, NO_NODE);
        sq_merge(s, &tn, tb, false);
        sq_add(s, pno, tn, NO_NODE, false);
    } else {
        sq_verify(s, x, pbefore, pno, NO_NODE);
    }
}

/* verify_tree's default case: the operands of one expression, unordered. */
static void sq_ops(SeqCtx *s, const uint32_t *ops, uint32_t n, TL **pno)
{
    for (uint32_t k = 0; k < n; k++) {
        TL *tb = NULL, *tn = NULL;
        uint32_t o = sq_strip(s->c, ops[k]);
        sq_verify(s, ops[k], &tb, &tn, NO_NODE);
        /* fold hoists a comma operand's left side out: (A, B) + C => A, (B + C) */
        if (!(o != NO_NODE && ntag(s->c, o) == N_BINARY &&
              npunct(s->c, o) == P_COMMA))
            sq_merge(s, &tn, tb, false);
        sq_add(s, pno, tn, NO_NODE, false);
    }
}

static bool sq_saved(SeqCtx *s, uint32_t x)
{
    for (size_t k = 0; k < s->nsaved; k++)
        if (s->saved[k] == x)
            return true;
    return false;
}

static void sq_mark_saved(SeqCtx *s, uint32_t x)
{
    if (s->nsaved == s->capsaved) {
        s->capsaved = s->capsaved ? s->capsaved * 2 : 16;
        s->saved = realloc(s->saved, s->capsaved * sizeof *s->saved);
    }
    s->saved[s->nsaved++] = x;
}

/* Whether a subtree has side effects (TREE_SIDE_EFFECTS). */
static bool sq_side(Checker *c, uint32_t x)
{
    uint32_t k, f = cfirst(c, x);
    for (k = f; k <= x; k++) {
        unsigned t = ntag(c, k);
        if (t == N_CALL || t == N_ASSIGN || t == N_POSTFIX || t == N_STMT_EXPR ||
            (t == N_UNARY && (npunct(c, k) == P_INC || npunct(c, k) == P_DEC)))
            return true;
    }
    return false;
}

/* stabilize_reference: the side-effecting leaves of an lvalue become
 * SAVE_EXPRs, evaluated once although the tree mentions them twice. */
static void sq_stab1(SeqCtx *s, uint32_t x)
{
    Checker *c = s->c;
    uint32_t k[3], n;
    x = sq_strip(c, x);
    if (x == NO_NODE || !sq_side(c, x))
        return;
    switch (ntag(c, x)) {
    case N_BINARY:
        if (npunct(c, x) == P_COMMA || npunct(c, x) == P_ANDAND ||
            npunct(c, x) == P_OROR)
            break;
        n = nkids(c, x, k, 3);
        for (uint32_t j = 0; j < n; j++)
            sq_stab1(s, k[j]);
        return;
    case N_CAST:
        n = nkids(c, x, k, 3);
        if (n >= 2)
            sq_stab1(s, k[1]);
        return;
    case N_UNARY:
        if (npunct(c, x) != P_INC && npunct(c, x) != P_DEC &&
            npunct(c, x) != P_STAR && npunct(c, x) != P_AMP) {
            n = nkids(c, x, k, 3);
            if (n)
                sq_stab1(s, k[0]);
            return;
        }
        break;
    default:
        break;
    }
    sq_mark_saved(s, x);
}

static void sq_stab(SeqCtx *s, uint32_t x)
{
    Checker *c = s->c;
    uint32_t k[3], n;
    x = sq_strip(c, x);
    if (x == NO_NODE)
        return;
    n = nkids(c, x, k, 3);
    switch (ntag(c, x)) {
    case N_UNARY:
        if (npunct(c, x) == P_STAR && n)
            sq_stab1(s, k[0]);
        break;
    case N_INDEX:
        if (n >= 2) {
            if (is_array(c, c->ty[strip_paren(c, k[0])])) {
                sq_stab(s, k[0]);
                sq_stab1(s, k[1]);
            } else {
                sq_stab1(s, k[0]);
                sq_stab1(s, k[1]);
            }
        }
        break;
    case N_MEMBER_EXPR:
        if (n) {
            if (c->nodes[x].flags & NF_ARROW)
                sq_stab1(s, k[0]);
            else
                sq_stab(s, k[0]);
        }
        break;
    default:
        break;
    }
}

static void sq_verify(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno,
                      uint32_t writer)
{
    Checker *c = s->c;
    TL *tmp_before, *tmp_nosp, *tmp_list2, *tmp_list3;
    uint32_t k[64], n;
restart:
    x = sq_strip(c, x);
    if (x == NO_NODE || node_err(c, x))
        return;
    if (sq_saved(s, x) && s->skip != x) {
        SaveEnt *t;
        for (t = s->saves; t; t = t->next)
            if (t->node == x)
                break;
        if (!t) {
            t = malloc(sizeof *t);
            t->next = s->saves;
            s->saves = t;
            t->node = x;
            tmp_before = tmp_nosp = tmp_list3 = NULL;
            s->skip = x;
            sq_verify(s, x, &tmp_before, &tmp_nosp, NO_NODE);
            sq_collide(s, tmp_nosp);
            sq_merge(s, &tmp_list3, tmp_nosp, false);
            t->before = tmp_before;
            t->after = tmp_list3;
        }
        sq_merge(s, pbefore, t->before, true);
        sq_add(s, pno, t->after, NO_NODE, true);
        return;
    }
    s->skip = NO_NODE;
    if (sq_cand(c, x))
        *pno = sq_new(s, *pno, x, writer);
    n = nkids(c, x, k, 64);
    switch (ntag(c, x)) {
    case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
    case N_ALIGNOF_TYPE: case N_COMPOUND_LIT: case N_STMT_EXPR: case N_GENERIC:
        return;
    case N_BINARY:
        if (n >= 2 && npunct(c, x) == P_COMMA && !sq_side(c, k[0]) &&
            !(c->ef[k[0]] & EF_SIDE)) {
            /* fold drops a side-effect-free left operand and gives the right
             * one the comma's location */
            uint32_t r = sq_strip(c, k[1]);
            sq_setloc(s, r, cnode_loc(c, x));
            x = r;
            goto restart;
        }
        if (n >= 2 && (npunct(c, x) == P_ANDAND || npunct(c, x) == P_OROR)) {
            /* fold: a constant left operand picks the result; a constant
             * deciding right operand swallows a side-effect-free left one */
            bool orr = npunct(c, x) == P_OROR;
            uint32_t l = sq_strip(c, k[0]), r = sq_strip(c, k[1]);
            if (c->ck[l] == K_ICE) {
                if ((c->cv[l] != 0) == orr)
                    return;
                x = r;
                goto restart;
            }
            if (c->ck[r] == K_ICE && (c->cv[r] != 0) == orr &&
                !sq_side(c, l) && !(c->ef[l] & EF_SIDE))
                return;
        }
        if (n >= 2 && (npunct(c, x) == P_COMMA || npunct(c, x) == P_ANDAND ||
                       npunct(c, x) == P_OROR)) {
            tmp_before = tmp_nosp = tmp_list2 = tmp_list3 = NULL;
            bool tr = npunct(c, x) != P_COMMA;
            if (tr)
                sq_verify_t(s, k[0], &tmp_before, &tmp_nosp);
            else
                sq_verify(s, k[0], &tmp_before, &tmp_nosp, NO_NODE);
            sq_collide(s, tmp_nosp);
            sq_merge(s, pbefore, tmp_before, false);
            sq_merge(s, pbefore, tmp_nosp, false);
            if (tr)
                sq_verify_t(s, k[1], &tmp_list3, &tmp_list2);
            else
                sq_verify(s, k[1], &tmp_list3, &tmp_list2, NO_NODE);
            sq_collide(s, tmp_list2);
            sq_merge(s, pbefore, tmp_list3, false);
            sq_merge(s, pno, tmp_list2, false);
            return;
        }
        if (n == 2 && sq_eq(c, sq_strip(c, k[0]), sq_strip(c, k[1])) &&
            !sq_side(c, k[0]) && !(c->ef[k[0]] & EF_SIDE) &&
            is_int(c, c->ty[k[0]])) {
            switch (npunct(c, x)) {     /* fold: x op x is a constant */
            case P_MINUS: case P_CARET: case P_LT: case P_GT: case P_LE:
            case P_GE: case P_EQEQ: case P_NE: case P_SLASH: case P_PERCENT:
                return;
            default:
                break;
            }
        }
        sq_ops(s, k, n, pno);
        return;
    case N_COND: {
        uint32_t cond = k[0], a1 = n == 3 ? k[1] : k[0], a2 = k[n - 1];
        if (n < 2)
            return;
        if (c->ck[sq_strip(c, cond)] == K_ICE) {    /* folded to one arm */
            uint32_t arm = c->cv[sq_strip(c, cond)] ? a1 : a2;
            x = arm;
            goto restart;
        }
        tmp_before = tmp_list2 = NULL;
        sq_verify_t(s, cond, &tmp_before, &tmp_list2);
        sq_collide(s, tmp_list2);
        sq_merge(s, pbefore, tmp_before, false);
        sq_merge(s, pbefore, tmp_list2, false);

        tmp_list3 = tmp_nosp = NULL;
        sq_verify(s, a1, &tmp_list3, &tmp_nosp, NO_NODE);
        sq_collide(s, tmp_nosp);
        sq_merge(s, pbefore, tmp_list3, false);

        tmp_list3 = tmp_list2 = NULL;
        sq_verify(s, a2, &tmp_list3, &tmp_list2, NO_NODE);
        sq_collide(s, tmp_list2);
        sq_merge(s, pbefore, tmp_list3, false);
        sq_merge(s, &tmp_nosp, tmp_list2, false);
        sq_add(s, pno, tmp_nosp, NO_NODE, false);
        return;
    }
    case N_POSTFIX:
        if (n >= 1)
            sq_verify(s, k[0], pno, pno, x);
        return;
    case N_UNARY:
        switch (npunct(c, x)) {
        case P_INC: case P_DEC:
            if (n >= 1) {
                sq_stab(s, k[0]);
                sq_verify(s, k[0], pno, pno, x);
            }
            return;
        case P_AMP:
            if (n >= 1) {
                uint32_t o = sq_strip(c, k[0]);
                if (o != NO_NODE && ntag(c, o) == N_IDENT)
                    return;
                x = o;
                writer = NO_NODE;
                goto restart;
            }
            return;
        case P_STAR:
            sq_ops(s, k, n, pno);
            return;
        default:
            if (n >= 1) {
                x = k[0];
                writer = NO_NODE;
                goto restart;
            }
            return;
        }
    case N_CAST:
        if (n >= 2) {
            x = k[1];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_VA_ARG:
        if (n >= 1) {
            x = k[0];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_MEMBER_EXPR:
        if (n >= 1) {
            x = k[0];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_INDEX:
        sq_ops(s, k, n < 2 ? n : 2, pno);
        return;
    case N_ASSIGN: {
        int op = npunct(c, x);
        if (n < 2)
            return;
        tmp_before = tmp_nosp = tmp_list3 = NULL;
        if (op != P_ASSIGN)
            sq_stab(s, k[0]);
        if (op == P_ASSIGN) {
            sq_verify(s, k[1], &tmp_before, &tmp_nosp, NO_NODE);
        } else {
            /* lhs = lhs op rhs: the right side is a binary expression */
            uint32_t two[2];
            two[0] = k[0];
            two[1] = k[1];
            sq_ops(s, two, 2, &tmp_nosp);
        }
        sq_verify(s, k[0], &tmp_list3, &tmp_list3, x);
        sq_add(s, &tmp_before, tmp_list3, x, true);
        sq_collide(s, tmp_before);
        sq_add(s, pno, tmp_list3, x, false);
        sq_collide1(s, k[0], x, tmp_nosp, true);

        sq_merge(s, pbefore, tmp_before, false);
        if (sq_cand(c, sq_strip(c, k[0])))
            sq_merge(s, &tmp_nosp, sq_new(s, NULL, sq_strip(c, k[0]), x), false);
        sq_add(s, pno, tmp_nosp, NO_NODE, true);
        return;
    }
    case N_CALL: {
        uint32_t j;
        tmp_before = tmp_nosp = NULL;
        if (n < 1)
            return;
        sq_verify(s, k[0], &tmp_before, &tmp_nosp, NO_NODE);
        for (j = 1; j < n; j++) {
            tmp_list2 = tmp_list3 = NULL;
            sq_verify(s, k[j], &tmp_list2, &tmp_list3, NO_NODE);
            sq_merge(s, &tmp_list3, tmp_list2, false);
            sq_add(s, &tmp_before, tmp_list3, NO_NODE, false);
        }
        sq_add(s, &tmp_before, tmp_nosp, NO_NODE, false);
        sq_collide(s, tmp_before);
        sq_add(s, pbefore, tmp_before, NO_NODE, false);
        return;
    }
    default:
        return;
    }
}

/* A full expression: gcc's verify_sequence_points. */
bool sq_for_cond(Checker *c, uint32_t p, uint32_t i)
{
    uint32_t kids[32], cnt = node_children(c->nodes, p, kids, 32);
    return cnt > 2 && kids[2] == i;
}

static bool sq_truth_wraps(Checker *c, uint32_t e)
{
    e = sq_strip(c, e);
    switch (ntag(c, e)) {
    case N_COND:
        return false;
    case N_UNARY:
        return npunct(c, e) != P_BANG;
    case N_BINARY:
        switch (npunct(c, e)) {
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR: case P_COMMA:
            return false;
        case P_MINUS: case P_CARET:     /* fold turns `a - b != 0` into `a != b` */
            return !is_int(c, c->ty[e]);
        default:
            return true;
        }
    default:
        return true;
    }
}

void sq_check(Checker *c, uint32_t e, bool cond)
{
    SeqCtx s;
    TL *before = NULL, *after = NULL;
    if (!diag_enabled(c->diag, "sequence-point") || node_err(c, e) ||
        e - cfirst(c, e) > 400)
        return;
    memset(&s, 0, sizeof s);
    s.c = c;
    s.skip = NO_NODE;
    if (cond && sq_truth_wraps(c, e)) {
        /* gcc checks `e != 0`: one more level of operand merging */
        sq_ops(&s, &e, 1, &after);
    } else if (c->par[e] != NO_NODE && ntag(c, c->par[e]) == N_RETURN) {
        /* c_finish_return checks `<result> = e`: an assignment to a
         * variable the check does not track */
        TL *tb = NULL, *tn = NULL;
        sq_verify(&s, e, &tb, &tn, NO_NODE);
        sq_collide(&s, tb);
        sq_merge(&s, &before, tb, false);
        sq_add(&s, &after, tn, NO_NODE, true);
    } else {
        sq_verify(&s, e, &before, &after, NO_NODE);
    }
    sq_collide(&s, after);
    for (size_t k = 0; k < s.npool; k++)
        free(s.pool[k]);
    free(s.pool);
    free(s.saved);
    free(s.lnode);
    free(s.lloc);
    while (s.saves) {
        SaveEnt *nx = s.saves->next;
        free(s.saves);
        s.saves = nx;
    }
}
