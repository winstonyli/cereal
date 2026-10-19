/* cgdata.c - the static objects of a unit as x86-64 data: the byte image of a
 * data node (the TAST's TO_DATA with TO_ELEM kids), the addresses inside it
 * as relocations, and the state the back end keeps between declarations
 * (names, what is defined, tentative definitions, the object file).
 *
 * An initializer element is evaluated the way a compiler folds a constant
 * expression: integers and floats to values, lvalues and addresses to a
 * symbol (or string literal) plus a byte offset.  tests/tast_data.py holds the
 * same semantics for the TAST's own tests.  Not supported (the object is
 * reported in a comment): long double, compound literals, comparisons and
 * division in an initializer, bit-fields of addresses. */
#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/cgx86.h"

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
    if (c->opt.dump_asm)
        mir_print_data(d, c->opt.tast);
    if (x->obj)
        mobj_add_data(x->obj, d);
    if (strncmp(d->name, ".L", 2))
        vec_push(&x->defined, dup_n(d->name, strlen(d->name)));
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

typedef struct Val {
    enum { V_INT, V_FLT, V_LV, V_ADDR } k;
    int64_t i;                  /* V_INT: the value; V_LV, V_ADDR: byte offset */
    double f;
    bool str;                   /* the target: a string literal node (str) or a symbol (ref) */
    uint32_t ref, node;
} Val;

typedef struct DEv {
    El *e;
    Checker *c;
    MData d;
    VEC(MData) lits;            /* the string literals the addresses point to */
    char why[96];
} DEv;

static bool bad(DEv *v, const char *fmt, ...)
{
    if (!v->why[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v->why, sizeof v->why, fmt, ap);
        va_end(ap);
    }
    return false;
}

static const TNode *tn(DEv *v, uint32_t n) { return &v->e->nodes.data[n]; }
static uint32_t tk(DEv *v, uint32_t n, uint32_t j) { return v->e->kids.data[tn(v, n)->kid0 + j]; }

static uint64_t tsize(DEv *v, TypeId t)
{
    bool ok;
    uint64_t sz = type_size(&v->c->tt, t, &ok);
    return ok ? sz : 0;
}

/* An integer value as the type holds it (sign- or zero-extended from its width). */
static int64_t trunc_to(DEv *v, TypeId t, int64_t x)
{
    TypeTable *tt = &v->c->tt;
    uint64_t sz = tsize(v, t);
    if (type_ckind(tt, t) == TY_BOOL)
        return x != 0;
    if (sz >= 8 || sz == 0)
        return x;
    if (type_is_signed(tt, t))
        return (int64_t)((uint64_t)x << (64 - 8 * sz)) >> (64 - 8 * sz);
    return (int64_t)((uint64_t)x & ((1ull << (8 * sz)) - 1));
}

static bool is_float_ty(DEv *v, TypeId t)
{
    TypeKind k = type_ckind(&v->c->tt, t);
    return k == TY_FLOAT || k == TY_DOUBLE;
}

static bool is_int_ty(DEv *v, TypeId t)
{
    TypeKind k = type_ckind(&v->c->tt, t);
    return (k >= TY_BOOL && k <= TY_ULLONG) || k == TY_ENUM || k == TY_PTR;
}

static bool ev(DEv *v, uint32_t n, Val *r)
{
    const TNode *t = tn(v, n);
    Val a, b;
    memset(r, 0, sizeof *r);
    switch (t->op) {
    case TO_CONST:
        r->k = V_INT;
        r->i = t->val;
        return true;
    case TO_FCONST:
        r->k = V_FLT;
        if (t->width == 4) {
            float f;
            memcpy(&f, v->e->bytes.data + t->val, 4);
            r->f = f;
        } else if (t->width == 8) {
            memcpy(&r->f, v->e->bytes.data + t->val, 8);
        } else {
            return bad(v, "long double in an initializer");
        }
        return true;
    case TO_SYM:
        r->k = V_LV;
        r->ref = t->sym;
        return true;
    case TO_STR:
        r->k = V_LV;
        r->str = true;
        r->node = n;
        return true;
    case TO_CAST: {
        TypeId to = t->ty, from = tn(v, tk(v, n, 0))->ty;
        if (!ev(v, tk(v, n, 0), &a))
            return false;
        if (is_float_ty(v, to)) {
            if (a.k == V_INT)
                r->f = type_is_signed(&v->c->tt, from) ? (double)a.i : (double)(uint64_t)a.i;
            else if (a.k == V_FLT)
                r->f = a.f;
            else
                return bad(v, "address cast to a floating type");
            if (type_ckind(&v->c->tt, to) == TY_FLOAT)
                r->f = (float)r->f;
            r->k = V_FLT;
            return true;
        }
        if (!is_int_ty(v, to))
            return bad(v, "cast to a type outside the integer and float subset");
        if (a.k == V_FLT) {
            if (!(a.f > -9.2e18 && a.f < 9.2e18))       /* also NaN */
                return bad(v, "float to integer of an out-of-range value");
            r->i = (int64_t)a.f;
        } else {
            if (a.k != V_INT && type_ckind(&v->c->tt, to) == TY_BOOL)
                return bad(v, "address cast to _Bool");
            *r = a;
        }
        if (r->k == V_FLT || r->k == V_INT) {
            r->k = V_INT;
            r->i = trunc_to(v, to, r->i);
        }
        return true;
    }
    case TO_ADDR:
        if (!ev(v, tk(v, n, 0), r))
            return false;
        if (r->k != V_LV)
            return bad(v, "address of a value");
        r->k = V_ADDR;
        return true;
    case TO_PTRADD:
        if (!ev(v, tk(v, n, 0), &a) || !ev(v, tk(v, n, 1), &b))
            return false;
        if (b.k != V_INT || (a.k != V_ADDR && a.k != V_INT))
            return bad(v, "pointer arithmetic outside address plus integer");
        *r = a;
        r->i = (int64_t)((uint64_t)a.i + (uint64_t)b.i * (uint64_t)t->val);
        return true;
    case TO_DEREF:
        if (!ev(v, tk(v, n, 0), r))
            return false;
        if (r->k != V_ADDR)
            return bad(v, "dereference of a value");
        r->k = V_LV;
        return true;
    case TO_FIELD:
        if (t->width)
            return bad(v, "bit-field in an initializer");
        if (!ev(v, tk(v, n, 0), r))
            return false;
        if (r->k != V_LV)
            return bad(v, "member of a value");
        r->i += t->val;
        return true;
    case TO_UNOP:
        if (!ev(v, tk(v, n, 0), &a))
            return false;
        if (a.k == V_FLT && (t->pun == P_MINUS || t->pun == P_PLUS)) {
            *r = a;
            if (t->pun == P_MINUS)
                r->f = -a.f;
            return true;
        }
        if (a.k != V_INT)
            return bad(v, "unary operator on a non-integer in an initializer");
        r->k = V_INT;
        if (t->pun == P_MINUS)
            r->i = (int64_t)(0 - (uint64_t)a.i);
        else if (t->pun == P_TILDE)
            r->i = ~a.i;
        else if (t->pun == P_BANG)
            r->i = !a.i;
        else if (t->pun == P_PLUS)
            r->i = a.i;
        else
            return bad(v, "unary operator in an initializer");
        r->i = trunc_to(v, t->ty, r->i);
        return true;
    case TO_BINOP: {
        uint64_t x, y;
        if (!ev(v, tk(v, n, 0), &a) || !ev(v, tk(v, n, 1), &b))
            return false;
        if (a.k == V_FLT && b.k == V_FLT) {
            double f;
            switch (t->pun) {
            case P_PLUS: f = a.f + b.f; break;
            case P_MINUS: f = a.f - b.f; break;
            case P_STAR: f = a.f * b.f; break;
            case P_SLASH: f = a.f / b.f; break;
            default: return bad(v, "binary operator on floats in an initializer");
            }
            r->k = V_FLT;
            r->f = type_ckind(&v->c->tt, t->ty) == TY_FLOAT ? (double)(float)f : f;
            return true;
        }
        if (a.k != V_INT || b.k != V_INT)
            return bad(v, "binary operator outside integers in an initializer");
        x = (uint64_t)a.i;
        y = (uint64_t)b.i;
        r->k = V_INT;
        switch (t->pun) {
        case P_PLUS: r->i = (int64_t)(x + y); break;
        case P_MINUS: r->i = (int64_t)(x - y); break;
        case P_STAR: r->i = (int64_t)(x * y); break;
        case P_PIPE: r->i = (int64_t)(x | y); break;
        case P_AMP: r->i = (int64_t)(x & y); break;
        case P_CARET: r->i = (int64_t)(x ^ y); break;
        case P_SHL:
        case P_SHR: {
            unsigned w = (unsigned)(8 * tsize(v, t->ty));
            if (b.i < 0 || b.i >= (int64_t)w)
                return bad(v, "shift count out of range in an initializer");
            if (t->pun == P_SHL)
                r->i = (int64_t)(x << b.i);
            else if (type_is_signed(&v->c->tt, t->ty))
                r->i = a.i >> b.i;
            else
                r->i = (int64_t)(x >> b.i);
            break;
        }
        default:
            return bad(v, "binary operator in an initializer");
        }
        r->i = trunc_to(v, t->ty, r->i);
        return true;
    }
    default:
        return bad(v, "initializer expression %d", t->op);
    }
}

/* Where an address value points, as a relocation in the image at off. */
static bool add_reloc(DEv *v, uint32_t off, const Val *a)
{
    MReloc r;
    r.off = off;
    r.addend = a->i;
    if (a->str) {               /* the literal becomes a read-only object of its own */
        const TNode *s = tn(v, a->node);
        MData lit;
        char nm[32];
        memset(&lit, 0, sizeof lit);
        snprintf(nm, sizeof nm, ".LCd%u", x86_get(v->c)->nlits++);
        lit.name = dup_n(nm, strlen(nm));
        lit.sec = MD_RODATA;
        lit.align = 1;
        lit.size = s->width;
        lit.bytes = xmalloc(s->width ? s->width : 1);
        memcpy(lit.bytes, v->e->bytes.data + s->val, s->width);
        vec_push(&v->lits, lit);
        r.name = dup_n(nm, strlen(nm));
    } else {
        const char *nm = x86_symname(v->c, v->e->fname, a->ref);
        r.name = dup_n(nm, strlen(nm));
    }
    vec_push(&v->d.rels, r);
    return true;
}

static bool elem(DEv *v, uint32_t k)
{
    const TNode *t = tn(v, k);
    uint32_t off = (uint32_t)t->val, kid = tk(v, k, 0);
    uint64_t sz = tsize(v, t->ty);
    TypeKind kind = type_ckind(&v->c->tt, t->ty);
    Val a;
    if (off + sz > v->d.size && !(kind == TY_ARRAY))
        return bad(v, "initializer element outside its object");
    if (kind == TY_ARRAY) {     /* a string: its bytes, cut to the array */
        const TNode *s = tn(v, kid);
        uint64_t m = s->width < sz ? s->width : sz;
        if (s->op != TO_STR || off + m > v->d.size)
            return bad(v, "array initializer form");
        memcpy(v->d.bytes + off, v->e->bytes.data + s->val, m);
        return true;
    }
    if (!ev(v, kid, &a))
        return false;
    if (t->width) {             /* a bit-field: merge into the bytes it covers */
        unsigned b = t->pun, w = t->width, nb = (b + w + 7) / 8, i;
        uint64_t cur = 0, mask = (w >= 64 ? ~0ull : (1ull << w) - 1) << b;
        if (a.k != V_INT)
            return bad(v, "non-integer in a bit-field");
        for (i = nb; i-- > 0;)
            cur = cur << 8 | v->d.bytes[off + i];
        cur = (cur & ~mask) | (((uint64_t)a.i << b) & mask);
        for (i = 0; i < nb; i++)
            v->d.bytes[off + i] = (uint8_t)(cur >> (8 * i));
        return true;
    }
    if (is_float_ty(v, t->ty)) {
        if (a.k != V_FLT)
            return bad(v, "non-floating value in a floating object");
        if (kind == TY_FLOAT) {
            float f = (float)a.f;
            memcpy(v->d.bytes + off, &f, 4);
        } else {
            memcpy(v->d.bytes + off, &a.f, 8);
        }
        return true;
    }
    if (!is_int_ty(v, t->ty) || (sz != 1 && sz != 2 && sz != 4 && sz != 8))
        return bad(v, "element of a type outside the integer and float subset");
    if (a.k == V_INT) {
        uint64_t x = (uint64_t)a.i;
        uint64_t i;
        for (i = 0; i < sz; i++)
            v->d.bytes[off + i] = (uint8_t)(x >> (8 * i));
        return true;
    }
    if (a.k != V_ADDR || sz != 8)
        return bad(v, "address in a narrow or non-address element");
    return add_reloc(v, off, &a);
}

static bool obj_const(DEv *v, TypeId t)
{
    TypeTable *tt = &v->c->tt;
    while (type_ckind(tt, t) == TY_ARRAY)
        t = type_base(tt, type_canon(tt, t));
    return TYPE_QUALS(t) & TQ_CONST;
}

bool x86_data_node(El *e, uint32_t n, char *why, size_t nwhy)
{
    DEv v;
    const TNode *t = &e->nodes.data[n];
    const CSym *s = csym(e->c, t->sym);
    bool ok = false, zero = true;
    uint64_t i, sz;
    size_t k;
    memset(&v, 0, sizeof v);
    v.e = e;
    v.c = e->c;
    sz = type_size(&e->c->tt, t->ty, &ok);
    if (!ok || !sz) {
        snprintf(why, nwhy, "object of unknown size");
        return false;
    }
    {
        const char *nm = x86_symname(e->c, e->fname, t->sym);
        v.d.name = dup_n(nm, strlen(nm));
    }
    v.d.global = s->linkage == LK_EXTERNAL;
    v.d.align = type_align(&e->c->tt, t->ty);
    if (s->align > v.d.align)
        v.d.align = s->align;
    if (!v.d.align)
        v.d.align = 1;
    v.d.size = sz;
    v.d.bytes = xcalloc(sz, 1);
    for (i = 0; i < t->nkids && ok; i++)
        ok = elem(&v, e->kids.data[t->kid0 + i]);
    if (ok) {
        for (i = 0; i < sz && zero; i++)
            zero = !v.d.bytes[i];
        zero = zero && !v.d.rels.len;
        v.d.sec = obj_const(&v, t->ty) && !v.d.rels.len ? MD_RODATA : zero ? MD_BSS : MD_DATA;
        if (v.d.sec == MD_BSS) {
            free(v.d.bytes);
            v.d.bytes = NULL;
        }
        for (k = 0; k < v.lits.len; k++)
            x86_emit_data(e->c, &v.lits.data[k]);
        x86_emit_data(e->c, &v.d);
    }
    for (k = 0; k < v.lits.len; k++)
        mdata_free(&v.lits.data[k]);
    vec_free(&v.lits);
    mdata_free(&v.d);
    if (!ok)
        snprintf(why, nwhy, "%s", v.why);
    return ok;
}

/* ---- the end of the unit --------------------------------------------------- */

void x86_finish(Checker *c)
{
    X86 *x = c->x86;
    size_t i;
    if (!x)
        return;
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
    free(x);
    c->x86 = NULL;
}
