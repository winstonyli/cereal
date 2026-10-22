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

void ev_kill_obj(Ev *v, uint32_t id)
{
    Obj *o = &v->obj.data[id - 1];
    o->live = false;
    free(o->b);
    free(o->init);
    o->b = o->init = NULL;
}


static void sd_init(Ev *v, size_t i);

/* The bytes [p, p + n) of a live object, or NULL after a trap. */
Obj *ev_access(Ev *v, uint64_t p, uint64_t n, const char *what)
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
    if (off < 0 || (uint64_t)off + n > o->size) {
        TRAP(v, "%s out of bounds", what);
        return NULL;
    }
    return o;
}

static uint64_t load_n(Ev *v, uint64_t p, const Ti *ti)
{
    Obj *o = ev_access(v, p, ti->bytes, "read");
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
    return ev_canon(ti, x);
}

static void store_n(Ev *v, uint64_t p, const Ti *ti, uint64_t x)
{
    Obj *o = ev_access(v, p, ti->bytes, "write");
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
        TRAP(v, "write to a string literal");
        return;
    }
    memmove(d->b + poff(dst), s->b + poff(src), n);
    memmove(d->init + poff(dst), s->init + poff(src), n);
}

void ev_zero_n(Ev *v, uint64_t dst, uint64_t n)
{
    Obj *d = ev_access(v, dst, n, "write");
    if (!d)
        return;
    memset(d->b + poff(dst), 0, n);
    memset(d->init + poff(dst), 1, n);
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
    if (!from->flt && !to->flt)
        return ev_canon(to, a);
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

static uint64_t compare(Ev *v, int p, TypeId ot, uint64_t a, uint64_t b)
{
    Ti ti;
    bool sg;
    if (!ev_tinfo(v, ot, &ti))
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
    if (!g) {                           /* a declared function: the host library, or unsupported */
        uint32_t fs = t->pun == 1 ? v->obj.data[pid(callee) - 1].fn - 1 : t->sym;
        const char *name = cident(v->c, csym(v->c, fs)->name);
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
        return run_fn(r, g, args, g->np, mkptr(id, 0));
    }
    return run_fn(r, g, args, na, 0);
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

/* The object of Sd i, made on first use; its contents wait for the first access. */
static uint32_t sd_obj(Ev *v, size_t i)
{
    Sd *s = &v->sd.data[i];
    uint64_t sz = 0;
    uint32_t id;
    Obj *o;
    if (s->obj)
        return s->obj;
    if (!s->stub && !size_of(v, s->ty, &sz))
        return 0;
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
        size_t k;
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
    }
}

/* The object of a static-storage symbol.  Without a definition registered, a
 * global gets a stub: its address is usable, its contents are not. */
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
            (!s || ((s->node == TN) && d->node != TN)))
            s = d;
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

static uint64_t ev_node(Fr *r, uint32_t n)
{
    Ev *v = r->v;
    const TNode *t = NODE(r, n);
    uint64_t a, b;
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
            ev_copy_n(v, a, b, sz);
            return a;
        }
        if (!ev_tinfo(v, t->ty, &ti))
            return 0;
        if (is_bf(r, KID(r, n, 0)))
            return bf_store(v, a, &ti, NODE(r, KID(r, n, 0))->pun, NODE(r, KID(r, n, 0))->width, b);
        b = ev_canon(&ti, b);
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
        if (!ev_tinfo(v, NODE(r, s)->ty, &ti))
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
        if (!ev_tinfo(v, t->ty, &ti) || !ev_tinfo(v, from, &ts))
            return 0;
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
        if (v->res.status)
            return 0;
        if (t->pun == P_BANG) {
            if (!ev_tinfo(v, NODE(r, KID(r, n, 0))->ty, &ts))
                return 0;
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
            return ev_canon(&ti, 0 - a);
        }
        if (t->pun == P_TILDE)
            return ev_canon(&ti, ~a);
        if (t->pun == P_PLUS)
            return a;
        UNSUP(v, "unary operator %d", t->pun);
        return 0;
    case TO_BINOP:
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status || !ev_tinfo(v, t->ty, &ti))
            return 0;
        return ti.flt ? fbin(v, t->pun, &ti, a, b) : int_arith(v, t->pun, t->ty, a, b);
    case TO_CMP:
        a = ev_node(r, KID(r, n, 0));
        b = ev_node(r, KID(r, n, 1));
        if (v->res.status || !ev_tinfo(v, NODE(r, KID(r, n, 0))->ty, &ts))
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
        if (!ev_tinfo(v, t->ty, &ti))
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
    case TO_INTRIN:
        return ev_intrin(r, n);
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
        ev_fail(v, EV_FUEL, "exceeded the call depth limit");
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
            ev_copy_n(v, mkptr(l.obj, 0), args[i], sz);
        else if (ev_tinfo(v, f->par[i].ty, &ti))
            store_n(v, mkptr(l.obj, 0), &ti, ev_canon(&ti, args[i]));
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
            if (ev_tinfo(v, NODE(&r, b->val)->ty, &ct) && ct.flt)
                c = ev_fget(&ct, c) != 0 || ev_fget(&ct, c) != ev_fget(&ct, c);
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
                        ev_copy_n(v, mkptr(id, 0), x, sz);
                        result = mkptr(id, 0);
                    }
                } else if (!type_is_void(&v->c->tt, f->ret)) {
                    Ti ti;
                    if (ev_tinfo(v, f->ret, &ti))
                        result = ev_canon(&ti, x);
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
    vec_free(&v->rec);
    vec_free(&v->lab);
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

EvRes ev_call(Ev *v, uint32_t fsym, const uint64_t *args, size_t n, uint64_t *out)
{
    Fn *f = find_fn(v, fsym);
    Fr top;
    uint64_t x = 0;
    memset(&v->res, 0, sizeof v->res);
    v->fuel = v->o.fuel;
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
            ev_kill_obj(v, top.own.data[i]);
        vec_free(&top.own);
    }
    return v->res;
}

/* ---- the checker's hooks ------------------------------------------------------- */

Ev *ev_of(Checker *c)
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
        if (v->sd.data[i].ref == t->sym && v->sd.data[i].node == n && !v->sd.data[i].stub) {
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
    v->fuel = v->o.fuel;
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
    ev_free(c->ev);
    c->ev = NULL;
}
