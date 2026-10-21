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

/* The argument as a C literal (a float as its exact value). */
static void sweep_lit(FILE *out, const Ti *ti, uint64_t x)
{
    if (ti->flt)
        fprintf(out, "%a", ev_fget(ti, x));
    else if (ti->sg) {
        long long n = (long long)x;
        fprintf(out, "%lld%s", n, n > 2147483647 || n < -2147483647 ? "LL" : "");
    } else if (ti->bytes == 8)
        fprintf(out, "%lluULL", (unsigned long long)x);
    else
        fprintf(out, "%llu%s", (unsigned long long)x, x > 2147483647 ? "LL" : "");
}

static bool sweep_ty(Ev *v, TypeId t, Ti *ti)
{
    TypeKind k = type_ckind(&v->c->tt, t);
    if (!((k >= TY_BOOL && k <= TY_ULLONG) || k == TY_FLOAT || k == TY_DOUBLE))
        return false;
    return ev_tinfo(v, t, ti);
}

static void sweep_typename(Checker *c, FILE *out, TypeId t)
{
    StrBuf sb = {0};
    type_print(&c->tt, &sb, type_canon(&c->tt, t));
    fputs(sb_cstr(&sb), out);
    sb_free(&sb);
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
        Ti rt, pt[16];
        uint64_t rs;
        bool ok;
        if (f->fsym == SYM_NONE || !f->user || f->variadic || f->np > 16)
            continue;
        name = cident(c, csym(c, f->fsym)->name);
        if (!strcmp(name, "main"))
            continue;
        ok = sweep_ty(v, f->ret, &rt);
        for (k = 0; k < f->np && ok; k++)
            ok = sweep_ty(v, f->par[k].ty, &pt[k]);
        if (!ok) {
            v->res.status = EV_OK;
            continue;
        }
        fprintf(out, "fn %s ", name);
        sweep_typename(c, out, f->ret);
        for (k = 0; k < f->np; k++) {
            fputc('|', out);
            sweep_typename(c, out, f->par[k].ty);
        }
        putc(10, out);
        rs = (uint64_t)c->opt.eval_seed * 0x9e3779b97f4a7c15ull;
        for (k = 0; name[k]; k++)
            rs = (rs ^ (unsigned char)name[k]) * 1099511628211ull;
        rs |= 1;
        for (j = 0; j < c->opt.eval_sweep; j++) {
            uint64_t args[16], x = 0;
            EvRes r;
            fprintf(out, "call %s (", name);
            for (k = 0; k < f->np; k++) {
                args[k] = sweep_arg(&pt[k], &rs);
                if (k)
                    fputc(',', out);
                sweep_lit(out, &pt[k], args[k]);
            }
            fputs(") ", out);
            r = ev_call(v, f->fsym, args, f->np, &x);
            if (r.status != EV_OK)
                fprintf(out, "%s %s%c", stop_name(r.status), r.why, 10);
            else if (rt.flt)
                fprintf(out, "f:%a%c", ev_fget(&rt, x), 10);
            else
                fprintf(out, "i:%lld%c", (long long)x, 10);
        }
    }
}

void eval_finish(Checker *c)
{
    if (c->opt.eval == EVAL_SWEEP)
        eval_sweep(c);
    else if (c->opt.eval == EVAL_RUN)
        eval_main(c);
}
