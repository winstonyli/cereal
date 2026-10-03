/* -Warray-parameter= and -Wvla-parameter: a function redeclared with an array
 * parameter whose bound differs from the first declaration's (gcc's
 * warn_parm_array_mismatch and warn_parm_ptrarray_mismatch).
 *
 * Every prototype records, per parameter, how it was declared: a pointer, an
 * array (the bound written in the first brackets) or a pointer to an array,
 * and the bounds of each bracket pair.  The first declaration's record stays
 * with the symbol; each redeclaration is compared with it. */

#include "c/check_int.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TT (&c->tt)

static int tpunct(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return P_NONE;
    t = &c->u->toks[tok].t;
    return t->kind == TK_PUNCT ? t->punct : P_NONE;
}

static unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static SrcLoc tloc(const Checker *c, uint32_t tok)
{
    return tok >= c->u->ntoks ? c->last_bol : ctok_loc(c, tok);
}

enum { D_NONE, D_CONST, D_STAR, D_EXPR };

typedef struct PDim {
    uint8_t k;               /* D_* */
    uint64_t n;              /* D_CONST */
    char *txt;               /* D_EXPR: the expression, as gcc names a bound */
    char *ttxt;              /* ... and inside a type: f(0), not f (0) */
    bool inner;              /* D_STAR past the first brackets: printed 0 */
    uint32_t arg;            /* D_EXPR naming a parameter: its position, 1-based */
    char *key;               /* D_EXPR: txt with a commutative top operator sorted */
    bool bad;                /* D_EXPR: not an integer (gcc reports an error) */
} PDim;

enum { L_PTR, L_ARR, L_FUN };

typedef struct PLev {            /* one declarator level, the parameter's own first */
    uint8_t k;               /* L_* */
    unsigned quals;          /* L_PTR: the pointer's qualifiers */
    char *txt;               /* L_FUN: "(int, char *)" */
} PLev;

typedef struct PParm {
    uint32_t nl;             /* levels recorded (nested declarators); 0: use np */
    PLev *lv;
    bool arr;                /* declared as an array */
    bool stat;               /* [static n] */
    bool rst;                /* a restrict-qualified pointer */
    bool unk;                /* a variable-length typedef: not described */
    bool named;              /* loc is the parameter's name */
    unsigned np;             /* pointer levels around the brackets (arr unset) */
    unsigned quals;          /* qualifiers inside the first brackets */
    uint32_t nd;             /* the bracket pairs; 0: any other type */
    PDim *d;                 /* natural order: d[0] is the first brackets */
    TypeId ty;               /* nd == 0: the parameter's type */
    TypeId base;             /* nd > 0: the element type */
    SrcLoc loc;
} PParm;

typedef struct CParmDesc {
    uint32_t n;
    PParm p[1];
} CParmDesc;

static const CParmDesc *desc_of(const Checker *c, uint32_t d)
{
    return (const CParmDesc *)c->pdescs.data[d - 1];
}

static void pdesc_free(CParmDesc *pd)
{
    uint32_t i, j;
    for (i = 0; i < pd->n; i++) {
        for (j = 0; j < pd->p[i].nd; j++)
            {
                free(pd->p[i].d[j].txt);
                free(pd->p[i].d[j].ttxt);
                free(pd->p[i].d[j].key);
            }
        free(pd->p[i].d);
        for (j = 0; j < pd->p[i].nl; j++)
            free(pd->p[i].lv[j].txt);
        free(pd->p[i].lv);
    }
    free(pd);
}

void cparm_free(Checker *c)
{
    size_t i;
    for (i = 0; i < c->pdescs.len; i++)
        if (c->pdescs.data[i])
            pdesc_free((CParmDesc *)c->pdescs.data[i]);
    vec_free(&c->pdescs);
}

/* cdecl.c param_visit keeps the pre-decay array type in CV[p] bits 32+. */
static TypeId pre_of(const Checker *c, uint32_t node)
{
    return (TypeId)(c->cv[node] >> 32);
}

/* The array dimensions typedef'd into type e, appended after the declarator's;
 * false when one is variable. */
static bool tail_dims(Checker *c, PParm *o, TypeId e)
{
    for (;;) {
        TypeKind k = type_ckind(TT, e);
        const TypeEnt *en;
        PDim *d;
        if (k == TY_VLA)
            return false;
        if (k != TY_ARRAY)
            break;
        e = type_canon(TT, e);
        en = type_ent(TT, e);
        o->d = xrealloc(o->d, (o->nd + 1) * sizeof *o->d);
        d = &o->d[o->nd++];
        memset(d, 0, sizeof *d);
        if (!(en->flags & TF_INCOMPLETE) && en->n) {
            d->k = D_CONST;
            d->n = en->n;
        }
        e = type_base(TT, e);
    }
    o->base = e;
    return true;
}


/* ---- building ------------------------------------------------------------------ */

static uint32_t min_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = c->nodes[i].tok;
    for (k = cfirst(c, i); k < i; k++)
        if (c->nodes[k].tok < m)
            m = c->nodes[k].tok;
    return m;
}

static void dim_of(Checker *c, uint32_t dn, PDim *d, bool first,
                   char *const *names, uint32_t nnames)
{
    uint32_t sz = cdecl_array_size_node(c, dn), j;
    char *r, *w;
    memset(d, 0, sizeof *d);
    if ((cnode(c, dn)->flags & NF_STAR) && !(c->cv[dn] & 1)) {
        /* gcc keeps only the first [*]; the others become [0] */
        d->k = D_STAR;
        d->inner = !first;
        return;
    }
    if (sz == NO_NODE)
        return;
    if (c->ck[sz] == K_ICE || c->ck[sz] == K_FOLD) {
        int64_t v = cexpr_sval(c, sz);
        if (v > 0) {
            d->k = D_CONST;
            d->n = (uint64_t)v;
        }
        return;
    }
    d->k = D_EXPR;
    d->bad = !type_is_integer(TT, c->ty[sz]);
    d->txt = xstrdup(cexpr_str(c, sz));
    if (ntag(c, sz) == N_BINARY &&
        (tpunct(c, c->nodes[sz].tok) == P_PLUS ||
         tpunct(c, c->nodes[sz].tok) == P_STAR)) {
        uint32_t k2[3];
        if (node_children(c->nodes, sz, k2, 3) == 2) {
            char *a = xstrdup(cexpr_str(c, k2[0])), *b = xstrdup(cexpr_str(c, k2[1]));
            size_t l = strlen(a) + strlen(b) + 4;
            d->key = xmalloc(l);
            snprintf(d->key, l, "%s%c%s", strcmp(a, b) <= 0 ? a : b,
                     tpunct(c, c->nodes[sz].tok) == P_PLUS ? 43 : 42,
                     strcmp(a, b) <= 0 ? b : a);
            free(a);
            free(b);
        }
    }
    d->ttxt = xstrdup(d->txt);
    {
        /* gcc's %E spells (T) x and a leading ++x with a space */
        size_t i, n = strlen(d->txt), k = 0;
        char *t = xmalloc(n * 2 + 2);
        if (!strncmp(d->txt, "++", 2) || !strncmp(d->txt, "--", 2))
            t[k++] = 32;
        for (i = 0; i < n; i++) {
            t[k++] = d->txt[i];
            if (d->txt[i] == 41 && (isalnum((unsigned char)d->txt[i + 1]) ||
                                    d->txt[i + 1] == 95 || d->txt[i + 1] == 40))
                t[k++] = 32;
        }
        t[k] = 0;
        free(d->txt);
        d->txt = t;
    }
    for (r = w = d->ttxt; *r; r++) {
        /* gcc prints a call as f(0) */
        if (*r == 32 && r[1] == 40 && w > d->ttxt &&
            (isalnum((unsigned char)w[-1]) || w[-1] == 95))
            continue;
        *w++ = *r;
    }
    *w = 0;
    for (j = 0; j < nnames; j++)
        if (names[j] && !strcmp(names[j], d->txt))
            d->arg = j + 1;
}

/* "(int, char *)": the parameter list of function type f. */
static char *fparams(Checker *c, TypeId f)
{
    StrBuf sb = {0};
    const TypeEnt *en = type_ent(TT, type_canon(TT, f));
    const TypeId *ps = type_params(TT, type_canon(TT, f));
    uint64_t k;
    char *r;
    sb_putc(&sb, '(');
    if (!en->n && !(en->flags & TF_NOPROTO))
        sb_puts(&sb, "void");
    for (k = 0; k < en->n; k++) {
        StrBuf t = {0};
        type_print(TT, &t, ps[k]);
        if (k)
            sb_puts(&sb, ", ");
        sb_puts(&sb, sb_cstr(&t));
        sb_free(&t);
    }
    if (en->flags & TF_VARIADIC)
        sb_puts(&sb, en->n ? ", ..." : "...");
    sb_putc(&sb, ')');
    r = xstrdup(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

/* Declarators with a function level or an array past a pointer: record every
 * level (w[0] is the one nearest the base type) so the type can be printed. */
static void levels_of(Checker *c, uint32_t p, PParm *o, const uint32_t *w,
                      uint32_t nw, char *const *names, uint32_t nnames)
{
    TypeId t = o->ty, pre = pre_of(c, p);
    uint32_t i, nd = 0;
    if (nw && ntag(c, w[nw - 1]) == N_ARRAY) {
        if (!pre)
            return;
        t = pre;
    } else if (!nw || ntag(c, w[nw - 1]) != N_PTR) {
        return;
    }
    o->lv = xcalloc(nw, sizeof *o->lv);
    o->d = xcalloc(nw, sizeof *o->d);
    o->nl = nw;
    for (i = 0; i < nw; i++) {
        uint32_t d = w[nw - 1 - i];
        PLev *l = &o->lv[i];
        TypeKind k = type_ckind(TT, t);
        switch (ntag(c, d)) {
        case N_ARRAY:
            if (k != TY_ARRAY && k != TY_VLA)
                goto bad;
            l->k = L_ARR;
            dim_of(c, d, &o->d[nd], nd == 0 && i == 0, names, nnames);
            if (o->d[nd].bad)
                o->unk = true;
            if (i == 0) {
                o->stat = (cnode(c, d)->flags & NF_STATIC) != 0;
                o->quals = cdecl_quals_of(c, d);
            }
            nd++;
            break;
        case N_PTR:
            if (k != TY_PTR)
                goto bad;
            l->k = L_PTR;
            l->quals = TYPE_QUALS(t);
            break;
        default:
            if (k != TY_FUNC)
                goto bad;
            l->k = L_FUN;
            l->txt = fparams(c, t);
            break;
        }
        t = type_base(TT, t);
    }
    o->nd = nd;
    o->arr = o->lv[0].k == L_ARR;
    o->base = t;
    if (!nd)
        goto bad;
    if (!tail_dims(c, o, t))
        o->unk = true;
    else {
        o->lv = xrealloc(o->lv, (o->nl + o->nd - nd) * sizeof *o->lv);
        for (i = nd; i < o->nd; i++) {
            memset(&o->lv[o->nl], 0, sizeof *o->lv);
            o->lv[o->nl++].k = L_ARR;
        }
    }
    return;
bad:
    for (i = 0; i < o->nl; i++)
        free(o->lv[i].txt);
    for (i = 0; i < nd; i++) {
        free(o->d[i].txt);
        free(o->d[i].ttxt);
        free(o->d[i].key);
    }
    free(o->lv);
    free(o->d);
    o->lv = NULL;
    o->d = NULL;
    o->nl = o->nd = 0;
    o->arr = o->stat = false;
    o->quals = 0;
}

/* How parameter node p was declared. */
static void parm_of(Checker *c, uint32_t p, PParm *o, char *const *names,
                    uint32_t nnames)
{
    uint32_t kids[32], nk, l = NO_NODE, d, dn[16], nd = 0, m, w[32], nw = 0;
    bool ptr_after = false, other = false;
    TypeId t = c->ty[p], pt, e;
    memset(o, 0, sizeof *o);
    o->ty = t;
    o->rst = type_ckind(TT, t) == TY_PTR && (TYPE_QUALS(t) & TQ_RESTRICT);
    for (m = cfirst(c, p); m <= p; m++)
        if (ntag(c, m) == N_NAME) {
            o->loc = cnode_loc(c, m);
            o->named = true;
            break;
        }
    if (m > p)
        o->loc = tloc(c, min_tok(c, p));
    if (c->nodes[p].size > 1 && ntag(c, p - 1) != N_ATTRIBUTE) {
        l = p - 1;                 /* the last child comes right before its parent */
    } else {
        nk = node_children(c->nodes, p, kids, 32);
        for (m = nk > 32 ? 32 : nk; m > 0; m--)
            if (ntag(c, kids[m - 1]) != N_ATTRIBUTE) {
                l = kids[m - 1];
                break;
            }
    }
    if (l != NO_NODE && !is_declarator_tag(ntag(c, l)))
        l = NO_NODE;
    if (type_ckind(TT, t) != TY_PTR)
        return;
    for (d = l; d != NO_NODE; d = cdecl_inner_decl(c, d)) {
        unsigned tg = ntag(c, d);
        if (nw < 32 && (tg == N_ARRAY || tg == N_PTR || tg == N_FUNC))
            w[nw++] = d;
        else if (nw == 32)
            other = true;
        if (tg == N_ARRAY) {
            if (nd == 16 || ptr_after)
                other = true;
            else
                dn[nd++] = d;
        } else if (tg == N_PTR) {
            if (nd) {
                ptr_after = true;
                o->np++;
            }
        } else if (tg == N_FUNC) {
            other = true;
        }
    }
    if (other) {
        if (nw < 32)
            levels_of(c, p, o, w, nw, names, nnames);
        return;
    }
    if (!nd) {
        /* no brackets written: an array typedef, or a pointer to one */
        TypeId pre = pre_of(c, p), q = pre ? pre : type_base(TT, t);
        if (type_ckind(TT, q) != TY_ARRAY)
            return;
        o->arr = pre != 0;
        if (!tail_dims(c, o, q)) {
            free(o->d);
            o->d = NULL;
            o->nd = 0;
            o->unk = true;
        }
        return;
    }
    /* the brackets are nested outermost-last: reverse into source order */
    o->d = xcalloc(nd, sizeof *o->d);
    o->nd = nd;
    for (m = 0; m < nd; m++)
        dim_of(c, dn[nd - 1 - m], &o->d[m], m == 0 && !ptr_after, names,
                nnames);
    for (m = 0; m < nd; m++)
        if (o->d[m].bad)
            o->unk = true;
    o->arr = !ptr_after;
    if (o->arr) {
        const Node *an = cnode(c, dn[nd - 1]);
        o->stat = (an->flags & NF_STATIC) != 0;
        o->quals = cdecl_quals_of(c, dn[nd - 1]);
    }
    pt = type_base(TT, t);
    for (m = 1; m < o->np && type_ckind(TT, pt) == TY_PTR; m++)
        pt = type_base(TT, pt);
    e = pt;
    for (m = o->arr ? 1 : 0; m < nd; m++) {
        TypeKind k = type_ckind(TT, e);
        if (k != TY_ARRAY && k != TY_VLA)
            break;
        e = type_base(TT, e);
    }
    o->base = e;
    if (!tail_dims(c, o, e)) {
        for (m = 0; m < o->nd; m++) {
            free(o->d[m].txt);
            free(o->d[m].ttxt);
            free(o->d[m].key);
        }
        free(o->d);
        o->d = NULL;
        o->nd = 0;
        o->arr = false;
        o->unk = true;
    }
}

/* Is parameter j (0-based) of a function with record d declared as a restrict pointer? */
bool cparm_restrict(Checker *c, uint32_t d, uint32_t j)
{
    const CParmDesc *pd = d ? (const CParmDesc *)c->pdescs.data[d - 1] : NULL;
    return pd && j < pd->n && pd->p[j].rst;
}

uint32_t cparm_make(Checker *c, uint32_t fnode)
{
    uint32_t kids[64], n, j, np = 0, i;
    CParmDesc *pd;
    uint32_t pn[64];
    char *names[64];
    if (fnode == NO_NODE || (cnode(c, fnode)->flags & NF_KR))
        return 0;
    n = node_children(c->nodes, fnode, kids, 64);
    for (j = 0; j < n && j < 64; j++) {
        uint32_t p = kids[j];
        bool named;
        if (!is_real_param(c, p))
            continue;
        named = c->cv[p] & 1;
        if (!named && type_ckind(TT, c->ty[p]) == TY_VOID)
            continue;
        pn[np] = p;
        names[np] = NULL;
        np++;
    }
    if (!np)
        return 0;
    pd = xcalloc(1, sizeof *pd + (np - 1) * sizeof pd->p[0]);
    pd->n = np;
    for (i = 0; i < np; i++)
        if (c->cv[pn[i]] & 1 && c->cb[pn[i]])
            names[i] = (char *)cident(c, csym(c, c->cb[pn[i]] - 1)->name);
    for (i = 0; i < np; i++)
        parm_of(c, pn[i], &pd->p[i], names, np);
    vec_push(&c->pdescs, (struct CParmDesc *)pd);
    return (uint32_t)c->pdescs.len;
}

unsigned cparm_implied(Checker *c, uint32_t d, CImplied *out, unsigned max)
{
    const CParmDesc *pd = d ? desc_of(c, d) : NULL;
    unsigned i, n = 0;
    for (i = 0; pd && i < pd->n && n < max; i++) {
        const PParm *p = &pd->p[i];
        if (p->arr && p->nd && p->d[0].k == D_STAR && !p->d[0].inner) {
            out[n].ptr = i + 1;
            out[n].size = 0;
            out[n].bloc = p->loc;
            out[n].bnamed = false;
            n++;
        } else if (p->arr && p->nd && p->d[0].k == D_EXPR && p->d[0].arg &&
            p->d[0].arg <= pd->n) {
            out[n].ptr = i + 1;
            out[n].size = p->d[0].arg;
            out[n].bloc = pd->p[p->d[0].arg - 1].loc;
            out[n].bnamed = pd->p[p->d[0].arg - 1].named;
            n++;
        }
    }
    return n;
}

/* A redeclaration whose record was not kept: forget it again. */
void cparm_release(Checker *c, uint32_t d)
{
    if (d && d == c->pdescs.len) {
        pdesc_free((CParmDesc *)c->pdescs.data[d - 1]);
        c->pdescs.len--;
    }
}

/* ---- printing ------------------------------------------------------------------ */

static void put_dim(StrBuf *sb, const PDim *d)
{
    switch (d->k) {
    case D_CONST:
        sb_printf(sb, "%" PRIu64, d->n);
        break;
    case D_STAR:
        sb_putc(sb, d->inner ? '0' : '*');
        break;
    case D_EXPR:
        sb_puts(sb, d->ttxt);
        break;
    default:
        break;
    }
}

/* One pair of brackets; the first ones of an array parameter carry
 * [static const n]. */
static void put_arr(StrBuf *sb, const PParm *p, uint32_t i)
{
    bool sp = false;
    sb_putc(sb, '[');
    if (i == 0 && p->arr) {
        if (p->stat) {
            sb_puts(sb, "static");
            sp = true;
        }
        if (p->quals & TQ_CONST) {
            sb_puts(sb, sp ? " const" : "const");
            sp = true;
        }
        if (p->quals & TQ_VOLATILE) {
            sb_puts(sb, sp ? " volatile" : "volatile");
            sp = true;
        }
        if (p->quals & TQ_RESTRICT) {
            sb_puts(sb, sp ? " restrict" : "restrict");
            sp = true;
        }
        if (p->quals & TQ_ATOMIC) {
            sb_puts(sb, sp ? " _Atomic" : "_Atomic");
            sp = true;
        }
        if (sp && p->d[i].k != D_NONE)
            sb_putc(sb, ' ');
        if (sp && p->d[i].k == D_EXPR)
            sb_putc(sb, ' ');
    }
    put_dim(sb, &p->d[i]);
    sb_putc(sb, ']');
}

/* A recorded declarator: wrap from the parameter's own level outwards, as
 * gcc's type printer does (void (* (*[2])[n])(void)). */
static void put_levels(Checker *c, StrBuf *sb, const PParm *p)
{
    char *s = xstrdup("");
    uint32_t i, di = 0;
    StrBuf t = {0};
    type_print(TT, sb, p->base);
    for (i = 0; i < p->nl; i++) {
        const PLev *l = &p->lv[i];
        char *r;
        t.len = 0;
        if (l->k == L_PTR) {
            sb_putc(&t, '*');
            if (l->quals & TQ_CONST)
                sb_puts(&t, " const");
            if (l->quals & TQ_VOLATILE)
                sb_puts(&t, " volatile");
            if (l->quals & TQ_RESTRICT)
                sb_puts(&t, " restrict");
            if (*s == '(' || (l->quals && *s && *s != '[' && *s != ')'))
                sb_putc(&t, 32);
            sb_puts(&t, s);
        } else {
            if (*s == '*') {
                sb_putc(&t, '(');
                sb_puts(&t, s);
                sb_putc(&t, ')');
            } else {
                sb_puts(&t, s);
            }
            if (l->k == L_ARR)
                put_arr(&t, p, di++);
            else
                sb_puts(&t, l->txt);
        }
        r = xstrdup(sb_cstr(&t));
        free(s);
        s = r;
    }
    sb_putc(sb, 32);
    sb_puts(sb, s);
    free(s);
    sb_free(&t);
}

/* The parameter's type as gcc prints it: int[n + 1], int (*)[2], int *. */
static const char *pstr(Checker *c, StrBuf *sb, const PParm *p)
{
    uint32_t i;
    const char *b;
    size_t bl;
    sb->len = 0;
    if (!p->nd) {
        type_print(TT, sb, p->ty);
        return sb_cstr(sb);
    }
    if (p->nl) {
        put_levels(c, sb, p);
        return sb_cstr(sb);
    }
    type_print(TT, sb, p->base);
    b = sb_cstr(sb);
    bl = strlen(b);
    if (!p->arr) {
        unsigned k;
        if (!(bl && b[bl - 1] == '*'))
            sb_putc(sb, 32);
        sb_putc(sb, 40);
        for (k = 0; k < (p->np ? p->np : 1); k++)
            sb_putc(sb, '*');
        sb_putc(sb, 41);
    }
    for (i = 0; i < p->nd; i++) {
        bool sp = false;
        sb_putc(sb, '[');
        if (i == 0 && p->arr) {
            if (p->stat) {
                sb_puts(sb, "static");
                sp = true;
            }
            if (p->quals & TQ_CONST) {
                sb_puts(sb, sp ? " const" : "const");
                sp = true;
            }
            if (p->quals & TQ_VOLATILE) {
                sb_puts(sb, sp ? " volatile" : "volatile");
                sp = true;
            }
            if (p->quals & TQ_RESTRICT) {
                sb_puts(sb, sp ? " restrict" : "restrict");
                sp = true;
            }
            if (p->quals & TQ_ATOMIC) {
                sb_puts(sb, sp ? " _Atomic" : "_Atomic");
                sp = true;
            }
            if (sp && p->d[i].k != D_NONE)
                sb_putc(sb, ' ');
            if (sp && p->d[i].k == D_EXPR)
                sb_putc(sb, ' ');
        }
        put_dim(sb, &p->d[i]);
        sb_putc(sb, ']');
    }
    return sb_cstr(sb);
}

/* "argument 2" or "'f (0)'": a bound expression as gcc names it. */
static const char *bstr(StrBuf *sb, const PDim *d)
{
    sb->len = 0;
    if (d->arg)
        sb_printf(sb, "argument %u", d->arg);
    else
        sb_printf(sb, "'%s'", d->txt);
    return sb_cstr(sb);
}

/* ---- comparing ----------------------------------------------------------------- */

static unsigned count_vla(const PParm *p, bool star_only)
{
    unsigned i, n = 0;
    for (i = 0; i < p->nd; i++)
        if (p->d[i].k == D_STAR || (!star_only && p->d[i].k == D_EXPR))
            n++;
    return n;
}

static bool dim_same(const PDim *a, const PDim *b)
{
    uint8_t ka = a->k == D_STAR && a->inner ? D_NONE : a->k;
    uint8_t kb = b->k == D_STAR && b->inner ? D_NONE : b->k;
    if (ka != kb)
        return false;
    switch (a->k) {
    case D_CONST:
        return a->n == b->n;
    case D_EXPR:
        return a->arg || b->arg ? a->arg == b->arg :
               !strcmp(a->key ? a->key : a->txt, b->key ? b->key : b->txt);
    default:
        return true;
    }
}

/* The first brackets hold a variable bound. */
static bool vla0(const PParm *p)
{
    return p->nd && p->d[0].k >= D_STAR;
}

/* A variable length array as gcc sees it: a first [*] or any bound that is
 * not constant (an inner [*] is just [0]). */
static bool vlaany(const PParm *p)
{
    unsigned i;
    if (vla0(p))
        return true;
    for (i = 0; i < p->nd; i++)
        if (p->d[i].k == D_EXPR)
            return true;
    return false;
}

static const char *plural(unsigned n)
{
    return n == 1 ? "" : "s";
}

static void cmp_param(Checker *c, const PParm *o, const PParm *n, unsigned no)
{
    StrBuf ns = {0}, os = {0}, bs = {0}, bo = {0};
    Diagnostic *d;
    unsigned olv = count_vla(o, false), nlv = count_vla(n, false);
    int lvl = diag_option_level(c->diag, "array-parameter=", 2);
    if (o->unk || n->unk)
        goto out;
    if (!n->arr && !o->arr) {
        /* pointers to arrays: the bounds past the first */
        unsigned i, cnt = 0, w = 0;
        bool vla = false;
        StrBuf lst = {0};
        if (!n->nd || n->nd != o->nd)
            goto out;
        for (i = 0; i < n->nd; i++)
            if (!dim_same(&o->d[i], &n->d[i])) {
                cnt++;
                if (n->d[i].k == D_EXPR || o->d[i].k == D_EXPR)
                    vla = true;
                if (w++)
                    sb_puts(&lst, ", ");
                sb_printf(&lst, "%u", i + 1);
            }
        if (cnt) {
            d = cwarn_d(c, DL_WARNING, n->loc, vla ? "vla-parameter" :
                        "array-parameter=", "mismatch in bound%s %s of "
                        "argument %u declared as '%s'", plural(cnt),
                        sb_cstr(&lst), no, pstr(c, &ns, n));
            if (d)
                cnote(c, d, o->loc, "previously declared as '%s'",
                      pstr(c, &os, o));
        }
        sb_free(&lst);
        goto out;
    }
    if (!n->arr) {
        /* declared as an array first, now as a pointer */
        if (o->d[0].k == D_NONE) {
            if (vlaany(o)) {
                d = cwarn_d(c, DL_WARNING, n->loc, "vla-parameter", "argument "
                            "%u of type '%s' declared as an ordinary array",
                            no, pstr(c, &ns, n));
                if (d)
                    cnote(c, d, o->loc, "previously declared as a variable "
                          "length array '%s'", pstr(c, &os, o));
            }
            goto out;
        }
        d = cwarn_d(c, DL_WARNING, n->loc, vla0(o) ? "vla-parameter" :
                    "array-parameter=", "argument %u of type '%s' declared as "
                    "a pointer", no, pstr(c, &ns, n));
        if (d)
            cnote(c, d, o->loc, "previously declared as %s '%s'",
                  vla0(o) ? "a variable length array" : "an array",
                  pstr(c, &os, o));
        goto out;
    }
    if (!o->arr) {
        /* declared as a pointer first, now as an array */
        if (vlaany(n)) {
            d = cwarn_d(c, DL_WARNING, n->loc, "vla-parameter", "argument "
                        "%u of type '%s' declared as a variable length array",
                        no, pstr(c, &ns, n));
            if (d)
                cnote(c, d, o->loc, "previously declared as a pointer '%s'",
                      pstr(c, &os, o));
        } else if (n->d[0].k != D_NONE && (lvl >= 2 || n->stat)) {
            d = cwarn_d(c, DL_WARNING, n->loc, "array-parameter=", "argument "
                        "%u of type '%s' with mismatched bound", no,
                        pstr(c, &ns, n));
            if (d)
                cnote(c, d, o->loc, "previously declared as '%s'",
                      pstr(c, &os, o));
        }
        goto out;
    }
    if (vlaany(o) != vlaany(n)) {
        d = cwarn_d(c, DL_WARNING, n->loc, "vla-parameter", "argument %u of "
                    "type '%s' declared as %s", no, pstr(c, &ns, n),
                    vlaany(n) ? "a variable length array" : "an ordinary array");
        if (d)
            cnote(c, d, o->loc, "previously declared as %s '%s'",
                  vlaany(o) ? "a variable length array" : "an ordinary array",
                  pstr(c, &os, o));
        goto out;
    }
    if (vlaany(n)) {
        unsigned ost = count_vla(o, true), nst = count_vla(n, true), i, j = 0;
        if (!(ost && nst) && olv != nlv) {
            d = cwarn_d(c, DL_WARNING, n->loc, "vla-parameter", "argument %u "
                        "of type '%s' declared with %u variable bound%s", no,
                        pstr(c, &ns, n), nlv, plural(nlv));
            if (d)
                cnote(c, d, o->loc, "previously declared as '%s' with %u "
                      "variable bound%s", pstr(c, &os, o), olv, plural(olv));
            goto out;
        }
        if (!ost != !nst) {
            if (nst > ost) {
                d = cwarn_d(c, DL_WARNING, n->loc, "vla-parameter", "argument "
                            "%u of type '%s' declared with %u unspecified "
                            "variable bound%s", no, pstr(c, &ns, n), nst,
                            plural(nst));
                if (d)
                    cnote(c, d, o->loc, "previously declared as '%s' with %u "
                          "unspecified variable bound%s", pstr(c, &os, o), ost,
                          plural(ost));
            } else {
                d = cwarn_d(c, DL_WARNING, o->loc, "vla-parameter", "argument "
                            "%u of type '%s' declared with %u unspecified "
                            "variable bound%s", no, pstr(c, &os, o), ost,
                            plural(ost));
                if (d)
                    cnote(c, d, n->loc, "subsequently declared as '%s' with %u "
                          "unspecified variable bound%s", pstr(c, &ns, n), nst,
                          plural(nst));
            }
            goto out;
        }
        /* the variable bounds, in order */
        for (i = 0; i < n->nd; i++) {
            const PDim *nb = &n->d[i], *ob;
            if (nb->k < D_STAR)
                continue;
            while (j < o->nd && o->d[j].k < D_STAR)
                j++;
            if (j >= o->nd)
                break;
            ob = &o->d[j++];
            if (nb->k == D_EXPR && ob->k == D_EXPR && !dim_same(ob, nb)) {
                d = cwarn_d(c, DL_WARNING, n->loc, "vla-parameter", "argument "
                            "%u of type '%s' declared with mismatched bound "
                            "%s", no, pstr(c, &ns, n), bstr(&bs, nb));
                if (d)
                    cnote(c, d, o->loc, "previously declared as '%s' with "
                          "bound %s", pstr(c, &os, o), bstr(&bo, ob));
            }
        }
        if (n->d[0].k < D_STAR || o->d[0].k < D_STAR) {
            if (!dim_same(&o->d[0], &n->d[0]) && (lvl >= 2 || n->stat ||
                                                   o->stat)) {
                d = cwarn_d(c, DL_WARNING, n->loc, "array-parameter=",
                            "argument %u of type '%s' with mismatched bound",
                            no, pstr(c, &ns, n));
                if (d)
                    cnote(c, d, o->loc, "previously declared as '%s'",
                          pstr(c, &os, o));
            }
        }
        goto out;
    }
    if (!dim_same(&o->d[0], &n->d[0]) && (lvl >= 2 || n->stat || o->stat)) {
        d = cwarn_d(c, DL_WARNING, n->loc, "array-parameter=", "argument %u "
                    "of type '%s' with mismatched bound", no, pstr(c, &ns, n));
        if (d)
            cnote(c, d, o->loc, "previously declared as '%s'",
                  pstr(c, &os, o));
    }
out:
    sb_free(&ns);
    sb_free(&os);
    sb_free(&bs);
    sb_free(&bo);
}

void cparm_compare(Checker *c, uint32_t nw, uint32_t old)
{
    const CParmDesc *n = desc_of(c, nw), *o = desc_of(c, old);
    uint32_t i;
    if (n == o || n->n != o->n)
        return;
    for (i = 0; i < n->n; i++)
        if (n->p[i].nd || o->p[i].nd)
            cmp_param(c, &o->p[i], &n->p[i], i + 1);
}
