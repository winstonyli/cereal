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
#include "c/eval_int.h"
#include "c/cintrin.h"
/* ---- failure -------------------------------------------------------------- */

void ev_fail(Ev *v, int st, const char *fmt, ...)
{
    if (v->res.status == EV_OK) {
        va_list ap;
        va_start(ap, fmt);
        v->res.status = st;
        v->f_obj = v->ld_obj;
        v->f_off = v->ld_off;
        vsnprintf(v->res.why, sizeof v->res.why, fmt, ap);
        va_end(ap);
    }
}

static bool is_agg(Ev *v, TypeId t)
{
    TypeKind k = type_ckind(&v->c->tt, t);
    return k == TY_ARRAY || type_is_record(&v->c->tt, t);
}

/* An integer, enum, bool or pointer type; false (and a reason) otherwise. */
bool ev_tinfo(Ev *v, TypeId t, Ti *ti)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok;
    uint64_t sz;
    memset(ti, 0, sizeof *ti);
    if (k == TY_PTR || k == TY_NULLPTR) {
        ti->bytes = 8;
        ti->ptr = true;
        if (k == TY_PTR && type_ckind(tt, type_base(tt, t)) == TY_FUNC)
            ti->fty = type_base(tt, t);
        else if (k == TY_PTR) {
            sz = type_size(tt, type_base(tt, t), &ok);
            ti->psz = ok && sz < 4096 ? (unsigned)sz : 0;
        }
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

uint64_t ev_canon(const Ti *ti, uint64_t x)
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

uint32_t ev_new_obj(Ev *v, uint64_t size, bool ro)
{
    Obj o;
    if (v->objlim && v->obj.len >= v->objlim) {
        ev_fail(v, EV_FUEL, "exceeded the object limit");
        return 0;
    }
    if (size > (1u << 30)) {
        TRAP(v, "object of %llu bytes", (unsigned long long)size);
        return 0;
    }
    memset(&o, 0, sizeof o);
    o.size = size;
    o.b = calloc(size ? size : 1, 1);
    o.init = calloc(size ? size : 1, 1);
    if (!o.b || !o.init) {
        free(o.b);
        free(o.init);
        TRAP(v, "object of %llu bytes", (unsigned long long)size);
        return 0;
    }
    o.live = true;
    o.ro = ro;
    vec_push(&v->obj, o);
    return (uint32_t)v->obj.len;
}

void ev_kill_obj(Ev *v, uint32_t id)
{
    Obj *o = &v->obj.data[id - 1];
    o->live = false;
    free(o->b);
    free(o->init);
    free(o->sh);
    o->b = o->init = NULL;
    o->sh = NULL;
}

/* Drops the dead objects at the end of the table, so their ids are made again.  Only for a caller
 * that holds no pointer into them (the sweep, between calls): a stale pointer would otherwise
 * find a new object where it now traps. */
void ev_trim(Ev *v)
{
    while (v->obj.len && !v->obj.data[v->obj.len - 1].live)
        v->obj.len--;
}

void ev_unshadow(Obj *o, uint64_t off, uint64_t n)
{
    if (o->sh)
        memset(o->sh + off, 0, n * sizeof *o->sh);
}


static void sd_init(Ev *v, size_t i);

/* A lazy object grows to cover `need` bytes (rounded up to 16, at most EV_LAZY_CAP): the new bytes are
 * zero and uninitialized.  Its id and address as an object do not change. */
static void lazy_grow(Obj *o, uint64_t need)
{
    uint64_t n = (need + 15) & ~(uint64_t)15;
    if (n > EV_LAZY_CAP)
        n = EV_LAZY_CAP;
    o->b = xrealloc(o->b, n);
    o->init = xrealloc(o->init, n);
    memset(o->b + o->size, 0, n - o->size);
    memset(o->init + o->size, 0, n - o->size);
    if (o->sh) {
        o->sh = xrealloc(o->sh, n * sizeof *o->sh);
        memset(o->sh + o->size, 0, (n - o->size) * sizeof *o->sh);
    }
    o->size = n;
}

/* The bytes [p, p + n) of a live object, or NULL after a trap. */
Obj *ev_access(Ev *v, uint64_t p, uint64_t n, const char *what)
{
    Obj *o;
    int64_t off = poff(p);
    if (pid(p) == 0) {
        TRAP(v, "%s through a null pointer", what);
        return NULL;
    }
    if (pid(p) > v->obj.len) {          /* a pointer made of other bytes */
        TRAP(v, "%s through a pointer to no object", what);
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
    if (o->kind == OK_LABEL) {
        TRAP(v, "%s through a label address", what);
        return NULL;
    }
    if (o->stub) {
        UNSUP(v, "%s of a global object with no definition in this unit", what);
        return NULL;
    }
    if (o->sd1) {                       /* a static object, initialized on its first access */
        sd_init(v, o->sd1 - 1);
        if (v->res.status)
            return NULL;
        o = &v->obj.data[pid(p) - 1];
    }
    if (off < 0 || n > o->size || (uint64_t)off > o->size - n) {      /* n may be huge: no wrapping sum */
        if (o->lazy && off >= 0 && n <= EV_LAZY_CAP && (uint64_t)off <= EV_LAZY_CAP - n)
            lazy_grow(o, (uint64_t)off + n);
        else {
            TRAP(v, "%s out of bounds (offset %lld, n %llu, size %llu)", what, (long long)off, (unsigned long long)n, (unsigned long long)o->size);
            return NULL;
        }
    }
    return o;
}

void ev_lazy_zero(Obj *o, uint64_t off, uint64_t n)
{
    if (o->lazy)
        memset(o->init + off, 1, n);        /* untouched bytes are zero already */
}

/* Tracking: the shadow of the ti->bytes bytes at off of o, as one slot expression (0: all concrete). */
static uint32_t load_sh(Ev *v, const Obj *o, uint64_t off, const Ti *ti)
{
    uint32_t x = 0;
    unsigned i;
    bool any = false;
    if (!v->track || !o->sh || ti->flt || ti->ptr)
        return 0;
    for (i = 0; i < ti->bytes && !any; i++)
        any = o->sh[off + i] != 0;
    if (!any)
        return 0;
    for (i = ti->bytes; i-- > 0;) {
        uint32_t b = o->sh[off + i] ? o->sh[off + i] : sx_const(&v->sx, 8, o->b[off + i]);
        x = x ? sx_join(&v->sx, x, b) : b;
    }
    return sx_ext(&v->sx, x, ti->bytes, ti->sg, ti->boolean);
}

/* The bytes [off, off + n) of o now hold the slot expression sh (0: concrete). */
static void store_sh(Ev *v, Obj *o, uint64_t off, unsigned n, uint32_t sh)
{
    unsigned i;
    if (!sh) {
        ev_unshadow(o, off, n);
        return;
    }
    if (!o->sh)
        o->sh = xcalloc(o->size ? o->size : 1, sizeof *o->sh);
    for (i = 0; i < n; i++)
        o->sh[off + i] = i < 8 ? sx_byte(&v->sx, sh, i) : 0;
}

/* The shadow of a value cast to ti (as ev_canon does it). */
static uint32_t canon_sh(Ev *v, const Ti *ti, uint32_t sh)
{
    if (!sh || ti->flt || ti->ptr)
        return 0;
    if (ti->bytes >= 8 && !ti->boolean)
        return sh;
    return sx_ext(&v->sx, sh, ti->bytes, ti->sg, ti->boolean);
}

/* Sets v->sh to the loaded value's shadow. */
static uint64_t load_n(Ev *v, uint64_t p, const Ti *ti)
{
    Obj *o = ev_access(v, p, ti->bytes, "read");
    uint64_t x = 0;
    unsigned i;
    v->sh = 0;
    if (!o)
        return 0;
    if (o->lazy && v->lazy_fill) {
        for (i = 0; i < ti->bytes && o->init[poff(p) + i]; i++)
            ;
        if (i < ti->bytes && v->lazy_fill(v, pid(p), (uint64_t)poff(p), ti))
            o = &v->obj.data[pid(p) - 1];       /* the fill may have made objects */
    }
    for (i = 0; i < ti->bytes; i++) {
        if (o->init[poff(p) + i] != 1) {
            if (!o->init[poff(p) + i]) {
                v->ld_obj = pid(p);
                v->ld_off = poff(p) + (int32_t)i;
                TRAP(v, "read of an uninitialized value");
                return 0;
            }
            if (!ti->ptr) {             /* init 2: a pointer the sweep drew; its bytes differ in gcc's build */
                v->ld_obj = pid(p);
                v->ld_off = poff(p) + (int32_t)i;
                UNSUP(v, "read of a pointer as an integer");
                return 0;
            }
        }
        x |= (uint64_t)o->b[poff(p) + i] << (8 * i);
    }
    if (ti->ptr) {
        v->ld_obj = pid(p);
        v->ld_off = poff(p);
    }
    v->sh = load_sh(v, o, (uint64_t)poff(p), ti);
    return ev_canon(ti, x);
}

/* False (after UNSUP) if a write of n bytes at off would overwrite part of a pointer the sweep drew: gcc's
 * bytes would differ (see load_n); a whole slot is just an integer in both.  Every write path asks. */
bool ev_pun_ok(Ev *v, const Obj *o, uint64_t off, uint64_t n)
{
    uint64_t i;
    if (!o->lazy || (off % 8 == 0 && n % 8 == 0))
        return true;
    for (i = 0; i < n; i++)
        if (o->init[off + i] == 2) {
            UNSUP(v, "write of an integer over a pointer");
            return false;
        }
    return true;
}

static void store_sh_n(Ev *v, uint64_t p, const Ti *ti, uint64_t x, uint32_t sh)
{
    Obj *o = ev_access(v, p, ti->bytes, "write");
    unsigned i;
    if (!o)
        return;
    if (o->ro) {
        TRAP(v, "write to a string literal or a const object");
        return;
    }
    if (!ti->ptr && !ev_pun_ok(v, o, poff(p), ti->bytes))
        return;
    for (i = 0; i < ti->bytes; i++) {
        o->b[poff(p) + i] = (uint8_t)(x >> (8 * i));
        o->init[poff(p) + i] = o->lazy && ti->ptr && x ? 2 : 1;   /* 2: a pointer, as lazy_fill marks it (see load_n); NULL is zero bytes in gcc too */
    }
    store_sh(v, o, (uint64_t)poff(p), ti->bytes, v->track && !ti->flt && !ti->ptr ? sh : 0);
}

static void store_n(Ev *v, uint64_t p, const Ti *ti, uint64_t x)
{
    store_sh_n(v, p, ti, x, 0);
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
    if (!bf_window(v, pun, width, &n) || !(o = ev_access(v, p, n, "read")))
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
    if (!bf_window(v, pun, width, &n) || !(o = ev_access(v, p, n, "write")))
        return 0;
    if (o->ro) {
        TRAP(v, "write to a string literal or a const object");
        return 0;
    }
    for (i = 0; i < n; i++)
        x |= (uint64_t)o->b[poff(p) + i] << (8 * i);
    x = (x & ~(m << pun)) | ((val & m) << pun);
    for (i = 0; i < n; i++) {
        o->b[poff(p) + i] = (uint8_t)(x >> (8 * i));
        o->init[poff(p) + i] = 1;
    }
    ev_unshadow(o, (uint64_t)poff(p), n);
    return bf_ext(ti, width, val);
}

void ev_copy_n(Ev *v, uint64_t dst, uint64_t src, uint64_t n)
{
    Obj *d, *s;
    if (n == 0)
        return;
    s = ev_access(v, src, n, "read");
    d = s ? ev_access(v, dst, n, "write") : NULL;
    if (!d)
        return;
    if (d->ro) {
        TRAP(v, "write to a string literal or a const object");
        return;
    }
    if (!ev_pun_ok(v, d, (uint64_t)poff(dst), n))
        return;
    if (s->lazy) {                      /* a pointer the sweep drew moves only whole, to a slot of the same alignment (see load_n) */
        uint64_t i, so = (uint64_t)poff(src);
        for (i = 0; i < n; i++)
            if (s->init[so + i] == 2 && (((so + i) & ~7ull) < so || ((so + i) & ~7ull) + 8 > so + n || (so ^ (uint64_t)poff(dst)) % 8)) {
                UNSUP(v, "read of a pointer as an integer");
                return;
            }
    }
    ev_lazy_zero(s, (uint64_t)poff(src), n);
    memmove(d->b + poff(dst), s->b + poff(src), n);
    memmove(d->init + poff(dst), s->init + poff(src), n);
    if (s->sh) {
        if (!d->sh)
            d->sh = xcalloc(d->size ? d->size : 1, sizeof *d->sh);
        memmove(d->sh + poff(dst), s->sh + poff(src), n * sizeof *d->sh);
    } else {
        ev_unshadow(d, (uint64_t)poff(dst), n);
    }
}

void ev_zero_n(Ev *v, uint64_t dst, uint64_t n)
{
    Obj *d = ev_access(v, dst, n, "write");
    if (!d || !ev_pun_ok(v, d, (uint64_t)poff(dst), n))
        return;
    memset(d->b + poff(dst), 0, n);
    memset(d->init + poff(dst), 1, n);
    ev_unshadow(d, (uint64_t)poff(dst), n);
}

/* ---- floating point: the slot holds the IEEE bits of a float (low 32) or double ---- */

double ev_fget(const Ti *ti, uint64_t x)
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

uint64_t ev_fput(const Ti *ti, double d)
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
        float x = (float)ev_fget(ti, a), y = (float)ev_fget(ti, b), r;
        switch (p) {
        case P_PLUS: r = x + y; break;
        case P_MINUS: r = x - y; break;
        case P_STAR: r = x * y; break;
        case P_SLASH: r = x / y; break;
        default: UNSUP(v, "float operator %d", p); return 0;
        }
        return ev_fput(ti, r);
    } else {
        double x = ev_fget(ti, a), y = ev_fget(ti, b), r;
        switch (p) {
        case P_PLUS: r = x + y; break;
        case P_MINUS: r = x - y; break;
        case P_STAR: r = x * y; break;
        case P_SLASH: r = x / y; break;
        default: UNSUP(v, "float operator %d", p); return 0;
        }
        return ev_fput(ti, r);
    }
}

static uint64_t fcompare(Ev *v, int p, const Ti *ti, uint64_t a, uint64_t b)
{
    double x = ev_fget(ti, a), y = ev_fget(ti, b);
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
    if (!from->flt && !to->flt) {
        if (from->ptr && !to->ptr && !to->boolean && a && v->lazy_fill) {
            UNSUP(v, "address as an integer");     /* the sweep's addresses are not gcc's: a result built from one differs */
            return 0;
        }
        return ev_canon(to, a);
    }
    if (from->flt && to->flt)
        return ev_fput(to, ev_fget(from, a));
    if (to->flt) {
        if (from->bytes == 8 && !from->sg)
            return ev_fput(to, to->bytes == 4 ? (double)(float)(uint64_t)a : (double)(uint64_t)a);
        return ev_fput(to, to->bytes == 4 ? (double)(float)(int64_t)a : (double)(int64_t)a);
    } else {
        double d = ev_fget(from, a), lo, hi;
        if (to->boolean)
            return d != 0 || d != d;
        if (d != d && v->fold)
            return 0;                   /* gcc folds NaN to 0 */
        if (d != d) {
            TRAP(v, "conversion of NaN to an integer");
            return 0;
        }
        if (to->sg) {
            hi = to->bytes == 8 ? 9223372036854775808.0 : (double)(1ull << (8 * to->bytes - 1));
            lo = -hi;
            if (v->fold && (!(d > lo - 1 && d < hi) || (to->bytes == 8 && d < lo)))
                return ev_canon(to, d < 0 ? (uint64_t)(int64_t)lo : (to->bytes == 8 ? 0x7fffffffffffffffull : (uint64_t)(1ull << (8 * to->bytes - 1)) - 1));
            if (!(d > lo - 1 && d < hi) || (to->bytes == 8 && d < lo)) {
                TRAP(v, "floating-point value out of range for the integer type");
                return 0;
            }
            return ev_canon(to, (uint64_t)(int64_t)d);
        }
        hi = to->bytes == 8 ? 18446744073709551616.0 : (double)(1ull << (8 * to->bytes));
        if (v->fold && !(d > -1 && d < hi))
            return ev_canon(to, d < 0 ? 0 : to->bytes == 8 ? ~0ull : (1ull << (8 * to->bytes)) - 1);
        if (!(d > -1 && d < hi)) {
            TRAP(v, "floating-point value out of range for the integer type");
            return 0;
        }
        return ev_canon(to, (uint64_t)d);
    }
}

/* ---- arithmetic ------------------------------------------------------------ */

static bool fits(const Ti *ti, int64_t x)
{
    return ti->bytes >= 8 || (int64_t)ev_canon(ti, (uint64_t)x) == x;
}

static uint64_t int_arith(Ev *v, int p, TypeId ty, uint64_t a, uint64_t b)
{
    Ti ti;
    int64_t sa = (int64_t)a, sb = (int64_t)b, r = 0;
    bool ovf = false;
    if (!ev_tinfo(v, ty, &ti))
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
        return ev_canon(&ti, p == P_PLUS ? a + b : p == P_MINUS ? a - b : a * b);
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
                return p == P_SLASH ? ev_canon(&ti, 0 - a) : 0;
            }
            return ev_canon(&ti, (uint64_t)(p == P_SLASH ? sa / sb : sa % sb));
        }
        return ev_canon(&ti, p == P_SLASH ? a / b : a % b);
    case P_AMP: return ev_canon(&ti, a & b);
    case P_PIPE: return ev_canon(&ti, a | b);
    case P_CARET: return ev_canon(&ti, a ^ b);
    case P_SHL:
    case P_SHR:
        if (b >= 8 * ti.bytes) {        /* also negative counts */
            TRAP(v, "shift count out of range");
            return 0;
        }
        if (p == P_SHR)
            return ev_canon(&ti, ti.sg ? (uint64_t)(sa >> b) : a >> b);
        if (ti.sg && sa < 0 && !v->o.wrap) {
            TRAP(v, "left shift of a negative value");
            return 0;
        }
        return ev_canon(&ti, a << b);
    default:
        UNSUP(v, "binary operator %d", p);
        return 0;
    }
}

/* The sweep reads what the program compares its inputs with (eval_run.c, guided calls). */
static void ev_note(Ev *v, uint64_t x)
{
    if (v->dict_on && v->dict.len < 512)
        vec_push(&v->dict, x);
}

/* A difference or an order of two pointers is defined only within one object (== and != are defined
 * anywhere); the sweep retries a call that stops here with the two objects made one (sub_a, sub_b). */
static bool same_object(Ev *v, uint64_t a, uint64_t b, const char *what)
{
    if (pid(a) == pid(b))
        return true;
    v->sub_a = pid(a);
    v->sub_b = pid(b);
    TRAP(v, "%s of pointers into different objects", what);
    return false;
}

static uint64_t compare(Ev *v, int p, TypeId ot, uint64_t a, uint64_t b)
{
    Ti ti;
    bool sg;
    if (!ev_tinfo(v, ot, &ti))
        return 0;
    sg = ti.sg;
    ev_note(v, a);
    ev_note(v, b);
    if (ti.ptr && p != P_EQEQ && p != P_NE && pid(a) && pid(b) && !same_object(v, a, b, "comparison"))
        return 0;
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
    uint32_t id = ev_new_obj(r->v, size, false);
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

/* ---- the functions' index ------------------------------------------------- */

static uint64_t hash_id(const Checker *c, uint32_t fsym)
{
    return ((uint64_t)(uintptr_t)c * 0x9e3779b97f4a7c15ull ^ fsym) * 0xff51afd7ed558ccdull;
}

static uint64_t hash_name(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static bool fn_external(Checker *c, uint32_t fsym)
{
    return !(fsym & SYM_LOCAL) && csym(c, fsym)->linkage == LK_EXTERNAL;
}

static const char *fn_name(Checker *c, uint32_t fsym)
{
    return cident(c, csym(c, fsym)->name);
}

/* Add the functions pushed since the last call; both tables are open addressing, entry = index + 1. */
static void fn_sync(Ev *v)
{
    size_t i;
    if (v->fi_cap < 4 * v->fn.len + 8) {
        free(v->fi_id);
        free(v->fi_nm);
        for (v->fi_cap = 64; v->fi_cap < 8 * v->fn.len + 8;)
            v->fi_cap *= 2;
        v->fi_id = xcalloc(v->fi_cap, sizeof *v->fi_id);
        v->fi_nm = xcalloc(v->fi_cap, sizeof *v->fi_nm);
        v->fi_done = 0;
    }
    for (i = v->fi_done; i < v->fn.len; i++) {
        const Fn *f = &v->fn.data[i];
        size_t k;
        if (f->fsym == SYM_NONE)
            continue;
        for (k = hash_id(f->c, f->fsym) & (v->fi_cap - 1); v->fi_id[k]; k = (k + 1) & (v->fi_cap - 1))
            ;
        v->fi_id[k] = (uint32_t)i + 1;
        /* an external definition: not an inline one, which each file has its own copy of (the intrinsics of
         * <emmintrin.h>) */
        if (!v->link || !f->user || !fn_external(f->c, f->fsym) || (csym(f->c, f->fsym)->flags & CSF_INLINE))
            continue;
        for (k = hash_name(fn_name(f->c, f->fsym)) & (v->fi_cap - 1); v->fi_nm[k]; k = (k + 1) & (v->fi_cap - 1))
            if (!strcmp(fn_name(v->fn.data[v->fi_nm[k] - 1].c, v->fn.data[v->fi_nm[k] - 1].fsym),
                        fn_name(f->c, f->fsym)))
                v->dup_def = fn_name(f->c, f->fsym);
        v->fi_nm[k] = (uint32_t)i + 1;
    }
    v->fi_done = v->fn.len;
}

/* The body of function fsym of file c, in that file. */
static Fn *find_fn(Ev *v, const Checker *c, uint32_t fsym)
{
    size_t k;
    fn_sync(v);
    for (k = hash_id(c, fsym) & (v->fi_cap - 1); v->fi_id[k]; k = (k + 1) & (v->fi_cap - 1))
        if (v->fn.data[v->fi_id[k] - 1].fsym == fsym && v->fn.data[v->fi_id[k] - 1].c == c)
            return &v->fn.data[v->fi_id[k] - 1];
    return NULL;
}

/* ... or, linking, the external definition of its name in another file. */
static Fn *find_def(Ev *v, Checker *c, uint32_t fsym)
{
    Fn *g = find_fn(v, c, fsym);
    const char *nm;
    size_t k;
    if (g || !v->link || !fn_external(c, fsym))
        return g;
    nm = fn_name(c, fsym);
    for (k = hash_name(nm) & (v->fi_cap - 1); v->fi_nm[k]; k = (k + 1) & (v->fi_cap - 1)) {
        Fn *d = &v->fn.data[v->fi_nm[k] - 1];
        if (!strcmp(fn_name(d->c, d->fsym), nm))
            return d;
    }
    return NULL;
}

/* A function's object: one per definition (a declaration without a body in this file takes the
 * definition's, when linking). */
uint32_t ev_function_obj(Ev *v, Checker *c, uint32_t fsym)
{
    Fn *g = find_def(v, c, fsym);
    size_t i;
    Obj o;
    if (g) {
        c = g->c;
        fsym = g->fsym;
    }
    for (i = 0; i < v->obj.len; i++)
        if (v->obj.data[i].fn == fsym + 1 && v->obj.data[i].fc == c)
            return (uint32_t)i + 1;
    memset(&o, 0, sizeof o);
    o.fn = fsym + 1;
    o.fc = c;
    o.live = true;
    vec_push(&v->obj, o);
    return (uint32_t)v->obj.len;
}

static uint64_t ev_node(Fr *r, uint32_t n);
/* ash: a shadow per argument (NULL: all concrete); *rsh: the result's shadow. */
static uint64_t run_fn(Fr *caller, Fn *f, const uint64_t *args, const uint32_t *ash, size_t na, uint64_t va,
                       uint32_t *rsh);

#define NODE(r, n) (&(r)->f->nodes[n])
#define KID(r, n, j) ((r)->f->kids[NODE(r, n)->kid0 + (j)])

/* A call of a host function: the arguments with their types, the result as the call's type. */
static uint64_t host_call(Fr *r, uint32_t n, uint32_t first, uint32_t na, HostFn h, const uint64_t *args)
{
    Ev *v = r->v;
    HArg ha[16];
    uint32_t j;
    uint64_t x;
    Ti ti;
    for (j = 0; j < na; j++) {
        ha[j].x = args[j];
        ha[j].ty = NODE(r, KID(r, n, first + j))->ty;
    }
    x = h(v, ha, na);
    if (v->res.status || type_is_void(&v->c->tt, NODE(r, n)->ty) || !ev_tinfo(v, NODE(r, n)->ty, &ti))
        return 0;
    return ev_canon(&ti, x);
}

__extension__ typedef __int128 i128;

/* {add,sub,mul}_overflow (a, b, &r): the exact result in 128 bits, stored truncated to r's type;
 * the flag says the stored value differs.  Operands are evaluated right to left, as cgx86.c. */
static uint64_t ev_ovf(Fr *r, uint32_t n, const BiRow *row)
{
    Ev *v = r->v;
    TypeTable *tt = &v->c->tt;
    Ti ta, tb, tr;
    TypeId pt;
    uint64_t a, b, p, back;
    i128 A, B, R = 0;
    bool big = false;
    p = ev_node(r, KID(r, n, 2));
    b = ev_node(r, KID(r, n, 1));
    a = ev_node(r, KID(r, n, 0));
    pt = type_canon(tt, NODE(r, KID(r, n, 2))->ty);
    if (v->res.status || !ev_tinfo(v, NODE(r, KID(r, n, 0))->ty, &ta) ||
        !ev_tinfo(v, NODE(r, KID(r, n, 1))->ty, &tb))
        return 0;
    if (type_ckind(tt, pt) != TY_PTR || !ev_tinfo(v, type_base(tt, pt), &tr)) {
        UNSUP(v, "overflow built-in result is not a pointer to an integer");
        return 0;
    }
    A = ta.sg ? (i128)(int64_t)a : (i128)a;
    B = tb.sg ? (i128)(int64_t)b : (i128)b;
    switch (row->arg & 3) {
    case OV_ADD: R = A + B; break;
    case OV_SUB: R = A - B; break;
    default: big = __builtin_mul_overflow(A, B, &R); break;
    }
    store_n(v, p, &tr, (uint64_t)R);
    back = ev_canon(&tr, (uint64_t)R);
    return big || (tr.sg ? (i128)(int64_t)back : (i128)back) != R;
}

/* An atomic built-in on one thread: a plain load, store or read-modify-write; the memory
 * order is ignored.  The object's width comes from the pointee, the operation is on its raw
 * bytes. */
static uint64_t ev_atomic(Fr *r, uint32_t n, const BiRow *row)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    TypeTable *tt = &v->c->tt;
    int op = row->arg & 15, fl = row->arg >> 4;
    bool by_ptr = fl & AF_PTR, ok = false;
    TypeId pt = type_canon(tt, NODE(r, KID(r, n, 0))->ty);
    Ti raw, rt;
    uint64_t sz = 0, p, x = 0, old, y = 0, q = 0;
    memset(&raw, 0, sizeof raw);
    if (type_ckind(tt, pt) == TY_PTR)
        sz = type_size(tt, type_base(tt, pt), &ok);
    if (!ok || (sz != 1 && sz != 2 && sz != 4 && sz != 8)) {
        UNSUP(v, "atomic object that is not a pointer to 1, 2, 4 or 8 bytes");
        return 0;
    }
    raw.bytes = (unsigned)sz;
    p = ev_node(r, KID(r, n, 0));
    if (v->res.status)
        return 0;
    if (op != AT_LOAD && op != AT_ST0) {        /* the value operand, or where it is */
        x = ev_node(r, KID(r, n, 1));
        if (op == AT_CAS && !(fl & AF_EXPV)) {
            q = x;
            x = load_n(v, q, &raw);
        } else if (by_ptr) {
            x = load_n(v, x, &raw);
        }
        x = ev_canon(&raw, x);
    }
    old = load_n(v, p, &raw);
    if (v->res.status)
        return 0;
    switch (op) {
    case AT_LOAD:
        if (by_ptr)
            store_n(v, ev_node(r, KID(r, n, 1)), &raw, old);
        y = old;
        break;
    case AT_STORE: case AT_ST0: case AT_XCHG:
        store_n(v, p, &raw, op == AT_ST0 ? 0 : x);
        if (op == AT_XCHG && by_ptr)
            store_n(v, ev_node(r, KID(r, n, 2)), &raw, old);
        y = old;
        break;
    case AT_CAS: {
        uint64_t want = ev_node(r, KID(r, n, 2));
        if (by_ptr)
            want = load_n(v, want, &raw);
        want = ev_canon(&raw, want);
        if (old == x) {
            store_n(v, p, &raw, want);
        } else if (!(fl & AF_EXPV)) {
            store_n(v, q, &raw, old);
        }
        y = (fl & AF_OLD) ? old : old == x;
        break;
    }
    default:
        y = op == AT_ADD ? old + x : op == AT_SUB ? old - x : op == AT_AND ? old & x :
            op == AT_OR ? old | x : op == AT_XOR ? old ^ x : ~(old & x);
        y = ev_canon(&raw, y);
        store_n(v, p, &raw, y);
        if (!(fl & AF_NEW))
            y = old;
        break;
    }
    if (v->res.status || type_is_void(tt, t->ty) || !ev_tinfo(v, t->ty, &rt))
        return 0;
    return ev_canon(&rt, y);
}

/* A TO_INTRIN node (cintrin.h).  The integer operations are bi_int, shared with the front end's
 * folder; an undefined result stops the evaluation.  A row with no meaning here says so. */
static uint64_t ev_intrin(Fr *r, uint32_t n)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    const BiRow *row = &bi_rows[t->pun];
    Ti ti, ta;
    uint64_t a[16], x, y = 0;
    uint32_t j, na = t->nkids;
    int st;
    if (!t->pun || na > 16)
        goto unsup;
    switch (row->cls) {
    case BIC_ATOMIC:
        return ev_atomic(r, n, row);
    case BIC_OVF:
        return ev_ovf(r, n, row);
    case BIC_EXPECT:
        if (NODE(r, KID(r, n, 1))->op != TO_CONST)
            goto unsup;
        return ev_node(r, KID(r, n, 0));
    case BIC_PREFETCH:
        ev_node(r, KID(r, n, 0));
        return 0;
    case BIC_SFENCE: case BIC_LFENCE: case BIC_MFENCE: case BIC_PAUSE:
        return 0;
    case BIC_UD2:
        TRAP(v, "__builtin_unreachable reached");
        return 0;
    case BIC_HUGE:
        return ev_tinfo(v, t->ty, &ti) ? ev_fput(&ti, __builtin_inf()) : 0;
    case BIC_SQRT:
        x = ev_node(r, KID(r, n, 0));
        if (v->res.status || !ev_tinfo(v, t->ty, &ti))
            return 0;
        return ev_fput(&ti, ti.bytes == 4 ? (double)__builtin_sqrtf((float)ev_fget(&ti, x))
                                          : __builtin_sqrt(ev_fget(&ti, x)));
    case BIC_LIBC: {
        HostFn h = ev_host_fn(row->name);
        for (j = na; j-- > 0;)
            a[j] = ev_node(r, KID(r, n, j));
        if (v->res.status)
            return 0;
        if (!h)
            goto unsup;
        return host_call(r, n, 0, na, h, a);
    }
    case BIC_CLZ: case BIC_CTZ: case BIC_CLRSB: case BIC_FFS: case BIC_POPCNT:
    case BIC_PARITY: case BIC_BSWAP: case BIC_STDC:
        x = ev_node(r, KID(r, n, 0));
        if (v->res.status || !ev_tinfo(v, NODE(r, KID(r, n, 0))->ty, &ta) ||
            !ev_tinfo(v, t->ty, &ti))
            return 0;
        st = bi_int(row, ta.bytes, x, &y);
        if (st == BI_UNDEF) {
            TRAP(v, "built-in %s: result undefined", row->name);
            return 0;
        }
        if (st == BI_OK)
            return ev_canon(&ti, y);
        break;
    default:
        break;
    }
unsup:
    UNSUP(v, "built-in %s", cident(v->c, t->sym));
    return 0;
}

/* A call into another file: a record or array argument or result is copied as bytes, so both sides
 * must give it the same size (field offsets are assumed equal: the files share the declaration). */
static bool agg_size(Checker *c, TypeId ty, uint64_t *sz)
{
    bool ok;
    if (type_ckind(&c->tt, ty) != TY_ARRAY && !type_is_record(&c->tt, ty))
        return false;
    *sz = type_size(&c->tt, ty, &ok);
    return ok;
}

static bool link_sizes_ok(Fr *r, uint32_t n, uint32_t first, size_t na, const Fn *g, TypeId rty)
{
    Ev *v = r->v;
    size_t j;
    uint64_t a, b;
    for (j = 0; j < na && j < g->np; j++)
        if (agg_size(v->c, NODE(r, KID(r, n, first + j))->ty, &a) != agg_size(g->c, g->par[j].ty, &b) ||
            (agg_size(v->c, NODE(r, KID(r, n, first + j))->ty, &a) && a != b)) {
            UNSUP(v, "call across files: argument %zu is a record of different sizes in the two files", j + 1);
            return false;
        }
    if (agg_size(v->c, rty, &a) != agg_size(g->c, g->ret, &b) || (agg_size(v->c, rty, &a) && a != b)) {
        UNSUP(v, "call across files: the result is a record of different sizes in the two files");
        return false;
    }
    return true;
}

static uint64_t ev_call_node(Fr *r, uint32_t n, uint32_t *sh)
{
    const TNode *t = NODE(r, n);
    Ev *v = r->v;
    uint64_t args[16], callee = 0;
    uint32_t ash[16];
    uint32_t j, first = t->pun == 1 ? 1 : 0, na = t->nkids - first;
    Fn *g;
    if (na > 16) {
        UNSUP(v, "call with more than 16 arguments");
        return 0;
    }
    for (j = na; j-- > 0;) {            /* right to left, as cgx86.c */
        args[j] = ev_node(r, KID(r, n, first + j));
        ash[j] = v->sh;
    }
    if (t->pun == 1) {
        callee = ev_node(r, KID(r, n, 0));
        if (v->res.status)
            return 0;
        if (pid(callee) == 0) {
            TRAP(v, "call through a null pointer");
            return 0;
        }
        if (pid(callee) > v->obj.len || !v->obj.data[pid(callee) - 1].fn || poff(callee) != 0) {
            TRAP(v, "call through a pointer that is not a function");
            return 0;
        }
        g = find_def(v, v->obj.data[pid(callee) - 1].fc, v->obj.data[pid(callee) - 1].fn - 1);
    } else {
        g = find_def(v, v->c, t->sym);
    }
    if (v->res.status)
        return 0;
    if (g && g->c != v->c && !link_sizes_ok(r, n, first, na, g, t->ty))
        return 0;
    if (!g) {                           /* a declared function: the host library, or unsupported */
        uint32_t fs = t->pun == 1 ? v->obj.data[pid(callee) - 1].fn - 1 : t->sym;
        Checker *fc = t->pun == 1 ? v->obj.data[pid(callee) - 1].fc : v->c;
        const char *name = cident(fc, csym(fc, fs)->name);
        HostFn h = ev_host_fn(name);
        if (!h) {
            UNSUP(v, "call of %s, which has no body here and is not in the host library", name);
            return 0;
        }
        return host_call(r, n, first, na, h, args);
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
        id = ev_new_obj(v, total, false);
        if (!id)
            return 0;
        vec_push(&r->own, id);          /* gone with the caller's frame, after the call returns */
        for (j = (uint32_t)g->np; j < na; j++) {
            TypeId at_ = NODE(r, KID(r, n, first + j))->ty;
            if (is_agg(v, at_)) {
                if (!size_of(v, at_, &sz))
                    return 0;
                ev_copy_n(v, mkptr(id, (int64_t)at), args[j], sz);
                at += (sz + 7) / 8 * 8;
            } else {
                Ti ti;
                if (!ev_tinfo(v, at_, &ti))
                    return 0;
                ti.bytes = 8;
                ti.sg = false;
                store_n(v, mkptr(id, (int64_t)at), &ti, args[j]);
                at += 8;
            }
        }
        return run_fn(r, g, args, ash, g->np, mkptr(id, 0), sh);
    }
    return run_fn(r, g, args, ash, na, 0, sh);
}

/* Store the elements (TO_ELEM kids) of node n into the zeroed object at p. */
static bool addr_node(Fr *r, uint32_t n);

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
            if (!ev_tinfo(v, el->ty, &ti))
                return;
            x = ev_node(r, vn);
            bf_store(v, mkptr(pid(p), poff(p) + el->val), &ti, el->pun, el->width, x);
        } else if (vt->op == TO_STR) {
            uint64_t sp = ev_node(r, vn), cn = (uint64_t)vt->width;
            uint64_t esz;
            if (!size_of(v, el->ty, &esz))
                return;
            ev_copy_n(v, mkptr(pid(p), poff(p) + el->val), sp, cn < esz ? cn : esz);
        } else if (is_agg(v, el->ty)) {
            uint64_t esz;
            if (!size_of(v, el->ty, &esz))
                return;
            ev_copy_n(v, mkptr(pid(p), poff(p) + el->val), ev_node(r, vn), esz);
        } else if (ev_tinfo(v, el->ty, &ti)) {
            x = ev_node(r, vn);
            store_n(v, mkptr(pid(p), poff(p) + el->val), &ti, x);
            if (v->rec_id && pid(p) == v->rec_id && !v->res.status && addr_node(r, vn)) {
                vec_push(&v->rec, (uint32_t)el->val);
                vec_push(&v->rec, ti.bytes);
            }
        }
    }
}

/* Whether the value of node n is an address (a relocation in a static image),
 * told from the tree: the evaluator's slot does not say. */
static bool addr_node(Fr *r, uint32_t n)
{
    const TNode *t = NODE(r, n);
    switch (t->op) {
    case TO_ADDR:
    case TO_LABADDR:
        return true;
    case TO_CAST:
    case TO_PTRADD:
        return addr_node(r, KID(r, n, 0));
    default:
        return false;
    }
}

/* ---- the static objects' index ---------------------------------------------- */

#define SD_GLOBAL 0xffffffffu           /* the "function" of a file-scope object */

static uint64_t hash_sd(const Checker *c, uint32_t ref, uint32_t fn)
{
    return (hash_id(c, ref) ^ fn) * 0xc4ceb9fe1a85ec53ull;
}

/* Add the Sds pushed since the last call: by (file, ref, function) keeping the defined one over a
 * tentative one, and, linking, by name for the external ones, keeping the best (defined, else the
 * first tentative one).  Open addressing, entry = index + 1. */
static void sd_sync(Ev *v)
{
    size_t i;
    if (v->si_cap < 4 * v->sd.len + 8) {
        free(v->si_id);
        free(v->si_nm);
        for (v->si_cap = 64; v->si_cap < 8 * v->sd.len + 8;)
            v->si_cap *= 2;
        v->si_id = xcalloc(v->si_cap, sizeof *v->si_id);
        v->si_nm = xcalloc(v->si_cap, sizeof *v->si_nm);
        v->si_done = 0;
    }
    for (i = v->si_done; i < v->sd.len; i++) {
        const Sd *d = &v->sd.data[i];
        uint32_t fn = d->local ? d->fn : SD_GLOBAL;
        size_t k;
        for (k = hash_sd(d->c, d->ref, fn) & (v->si_cap - 1); v->si_id[k]; k = (k + 1) & (v->si_cap - 1)) {
            const Sd *o = &v->sd.data[v->si_id[k] - 1];
            if (o->c == d->c && o->ref == d->ref && (o->local ? o->fn : SD_GLOBAL) == fn)
                break;
        }
        if (!v->si_id[k] || (v->sd.data[v->si_id[k] - 1].node == TN && d->node != TN))
            v->si_id[k] = (uint32_t)i + 1;
        if (!v->link || d->local || d->stub || d->alias || csym(d->c, d->ref)->linkage != LK_EXTERNAL)
            continue;
        for (k = hash_name(cident(d->c, csym(d->c, d->ref)->name)) & (v->si_cap - 1); v->si_nm[k];
             k = (k + 1) & (v->si_cap - 1)) {
            const Sd *o = &v->sd.data[v->si_nm[k] - 1];
            if (!strcmp(cident(o->c, csym(o->c, o->ref)->name), cident(d->c, csym(d->c, d->ref)->name)))
                break;
        }
        if (!v->si_nm[k] || (v->sd.data[v->si_nm[k] - 1].node == TN && d->node != TN))
            v->si_nm[k] = (uint32_t)i + 1;
    }
    v->si_done = v->sd.len;
}

/* The Sd of (file, ref, function), NULL if none. */
static Sd *sd_find(Ev *v, const Checker *c, uint32_t ref, uint32_t fn)
{
    size_t k;
    sd_sync(v);
    for (k = hash_sd(c, ref, fn) & (v->si_cap - 1); v->si_id[k]; k = (k + 1) & (v->si_cap - 1)) {
        Sd *o = &v->sd.data[v->si_id[k] - 1];
        if (o->c == c && o->ref == ref && (o->local ? o->fn : SD_GLOBAL) == fn)
            return o;
    }
    return NULL;
}

/* The best external Sd of a name (index + 1), 0: none. */
static size_t sd_named(Ev *v, const char *nm)
{
    size_t k;
    sd_sync(v);
    for (k = hash_name(nm) & (v->si_cap - 1); v->si_nm[k]; k = (k + 1) & (v->si_cap - 1)) {
        const Sd *o = &v->sd.data[v->si_nm[k] - 1];
        if (!strcmp(cident(o->c, csym(o->c, o->ref)->name), nm))
            return v->si_nm[k];
    }
    return 0;
}

/* The object of Sd i, made on first use; its contents wait for the first access. */
static uint32_t sd_obj(Ev *v, size_t i)
{
    Sd *s = &v->sd.data[i];
    uint64_t sz = 0;
    uint32_t id;
    Obj *o;
    if (s->obj)
        return s->obj;
    if (s->alias)
        return sd_obj(v, s->alias - 1);
    if (!s->stub) {
        Checker *save = v->c;
        bool ok;
        v->c = s->c;                    /* the type is the defining file's */
        ok = size_of(v, s->ty, &sz);
        v->c = save;
        if (!ok)
            return 0;
    }
    id = ev_new_obj(v, sz, false);
    if (!id)
        return 0;
    s->obj = id;
    o = &v->obj.data[id - 1];
    o->kind = OK_STATIC;
    o->ref = s->ref;
    o->fname = s->local ? v->fn.data[s->fn].fname : 0;
    o->stub = s->stub;
    o->sd1 = s->stub ? 0 : (uint32_t)i + 1;
    return id;
}

/* A const object (or an array of them, through typedefs): writing it is undefined, and gcc's build crashes. */
static bool const_object(const TypeTable *tt, TypeId t)
{
    for (;;) {
        const TypeEnt *e;
        if (TYPE_QUALS(t) & TQ_CONST)
            return true;
        e = type_ent(tt, TYPE_UNQUAL(t));
        if (e->kind != TY_ARRAY && e->kind != TY_TYPEDEF)
            return false;
        t = e->base;
    }
}

/* Zero Sd i's object and store its initializer (before: it may point at itself). */
static void sd_init(Ev *v, size_t i)
{
    Sd *s = &v->sd.data[i];
    uint32_t id = s->obj;
    Obj *o = &v->obj.data[id - 1];
    uint64_t sz = o->size;
    o->sd1 = 0;
    ev_zero_n(v, mkptr(id, 0), sz);
    if (s->node != TN) {
        Fr ir;
        Fn *f = &v->fn.data[s->fn];
        Checker *save = v->c;
        size_t k;
        v->c = s->c;                    /* the initializer is in the file that defines it */
        memset(&ir, 0, sizeof ir);
        ir.v = v;
        ir.f = f;
        ir.reg = xcalloc(f->nreg + 1, sizeof *ir.reg);
        ir.tmp = xcalloc(f->ntmp + 1, sizeof *ir.tmp);
        init_elems(&ir, s->node, mkptr(id, 0));
        for (k = 0; k < ir.own.len; k++)
            ev_kill_obj(v, ir.own.data[k]);
        vec_free(&ir.own);
        free(ir.reg);
        free(ir.tmp);
        v->c = save;
    }
    if (!s->stub && const_object(&s->c->tt, s->ty))
        v->obj.data[id - 1].ro = true;
}

/* The object of a static-storage symbol.  Without a definition registered, a
 * global gets a stub: its address is usable, its contents are not. */
static uint32_t static_obj(Fr *r, uint32_t ref)
{
    Ev *v = r->v;
    bool local = (ref & SYM_LOCAL) != 0;
    Sd *s = sd_find(v, v->c, ref, local ? (uint32_t)(r->f - v->fn.data) : SD_GLOBAL);
    if (s && s->alias)
        return sd_obj(v, s->alias - 1);
    if (v->link && !local && (!s || s->stub || s->node == TN) && csym(v->c, ref)->linkage == LK_EXTERNAL) {
        /* the definition of this name in any file, else the first tentative one */
        size_t best = sd_named(v, cident(v->c, csym(v->c, ref)->name));
        if (best && (!s || s != &v->sd.data[best - 1])) {
            size_t si;
            if (!s) {
                Sd d;
                memset(&d, 0, sizeof d);
                d.c = v->c;
                d.ref = ref;
                d.node = TN;
                d.stub = true;
                vec_push(&v->sd, d);
                s = &v->sd.data[v->sd.len - 1];
            }
            si = (size_t)(s - v->sd.data);
            v->sd.data[si].alias = (uint32_t)best;
            return sd_obj(v, best - 1);
        }
    }
    if (!s) {
        uint32_t h = local ? 0 : ev_host_object(v, cident(v->c, csym(v->c, ref)->name));
        Sd d;
        if (h)
            return h;
        if (local) {
            UNSUP(v, "static local with no definition in this unit");
            return 0;
        }
        memset(&d, 0, sizeof d);
        d.c = v->c;
        d.ref = ref;
        d.node = TN;
        d.stub = true;
        vec_push(&v->sd, d);
        s = &v->sd.data[v->sd.len - 1];
    }
    return sd_obj(v, (size_t)(s - v->sd.data));
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
        ev_kill_obj(v, *slot);             /* a new lifetime: a fresh object */
        v->obj.data[*slot - 1].live = true;
        v->obj.data[*slot - 1].b = xcalloc(sz ? sz : 1, 1);
        v->obj.data[*slot - 1].init = xcalloc(sz ? sz : 1, 1);
        id = *slot;
    } else {
        Loc l;
        id = ev_new_obj(v, sz, false);
        if (!id)
            return 0;
        vec_push(&r->own, id);
        l.ref = key;
        l.obj = id;
        vec_push(&r->loc, l);
    }
    p = mkptr(id, 0);
    if (t->op == TO_COMPLIT)            /* what the list leaves out is zero */
        ev_zero_n(v, p, sz);
    if (t->nkids == 0)
        return p;
    if (NODE(r, KID(r, n, 0))->op == TO_ELEM) {
        ev_zero_n(v, p, sz);
        init_elems(r, n, p);
        return p;
    }
    if (is_agg(v, t->ty)) {
        x = ev_node(r, KID(r, n, 0));
        ev_copy_n(v, p, x, sz);
    } else if (ev_tinfo(v, t->ty, &ti)) {
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
        ev_zero_n(v, ap, 24);
        store_n(v, mkptr(pid(ap), poff(ap) + 8), &pt, r->va);
        return 0;
    case 2:
        return 0;
    case 3:
        ev_copy_n(v, ap, ev_node(r, KID(r, n, 1)), 24);
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
        if (ev_access(v, p, sz, "va_arg read") == NULL)
            return 0;
        sz = (sz + 7) / 8 * 8;
    }
    {
        uint64_t x = agg ? p : ev_tinfo(v, t->ty, &ti) ? load_n(v, p, &ti) : 0;
        if (v->res.status)
            return 0;
        store_n(v, mkptr(pid(ap), poff(ap) + 8), &pt, mkptr(pid(p), (int64_t)poff(p) + (int64_t)sz));
        return x;
    }
}

static void path_add(Ev *v, PathEnt e)
{
    if (v->path.len >= PATH_MAX_ENT)
        v->path_full = true;
    else
        vec_push(&v->path, e);
}

static void mark_seen(Ev *v, Blk *b, unsigned bit)
{
    uint64_t m = (uint64_t)1 << bit;
    if (!(b->seen & m)) {
        b->seen |= m;
        v->newseen++;
    }
}

/* A symbolic value the evaluator uses as a number (an index, a divisor, a shift count) keeps
 * its value on the path: the entry is the equation. */
static void pin(Ev *v, uint32_t sh, uint64_t x)
{
    PathEnt e;
    if (!v->track || !sh)
        return;
    memset(&e, 0, sizeof e);
    e.expr = sx_op(&v->sx, SX_EQ, sh, sx_const(&v->sx, 64, x));
    e.want = true;
    path_add(v, e);
}

/* The shadow of an integer operation: op on the operands' expressions (a concrete operand
 * enters as a constant), cast to ti.  0 when neither operand is symbolic. */
static uint32_t op_sh(Ev *v, int op, const Ti *ti, uint64_t a, uint32_t ash, uint64_t b, uint32_t bsh)
{
    bool un = op == SX_NEG || op == SX_NOT || op == SX_LNOT;
    if (!v->track || (!ash && !bsh) || ti->flt || ti->ptr)
        return 0;
    if (!ash)
        ash = sx_const(&v->sx, 64, a);
    if (!un && !bsh)
        bsh = sx_const(&v->sx, 64, b);
    return canon_sh(v, ti, sx_op(&v->sx, op, ash, un ? 0 : bsh));
}

/* The slot expression op for a binary operator on integers, or -1. */
static int bin_op(int p, bool sg)
{
    switch (p) {
    case P_PLUS: return SX_ADD;
    case P_MINUS: return SX_SUB;
    case P_STAR: return SX_MUL;
    case P_SLASH: return sg ? SX_SDIV : SX_UDIV;
    case P_PERCENT: return sg ? SX_SREM : SX_UREM;
    case P_AMP: return SX_AND;
    case P_PIPE: return SX_OR;
    case P_CARET: return SX_XOR;
    case P_SHL: return SX_SHL;
    case P_SHR: return sg ? SX_ASHR : SX_LSHR;
    default: return -1;
    }
}

static uint64_t ev_node1(Fr *r, uint32_t n, uint32_t *sh);

/* The value of node n; v->sh is its shadow (the cases that propagate set *sh, the rest leave 0). */
static uint64_t ev_node(Fr *r, uint32_t n)
{
    uint32_t sh = 0;
    uint64_t x = ev_node1(r, n, &sh);
    r->v->sh = sh;
    return x;
}

static uint64_t ev_node1(Fr *r, uint32_t n, uint32_t *sh)
{
    Ev *v = r->v;
    const TNode *t = NODE(r, n);
    uint64_t a, b;
    uint32_t ash, bsh;
    Ti ti, ts;
    if (v->res.status)
        return 0;
    if (--v->fuel < 0) {
        ev_fail(v, EV_FUEL, "exceeded the step limit");
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
            return mkptr(ev_function_obj(v, v->c, t->sym), 0);
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
            uint32_t id = ev_new_obj(v, (uint64_t)t->width, true);
            if (!id)
                return 0;
            ob = &v->obj.data[id - 1];
            ob->kind = OK_STR;
            memcpy(ob->b, r->f->bytes + t->val, t->width);
            memset(ob->init, 1, t->width);
            *o = id + 1;
        }
        return mkptr(*o - 1, 0);
    }
    case TO_LABADDR: {                  /* a label: an object of its own, only its address is usable */
        uint32_t fi = (uint32_t)(r->f - v->fn.data), id = (uint32_t)t->val, k;
        Obj *lo;
        for (k = 0; k + 2 < v->lab.len; k += 3)
            if (v->lab.data[k] == fi && v->lab.data[k + 1] == id)
                return mkptr(v->lab.data[k + 2], 0);
        k = ev_new_obj(v, 0, true);
        if (!k)
            return 0;
        lo = &v->obj.data[k - 1];
        lo->kind = OK_LABEL;
        lo->ref = id;
        lo->fname = r->f->fname;
        vec_push(&v->lab, fi);
        vec_push(&v->lab, id);
        vec_push(&v->lab, k);
        return mkptr(k, 0);
    }
    case TO_LOAD:
        a = ev_node(r, KID(r, n, 0));
        if (is_agg(v, t->ty) || v->res.status)
            return a;
        if (!ev_tinfo(v, t->ty, &ti))
            return 0;
        if (is_bf(r, KID(r, n, 0)))
            return bf_load(v, a, &ti, NODE(r, KID(r, n, 0))->pun, NODE(r, KID(r, n, 0))->width);
        a = load_n(v, a, &ti);
        *sh = v->sh;
        return a;
    case TO_STORE: {
        uint64_t sz;
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        bsh = v->sh;
        if (v->res.status)
            return 0;
        if (is_agg(v, t->ty)) {
            if (!size_of(v, t->ty, &sz))
                return 0;
            ev_copy_n(v, a, b, sz);
            return a;
        }
        if (!ev_tinfo(v, t->ty, &ti))
            return 0;
        if (is_bf(r, KID(r, n, 0)))
            return bf_store(v, a, &ti, NODE(r, KID(r, n, 0))->pun, NODE(r, KID(r, n, 0))->width, b);
        b = ev_canon(&ti, b);
        bsh = canon_sh(v, &ti, bsh);
        store_sh_n(v, a, &ti, b, bsh);
        *sh = bsh;
        return b;
    }
    case TO_POST: {
        uint32_t s = KID(r, n, 0);
        if (NODE(r, s)->op != TO_STORE) {
            UNSUP(v, "post of a non-store");
            return 0;
        }
        a = ev_node(r, KID(r, s, 0));
        if (!ev_tinfo(v, NODE(r, s)->ty, &ti))
            return 0;
        v->sh = 0;
        b = is_bf(r, KID(r, s, 0)) ? bf_load(v, a, &ti, NODE(r, KID(r, s, 0))->pun, NODE(r, KID(r, s, 0))->width)
                                   : load_n(v, a, &ti);
        bsh = v->sh;
        ev_node(r, s);
        *sh = bsh;
        return b;
    }
    case TO_CAST: {
        TypeId from = NODE(r, KID(r, n, 0))->ty;
        a = ev_node(r, KID(r, n, 0));
        ash = v->sh;
        if (type_is_void(&v->c->tt, t->ty))
            return 0;
        if (!ev_tinfo(v, t->ty, &ti) || !ev_tinfo(v, from, &ts))
            return 0;
        if (!ts.flt && !ts.ptr && !ti.flt)
            *sh = canon_sh(v, &ti, ash);
        return convert(v, &ti, &ts, a);
    }
    case TO_FCONST:
        if (!ev_tinfo(v, t->ty, &ti))
            return 0;
        a = 0;
        memcpy(&a, r->f->bytes + t->val, t->width < 8 ? t->width : 8);
        return a;
    case TO_F2I: {
        TypeId from = NODE(r, KID(r, n, 0))->ty;
        double d;
        a = ev_node(r, KID(r, n, 0));
        if (v->res.status || !ev_tinfo(v, t->ty, &ti) || !ev_tinfo(v, from, &ts))
            return 0;
        d = ev_fget(&ts, a);
        if (t->pun) {                   /* round to nearest even, as cvtss2si does */
            volatile double m = 4503599627370496.0;     /* 2^52: adding it rounds to an integer */
            if (d > -m && d < m)
                d = d < 0 ? (d - m) + m : (d + m) - m;
        }
        if (d != d || d >= (ti.bytes == 8 ? 9223372036854775808.0 : 2147483648.0) ||
            d < (ti.bytes == 8 ? -9223372036854775808.0 : -2147483648.0))
            return ev_canon(&ti, ti.bytes == 8 ? 0x8000000000000000ull : 0x80000000ull);
        return ev_canon(&ti, (uint64_t)(int64_t)d);
    }
    case TO_UNOP:
        a = ev_node(r, KID(r, n, 0));
        ash = v->sh;
        if (v->res.status)
            return 0;
        if (t->pun == P_BANG) {
            if (!ev_tinfo(v, NODE(r, KID(r, n, 0))->ty, &ts))
                return 0;
            memset(&ti, 0, sizeof ti);
            ti.bytes = 8;
            if (!ts.flt && !ts.ptr)
                *sh = op_sh(v, SX_LNOT, &ti, a, ash, 0, 0);
            return ts.flt ? !(ev_fget(&ts, a) != 0 || ev_fget(&ts, a) != ev_fget(&ts, a)) : a == 0;
        }
        if (!ev_tinfo(v, t->ty, &ti))
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
            *sh = op_sh(v, SX_NEG, &ti, a, ash, 0, 0);
            return ev_canon(&ti, 0 - a);
        }
        if (t->pun == P_TILDE) {
            *sh = op_sh(v, SX_NOT, &ti, a, ash, 0, 0);
            return ev_canon(&ti, ~a);
        }
        if (t->pun == P_PLUS) {
            *sh = ash;
            return a;
        }
        UNSUP(v, "unary operator %d", t->pun);
        return 0;
    case TO_BINOP:
        a = ev_node(r, KID(r, n, 0));
        ash = v->sh;
        b = ev_node(r, KID(r, n, 1));
        bsh = v->sh;
        if (v->res.status || !ev_tinfo(v, t->ty, &ti))
            return 0;
        if (!ti.flt && (t->pun == P_SLASH || t->pun == P_PERCENT || t->pun == P_SHL || t->pun == P_SHR))
            pin(v, bsh, b);
        if (!ti.flt && bin_op(t->pun, ti.sg) >= 0)
            *sh = op_sh(v, bin_op(t->pun, ti.sg), &ti, a, ash, b, bsh);
        return ti.flt ? fbin(v, t->pun, &ti, a, b) : int_arith(v, t->pun, t->ty, a, b);
    case TO_CMP:
        a = ev_node(r, KID(r, n, 0));
        ash = v->sh;
        b = ev_node(r, KID(r, n, 1));
        bsh = v->sh;
        if (v->res.status || !ev_tinfo(v, NODE(r, KID(r, n, 0))->ty, &ts))
            return 0;
        if (!ts.flt && !ts.ptr) {
            Ti w64;
            memset(&w64, 0, sizeof w64);
            w64.bytes = 8;
            switch (t->pun) {
            case P_EQEQ: *sh = op_sh(v, SX_EQ, &w64, a, ash, b, bsh); break;
            case P_NE: *sh = op_sh(v, SX_NE, &w64, a, ash, b, bsh); break;
            case P_LT: *sh = op_sh(v, ts.sg ? SX_SLT : SX_ULT, &w64, a, ash, b, bsh); break;
            case P_LE: *sh = op_sh(v, ts.sg ? SX_SLE : SX_ULE, &w64, a, ash, b, bsh); break;
            case P_GT: *sh = op_sh(v, ts.sg ? SX_SLT : SX_ULT, &w64, b, bsh, a, ash); break;
            case P_GE: *sh = op_sh(v, ts.sg ? SX_SLE : SX_ULE, &w64, b, bsh, a, ash); break;
            default: break;
            }
        }
        return ts.flt ? fcompare(v, t->pun, &ts, a, b) : compare(v, t->pun, NODE(r, KID(r, n, 0))->ty, a, b);
    case TO_COMMA:
        ev_node(r, KID(r, n, 0));
        a = ev_node(r, KID(r, n, 1));
        *sh = v->sh;
        return a;
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
        if (!ev_tinfo(v, t->ty, &ti))
            return 0;
        a = load_n(v, a, &ti);
        *sh = v->sh;
        return a;
    case TO_PTRADD: {
        int64_t off;
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status)
            return 0;
        pin(v, v->sh, b);
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
        if (!same_object(v, a, b, "subtraction"))
            return 0;
        if (((int64_t)poff(a) - (int64_t)poff(b)) % t->val) {      /* undefined; gcc shifts, a division truncates */
            TRAP(v, "pointer difference is not a multiple of the element size");
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
        return ev_call_node(r, n, sh);
    case TO_INTRIN:
        return ev_intrin(r, n);
    case TO_ASM: {
        /* The kids are the template, the constraint letters, then val outputs (lvalues) and the inputs.
         * The instructions are not run; in the sweep's state-building phase every output is 0 (a
         * hardware query such as cpuid then reports no features) and the results are not compared. */
        int64_t k;
        if (!v->build) {
            UNSUP(v, "operation asm");
            return 0;
        }
        for (k = 0; k < t->val && !v->res.status; k++) {
            uint32_t lv = KID(r, n, 2 + (uint32_t)k);
            uint64_t p = ev_node(r, lv);
            if (!v->res.status && ev_tinfo(v, NODE(r, lv)->ty, &ti))
                store_sh_n(v, p, &ti, 0, 0);
        }
        return 0;
    }
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

static uint64_t run_fn(Fr *caller, Fn *f, const uint64_t *args, const uint32_t *ash, size_t na, uint64_t va,
                       uint32_t *rsh)
{
    Ev *v = caller->v;
    Fr r;
    uint32_t bi = f->entry;
    size_t i;
    uint64_t result = 0;
    uint32_t rs = 0;
    *rsh = 0;
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
        ev_fail(v, EV_FUEL, "exceeded the call depth limit");
        return 0;
    }
    v->depth++;
    r.up_c = v->c;
    v->c = f->c;
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
            ev_copy_n(v, mkptr(l.obj, 0), args[i], sz);
        else if (ev_tinfo(v, f->par[i].ty, &ti))
            store_sh_n(v, mkptr(l.obj, 0), &ti, ev_canon(&ti, args[i]), ash ? canon_sh(v, &ti, ash[i]) : 0);
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
            uint32_t csh = v->sh;
            Ti ct;
            if (v->res.status)
                break;
            if (ev_tinfo(v, NODE(&r, b->val)->ty, &ct) && ct.flt) {
                c = ev_fget(&ct, c) != 0 || ev_fget(&ct, c) != ev_fget(&ct, c);
                csh = 0;
            }
            bi = c ? b->a : b->b;
            mark_seen(v, b, c ? 0 : 1);
            v->nbr += v->track;
            if (v->track && csh) {
                PathEnt e = { csh, b, c ? 0u : 1u, c != 0 };
                path_add(v, e);
            }
            continue;
        }
        case TK_SWTCH: {
            uint64_t c = ev_node(&r, b->val);
            uint32_t csh = v->sh;
            uint32_t to = b->a, edge = (uint32_t)b->nc;
            for (k = 0; k < b->nc; k++) {
                ev_note(v, (uint64_t)b->cases[k].v);
                if ((uint64_t)b->cases[k].v == c) {
                    to = b->cases[k].to;
                    edge = (uint32_t)k;
                }
            }
            bi = to;
            mark_seen(v, b, blk_edge_bit(b, edge));
            v->nbr += v->track;
            if (v->track && csh) {
                PathEnt e = { csh, b, edge, true };
                path_add(v, e);
            }
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
                        ev_copy_n(v, mkptr(id, 0), x, sz);
                        result = mkptr(id, 0);
                    }
                } else if (!type_is_void(&v->c->tt, f->ret)) {
                    Ti ti;
                    if (ev_tinfo(v, f->ret, &ti)) {
                        result = ev_canon(&ti, x);
                        rs = canon_sh(v, &ti, v->sh);
                    }
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
        ev_kill_obj(v, r.own.data[i]);
    vec_free(&r.own);
    vec_free(&r.loc);
    free(r.reg);
    free(r.tmp);
    v->c = r.up_c;
    v->depth--;
    *rsh = rs;
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
        free(v->obj.data[i].sh);
    }
    sx_free(&v->sx);
    vec_free(&v->path);
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
    free(v->fi_id);
    free(v->fi_nm);
    free(v->si_id);
    free(v->si_nm);
    vec_free(&v->rec);
    vec_free(&v->lab);
    vec_free(&v->dict);
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
    f.c = e->c;
    f.fsym = e->fsym;
    f.fname = e->fname;
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
        d.c = e->c;
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
    d.c = v->c;
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
    f.user = !cin_system(e->c, csym(e->c, e->fsym)->def_loc);
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

/* The budgets of one evaluation: steps, and objects made (killed objects keep their table slot,
 * so a loop of calls would otherwise grow the table without bound). */
static void start_budget(Ev *v)
{
    v->fuel = v->o.fuel;
    v->objlim = v->obj.len + EV_CALL_OBJS;
}

EvRes ev_call(Ev *v, uint32_t fsym, const uint64_t *args, size_t n, uint64_t *out)
{
    Fn *f = find_fn(v, v->c, fsym);
    Fr top;
    uint64_t x = 0;
    memset(&v->res, 0, sizeof v->res);
    start_budget(v);
    memset(&top, 0, sizeof top);
    top.v = v;
    if (!f)
        UNSUP(v, "no retained body for the function");
    else
        x = run_fn(&top, f, args, v->ash, n, 0, &v->rsh);
    if (!v->res.status && out)
        *out = x;
    {
        size_t i;
        for (i = 0; i < top.own.len; i++)
            ev_kill_obj(v, top.own.data[i]);
        vec_free(&top.own);
    }
    v->objlim = 0;
    return v->res;
}

/* ---- the checker's hooks ------------------------------------------------------- */

Ev *ev_of(Checker *c)
{
    if (c->opt.link) {
        struct EvLink *lk = c->opt.link;
        if (!lk->ev) {
            EvOpts o;
            memset(&o, 0, sizeof o);
            o.fuel = 50000000;
            o.depth = 1000;
            o.wrap = c->opt.eval_wrap;
            lk->ev = ev_new(c, &o);
            lk->ev->link = true;
        }
        lk->ev->c = c;
        c->ev = lk->ev;
        return lk->ev;
    }
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

void eval_statics(Checker *c, El *e)
{
    size_t i;
    for (i = 0; i < e->nodes.len; i++)
        if (e->nodes.data[i].op == TO_DATA) {
            ev_add_data(ev_of(c), e);
            return;
        }
}

void ev_image_free(EvImage *img)
{
    size_t i;
    for (i = 0; i < img->nrel; i++)
        free(img->rel[i].lit);
    free(img->rel);
    free(img->bytes);
    memset(img, 0, sizeof *img);
}

static bool img_fail(Ev *v, EvImage *img, const char *why)
{
    char msg[96];
    snprintf(msg, sizeof msg, "%s", why[0] ? why : "unsupported");
    memset(&v->res, 0, sizeof v->res);
    ev_image_free(img);
    memcpy(img->why, msg, sizeof msg);
    return false;
}

bool ev_object(Ev *v, El *e, uint32_t n, EvImage *img)
{
    const TNode *t = &e->nodes.data[n];
    Sd *s = NULL;
    Obj *o;
    size_t i, k, idx;
    uint32_t id;
    memset(img, 0, sizeof *img);
    for (i = v->sd.len; i-- > 0;)
        if (v->sd.data[i].c == v->c && v->sd.data[i].ref == t->sym && v->sd.data[i].node == n && !v->sd.data[i].stub) {
            s = &v->sd.data[i];
            break;
        }
    if (!s)
        return img_fail(v, img, "static object not registered with the evaluator");
    idx = (size_t)(s - v->sd.data);
    if (!t->nkids) {                    /* all zero: no bytes (an object of any size) */
        if (!size_of(v, t->ty, &img->size))
            return img_fail(v, img, v->res.why);
        return true;
    }
    memset(&v->res, 0, sizeof v->res);
    start_budget(v);
    id = sd_obj(v, idx);
    if (!id)
        return img_fail(v, img, v->res.why);
    if (v->obj.data[id - 1].sd1) {
        v->rec.len = 0;
        v->rec_id = id;
        v->fold = true;                 /* gcc folds a static initializer: conversions saturate */
        sd_init(v, idx);
        v->fold = false;
        v->rec_id = 0;
    }
    if (v->res.status)
        return img_fail(v, img, v->res.why);
    v->objlim = 0;
    o = &v->obj.data[id - 1];
    img->size = o->size;
    img->bytes = dup_n(o->b, (size_t)o->size);
    for (k = 0; k + 1 < v->rec.len; k += 2) {
        uint64_t x = 0;
        EvReloc r;
        Obj *g;
        unsigned j;
        if (v->rec.data[k + 1] != 8)
            return img_fail(v, img, "address in a narrow or non-address element");
        for (j = 0; j < 8; j++)
            x |= (uint64_t)img->bytes[v->rec.data[k] + j] << (8 * j);
        if (!pid(x))
            continue;               /* a null (or an integer made a pointer) */
        if (pid(x) > v->obj.len)
            return img_fail(v, img, "address of nothing");
        g = &v->obj.data[pid(x) - 1];
        memset(&r, 0, sizeof r);
        r.off = v->rec.data[k];
        r.addend = poff(x);
        if (g->fn) {
            r.kind = EV_R_SYM;
            r.ref = g->fn - 1;
        } else if (g->kind == OK_STATIC) {
            r.kind = EV_R_SYM;
            r.ref = g->ref;
            r.fname = g->fname;
        } else if (g->kind == OK_STR) {
            r.kind = EV_R_STR;
            r.litsize = g->size;
            r.lit = dup_n(g->b, (size_t)g->size);
        } else if (g->kind == OK_LABEL) {
            r.kind = EV_R_LABEL;
            r.ref = g->ref;
            r.fname = g->fname;
        } else {
            return img_fail(v, img, "address of a temporary in an initializer");
        }
        img->rel = xrealloc(img->rel, (img->nrel + 1) * sizeof *img->rel);
        img->rel[img->nrel++] = r;
        memset(img->bytes + r.off, 0, 8);
    }
    /* A pointer element that holds an object's address but was not told from the
     * tree to be one would be written as an integer: stop instead. */
    for (i = 0; i < t->nkids; i++) {
        const TNode *el = &e->nodes.data[e->kids.data[t->kid0 + i]];
        uint64_t x = 0;
        unsigned j;
        bool known = false;
        if (el->width || type_ckind(&v->c->tt, el->ty) != TY_PTR || (uint64_t)el->val + 8 > img->size)
            continue;
        for (k = 0; k + 1 < v->rec.len; k += 2)
            known |= v->rec.data[k] == (uint32_t)el->val;
        if (known)
            continue;
        for (j = 0; j < 8; j++)
            x |= (uint64_t)o->b[el->val + j] << (8 * j);
        if (pid(x) && pid(x) <= v->obj.len &&
            (v->obj.data[pid(x) - 1].kind || v->obj.data[pid(x) - 1].fn))
            return img_fail(v, img, "address expression form outside the supported ones");
    }
    return true;
}


void eval_tentative(Checker *c, uint32_t ref)
{
    ev_add_tentative(ev_of(c), ref);
}

void eval_release(Checker *c)
{
    if (!c->opt.link)
        ev_free(c->ev);
    c->ev = NULL;
}

const char *ev_dup_def(Ev *v)
{
    fn_sync(v);
    return v->dup_def;
}

