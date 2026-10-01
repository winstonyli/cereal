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
} PDim;

typedef struct PParm {
    bool arr;                /* declared as an array */
    bool stat;               /* [static n] */
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
            }
        free(pd->p[i].d);
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
    d->txt = xstrdup(cexpr_str(c, sz));
    d->ttxt = xstrdup(d->txt);
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

/* How parameter node p was declared. */
static void parm_of(Checker *c, uint32_t p, PParm *o, char *const *names,
                    uint32_t nnames)
{
    uint32_t kids[32], nk, l = NO_NODE, d, dn[16], nd = 0, m;
    bool ptr_after = false, other = false;
    TypeId t = c->ty[p], pt, e;
    memset(o, 0, sizeof *o);
    o->ty = t;
    o->loc = tloc(c, min_tok(c, p));
    for (m = cfirst(c, p); m <= p; m++)
        if (ntag(c, m) == N_NAME) {
            o->loc = cnode_loc(c, m);
            break;
        }
    nk = node_children(c->nodes, p, kids, 32);
    for (m = nk > 32 ? 32 : nk; m > 0; m--)
        if (ntag(c, kids[m - 1]) != N_ATTRIBUTE) {
            l = kids[m - 1];
            break;
        }
    if (l == NO_NODE || !is_declarator_tag(ntag(c, l)))
        return;
    if (type_ckind(TT, t) != TY_PTR)
        return;
    for (d = l; d != NO_NODE; d = cdecl_inner_decl(c, d)) {
        unsigned tg = ntag(c, d);
        if (tg == N_ARRAY) {
            if (nd == 16 || ptr_after)
                other = true;
            else
                dn[nd++] = d;
        } else if (tg == N_PTR) {
            if (nd)
                ptr_after = true;
        } else if (tg == N_FUNC) {
            other = true;
        }
    }
    if (other || !nd)
        return;
    /* the brackets are nested outermost-last: reverse into source order */
    o->d = xcalloc(nd, sizeof *o->d);
    o->nd = nd;
    for (m = 0; m < nd; m++)
        dim_of(c, dn[nd - 1 - m], &o->d[m], m == 0 && !ptr_after, names,
                nnames);
    o->arr = !ptr_after;
    if (o->arr) {
        const Node *an = cnode(c, dn[nd - 1]);
        o->stat = (an->flags & NF_STATIC) != 0;
        o->quals = cdecl_quals_of(c, dn[nd - 1]);
    }
    pt = type_base(TT, t);
    e = pt;
    for (m = o->arr ? 1 : 0; m < nd; m++) {
        TypeKind k = type_ckind(TT, e);
        if (k != TY_ARRAY && k != TY_VLA)
            break;
        e = type_base(TT, e);
    }
    o->base = e;
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
    type_print(TT, sb, p->base);
    b = sb_cstr(sb);
    bl = strlen(b);
    if (!p->arr)
        sb_puts(sb, bl && b[bl - 1] == '*' ? "(*)" : " (*)");
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
            if (sp && p->d[i].k != D_NONE)
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
        return a->arg || b->arg ? a->arg == b->arg : !strcmp(a->txt, b->txt);
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
                if (n->d[i].k >= D_STAR || o->d[i].k >= D_STAR)
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
