/* eval.c - the evaluator (docs/EVAL_DESIGN.md, E1).
 *
 * Interprets a function's control-flow graph (tcfg_int.h) over byte-addressed
 * objects.  A scalar value is a 64-bit slot, canonical as in cgx86.c (sign- or
 * zero-extended from its type's width); a pointer is an object id (high 32 bits,
 * 0 is null) and a signed byte offset (low 32); an aggregate "value" is the
 * address of its bytes, copied where cgx86.c copies (store, local, argument,
 * return).  Undefined behaviour stops evaluation with a reason; it is never
 * given a value.  Not yet covered: floating point, bit-fields, compound
 * literals, variadic calls, built-ins, globals; each says so when reached.
 * No mutable globals: the state is the Ev. */
#include <stdarg.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/eval.h"

typedef struct Obj {
    uint8_t *b, *init;          /* bytes and one init flag per byte */
    uint64_t size;
    uint32_t fn;                /* a function's object: its fsym + 1 */
    bool live, ro;
} Obj;

typedef struct Blk {
    uint32_t *st;
    size_t nst;
    CCase *cases;
    size_t nc;
    uint32_t val, a, b;
    uint8_t term;
} Blk;

typedef struct Par {
    uint32_t ref;
    TypeId ty;
} Par;

typedef struct Fn {
    uint32_t fsym;
    TNode *nodes;
    uint32_t *kids;
    uint8_t *bytes;
    Par *par;
    size_t np;
    Blk *blk;
    size_t nblk;
    uint32_t entry, nreg, ntmp;
    bool variadic;
    TypeId ret;
    uint32_t *strobj;           /* per node: its string literal's object + 1 */
} Fn;

/* A static-storage object: a file-scope object (persistent ref) or a block-scope
 * static (local ref, of the function fn).  Created, zeroed and initialized on
 * its first reference; node is its TO_DATA (TN: tentative, zero), found in
 * the elements of Fn fn. */
typedef struct Sd {
    uint32_t ref, node, fn, obj;
    TypeId ty;
    bool local;
} Sd;

struct Ev {
    Checker *c;
    EvOpts o;
    EvRes res;
    VEC(Obj) obj;               /* object id n is obj.data[n - 1] */
    VEC(Fn) fn;
    VEC(Sd) sd;
    long fuel;
    unsigned depth;
};

typedef struct Loc {
    uint32_t ref, obj;
} Loc;

typedef struct Fr {
    Ev *v;
    Fn *f;
    uint32_t *reg, *tmp;        /* per reg/tmp N: its object, 0: none yet */
    uint64_t va;                /* a variadic function: the pointer to its extra arguments */
    VEC(Loc) loc;
    VEC(uint32_t) own;          /* objects that die with the frame */
    struct Fr *up;
} Fr;

/* ---- failure -------------------------------------------------------------- */

static void fail(Ev *v, int st, const char *fmt, ...)
{
    if (v->res.status == EV_OK) {
        va_list ap;
        va_start(ap, fmt);
        v->res.status = st;
        vsnprintf(v->res.why, sizeof v->res.why, fmt, ap);
        va_end(ap);
    }
}

#define UNSUP(v, ...) fail(v, EV_UNSUPPORTED, __VA_ARGS__)
#define TRAP(v, ...) fail(v, EV_TRAP, __VA_ARGS__)

/* ---- types ---------------------------------------------------------------- */

typedef struct Ti {
    unsigned bytes;
    bool sg, boolean, flt;       /* flt: float or double, the value is its IEEE bit pattern */
} Ti;

static bool is_agg(Ev *v, TypeId t)
{
    TypeKind k = type_ckind(&v->c->tt, t);
    return k == TY_ARRAY || type_is_record(&v->c->tt, t);
}

/* An integer, enum, bool or pointer type; false (and a reason) otherwise. */
static bool tinfo(Ev *v, TypeId t, Ti *ti)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok;
    uint64_t sz;
    memset(ti, 0, sizeof *ti);
    if (k == TY_PTR || k == TY_NULLPTR) {
        ti->bytes = 8;
        return true;
    }
    if (k == TY_FLOAT || k == TY_DOUBLE) {
        ti->bytes = k == TY_FLOAT ? 4 : 8;
        ti->flt = true;
        return true;
    }
    if (!((k >= TY_BOOL && k <= TY_ULLONG) || k == TY_ENUM)) {
        UNSUP(v, "value of a type outside the integer subset (kind %d)", (int)k);
        return false;
    }
    sz = type_size(tt, t, &ok);
    if (!ok || (sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
        UNSUP(v, "integer of %d bytes", (int)sz);
        return false;
    }
    ti->bytes = (unsigned)sz;
    ti->sg = type_is_signed(tt, t);
    ti->boolean = k == TY_BOOL;
    return true;
}

static uint64_t canon(const Ti *ti, uint64_t x)
{
    unsigned sh;
    if (ti->boolean)
        return x != 0;
    if (ti->bytes >= 8)
        return x;
    sh = 64 - 8 * ti->bytes;
    return ti->sg ? (uint64_t)((int64_t)(x << sh) >> sh) : (x << sh) >> sh;
}

static bool size_of(Ev *v, TypeId t, uint64_t *sz)
{
    bool ok;
    *sz = type_size(&v->c->tt, t, &ok);
    if (!ok) {
        UNSUP(v, "object of unknown size");
        return false;
    }
    return true;
}

/* ---- objects -------------------------------------------------------------- */

static uint32_t new_obj(Ev *v, uint64_t size, bool ro)
{
    Obj o;
    if (size > (1u << 30)) {
        TRAP(v, "object of %llu bytes", (unsigned long long)size);
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.size = size;
    o.b = xcalloc(size ? size : 1, 1);
    o.init = xcalloc(size ? size : 1, 1);
    o.live = true;
    o.ro = ro;
    vec_push(&v->obj, o);
    return (uint32_t)v->obj.len;
}

static void kill_obj(Ev *v, uint32_t id)
{
    Obj *o = &v->obj.data[id - 1];
    o->live = false;
    free(o->b);
    free(o->init);
    o->b = o->init = NULL;
}

static uint64_t mkptr(uint32_t id, int64_t off)
{
    return ((uint64_t)id << 32) | (uint32_t)(int32_t)off;
}

static int32_t poff(uint64_t p) { return (int32_t)(uint32_t)p; }
static uint32_t pid(uint64_t p) { return (uint32_t)(p >> 32); }

/* The bytes [p, p + n) of a live object, or NULL after a trap. */
static Obj *access(Ev *v, uint64_t p, uint64_t n, const char *what)
{
    Obj *o;
    int64_t off = poff(p);
    if (pid(p) == 0) {
        TRAP(v, "%s through a null pointer", what);
        return NULL;
    }
    o = &v->obj.data[pid(p) - 1];
    if (!o->live) {
        TRAP(v, "%s of an object after its lifetime", what);
        return NULL;
    }
    if (o->fn) {
        TRAP(v, "%s through a function pointer", what);
        return NULL;
    }
    if (off < 0 || (uint64_t)off + n > o->size) {
        TRAP(v, "%s out of bounds", what);
        return NULL;
    }
    return o;
}

static uint64_t load_n(Ev *v, uint64_t p, const Ti *ti)
{
    Obj *o = access(v, p, ti->bytes, "read");
    uint64_t x = 0;
    unsigned i;
    if (!o)
        return 0;
    for (i = 0; i < ti->bytes; i++) {
        if (!o->init[poff(p) + i]) {
            TRAP(v, "read of an uninitialized value");
            return 0;
        }
        x |= (uint64_t)o->b[poff(p) + i] << (8 * i);
    }
    return canon(ti, x);
}

static void store_n(Ev *v, uint64_t p, const Ti *ti, uint64_t x)
{
    Obj *o = access(v, p, ti->bytes, "write");
    unsigned i;
    if (!o)
        return;
    if (o->ro) {
        TRAP(v, "write to a string literal");
        return;
    }
    for (i = 0; i < ti->bytes; i++) {
        o->b[poff(p) + i] = (uint8_t)(x >> (8 * i));
        o->init[poff(p) + i] = 1;
    }
}

/* Bit-fields: the address is the byte holding the lowest bit, the field is `width`
 * bits starting `pun` bits into it; read and written through the n bytes it touches
 * (as cgx86 does).  Limitation: a read does not check that the other fields sharing
 * those bytes were initialized, and a write marks the whole window initialized. */

static bool bf_window(Ev *v, unsigned pun, unsigned width, unsigned *n)
{
    if (pun + width > 64) {
        UNSUP(v, "bit-field straddling eight bytes");
        return false;
    }
    *n = (pun + width + 7) / 8;
    return true;
}

static uint64_t bf_ext(const Ti *ti, unsigned width, uint64_t x)
{
    x <<= 64 - width;
    return ti->sg ? (uint64_t)((int64_t)x >> (64 - width)) : x >> (64 - width);
}

static bool is_bf(Fr *r, uint32_t lv)
{
    const TNode *f = &r->f->nodes[lv];
    return f->op == TO_FIELD && f->width;
}

static uint64_t bf_load(Ev *v, uint64_t p, const Ti *ti, unsigned pun, unsigned width)
{
    unsigned n, i;
    uint64_t x = 0;
    Obj *o;
    if (!bf_window(v, pun, width, &n) || !(o = access(v, p, n, "read")))
        return 0;
    for (i = 0; i < n; i++)
        x |= (uint64_t)o->b[poff(p) + i] << (8 * i);
    return bf_ext(ti, width, x >> pun);
}

static uint64_t bf_store(Ev *v, uint64_t p, const Ti *ti, unsigned pun, unsigned width, uint64_t val)
{
    unsigned n, i;
    uint64_t x = 0, m = width >= 64 ? ~(uint64_t)0 : ((uint64_t)1 << width) - 1;
    Obj *o;
    if (!bf_window(v, pun, width, &n) || !(o = access(v, p, n, "write")))
        return 0;
    if (o->ro) {
        TRAP(v, "write to a string literal");
        return 0;
    }
    for (i = 0; i < n; i++)
        x |= (uint64_t)o->b[poff(p) + i] << (8 * i);
    x = (x & ~(m << pun)) | ((val & m) << pun);
    for (i = 0; i < n; i++) {
        o->b[poff(p) + i] = (uint8_t)(x >> (8 * i));
        o->init[poff(p) + i] = 1;
    }
    return bf_ext(ti, width, val);
}

static void copy_n(Ev *v, uint64_t dst, uint64_t src, uint64_t n)
{
    Obj *d, *s;
    if (n == 0)
        return;
    s = access(v, src, n, "read");
    d = s ? access(v, dst, n, "write") : NULL;
    if (!d)
        return;
    if (d->ro) {
        TRAP(v, "write to a string literal");
        return;
    }
    memmove(d->b + poff(dst), s->b + poff(src), n);
    memmove(d->init + poff(dst), s->init + poff(src), n);
}

static void zero_n(Ev *v, uint64_t dst, uint64_t n)
{
    Obj *d = access(v, dst, n, "write");
    if (!d)
        return;
    memset(d->b + poff(dst), 0, n);
    memset(d->init + poff(dst), 1, n);
}

/* ---- floating point: the slot holds the IEEE bits of a float (low 32) or double ---- */

static double fget(const Ti *ti, uint64_t x)
{
    if (ti->bytes == 4) {
        uint32_t w = (uint32_t)x;
        float f;
        memcpy(&f, &w, 4);
        return f;
    }
    {
        double d;
        memcpy(&d, &x, 8);
        return d;
    }
}

static uint64_t fput(const Ti *ti, double d)
{
    uint64_t x = 0;
    if (ti->bytes == 4) {
        float f = (float)d;
        uint32_t w;
        memcpy(&w, &f, 4);
        return w;
    }
    memcpy(&x, &d, 8);
    return x;
}

/* + - * / in the type's own precision (a float operation rounds to float). */
static uint64_t fbin(Ev *v, int p, const Ti *ti, uint64_t a, uint64_t b)
{
    if (ti->bytes == 4) {
        float x = (float)fget(ti, a), y = (float)fget(ti, b), r;
        switch (p) {
        case P_PLUS: r = x + y; break;
        case P_MINUS: r = x - y; break;
        case P_STAR: r = x * y; break;
        case P_SLASH: r = x / y; break;
        default: UNSUP(v, "float operator %d", p); return 0;
        }
        return fput(ti, r);
    } else {
        double x = fget(ti, a), y = fget(ti, b), r;
        switch (p) {
        case P_PLUS: r = x + y; break;
        case P_MINUS: r = x - y; break;
        case P_STAR: r = x * y; break;
        case P_SLASH: r = x / y; break;
        default: UNSUP(v, "float operator %d", p); return 0;
        }
        return fput(ti, r);
    }
}

static uint64_t fcompare(Ev *v, int p, const Ti *ti, uint64_t a, uint64_t b)
{
    double x = fget(ti, a), y = fget(ti, b);
    switch (p) {
    case P_EQEQ: return x == y;
    case P_NE: return x != y;
    case P_LT: return x < y;
    case P_GT: return x > y;
    case P_LE: return x <= y;
    case P_GE: return x >= y;
    default:
        UNSUP(v, "comparison %d", p);
        return 0;
    }
}

/* A conversion between scalar types; floats convert through double, which is
 * exact for float and for every integer that fits 53 bits, and rounds as the
 * hardware does for the rest. */
static uint64_t convert(Ev *v, const Ti *to, const Ti *from, uint64_t a)
{
    if (!from->flt && !to->flt)
        return canon(to, a);
    if (from->flt && to->flt)
        return fput(to, fget(from, a));
    if (to->flt) {
        if (from->bytes == 8 && !from->sg)
            return fput(to, to->bytes == 4 ? (double)(float)(uint64_t)a : (double)(uint64_t)a);
        return fput(to, to->bytes == 4 ? (double)(float)(int64_t)a : (double)(int64_t)a);
    } else {
        double d = fget(from, a), lo, hi;
        if (to->boolean)
            return d != 0 || d != d;
        if (d != d) {
            TRAP(v, "conversion of NaN to an integer");
            return 0;
        }
        if (to->sg) {
            hi = to->bytes == 8 ? 9223372036854775808.0 : (double)(1ull << (8 * to->bytes - 1));
            lo = -hi;
            if (!(d > lo - 1 && d < hi) || (to->bytes == 8 && d < lo)) {
                TRAP(v, "floating-point value out of range for the integer type");
                return 0;
            }
            return canon(to, (uint64_t)(int64_t)d);
        }
        hi = to->bytes == 8 ? 18446744073709551616.0 : (double)(1ull << (8 * to->bytes));
        if (!(d > -1 && d < hi)) {
            TRAP(v, "floating-point value out of range for the integer type");
            return 0;
        }
        return canon(to, (uint64_t)d);
    }
}

/* ---- arithmetic ------------------------------------------------------------ */

static bool fits(const Ti *ti, int64_t x)
{
    return ti->bytes >= 8 || (int64_t)canon(ti, (uint64_t)x) == x;
}

static uint64_t int_arith(Ev *v, int p, TypeId ty, uint64_t a, uint64_t b)
{
    Ti ti;
    int64_t sa = (int64_t)a, sb = (int64_t)b, r = 0;
    bool ovf = false;
    if (!tinfo(v, ty, &ti))
        return 0;
    switch (p) {
    case P_PLUS:
    case P_MINUS:
    case P_STAR:
        if (ti.sg && !ti.boolean) {
            ovf = p == P_PLUS ? __builtin_add_overflow(sa, sb, &r)
                  : p == P_MINUS ? __builtin_sub_overflow(sa, sb, &r)
                                 : __builtin_mul_overflow(sa, sb, &r);
            if ((ovf || !fits(&ti, r)) && !v->o.wrap) {
                TRAP(v, "signed integer overflow");
                return 0;
            }
        }
        return canon(&ti, p == P_PLUS ? a + b : p == P_MINUS ? a - b : a * b);
    case P_SLASH:
    case P_PERCENT:
        if (b == 0) {
            TRAP(v, "division by zero");
            return 0;
        }
        if (ti.sg) {
            if (sb == -1) {
                if (p == P_SLASH && !v->o.wrap && !fits(&ti, (int64_t)(0 - a)) ) {
                    TRAP(v, "signed integer overflow");
                    return 0;
                }
                return p == P_SLASH ? canon(&ti, 0 - a) : 0;
            }
            return canon(&ti, (uint64_t)(p == P_SLASH ? sa / sb : sa % sb));
        }
        return canon(&ti, p == P_SLASH ? a / b : a % b);
    case P_AMP: return canon(&ti, a & b);
    case P_PIPE: return canon(&ti, a | b);
    case P_CARET: return canon(&ti, a ^ b);
    case P_SHL:
    case P_SHR:
        if (b >= 8 * ti.bytes) {        /* also negative counts */
            TRAP(v, "shift count out of range");
            return 0;
        }
        if (p == P_SHR)
            return canon(&ti, ti.sg ? (uint64_t)(sa >> b) : a >> b);
        if (ti.sg && sa < 0 && !v->o.wrap) {
            TRAP(v, "left shift of a negative value");
            return 0;
        }
        return canon(&ti, a << b);
    default:
        UNSUP(v, "binary operator %d", p);
        return 0;
    }
}

static uint64_t compare(Ev *v, int p, TypeId ot, uint64_t a, uint64_t b)
{
    Ti ti;
    bool sg;
    if (!tinfo(v, ot, &ti))
        return 0;
    sg = ti.sg;
    switch (p) {
    case P_EQEQ: return a == b;
    case P_NE: return a != b;
    case P_LT: return sg ? (int64_t)a < (int64_t)b : a < b;
    case P_GT: return sg ? (int64_t)a > (int64_t)b : a > b;
    case P_LE: return sg ? (int64_t)a <= (int64_t)b : a <= b;
    case P_GE: return sg ? (int64_t)a >= (int64_t)b : a >= b;
    default:
        UNSUP(v, "comparison %d", p);
        return 0;
    }
}

/* ---- frames and calls -------------------------------------------------------- */

static uint32_t frame_obj(Fr *r, uint64_t size)
{
    uint32_t id = new_obj(r->v, size, false);
    if (id)
        vec_push(&r->own, id);
    return id;
}

static uint32_t *lookup(Fr *r, uint32_t ref)
{
    size_t i;
    for (i = 0; i < r->loc.len; i++)
        if (r->loc.data[i].ref == ref)
            return &r->loc.data[i].obj;
    return NULL;
}

static uint32_t function_obj(Ev *v, uint32_t fsym)
{
    size_t i;
    Obj o;
    for (i = 0; i < v->obj.len; i++)
        if (v->obj.data[i].fn == fsym + 1)
            return (uint32_t)i + 1;
    memset(&o, 0, sizeof o);
    o.fn = fsym + 1;
    o.live = true;
    vec_push(&v->obj, o);
    return (uint32_t)v->obj.len;
}

static Fn *find_fn(Ev *v, uint32_t fsym)
{
    size_t i;
    for (i = 0; i < v->fn.len; i++)
        if (v->fn.data[i].fsym == fsym)
            return &v->fn.data[i];
    return NULL;
}

static uint64_t ev_node(Fr *r, uint32_t n);
static uint64_t run_fn(Fr *caller, Fn *f, const uint64_t *args, size_t na, uint64_t va);

#define NODE(r, n) (&(r)->f->nodes[n])
#define KID(r, n, j) ((r)->f->kids[NODE(r, n)->kid0 + (j)])

static uint64_t ev_call_node(Fr *r, uint32_t n)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    uint64_t args[16], callee = 0;
    uint32_t j, first = t->pun == 1 ? 1 : 0, na = t->nkids - first;
    Fn *g;
    if (na > 16) {
        UNSUP(v, "call with more than 16 arguments");
        return 0;
    }
    for (j = na; j-- > 0;)              /* right to left, as cgx86.c */
        args[j] = ev_node(r, KID(r, n, first + j));
    if (t->pun == 1) {
        callee = ev_node(r, KID(r, n, 0));
        if (v->res.status)
            return 0;
        if (pid(callee) == 0) {
            TRAP(v, "call through a null pointer");
            return 0;
        }
        if (!v->obj.data[pid(callee) - 1].fn || poff(callee) != 0) {
            TRAP(v, "call through a pointer that is not a function");
            return 0;
        }
        g = find_fn(v, v->obj.data[pid(callee) - 1].fn - 1);
    } else {
        g = find_fn(v, t->sym);
    }
    if (v->res.status)
        return 0;
    if (!g) {
        UNSUP(v, "call of a function with no retained body");
        return 0;
    }
    if (g->variadic && na >= g->np) {   /* the extra arguments: 8-byte slots, as the stack area */
        uint64_t total = 0, at = 0, sz;
        uint32_t id;
        for (j = (uint32_t)g->np; j < na; j++) {
            TypeId at_ = NODE(r, KID(r, n, first + j))->ty;
            if (is_agg(v, at_)) {
                if (!size_of(v, at_, &sz))
                    return 0;
                total += (sz + 7) / 8 * 8;
            } else {
                total += 8;
            }
        }
        id = new_obj(v, total, false);
        if (!id)
            return 0;
        vec_push(&r->own, id);          /* gone with the caller's frame, after the call returns */
        for (j = (uint32_t)g->np; j < na; j++) {
            TypeId at_ = NODE(r, KID(r, n, first + j))->ty;
            if (is_agg(v, at_)) {
                if (!size_of(v, at_, &sz))
                    return 0;
                copy_n(v, mkptr(id, (int64_t)at), args[j], sz);
                at += (sz + 7) / 8 * 8;
            } else {
                Ti ti;
                if (!tinfo(v, at_, &ti))
                    return 0;
                ti.bytes = 8;
                ti.sg = false;
                store_n(v, mkptr(id, (int64_t)at), &ti, args[j]);
                at += 8;
            }
        }
        return run_fn(r, g, args, g->np, mkptr(id, 0));
    }
    return run_fn(r, g, args, na, 0);
}

/* Store the elements (TO_ELEM kids) of node n into the zeroed object at p. */
static void init_elems(Fr *r, uint32_t n, uint64_t p)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    uint64_t x;
    uint32_t j;
    Ti ti;
    for (j = 0; j < t->nkids && !v->res.status; j++) {
        const TNode *el = NODE(r, KID(r, n, j));
        uint32_t vn = KID(r, KID(r, n, j), 0);
        const TNode *vt = NODE(r, vn);
        if (el->width) {
            if (!tinfo(v, el->ty, &ti))
                return;
            x = ev_node(r, vn);
            bf_store(v, mkptr(pid(p), poff(p) + el->val), &ti, el->pun, el->width, x);
        } else if (vt->op == TO_STR) {
            uint64_t sp = ev_node(r, vn), cn = (uint64_t)vt->width;
            uint64_t esz;
            if (!size_of(v, el->ty, &esz))
                return;
            copy_n(v, mkptr(pid(p), poff(p) + el->val), sp, cn < esz ? cn : esz);
        } else if (is_agg(v, el->ty)) {
            uint64_t esz;
            if (!size_of(v, el->ty, &esz))
                return;
            copy_n(v, mkptr(pid(p), poff(p) + el->val), ev_node(r, vn), esz);
        } else if (tinfo(v, el->ty, &ti)) {
            x = ev_node(r, vn);
            store_n(v, mkptr(pid(p), poff(p) + el->val), &ti, x);
        }
    }
}

/* The object of a static-storage symbol, made and initialized on first use. */
static uint32_t static_obj(Fr *r, uint32_t ref)
{
    Ev *v = r->v;
    bool local = (ref & SYM_LOCAL) != 0;
    uint32_t fi = (uint32_t)(r->f - v->fn.data);
    Sd *s = NULL;
    size_t i;
    for (i = 0; i < v->sd.len; i++) {
        Sd *d = &v->sd.data[i];
        if (d->ref == ref && d->local == local && (!local || d->fn == fi) &&
            (!s || (s->node == TN && d->node != TN)))
            s = d;
    }
    if (!s) {
        UNSUP(v, "%s with no definition in this unit", local ? "static local" : "global object");
        return 0;
    }
    if (!s->obj) {
        uint64_t sz;
        uint32_t id;
        if (!size_of(v, s->ty, &sz) || !(id = new_obj(v, sz, false)))
            return 0;
        s->obj = id;                    /* before the initializer: it may point at itself */
        zero_n(v, mkptr(id, 0), sz);
        if (s->node != TN) {
            Fr ir;
            Fn *f = &v->fn.data[s->fn];
            size_t k;
            memset(&ir, 0, sizeof ir);
            ir.v = v;
            ir.f = f;
            ir.reg = xcalloc(f->nreg + 1, sizeof *ir.reg);
            ir.tmp = xcalloc(f->ntmp + 1, sizeof *ir.tmp);
            init_elems(&ir, s->node, mkptr(id, 0));
            for (k = 0; k < ir.own.len; k++)
                kill_obj(v, ir.own.data[k]);
            vec_free(&ir.own);
            free(ir.reg);
            free(ir.tmp);
        }
    }
    return s->obj;
}

/* A local variable (key: its symbol ref) or a compound literal (key: its node):
 * a fresh object per evaluation, which lives with the frame.  Its address. */
static uint64_t ev_local(Fr *r, uint32_t n, uint32_t key)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    uint64_t sz, p, x;
    uint32_t *slot, id;
    Ti ti;
    if (!size_of(v, t->ty, &sz))
        return 0;
    slot = lookup(r, key);
    if (slot) {
        kill_obj(v, *slot);             /* a new lifetime: a fresh object */
        v->obj.data[*slot - 1].live = true;
        v->obj.data[*slot - 1].b = xcalloc(sz ? sz : 1, 1);
        v->obj.data[*slot - 1].init = xcalloc(sz ? sz : 1, 1);
        id = *slot;
    } else {
        Loc l;
        id = new_obj(v, sz, false);
        if (!id)
            return 0;
        vec_push(&r->own, id);
        l.ref = key;
        l.obj = id;
        vec_push(&r->loc, l);
    }
    p = mkptr(id, 0);
    if (t->op == TO_COMPLIT)            /* what the list leaves out is zero */
        zero_n(v, p, sz);
    if (t->nkids == 0)
        return p;
    if (NODE(r, KID(r, n, 0))->op == TO_ELEM) {
        zero_n(v, p, sz);
        init_elems(r, n, p);
        return p;
    }
    if (is_agg(v, t->ty)) {
        x = ev_node(r, KID(r, n, 0));
        copy_n(v, p, x, sz);
    } else if (tinfo(v, t->ty, &ti)) {
        x = ev_node(r, KID(r, n, 0));
        store_n(v, p, &ti, x);
    }
    return p;
}

/* va_start / va_end / va_copy / va_arg.  A va_list is the 24-byte record of the
 * System V ABI, of which only the overflow area pointer (bytes 8..16) is used:
 * it points into the call's extra arguments and each va_arg moves it on. */
static uint64_t ev_va(Fr *r, uint32_t n)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    uint64_t ap, p, sz = 8;
    Ti ti, pt;
    bool agg;
    ap = ev_node(r, KID(r, n, 0));
    memset(&pt, 0, sizeof pt);
    pt.bytes = 8;
    switch (t->pun) {
    case 1:
        if (!r->va) {
            UNSUP(v, "va_start outside a variadic function");
            return 0;
        }
        zero_n(v, ap, 24);
        store_n(v, mkptr(pid(ap), poff(ap) + 8), &pt, r->va);
        return 0;
    case 2:
        return 0;
    case 3:
        copy_n(v, ap, ev_node(r, KID(r, n, 1)), 24);
        return 0;
    default:
        break;
    }
    if (v->res.status)
        return 0;
    p = load_n(v, mkptr(pid(ap), poff(ap) + 8), &pt);
    if (v->res.status)
        return 0;
    agg = is_agg(v, t->ty);
    if (agg) {
        if (!size_of(v, t->ty, &sz))
            return 0;
        if (access(v, p, sz, "va_arg read") == NULL)
            return 0;
        sz = (sz + 7) / 8 * 8;
    }
    {
        uint64_t x = agg ? p : tinfo(v, t->ty, &ti) ? load_n(v, p, &ti) : 0;
        if (v->res.status)
            return 0;
        store_n(v, mkptr(pid(ap), poff(ap) + 8), &pt, mkptr(pid(p), (int64_t)poff(p) + (int64_t)sz));
        return x;
    }
}

static uint64_t ev_node(Fr *r, uint32_t n)
{
    Ev *v = r->v;
    const TNode *t = NODE(r, n);
    uint64_t a, b;
    Ti ti, ts;
    if (v->res.status)
        return 0;
    if (--v->fuel < 0) {
        fail(v, EV_FUEL, "exceeded the step limit");
        return 0;
    }
    switch (t->op) {
    case TO_CONST:
        return (uint64_t)t->val;
    case TO_SYM: {
        uint32_t *s = (t->sym & SYM_LOCAL) ? lookup(r, t->sym) : NULL, id;
        if (s)
            return mkptr(*s, 0);
        if (!(t->sym & SYM_LOCAL) && csym(v->c, t->sym)->kind == CS_FUNC)
            return mkptr(function_obj(v, t->sym), 0);
        id = static_obj(r, t->sym);
        return id ? mkptr(id, 0) : 0;
    }
    case TO_DATA:                       /* a block-scope static: made on first reference */
        return 0;
    case TO_TMP:
    case TO_REG: {
        uint32_t *slot = (t->op == TO_TMP ? r->tmp : r->reg) + t->val;
        uint64_t sz;
        if (!*slot) {
            if (!size_of(v, t->ty, &sz))
                return 0;
            *slot = frame_obj(r, sz);
            if (!*slot)
                return 0;
        }
        return mkptr(*slot, 0);
    }
    case TO_STR: {
        uint32_t *o = &r->f->strobj[n];
        if (!*o) {
            Obj *ob;
            uint32_t id = new_obj(v, (uint64_t)t->width, true);
            if (!id)
                return 0;
            ob = &v->obj.data[id - 1];
            memcpy(ob->b, r->f->bytes + t->val, t->width);
            memset(ob->init, 1, t->width);
            *o = id + 1;
        }
        return mkptr(*o - 1, 0);
    }
    case TO_LOAD:
        a = ev_node(r, KID(r, n, 0));
        if (is_agg(v, t->ty) || v->res.status)
            return a;
        if (!tinfo(v, t->ty, &ti))
            return 0;
        if (is_bf(r, KID(r, n, 0)))
            return bf_load(v, a, &ti, NODE(r, KID(r, n, 0))->pun, NODE(r, KID(r, n, 0))->width);
        return load_n(v, a, &ti);
    case TO_STORE: {
        uint64_t sz;
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status)
            return 0;
        if (is_agg(v, t->ty)) {
            if (!size_of(v, t->ty, &sz))
                return 0;
            copy_n(v, a, b, sz);
            return a;
        }
        if (!tinfo(v, t->ty, &ti))
            return 0;
        if (is_bf(r, KID(r, n, 0)))
            return bf_store(v, a, &ti, NODE(r, KID(r, n, 0))->pun, NODE(r, KID(r, n, 0))->width, b);
        b = canon(&ti, b);
        store_n(v, a, &ti, b);
        return b;
    }
    case TO_POST: {
        uint32_t s = KID(r, n, 0);
        if (NODE(r, s)->op != TO_STORE) {
            UNSUP(v, "post of a non-store");
            return 0;
        }
        a = ev_node(r, KID(r, s, 0));
        if (!tinfo(v, NODE(r, s)->ty, &ti))
            return 0;
        b = is_bf(r, KID(r, s, 0)) ? bf_load(v, a, &ti, NODE(r, KID(r, s, 0))->pun, NODE(r, KID(r, s, 0))->width)
                                   : load_n(v, a, &ti);
        ev_node(r, s);
        return b;
    }
    case TO_CAST: {
        TypeId from = NODE(r, KID(r, n, 0))->ty;
        a = ev_node(r, KID(r, n, 0));
        if (type_is_void(&v->c->tt, t->ty))
            return 0;
        if (!tinfo(v, t->ty, &ti) || !tinfo(v, from, &ts))
            return 0;
        return convert(v, &ti, &ts, a);
    }
    case TO_FCONST:
        if (!tinfo(v, t->ty, &ti))
            return 0;
        a = 0;
        memcpy(&a, r->f->bytes + t->val, t->width < 8 ? t->width : 8);
        return a;
    case TO_F2I: {
        TypeId from = NODE(r, KID(r, n, 0))->ty;
        double d;
        a = ev_node(r, KID(r, n, 0));
        if (v->res.status || !tinfo(v, t->ty, &ti) || !tinfo(v, from, &ts))
            return 0;
        d = fget(&ts, a);
        if (t->pun) {                   /* round to nearest even, as cvtss2si does */
            volatile double m = 4503599627370496.0;     /* 2^52: adding it rounds to an integer */
            if (d > -m && d < m)
                d = d < 0 ? (d - m) + m : (d + m) - m;
        }
        if (d != d || d >= (ti.bytes == 8 ? 9223372036854775808.0 : 2147483648.0) ||
            d < (ti.bytes == 8 ? -9223372036854775808.0 : -2147483648.0))
            return canon(&ti, ti.bytes == 8 ? 0x8000000000000000ull : 0x80000000ull);
        return canon(&ti, (uint64_t)(int64_t)d);
    }
    case TO_UNOP:
        a = ev_node(r, KID(r, n, 0));
        if (v->res.status)
            return 0;
        if (t->pun == P_BANG) {
            if (!tinfo(v, NODE(r, KID(r, n, 0))->ty, &ts))
                return 0;
            return ts.flt ? !(fget(&ts, a) != 0 || fget(&ts, a) != fget(&ts, a)) : a == 0;
        }
        if (!tinfo(v, t->ty, &ti))
            return 0;
        if (ti.flt) {
            if (t->pun == P_MINUS)
                return a ^ (ti.bytes == 4 ? 0x80000000ull : 0x8000000000000000ull);
            if (t->pun == P_PLUS)
                return a;
            UNSUP(v, "unary operator %d on a float", t->pun);
            return 0;
        }
        if (t->pun == P_MINUS) {
            if (ti.sg && !v->o.wrap && !fits(&ti, (int64_t)(0 - a))) {
                TRAP(v, "signed integer overflow");
                return 0;
            }
            return canon(&ti, 0 - a);
        }
        if (t->pun == P_TILDE)
            return canon(&ti, ~a);
        if (t->pun == P_PLUS)
            return a;
        UNSUP(v, "unary operator %d", t->pun);
        return 0;
    case TO_BINOP:
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status || !tinfo(v, t->ty, &ti))
            return 0;
        return ti.flt ? fbin(v, t->pun, &ti, a, b) : int_arith(v, t->pun, t->ty, a, b);
    case TO_CMP:
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status || !tinfo(v, NODE(r, KID(r, n, 0))->ty, &ts))
            return 0;
        return ts.flt ? fcompare(v, t->pun, &ts, a, b) : compare(v, t->pun, NODE(r, KID(r, n, 0))->ty, a, b);
    case TO_COMMA:
        ev_node(r, KID(r, n, 0));
        return ev_node(r, KID(r, n, 1));
    case TO_EXPRSTMT:
        ev_node(r, KID(r, n, 0));
        return 0;
    case TO_NOP:
        return 0;
    case TO_DEREF:
    case TO_ADDR:
        return ev_node(r, KID(r, n, 0));
    case TO_FIELD:
        a = ev_node(r, KID(r, n, 0));
        return mkptr(pid(a), (int64_t)poff(a) + t->val);
    case TO_RFIELD:
        if (t->width) {
            UNSUP(v, "bit-field read as an rvalue field");
            return 0;
        }
        a = ev_node(r, KID(r, n, 0));
        a = mkptr(pid(a), (int64_t)poff(a) + t->val);
        if (is_agg(v, t->ty) || v->res.status)
            return a;
        if (!tinfo(v, t->ty, &ti))
            return 0;
        return load_n(v, a, &ti);
    case TO_PTRADD: {
        int64_t off;
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status)
            return 0;
        if (__builtin_mul_overflow((int64_t)b, t->val, &off) ||
            __builtin_add_overflow(off, (int64_t)poff(a), &off) || off < INT32_MIN ||
            off > INT32_MAX) {
            TRAP(v, "pointer arithmetic out of range");
            return 0;
        }
        return mkptr(pid(a), off);
    }
    case TO_PTRDIFF:
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status)
            return 0;
        if (pid(a) != pid(b)) {
            TRAP(v, "subtraction of pointers into different objects");
            return 0;
        }
        return (uint64_t)(((int64_t)poff(a) - (int64_t)poff(b)) / t->val);
    case TO_LOCAL:
        ev_local(r, n, t->sym);
        return 0;
    case TO_COMPLIT:
        if (t->pun) {
            UNSUP(v, "static compound literal");
            return 0;
        }
        return ev_local(r, n, n);
    case TO_VA:
        return ev_va(r, n);
    case TO_CALL:
        return ev_call_node(r, n);
    default: {
        static const char *nm[] = {
#define X(o, s) s,
            TAST_OPS(X)
#undef X
        };
        UNSUP(v, "operation %s", t->op < TO_COUNT ? nm[t->op] : "?");
        return 0;
    }
    }
}

static uint64_t run_fn(Fr *caller, Fn *f, const uint64_t *args, size_t na, uint64_t va)
{
    Ev *v = caller->v;
    Fr r;
    uint32_t bi = f->entry;
    size_t i;
    uint64_t result = 0;
    memset(&r, 0, sizeof r);
    r.v = v;
    r.f = f;
    r.up = caller;
    r.va = va;
    if (na != f->np) {
        UNSUP(v, "call with a different number of arguments");
        return 0;
    }
    if (v->depth >= v->o.depth) {
        fail(v, EV_FUEL, "exceeded the call depth limit");
        return 0;
    }
    v->depth++;
    r.reg = xcalloc(f->nreg ? f->nreg : 1, sizeof *r.reg);
    r.tmp = xcalloc(f->ntmp ? f->ntmp : 1, sizeof *r.tmp);
    for (i = 0; i < f->np && !v->res.status; i++) {
        Ti ti;
        uint64_t sz;
        Loc l;
        if (!size_of(v, f->par[i].ty, &sz))
            break;
        l.ref = f->par[i].ref;
        l.obj = frame_obj(&r, sz);
        if (!l.obj)
            break;
        vec_push(&r.loc, l);
        if (is_agg(v, f->par[i].ty))
            copy_n(v, mkptr(l.obj, 0), args[i], sz);
        else if (tinfo(v, f->par[i].ty, &ti))
            store_n(v, mkptr(l.obj, 0), &ti, canon(&ti, args[i]));
    }
    while (!v->res.status) {
        Blk *b = &f->blk[bi];
        size_t k;
        for (k = 0; k < b->nst && !v->res.status; k++)
            ev_node(&r, b->st[k]);
        if (v->res.status)
            break;
        switch (b->term) {
        case TK_JMP:
            bi = b->a;
            continue;
        case TK_BR:
        {
            uint64_t c = ev_node(&r, b->val);
            Ti ct;
            if (v->res.status)
                break;
            if (tinfo(v, NODE(&r, b->val)->ty, &ct) && ct.flt)
                c = fget(&ct, c) != 0 || fget(&ct, c) != fget(&ct, c);
            bi = c ? b->a : b->b;
            continue;
        }
        case TK_SWTCH: {
            uint64_t c = ev_node(&r, b->val);
            uint32_t to = b->a;
            for (k = 0; k < b->nc; k++)
                if ((uint64_t)b->cases[k].v == c)
                    to = b->cases[k].to;
            bi = to;
            continue;
        }
        case TK_RET:
            if (b->val != TN) {
                uint64_t x = ev_node(&r, b->val);
                if (v->res.status)
                    break;
                if (is_agg(v, f->ret)) {
                    uint64_t sz;
                    uint32_t id;
                    if (!size_of(v, f->ret, &sz))
                        break;
                    id = frame_obj(caller, sz);     /* the caller's result temporary */
                    if (id) {
                        copy_n(v, mkptr(id, 0), x, sz);
                        result = mkptr(id, 0);
                    }
                } else if (!type_is_void(&v->c->tt, f->ret)) {
                    Ti ti;
                    if (tinfo(v, f->ret, &ti))
                        result = canon(&ti, x);
                }
            }
            break;
        default:
            UNSUP(v, "terminator %d", b->term);
            break;
        }
        break;
    }
    for (i = 0; i < r.own.len; i++)
        kill_obj(v, r.own.data[i]);
    vec_free(&r.own);
    vec_free(&r.loc);
    free(r.reg);
    free(r.tmp);
    v->depth--;
    return result;
}

/* ---- the interface ------------------------------------------------------------ */

Ev *ev_new(Checker *c, const EvOpts *o)
{
    Ev *v = xcalloc(1, sizeof *v);
    v->c = c;
    v->o = *o;
    v->fuel = o->fuel;
    return v;
}

void ev_free(Ev *v)
{
    size_t i, j;
    if (!v)
        return;
    for (i = 0; i < v->obj.len; i++) {
        free(v->obj.data[i].b);
        free(v->obj.data[i].init);
    }
    vec_free(&v->obj);
    for (i = 0; i < v->fn.len; i++) {
        Fn *f = &v->fn.data[i];
        for (j = 0; j < f->nblk; j++) {
            free(f->blk[j].st);
            free(f->blk[j].cases);
        }
        free(f->blk);
        free(f->nodes);
        free(f->kids);
        free(f->bytes);
        free(f->par);
        free(f->strobj);
    }
    vec_free(&v->fn);
    vec_free(&v->sd);
    free(v);
}

/* A copy of n bytes (n may be 0, p then NULL). */
static void *dup_n(const void *p, size_t n)
{
    void *q = xmalloc(n + 1);
    if (n)
        memcpy(q, p, n);
    return q;
}

/* A copy of the unit's nodes, with its static-storage objects (TO_DATA) registered. */
static Fn fn_of_el(Ev *v, El *e)
{
    Fn f;
    size_t i;
    memset(&f, 0, sizeof f);
    f.fsym = e->fsym;
    f.nodes = dup_n(e->nodes.data, e->nodes.len * sizeof *f.nodes);
    f.kids = dup_n(e->kids.data, e->kids.len * sizeof *f.kids);
    f.bytes = dup_n(e->bytes.data, e->bytes.len);
    f.strobj = xcalloc(e->nodes.len + 1, sizeof *f.strobj);
    for (i = 0; i < e->nodes.len; i++) {
        const TNode *t = &e->nodes.data[i];
        Sd d;
        if (t->op != TO_DATA)
            continue;
        memset(&d, 0, sizeof d);
        d.ref = t->sym;
        d.node = (uint32_t)i;
        d.fn = (uint32_t)v->fn.len;     /* the Fn about to be pushed */
        d.ty = t->ty;
        d.local = (t->sym & SYM_LOCAL) != 0;
        vec_push(&v->sd, d);
    }
    return f;
}

void ev_add_data(Ev *v, El *e)
{
    Fn f = fn_of_el(v, e);
    f.fsym = SYM_NONE;
    vec_push(&v->fn, f);
}

void ev_add_tentative(Ev *v, uint32_t ref)
{
    Sd d;
    memset(&d, 0, sizeof d);
    d.ref = ref;
    d.node = TN;
    d.ty = csym(v->c, ref)->ty;
    vec_push(&v->sd, d);
}

void ev_add_func(Ev *v, El *e, Cfg *g)
{
    Fn f = fn_of_el(v, e);
    size_t i;
    f.np = e->params.len;
    f.variadic = (type_ent(&e->c->tt, type_canon(&e->c->tt, csym(e->c, e->fsym)->ty))->flags & TF_VARIADIC) != 0;
    f.par = xmalloc((f.np + 1) * sizeof *f.par);
    for (i = 0; i < f.np; i++) {
        uint32_t ref = e->c->cb[e->params.data[i]] - 1;
        f.par[i].ref = ref;
        f.par[i].ty = csym(e->c, ref)->ty;
    }
    f.nblk = g->b.len;
    f.blk = xcalloc(f.nblk ? f.nblk : 1, sizeof *f.blk);
    for (i = 0; i < f.nblk; i++) {
        const CBlk *s = &g->b.data[i];
        Blk *d = &f.blk[i];
        d->nst = s->st.len;
        d->st = dup_n(s->st.data, s->st.len * sizeof *d->st);
        d->nc = s->cases.len;
        d->cases = dup_n(s->cases.data, s->cases.len * sizeof *d->cases);
        d->val = s->val;
        d->a = s->a;
        d->b = s->b;
        d->term = s->term;
    }
    f.nreg = g->nreg;
    f.ntmp = e->ntmp;
    f.ret = e->ret;
    vec_push(&v->fn, f);
}

EvRes ev_call(Ev *v, uint32_t fsym, const uint64_t *args, size_t n, uint64_t *out)
{
    Fn *f = find_fn(v, fsym);
    Fr top;
    uint64_t x = 0;
    memset(&v->res, 0, sizeof v->res);
    memset(&top, 0, sizeof top);
    top.v = v;
    if (!f)
        UNSUP(v, "no retained body for the function");
    else
        x = run_fn(&top, f, args, n, 0);
    if (!v->res.status && out)
        *out = x;
    {
        size_t i;
        for (i = 0; i < top.own.len; i++)
            kill_obj(v, top.own.data[i]);
        vec_free(&top.own);
    }
    return v->res;
}

/* ---- the checker's hooks ------------------------------------------------------- */

static Ev *ev_of(Checker *c)
{
    if (!c->ev) {
        EvOpts o;
        memset(&o, 0, sizeof o);
        o.fuel = 50000000;
        o.depth = 1000;
        o.wrap = c->opt.eval_wrap;
        c->ev = ev_new(c, &o);
    }
    return c->ev;
}

void eval_keep(Checker *c, El *e, Cfg *g)
{
    ev_add_func(ev_of(c), e, g);
}

void eval_data(Checker *c, El *e)
{
    ev_add_data(ev_of(c), e);
}

void eval_tentative(Checker *c, uint32_t ref)
{
    ev_add_tentative(ev_of(c), ref);
}

void eval_main(Checker *c)
{
    Ev *v = ev_of(c);
    size_t i;
    uint64_t x = 0;
    EvRes r;
    FILE *out = c->opt.tast;
    uint32_t fsym = SYM_NONE;
    for (i = 0; i < v->fn.len; i++)
        if (v->fn.data[i].fsym != SYM_NONE &&
            !strcmp(cident(c, csym(c, v->fn.data[i].fsym)->name), "main"))
            fsym = v->fn.data[i].fsym;
    if (!out)
        return;
    if (fsym == SYM_NONE) {
        fputs("eval: no main\n", out);
        return;
    }
    r = ev_call(v, fsym, NULL, 0, &x);
    if (r.status == EV_OK)
        fprintf(out, "eval main = %d\n", (int)(int32_t)x);
    else
        fprintf(out, "eval main: %s: %s\n",
                r.status == EV_UNSUPPORTED ? "unsupported" : r.status == EV_FUEL ? "limit" : "trap",
                r.why);
}

void eval_release(Checker *c)
{
    ev_free(c->ev);
    c->ev = NULL;
}
