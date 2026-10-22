/* cgdata.c - the static objects of a unit as x86-64 data: the byte image of a
 * data node (the TAST's TO_DATA with TO_ELEM kids), the addresses inside it
 * as relocations, and the state the back end keeps between declarations
 * (names, what is defined, tentative definitions, the object file).
 *
 * The bytes and relocations come from the evaluator (ev_object, eval.c), which
 * folds the initializer as gcc does; tests/tast_data.py checks them against
 * gcc's objects.  What the evaluator cannot do (long double, ...) is reported
 * in a comment. */
#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/cgx86.h"
#include "c/eval.h"

/* ---- state, names -------------------------------------------------------- */

X86 *x86_get(Checker *c)
{
    if (!c->x86) {
        c->x86 = xcalloc(1, sizeof *c->x86);
        if (c->opt.emit_obj)
            c->x86->obj = mobj_new();
    }
    return c->x86;
}

static char *dup_n(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

const char *x86_symname(Checker *c, uint32_t fname, uint32_t ref)
{
    const CSym *s = csym(c, ref);
    const char *id = cident(c, s->name);
    char buf[32], *p;
    size_t n;
    if (!(ref & SYM_LOCAL) || s->kind != CS_OBJ || s->sc != SC_STATIC)
        return id;
    snprintf(buf, sizeof buf, ".%u", ref & ~SYM_LOCAL);
    n = strlen(id) + strlen(buf) + (fname ? strlen(cident(c, fname)) + 1 : 0);
    p = xmalloc(n + 1);
    snprintf(p, n + 1, "%s%s%s%s", fname ? cident(c, fname) : "", fname ? "." : "", id, buf);
    vec_push(&x86_get(c)->names, p);
    return p;
}

void x86_emit_data(Checker *c, const MData *d)
{
    X86 *x = x86_get(c);
    size_t i;
    if (c->opt.dump_asm)
        mir_print_data(d, c->opt.tast);
    if (x->obj)
        mobj_add_data(x->obj, d);
    for (i = 0; i < d->rels.len; i++)
        vec_push(&x->refs, dup_n(d->rels.data[i].name, strlen(d->rels.data[i].name)));
    if (strncmp(d->name, ".L", 2))
        vec_push(&x->defined, dup_n(d->name, strlen(d->name)));
}


/* ---- functions ---------------------------------------------------------------- */

static void note_refs(X86 *x, const MFunc *f)
{
    size_t i;
    for (i = 0; i < f->code.len; i++) {
        const MInst *in = &f->code.data[i];
        if (in->a.k == MK_SYM)
            vec_push(&x->refs, dup_n(in->a.name, strlen(in->a.name)));
        if (in->b.k == MK_SYM)
            vec_push(&x->refs, dup_n(in->b.name, strlen(in->b.name)));
    }
}

static void emit_func(Checker *c, X86 *x, MFunc *f, FILE *out)
{
    note_refs(x, f);
    if (c->opt.dump_asm)
        mir_print(f, out);
    if (x->obj && !mobj_add(x->obj, f))
        fprintf(out, "# x86: %s unsupported: no encoding\n", f->name);
}

static void free_func(MFunc *f)
{
    size_t i;
    for (i = 0; i < f->pubs.len; i++)
        free(f->pubs.data[i].name);
    vec_free(&f->pubs);
    vec_free(&f->code);
}

void x86_func_done(Checker *c, MFunc *f, bool sys, FILE *out)
{
    X86 *x = x86_get(c);
    if (sys) {
        XPend p = {0};
        p.f = *f;
        vec_push(&x->pend, p);
    } else {
        emit_func(c, x, f, out);
        free_func(f);
    }
    memset(&f->code, 0, sizeof f->code);
    memset(&f->pubs, 0, sizeof f->pubs);
}

void x86_func_fail(Checker *c, const char *name, const char *why)
{
    XPend p = {0};
    p.f.name = name;
    p.why = dup_n(why, strlen(why));
    vec_push(&x86_get(c)->pend, p);
}

static bool is_ref(const X86 *x, const char *name)
{
    size_t i;
    for (i = 0; i < x->refs.len; i++)
        if (!strcmp(x->refs.data[i], name))
            return true;
    return false;
}

/* The system-header functions something refers to (and, in turn, what those refer to). */
static void settle_pending(Checker *c, X86 *x)
{
    bool again = true;
    size_t i;
    while (again) {
        again = false;
        for (i = 0; i < x->pend.len; i++) {
            XPend *p = &x->pend.data[i];
            if (p->done || !is_ref(x, p->f.name))
                continue;
            p->done = again = true;
            if (p->why)
                fprintf(c->opt.tast, "# x86: %s unsupported: %s\n", p->f.name, p->why);
            else
                emit_func(c, x, &p->f, c->opt.tast);
        }
    }
    for (i = 0; i < x->pend.len; i++) {
        free_func(&x->pend.data[i].f);
        free(x->pend.data[i].why);
    }
    vec_free(&x->pend);
}
static bool is_defined(const X86 *x, const char *name)
{
    size_t i;
    for (i = 0; i < x->defined.len; i++)
        if (!strcmp(x->defined.data[i], name))
            return true;
    return false;
}

void x86_tentative(Checker *c, uint32_t ref)
{
    vec_push(&x86_get(c)->tents, ref);
}

/* ---- the image of an object ---------------------------------------------- */

static bool obj_const(Checker *c, TypeId t)
{
    TypeTable *tt = &c->tt;
    while (type_ckind(tt, t) == TY_ARRAY)
        t = type_base(tt, type_canon(tt, t));
    return TYPE_QUALS(t) & TQ_CONST;
}

bool x86_data_node(El *e, uint32_t n, char *why, size_t nwhy)
{
    const TNode *t = &e->nodes.data[n];
    const CSym *s = csym(e->c, t->sym);
    EvImage img;
    MData d;
    VEC(MData) lits = {0};
    size_t k;
    bool zero = true;
    uint64_t i;
    if (!ev_object(ev_of(e->c), e, n, &img)) {
        snprintf(why, nwhy, "%s", img.why);
        return false;
    }
    if (!img.size) {
        ev_image_free(&img);
        snprintf(why, nwhy, "object of unknown size");
        return false;
    }
    memset(&d, 0, sizeof d);
    {
        const char *nm = x86_symname(e->c, e->fname, t->sym);
        d.name = dup_n(nm, strlen(nm));
    }
    d.global = s->linkage == LK_EXTERNAL;
    d.weak = d.global && (s->flags & CSF_WEAK);
    d.align = type_align(&e->c->tt, t->ty);
    if (s->align > d.align)
        d.align = s->align;
    if (!d.align)
        d.align = 1;
    d.size = img.size;
    d.bytes = img.bytes;                /* ours now; NULL is all zero */
    img.bytes = NULL;
    for (k = 0; k < img.nrel; k++) {
        const EvReloc *r = &img.rel[k];
        MReloc m;
        char nm[160];
        const char *name;
        if (r->kind == EV_R_SYM) {
            name = x86_symname(e->c, r->fname, r->ref);
        } else if (r->kind == EV_R_LABEL) {
            snprintf(nm, sizeof nm, ".L%s_a%u", cident(e->c, r->fname), r->ref);
            name = nm;
        } else {                        /* the literal becomes a read-only object of its own */
            MData lit;
            memset(&lit, 0, sizeof lit);
            snprintf(nm, sizeof nm, ".LCd%u", x86_get(e->c)->nlits++);
            lit.name = dup_n(nm, strlen(nm));
            lit.sec = MD_RODATA;
            lit.align = 1;
            lit.size = r->litsize;
            lit.bytes = xmalloc(r->litsize ? r->litsize : 1);
            memcpy(lit.bytes, r->lit, (size_t)r->litsize);
            vec_push(&lits, lit);
            name = nm;
        }
        m.off = r->off;
        m.addend = r->addend;
        m.name = dup_n(name, strlen(name));
        vec_push(&d.rels, m);
    }
    for (i = 0; d.bytes && i < d.size && zero; i++)
        zero = !d.bytes[i];
    zero = zero && !d.rels.len;
    d.sec = obj_const(e->c, t->ty) && !d.rels.len ? MD_RODATA : zero ? MD_BSS : MD_DATA;
    if (d.sec == MD_BSS) {
        free(d.bytes);
        d.bytes = NULL;
    }
    for (k = 0; k < lits.len; k++)
        x86_emit_data(e->c, &lits.data[k]);
    x86_emit_data(e->c, &d);
    for (k = 0; k < lits.len; k++)
        mdata_free(&lits.data[k]);
    vec_free(&lits);
    mdata_free(&d);
    ev_image_free(&img);
    return true;
}

/* ---- the end of the unit --------------------------------------------------- */

void x86_finish(Checker *c)
{
    X86 *x = c->opt.emit_obj ? x86_get(c) : c->x86;   /* a unit without definitions still writes its (empty) object */
    size_t i;
    if (!x)
        return;
    settle_pending(c, x);
    for (i = 0; i < x->tents.len; i++) {
        uint32_t ref = x->tents.data[i];
        const CSym *s = csym(c, ref);
        const char *nm = x86_symname(c, 0, ref);
        MData d;
        bool ok = false;
        uint64_t sz = type_size(&c->tt, s->ty, &ok);
        if (is_defined(x, nm) || !ok || !sz)
            continue;
        memset(&d, 0, sizeof d);
        d.name = dup_n(nm, strlen(nm));
        d.global = s->linkage == LK_EXTERNAL;
        d.weak = d.global && (s->flags & CSF_WEAK);
        d.sec = MD_BSS;
        d.align = type_align(&c->tt, s->ty);
        if (s->align > d.align)
            d.align = s->align;
        if (!d.align)
            d.align = 1;
        d.size = sz;
        x86_emit_data(c, &d);
        mdata_free(&d);
    }
    if (x->obj && !mobj_write(x->obj, c->opt.emit_obj))
        fprintf(stderr, "cannot write %s\n", c->opt.emit_obj);
    mobj_free(x->obj);
    for (i = 0; i < x->names.len; i++)
        free(x->names.data[i]);
    for (i = 0; i < x->defined.len; i++)
        free(x->defined.data[i]);
    vec_free(&x->names);
    vec_free(&x->defined);
    vec_free(&x->tents);
    for (i = 0; i < x->refs.len; i++)
        free(x->refs.data[i]);
    vec_free(&x->refs);
    free(x);
    c->x86 = NULL;
}
