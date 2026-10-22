/* eval_run.c - the evaluator's two clients: `cereal run` (eval_main) and the
 * --eval-sweep vector generator (eval_sweep).  Both drive ev_call. */
#include "c/eval_int.h"

static const char *stop_name(int st)
{
    return st == EV_UNSUPPORTED ? "unsupported" : st == EV_FUEL ? "limit" : st == EV_EXIT ? "exit" : "trap";
}

/* argv as evaluator objects: strings, and the array of pointers ending in NULL. */
static uint64_t make_argv(Ev *v, int argc, char **argv)
{
    uint32_t arr = ev_new_obj(v, (uint64_t)(argc + 1) * 8, false);
    int i;
    if (!arr)
        return 0;
    for (i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]) + 1;
        uint32_t id = ev_new_obj(v, len, false);
        uint64_t p;
        Obj *s, *a = &v->obj.data[arr - 1];
        if (!id)
            return 0;
        s = &v->obj.data[id - 1];
        memcpy(s->b, argv[i], len);
        memset(s->init, 1, len);
        p = mkptr(id, 0);
        memcpy(a->b + 8 * i, &p, 8);
        memset(a->init + 8 * i, 1, 8);
    }
    memset(v->obj.data[arr - 1].init + 8 * argc, 1, 8);     /* argv[argc] is NULL */
    return mkptr(arr, 0);
}

static void eval_main(Checker *c)
{
    Ev *v = ev_of(c);
    size_t i;
    uint64_t x = 0, args[2];
    EvRes r;
    Fn *f = NULL;
    int code = 125;                     /* a stop of the evaluator, not a program status */
    for (i = 0; i < v->fn.len; i++)
        if (v->fn.data[i].fsym != SYM_NONE &&
            !strcmp(cident(c, csym(c, v->fn.data[i].fsym)->name), "main"))
            f = &v->fn.data[i];
    v->o.out = c->opt.tast;
    v->o.err = stderr;
    if (c->diag->nerrors) {
        /* the diagnostics are the answer; the program is not run */
    } else if (!f) {
        fputs("cereal run: no main\n", stderr);
    } else if (f->np != 0 && f->np != 2) {
        fputs("cereal run: unsupported: main with other parameters than (void) or (int, char **)\n", stderr);
    } else {
        if (f->np == 2) {
            args[0] = (uint64_t)c->opt.eval_argc;
            args[1] = make_argv(v, c->opt.eval_argc, c->opt.eval_argv);
        }
        r = ev_call(v, f->fsym, args, f->np, &x);
        if (r.status == EV_OK || r.status == EV_EXIT) {
            code = (r.status == EV_OK ? (int)x : r.code) & 255;
        } else {
            if (v->o.out)
                fflush(v->o.out);
            fprintf(stderr, "cereal run: %s: %s\n", stop_name(r.status), r.why);
        }
    }
    if (v->o.out)
        fflush(v->o.out);
    if (c->opt.eval_status)
        *c->opt.eval_status = code;
}

/* ---- the sweep ------------------------------------------------------------------ */

static uint64_t rnd(uint64_t *s)
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ull;
}

/* A random argument of a scalar type, as a slot. */
static uint64_t sweep_arg(const Ti *ti, uint64_t *rs)
{
    if (ti->flt) {
        static const double pick[] = {0.0, 1.0, -1.0, 0.5, -2.5, 3.25, 7.0, 100.0, -1e3, 1e-3};
        unsigned k = (unsigned)(rnd(rs) % 12);
        double d = k < 10 ? pick[k]
                 : k == 10 ? (double)(rnd(rs) >> 11) / 9007199254740992.0 * 1000.0 - 500.0
                 : (double)((int)(rnd(rs) % 19) - 9);
        return ev_fput(ti, d);
    }
    if (ti->boolean)
        return rnd(rs) & 1;
    if (rnd(rs) % 10 < 7) {             /* small values, within the type */
        int64_t x = (int64_t)(rnd(rs) % 13) - 6;
        return ev_canon(ti, ti->sg || x >= 0 ? (uint64_t)x : (uint64_t)(x + 6));
    }
    return ev_canon(ti, rnd(rs));
}

/* ---- the sweep's calls: bytes in, bytes out ------------------------------------------
 * A call is the bytes of its inputs: the scalar parameters' values, then one blob per object
 * the call reaches (what a pointer parameter points to, a record parameter, and what a pointer
 * inside one of those points to; deeper pointers are NULL).  Its result is the return value
 * (a returned pointer as a tag: 0 null, else (pointee number + 1) << 32 | offset into that
 * object) followed by the final bytes of every pointed-to object, pointer fields zeroed.
 * The gcc side runs the same bytes through a thunk (tests/sweep_rt.h), which rebuilds the
 * pointers between the blobs (16 bytes apart, so a one-past-the-end pointer is not the next
 * blob's start), and must produce the same bytes.  A pointer parameter also has
 * an alias word: 0, or (blob + 1) << 32 | offset, to point into an earlier pointer's blob
 * instead of its own (for functions taking `start, end` or two cursors into one buffer). */

enum { SA_SCALAR, SA_PTR, SA_REC };
enum { SR_VOID, SR_SCALAR, SR_PTR };

#define SMAXOB 64               /* blobs per function */
#define SMAXIN 16384            /* bytes of input per call */

typedef struct SObj {
    TypeId ty;                  /* element type, 0: bytes */
    uint64_t esz, size, off;    /* element size, bytes of the blob, where it starts in the input */
    bool pointee;               /* a pointer points to it (a record parameter's blob is not one) */
    int pj;                     /* pointee: its number, for the tag of a returned pointer */
} SObj;

/* A pointer-typed field: blob and offset, the blob it points to by default (-1: none, NULL) and
 * the element type any blob it is linked to must have (see sweep_link). */
typedef struct SPf {
    int ob, child;
    uint32_t off;
    TypeId ty;
    bool link;
} SPf;

typedef struct SArg {
    int kind;
    Ti ti;                      /* SA_SCALAR */
    uint64_t off;               /* SA_SCALAR: its value; SA_PTR: its alias word (8 bytes) */
    int oi;                     /* SA_PTR, SA_REC: its blob */
} SArg;

typedef struct SFn {
    int rk;
    Ti rti;
    SArg a[16];
    SObj ob[SMAXOB];
    int nob, npj;
    VEC(SPf) pf;
    uint64_t insz;
    bool ptrfn, byval;
} SFn;

static void sweep_typename(Checker *c, StrBuf *sb, TypeId t)
{
    type_print(&c->tt, sb, type_canon(&c->tt, t));
}

/* The type has a spelling the gcc build can use. */
static bool sweep_named(Checker *c, TypeId t)
{
    StrBuf sb = {0};
    bool ok;
    sweep_typename(c, &sb, t);
    ok = !strchr(sb_cstr(&sb), '<');
    sb_free(&sb);
    return ok;
}

static bool sweep_scalar(TypeKind k)
{
    return (k >= TY_BOOL && k <= TY_ULLONG) || k == TY_FLOAT || k == TY_DOUBLE;
}

/* Random contents for an object of type t: scalars as sweep_arg, pointers null, padding zero. */
static void sweep_fill(Ev *v, TypeId t, uint8_t *dst, uint64_t *rs)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok;
    Ti ti;
    if (k == TY_PTR)
        return;
    if (k == TY_ARRAY) {
        TypeId e = type_base(tt, t);
        uint64_t es = type_size(tt, e, &ok), n, i;
        if (!ok || !es)
            return;
        n = type_size(tt, t, &ok) / es;
        for (i = 0; i < n && ok; i++)
            sweep_fill(v, e, dst + i * es, rs);
        return;
    }
    if (type_is_record(tt, t)) {
        Record *r = type_record(tt, t);
        uint32_t i, n = r->flags & RF_UNION && r->nfields > 1 ? 1 : r->nfields;
        for (i = 0; i < n; i++) {
            Field *fl = &tt->fields.data[r->fields + i];
            if (!(fl->flags & FF_BITFIELD))
                sweep_fill(v, fl->ty, dst + fl->off_bits / 8, rs);
        }
        return;
    }
    if (ev_tinfo(v, t, &ti)) {
        uint64_t x = sweep_arg(&ti, rs);
        memcpy(dst, &x, ti.bytes);
    }
    v->res.status = EV_OK;
}

/* The object a pointer of type pt points to, as a blob of `many` elements (four times as many
 * bytes for void and char). */
static bool sweep_pointee(Ev *v, TypeId pt, unsigned many, SObj *o)
{
    TypeTable *tt = &v->c->tt;
    TypeId e = type_base(tt, pt);
    uint64_t n = many;
    bool ok = false;
    memset(o, 0, sizeof *o);
    o->pointee = true;
    if (type_is_void(tt, e)) {
        o->esz = 1;
        n = 4 * many;
        ok = true;
    } else if (type_is_object(tt, e)) {
        o->ty = e;
        o->esz = type_size(tt, e, &ok);
        if (o->esz == 1)
            n = 4 * many;
        if (type_is_record(tt, e) && type_record(tt, e)->flags & RF_NOKEYWORD)
            ok = false;             /* __va_list_tag: no spelling to cast to */
    }
    o->size = o->esz * n;
    v->res.status = EV_OK;
    return ok && o->size && o->size <= 4096;
}

static bool sweep_par(Ev *v, TypeId t, SArg *a, SObj *o)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok = false;
    memset(a, 0, sizeof *a);
    memset(o, 0, sizeof *o);
    if (sweep_scalar(k)) {
        a->kind = SA_SCALAR;
        ok = ev_tinfo(v, t, &a->ti);
        v->res.status = EV_OK;
        return ok;
    }
    if (type_is_record(tt, t)) {
        a->kind = SA_REC;
        o->ty = t;
        o->size = o->esz = type_size(tt, t, &ok);
        ok = ok && !(type_record(tt, t)->flags & RF_NOKEYWORD);
        v->res.status = EV_OK;
        return ok && o->size && o->size <= 4096;
    }
    if (k == TY_PTR) {
        a->kind = SA_PTR;
        return sweep_pointee(v, t, 16, o);
    }
    return false;
}

/* Record the pointer fields of an object of type t at `base` in blob oi, and give each a blob of its
 * own to point to while the budget lasts (SMAXOB blobs, 16 KB of input; the shallow blobs come first). */
static void sweep_ptrs(Ev *v, SFn *s, int oi, TypeId t, uint64_t base)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok;
    if (k == TY_PTR) {
        SPf pf = {oi, -1, (uint32_t)base, 0, false};
        SObj n;
        if (sweep_pointee(v, t, 4, &n)) {
            pf.ty = n.ty;
            pf.link = true;
            if (s->nob < SMAXOB && s->insz + n.size <= SMAXIN) {
                pf.child = s->nob;
                n.off = s->insz;
                s->insz += n.size;
                s->ob[s->nob++] = n;
            }
        }
        vec_push(&s->pf, pf);
    } else if (k == TY_ARRAY) {
        TypeId e = type_base(tt, t);
        uint64_t es = type_size(tt, e, &ok), n, i;
        if (!ok || !es)
            return;
        n = type_size(tt, t, &ok) / es;
        for (i = 0; i < n && ok; i++)
            sweep_ptrs(v, s, oi, e, base + i * es);
    } else if (type_is_record(tt, t)) {
        Record *r = type_record(tt, t);
        uint32_t i, n = r->flags & RF_UNION && r->nfields > 1 ? 1 : r->nfields;
        for (i = 0; i < n; i++) {
            Field *fl = &tt->fields.data[r->fields + i];
            if (!(fl->flags & FF_BITFIELD))
                sweep_ptrs(v, s, oi, fl->ty, base + fl->off_bits / 8);
        }
    }
}

/* All pointer fields of blob oi (every element). */
static void sweep_blob_ptrs(Ev *v, SFn *s, int oi)
{
    SObj o = s->ob[oi];
    uint64_t e;
    if (o.ty)
        for (e = 0; e < o.size / o.esz; e++)
            sweep_ptrs(v, s, oi, o.ty, e * o.esz);
}

/* Where a pointer field points in a call: its 8 bytes in the input are 0 (its own blob, or NULL
 * if it has none), ~0 (NULL) or a tag `(blob + 1) << 32 | offset` into another blob of the same
 * element type, which is how a call gets sharing and cycles (`l_G->mainthread == L`).  Returns
 * the blob (-1: NULL) and its offset. */
static int sweep_link(const SFn *s, const SPf *pf, uint64_t w, uint32_t *off)
{
    *off = 0;
    if (!w)
        return pf->child;
    if (w == ~0ull || (w >> 32) > (uint64_t)s->nob)
        return -1;
    *off = (uint32_t)w;
    return (int)(w >> 32) - 1;
}

static void sweep_free(SFn *s)
{
    vec_free(&s->pf);
}

/* Can f be swept, and how: the pointers inside the objects it is given point to objects of their
 * own, as far as the budget goes. */
static bool sweep_fn(Ev *v, const Fn *f, SFn *s)
{
    TypeTable *tt = &v->c->tt;
    TypeKind rk = type_ckind(tt, f->ret);
    size_t k;
    int i;
    memset(s, 0, sizeof *s);
    if (type_is_void(tt, f->ret))
        s->rk = SR_VOID;
    else if (sweep_scalar(rk))
        s->rk = SR_SCALAR;
    else if (rk == TY_PTR)
        s->rk = SR_PTR;
    else
        return false;
    if (s->rk == SR_SCALAR && !ev_tinfo(v, f->ret, &s->rti)) {
        v->res.status = EV_OK;
        return false;
    }
    if (!sweep_named(v->c, f->ret))
        return false;
    for (k = 0; k < f->np; k++) {
        SArg *a = &s->a[k];
        SObj o;
        if (!sweep_par(v, f->par[k].ty, a, &o) || !sweep_named(v->c, f->par[k].ty))
            return false;
        if (a->kind == SA_SCALAR) {
            a->off = s->insz;
            s->insz += a->ti.bytes;
        } else {
            if (a->kind == SA_PTR) {
                a->off = s->insz;
                s->insz += 8;
            }
            a->oi = s->nob;
            o.off = s->insz;
            s->insz += o.size;
            s->ob[s->nob++] = o;
            s->ptrfn |= a->kind == SA_PTR;
            s->byval |= a->kind == SA_REC;
        }
    }
    for (i = 0; i < s->nob; i++)        /* the blobs grow as their pointers get blobs */
        sweep_blob_ptrs(v, s, i);
    for (i = 0; i < s->nob; i++)
        if (s->ob[i].pointee)
            s->ob[i].pj = s->npj++;
    s->ptrfn |= s->pf.len > 0;    /* pointers inside a record: NULL and wild links trap */
    return s->insz <= SMAXIN && (s->rk != SR_VOID || s->ptrfn);
}

static void sweep_hex(StrBuf *sb, const uint8_t *p, size_t n)
{
    static const char dig[] = "0123456789abcdef";
    size_t i;
    if (!n)
        sb_putc(sb, '-');
    for (i = 0; i < n; i++) {
        sb_putc(sb, dig[p[i] >> 4]);
        sb_putc(sb, dig[p[i] & 15]);
    }
}

/* The gcc side of one function: unpack the bytes, link the pointers, call, pack the result
 * (tests/sweep_rt.h). */
static void sweep_thunk(Checker *c, StrBuf *sb, const char *name, const Fn *f, const SFn *s)
{
    size_t k, i;
    uint64_t oo[SMAXOB], o = s->rk == SR_SCALAR ? s->rti.bytes : s->rk == SR_PTR ? 8 : 0;
    sb_printf(sb, "thunk %s %s static unsigned t_%s(const unsigned char *in, unsigned char *out){",
              name, s->byval ? "byval" : "-", name);
    for (k = 0; k < f->np; k++)
        if (s->a[k].kind != SA_REC)
            sb_printf(sb, "unsigned char a%zu[%u] __attribute__((aligned(16)));memcpy(a%zu,in+%llu,%u);",
                      k, s->a[k].kind == SA_PTR ? 8 : s->a[k].ti.bytes, k, (unsigned long long)s->a[k].off,
                      s->a[k].kind == SA_PTR ? 8 : s->a[k].ti.bytes);
    for (i = 0; i < (size_t)s->nob; i++)
        sb_printf(sb, "unsigned char b%zu[%llu] __attribute__((aligned(16)));memcpy(b%zu,in+%llu,%llu);",
                  i, (unsigned long long)s->ob[i].size + 16, i, (unsigned long long)s->ob[i].off,
                  (unsigned long long)s->ob[i].size);
    if (s->pf.len) {
        sb_puts(sb, "unsigned char *bp[]={");
        for (i = 0; i < (size_t)s->nob; i++)
            sb_printf(sb, "%sb%zu", i ? "," : "", i);
        sb_puts(sb, "};");
    }
    for (i = 0; i < s->pf.len; i++) {
        const SPf *pf = &s->pf.data[i];
        if (pf->child >= 0)
            sb_printf(sb, "SW_LK(b%d+%u,bp,b%d)", pf->ob, pf->off, pf->child);
        else
            sb_printf(sb, "SW_LK(b%d+%u,bp,0)", pf->ob, pf->off);
    }
    if (s->rk == SR_SCALAR)
        sb_puts(sb, "__auto_type r=");
    else if (s->rk == SR_PTR)
        sb_puts(sb, "void*r=(void*)");
    sb_printf(sb, "%s(", name);
    for (k = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        if (k)
            sb_putc(sb, ',');
        if (a->kind == SA_PTR) {
            size_t j;
            sb_printf(sb, "(void*)(*(unsigned long long*)a%zu?(unsigned char*)(", k);
            for (j = 0; j < k; j++)
                if (s->a[j].kind == SA_PTR)
                    sb_printf(sb, "*(unsigned long long*)a%zu>>32==%d?(void*)b%d:", k, s->a[j].oi + 1, s->a[j].oi);
            sb_printf(sb, "(void*)0)+(unsigned)*(unsigned long long*)a%zu:(unsigned char*)b%d)", k, a->oi);
        }
        else if (a->kind == SA_REC) {
            sb_puts(sb, "*(");
            sweep_typename(c, sb, s->ob[a->oi].ty);
            sb_printf(sb, "*)b%d", a->oi);
        } else if (a->ti.flt)
            sb_printf(sb, "*(%s*)a%zu", a->ti.bytes == 4 ? "float" : "double", k);
        else if (a->ti.boolean)
            sb_printf(sb, "*(_Bool*)a%zu", k);
        else
            sb_printf(sb, "*(%s%s*)a%zu", a->ti.sg ? "" : "unsigned ",
                      a->ti.bytes == 1 ? "char" : a->ti.bytes == 2 ? "short" : a->ti.bytes == 4 ? "int" : "long long", k);
    }
    sb_puts(sb, ");");
    if (s->rk == SR_SCALAR)
        sb_puts(sb, "memcpy(out,&r,sizeof r);");
    else if (s->rk == SR_PTR) {
        sb_puts(sb, "{unsigned long long t=0;if(r){t=~0ull;");
        for (i = 0; i < (size_t)s->nob; i++)
            if (s->ob[i].pointee)
                sb_printf(sb, "SW_IN(t,r,%d,b%zu,%llu)", s->ob[i].pj, i, (unsigned long long)s->ob[i].size);
        sb_puts(sb, "}memcpy(out,&t,8);}");
    }
    for (i = 0; i < (size_t)s->nob; i++)
        if (s->ob[i].pointee) {
            oo[i] = o;
            sb_printf(sb, "memcpy(out+%llu,b%zu,%llu);", (unsigned long long)o, i, (unsigned long long)s->ob[i].size);
            o += s->ob[i].size;
        }
    for (i = 0; i < s->pf.len; i++)
        if (s->ob[s->pf.data[i].ob].pointee)
            sb_printf(sb, "memset(out+%llu,0,8);", (unsigned long long)(oo[s->pf.data[i].ob] + s->pf.data[i].off));
    sb_printf(sb, "return %llu;}%c", (unsigned long long)o, 10);
}

/* One call of the sweep, printed as "call NAME IN => OUT" or "call NAME IN STOP why"; `bad`
 * is set when the result is a pointer to something that is not one of the objects. */
static int sweep_call(Ev *v, StrBuf *body, const char *name, const Fn *f, const SFn *s,
                      const uint8_t *in, bool show_stop, bool *bad)
{
    uint64_t slot[16] = {0}, x = 0, oo[SMAXOB];
    uint32_t ids[SMAXOB] = {0};
    uint8_t *out = xcalloc((size_t)s->insz + 16, 1);
    size_t k, n = 0;
    int i;
    EvRes r;
    for (i = 0; i < s->nob; i++) {
        const SObj *b = &s->ob[i];
        Obj *o;
        ids[i] = ev_new_obj(v, b->size, false);
        o = &v->obj.data[ids[i] - 1];
        memcpy(o->b, in + b->off, b->size);
        memset(o->init, 1, b->size);
    }
    for (k = 0; k < s->pf.len; k++) {
        const SPf *pf = &s->pf.data[k];
        Obj *p = &v->obj.data[ids[pf->ob] - 1];
        uint64_t w, ptr = 0;
        uint32_t off;
        int j;
        memcpy(&w, p->b + pf->off, 8);
        j = sweep_link(s, pf, w, &off);
        if (j >= 0)
            ptr = mkptr(ids[j], off);
        memcpy(p->b + pf->off, &ptr, 8);
    }
    for (k = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        if (a->kind == SA_SCALAR) {
            uint64_t raw = 0;
            memcpy(&raw, in + a->off, a->ti.bytes);
            slot[k] = ev_canon(&a->ti, raw);
        } else if (a->kind == SA_PTR) {
            uint64_t w = 0;
            memcpy(&w, in + a->off, 8);
            slot[k] = w ? mkptr(ids[(w >> 32) - 1], (uint32_t)w) : mkptr(ids[a->oi], 0);
        } else
            slot[k] = mkptr(ids[a->oi], 0);
    }
    r = ev_call(v, f->fsym, slot, f->np, &x);
    if (r.status == EV_OK) {
        if (s->rk == SR_SCALAR) {
            x = ev_canon(&s->rti, x);
            memcpy(out, &x, s->rti.bytes);
            n = s->rti.bytes;
        } else if (s->rk == SR_PTR) {
            uint64_t tag = pid(x) ? ~0ull : 0;
            for (i = 0; i < s->nob; i++)
                if (s->ob[i].pointee && pid(x) == ids[i])
                    tag = ((uint64_t)(s->ob[i].pj + 1) << 32) | (uint32_t)poff(x);
            if (tag == ~0ull)
                *bad = true;
            memcpy(out, &tag, 8);
            n = 8;
        }
        for (i = 0; i < s->nob; i++)
            if (s->ob[i].pointee) {
                oo[i] = n;
                memcpy(out + n, v->obj.data[ids[i] - 1].b, s->ob[i].size);
                n += s->ob[i].size;
            }
        for (k = 0; k < s->pf.len; k++)
            if (s->ob[s->pf.data[k].ob].pointee)
                memset(out + oo[s->pf.data[k].ob] + s->pf.data[k].off, 0, 8);
    }
    for (i = 0; i < s->nob; i++)
        ev_kill_obj(v, ids[i]);
    if (r.status == EV_OK || (show_stop && (!s->ptrfn || r.status == EV_UNSUPPORTED))) {
        sb_printf(body, "call %s ", name);
        sweep_hex(body, in, (size_t)s->insz);
        if (r.status == EV_OK) {
            sb_puts(body, " => ");
            sweep_hex(body, out, n);
            sb_putc(body, 10);
        } else
            sb_printf(body, " %s %s%c", stop_name(r.status), r.why, 10);
    }
    free(out);
    return r.status;
}

static int cmp64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Random input bytes for a call. */
static void sweep_gen(Ev *v, const Fn *f, const SFn *s, uint8_t *in, uint64_t *rs)
{
    static const Ti byte = {1, false, false, false};
    size_t k;
    int i;
    memset(in, 0, (size_t)s->insz);
    for (k = 0; k < f->np; k++)
        if (s->a[k].kind == SA_SCALAR) {
            uint64_t x = sweep_arg(&s->a[k].ti, rs);
            memcpy(in + s->a[k].off, &x, s->a[k].ti.bytes);
        }
    for (k = 1; k < f->np; k++) {       /* sometimes a pointer into an earlier pointer's object */
        const SArg *a = &s->a[k];
        size_t j, n = 0, pick;
        uint64_t w;
        if (a->kind != SA_PTR || rnd(rs) % 3)
            continue;
        for (j = 0; j < k; j++)
            n += s->a[j].kind == SA_PTR;
        if (!n)
            continue;
        pick = rnd(rs) % n;
        for (j = 0; j < k; j++)
            if (s->a[j].kind == SA_PTR && !pick--)
                break;
        w = (uint64_t)(s->a[j].oi + 1) << 32 |
            (rnd(rs) % (s->ob[s->a[j].oi].size / s->ob[a->oi].esz + 1)) * s->ob[a->oi].esz;
        memcpy(in + a->off, &w, 8);
    }
    for (i = 0; i < s->nob; i++) {
        const SObj *o = &s->ob[i];
        uint8_t *d = in + o->off;
        uint64_t e;
        for (e = 0; e < o->size / o->esz; e++) {
            if (o->ty)
                sweep_fill(v, o->ty, d + e * o->esz, rs);
            else
                d[e] = (uint8_t)sweep_arg(&byte, rs);
        }
    }
    for (k = 0; k < s->pf.len; k++) {   /* sometimes a pointer field shares or ends a blob */
        const SPf *pf = &s->pf.data[k];
        uint64_t w = 0, r = rnd(rs) % 8;
        int j, n = 0, pick;
        if (!pf->link || r > 2)
            continue;
        if (!r) {
            if (pf->child >= 0)
                w = ~0ull;              /* NULL */
        } else {                        /* a blob of the same element type, at an element boundary */
            for (j = 0; j < s->nob; j++)
                n += s->ob[j].pointee && s->ob[j].ty == pf->ty;
            if (!n)
                continue;
            pick = (int)(rnd(rs) % n);
            for (j = 0; j < s->nob; j++)
                if (s->ob[j].pointee && s->ob[j].ty == pf->ty && !pick--)
                    break;
            w = (uint64_t)(j + 1) << 32 | (rnd(rs) % (s->ob[j].size / s->ob[j].esz + 1)) * s->ob[j].esz;
        }
        memcpy(in + s->ob[pf->ob].off + pf->off, &w, 8);
    }
}

static void eval_sweep(Checker *c)
{
    Ev *v = ev_of(c);
    FILE *out = c->opt.tast;
    size_t i, k;
    long j;
    if (!out)
        return;
    for (i = 0; i < v->fn.len; i++) {
        Fn *f = &v->fn.data[i];
        const char *name;
        SFn s;
        uint64_t rs;
        uint8_t *in, *base;
        bool have_base = false, bad = false, saw_ok = false, saw_unsup = false, ok;
        StrBuf head = {0}, body = {0};
        if (f->fsym == SYM_NONE || !f->user || f->variadic || f->np > 16)
            continue;
        name = cident(c, csym(c, f->fsym)->name);
        if (!strcmp(name, "main"))
            continue;
        ok = sweep_fn(v, f, &s);
        if (!ok) {
            sweep_free(&s);
            v->res.status = EV_OK;
            continue;
        }
        sb_printf(&head, "fn %s ", name);
        sweep_typename(c, &head, f->ret);
        for (k = 0; k < f->np; k++) {
            sb_putc(&head, '|');
            sweep_typename(c, &head, f->par[k].ty);
        }
        sb_putc(&head, 10);
        sweep_thunk(c, &head, name, f, &s);
        rs = (uint64_t)c->opt.eval_seed * 0x9e3779b97f4a7c15ull;
        for (k = 0; name[k]; k++)
            rs = (rs ^ (unsigned char)name[k]) * 1099511628211ull;
        rs |= 1;
        in = xcalloc((size_t)s.insz + 1, 1);
        base = xcalloc((size_t)s.insz + 1, 1);
        v->dict.len = 0;
        v->dict_on = true;
        for (j = 0; j < c->opt.eval_sweep; j++) {
            int st;
            sweep_gen(v, f, &s, in, &rs);
            st = sweep_call(v, &body, name, f, &s, in, !saw_unsup, &bad);
            saw_ok |= st == EV_OK;
            saw_unsup |= st == EV_UNSUPPORTED && s.ptrfn;
            if (st == EV_OK || !have_base) {
                memcpy(base, in, (size_t)s.insz);
                have_base = true;
            }
        }
        /* guided calls: what the program compared its inputs with (and one either side of it)
         * goes into each scalar argument in turn; only calls that finish are kept */
        v->dict_on = false;
        if (v->dict.len) {
            size_t d, m = 0, budget = 64;
            qsort(v->dict.data, v->dict.len, sizeof *v->dict.data, cmp64);
            for (d = 0; d < v->dict.len; d++)
                if (!d || v->dict.data[d] != v->dict.data[d - 1])
                    v->dict.data[m++] = v->dict.data[d];
            for (d = 0; d < m && budget; d++) {
                int sd;
                for (sd = 0; sd < 3 && budget; sd++) {
                    uint64_t cand = v->dict.data[d] + (sd == 1) - (sd == 2);
                    for (k = 0; k < f->np && budget; k++) {
                        const SArg *a = &s.a[k];
                        uint64_t cur = 0, x;
                        if (a->kind != SA_SCALAR || a->ti.flt)
                            continue;
                        memcpy(&cur, base + a->off, a->ti.bytes);
                        x = ev_canon(&a->ti, cand);
                        if (x == ev_canon(&a->ti, cur))
                            continue;
                        memcpy(in, base, (size_t)s.insz);
                        memcpy(in + a->off, &x, a->ti.bytes);
                        budget--;
                        sweep_call(v, &body, name, f, &s, in, false, &bad);
                    }
                }
            }
        }
        if (!bad && (!s.ptrfn || saw_ok || saw_unsup)) {
            fputs(sb_cstr(&head), out);
            fputs(sb_cstr(&body), out);
        }
        free(in);
        free(base);
        sweep_free(&s);
        sb_free(&head);
        sb_free(&body);
    }
}

void eval_finish(Checker *c)
{
    if (c->opt.eval == EVAL_SWEEP)
        eval_sweep(c);
    else if (c->opt.eval == EVAL_RUN)
        eval_main(c);
}
