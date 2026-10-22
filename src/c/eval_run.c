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
 * A call is the bytes of its arguments (a scalar's value, the contents of the object a
 * pointer points to, a record by value), in parameter order; its result is the return value
 * (a returned pointer as a tag: 0 null, else (argument index + 1) << 32 | offset into that
 * argument's object) followed by the final bytes of every pointed-to object.  The gcc side
 * runs the same bytes through a thunk (tests/sweep_rt.h) and must produce the same bytes. */

enum { SA_SCALAR, SA_PTR, SA_REC };
enum { SR_VOID, SR_SCALAR, SR_PTR };

typedef struct SArg {
    int kind;
    Ti ti;                      /* SA_SCALAR */
    TypeId ty;                  /* SA_PTR: the element type (0: void, bytes); SA_REC: the record */
    uint64_t size, esz, off;    /* bytes of its blob, of one pointed-to element, and where it starts */
} SArg;

typedef struct SFn {
    int rk;
    Ti rti;
    SArg a[16];
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

static bool sweep_par(Ev *v, TypeId t, SArg *a)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok = false;
    uint64_t n = 16;
    memset(a, 0, sizeof *a);
    if (sweep_scalar(k)) {
        a->kind = SA_SCALAR;
        ok = ev_tinfo(v, t, &a->ti);
        a->size = a->ti.bytes;
    } else if (type_is_record(tt, t)) {
        a->kind = SA_REC;
        a->ty = t;
        a->size = type_size(tt, t, &ok);
    } else if (k == TY_PTR) {
        TypeId e = type_base(tt, t);
        a->kind = SA_PTR;
        if (type_is_void(tt, e)) {
            a->esz = 1;
            n = 64;
            ok = true;
        } else if (type_is_object(tt, e)) {
            a->ty = e;
            a->esz = type_size(tt, e, &ok);
            if (a->esz == 1)
                n = 64;
        }
        a->size = a->esz * n;
    }
    v->res.status = EV_OK;
    if (a->kind != SA_SCALAR && a->ty && type_is_record(tt, a->ty) && type_record(tt, a->ty)->flags & RF_NOKEYWORD)
        return false;               /* __va_list_tag: no spelling to cast to */
    return ok && a->size && a->size <= 4096;
}

/* Can f be swept, and how. */
static bool sweep_fn(Ev *v, const Fn *f, SFn *s)
{
    TypeTable *tt = &v->c->tt;
    TypeKind rk = type_ckind(tt, f->ret);
    size_t k;
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
        if (!sweep_par(v, f->par[k].ty, a) || !sweep_named(v->c, f->par[k].ty))
            return false;
        a->off = s->insz;
        s->insz += a->size;
        s->ptrfn |= a->kind == SA_PTR;
        s->byval |= a->kind == SA_REC;
    }
    return s->insz <= 16384 && (s->rk != SR_VOID || s->ptrfn);
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

/* The gcc side of one function: unpack the bytes, call, pack the result (tests/sweep_rt.h). */
static void sweep_thunk(Checker *c, StrBuf *sb, const char *name, const Fn *f, const SFn *s)
{
    size_t k;
    uint64_t o = s->rk == SR_SCALAR ? s->rti.bytes : s->rk == SR_PTR ? 8 : 0;
    sb_printf(sb, "thunk %s %s static unsigned t_%s(const unsigned char *in, unsigned char *out){",
              name, s->byval ? "byval" : "-", name);
    for (k = 0; k < f->np; k++)
        sb_printf(sb, "unsigned char a%zu[%llu] __attribute__((aligned(16)));memcpy(a%zu,in+%llu,%llu);",
                  k, (unsigned long long)s->a[k].size, k, (unsigned long long)s->a[k].off,
                  (unsigned long long)s->a[k].size);
    if (s->rk == SR_SCALAR)
        sb_puts(sb, "__auto_type r=");
    else if (s->rk == SR_PTR)
        sb_puts(sb, "void*r=(void*)");
    sb_printf(sb, "%s(", name);
    for (k = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        if (k)
            sb_putc(sb, ',');
        if (a->kind == SA_PTR)
            sb_printf(sb, "(void*)a%zu", k);
        else if (a->kind == SA_REC) {
            sb_puts(sb, "*(");
            sweep_typename(c, sb, a->ty);
            sb_printf(sb, "*)a%zu", k);
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
        for (k = 0; k < f->np; k++)
            if (s->a[k].kind == SA_PTR)
                sb_printf(sb, "SW_IN(t,r,%zu,a%zu,%llu)", k, k, (unsigned long long)s->a[k].size);
        sb_puts(sb, "}memcpy(out,&t,8);}");
    }
    for (k = 0; k < f->np; k++)
        if (s->a[k].kind == SA_PTR) {
            sb_printf(sb, "memcpy(out+%llu,a%zu,%llu);", (unsigned long long)o, k, (unsigned long long)s->a[k].size);
            o += s->a[k].size;
        }
    sb_printf(sb, "return %llu;}%c", (unsigned long long)o, 10);
}

/* One call of the sweep, printed as "call NAME IN => OUT" or "call NAME IN STOP why"; `bad`
 * is set when the result is a pointer to something that is not one of the arguments. */
static int sweep_call(Ev *v, StrBuf *body, const char *name, const Fn *f, const SFn *s,
                      const uint8_t *in, bool show_stop, bool *bad)
{
    uint64_t slot[16] = {0}, x = 0;
    uint32_t ids[16] = {0};
    uint8_t *out = xcalloc((size_t)s->insz + 16, 1);
    size_t k, n = 0;
    EvRes r;
    for (k = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        if (a->kind == SA_SCALAR) {
            uint64_t raw = 0;
            memcpy(&raw, in + a->off, a->ti.bytes);
            slot[k] = ev_canon(&a->ti, raw);
        } else {
            Obj *o;
            ids[k] = ev_new_obj(v, a->size, false);
            o = &v->obj.data[ids[k] - 1];
            memcpy(o->b, in + a->off, a->size);
            memset(o->init, 1, a->size);
            slot[k] = mkptr(ids[k], 0);
        }
    }
    r = ev_call(v, f->fsym, slot, f->np, &x);
    if (r.status == EV_OK) {
        if (s->rk == SR_SCALAR) {
            x = ev_canon(&s->rti, x);
            memcpy(out, &x, s->rti.bytes);
            n = s->rti.bytes;
        } else if (s->rk == SR_PTR) {
            uint64_t tag = pid(x) ? ~0ull : 0;
            for (k = 0; k < f->np; k++)
                if (ids[k] && pid(x) == ids[k] && s->a[k].kind == SA_PTR)
                    tag = ((uint64_t)(k + 1) << 32) | (uint32_t)poff(x);
            if (tag == ~0ull)
                *bad = true;
            memcpy(out, &tag, 8);
            n = 8;
        }
        for (k = 0; k < f->np; k++)
            if (s->a[k].kind == SA_PTR) {
                memcpy(out + n, v->obj.data[ids[k] - 1].b, s->a[k].size);
                n += s->a[k].size;
            }
    }
    for (k = 0; k < f->np; k++)
        if (ids[k])
            ev_kill_obj(v, ids[k]);
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
    memset(in, 0, (size_t)s->insz);
    for (k = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        uint8_t *d = in + a->off;
        uint64_t e;
        if (a->kind == SA_SCALAR) {
            uint64_t x = sweep_arg(&a->ti, rs);
            memcpy(d, &x, a->ti.bytes);
        } else if (a->kind == SA_REC)
            sweep_fill(v, a->ty, d, rs);
        else
            for (e = 0; e < a->size / a->esz; e++) {
                if (a->ty)
                    sweep_fill(v, a->ty, d + e * a->esz, rs);
                else
                    d[e] = (uint8_t)sweep_arg(&byte, rs);
            }
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
        bool have_base = false, bad = false, saw_ok = false, saw_unsup = false;
        StrBuf head = {0}, body = {0};
        if (f->fsym == SYM_NONE || !f->user || f->variadic || f->np > 16)
            continue;
        name = cident(c, csym(c, f->fsym)->name);
        if (!strcmp(name, "main"))
            continue;
        if (!sweep_fn(v, f, &s)) {
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
