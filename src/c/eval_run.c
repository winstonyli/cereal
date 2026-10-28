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

/* A random value of a scalar type, as a slot; once the dictionary is on (what the program compared
 * its inputs with), a third of the integers are one of its entries, or one either side. */
static uint64_t sweep_arg(Ev *v, const Ti *ti, uint64_t *rs)
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
    if (v->dict_use && v->dict.len && rnd(rs) % 3 == 0) {
        uint64_t x = v->dict.data[rnd(rs) % v->dict.len];
        unsigned d = (unsigned)(rnd(rs) % 4);
        return ev_canon(ti, x + (d == 1) - (d == 2));
    }
    if (rnd(rs) % 10 < 7) {             /* small values, within the type */
        int64_t x = (int64_t)(rnd(rs) % 13) - 6;
        return ev_canon(ti, ti->sg || x >= 0 ? (uint64_t)x : (uint64_t)(x + 6));
    }
    return ev_canon(ti, rnd(rs));
}

/* ---- the sweep's calls (docs/LAZY_BLOBS_DESIGN.md) ------------------------------------------
 * A call makes its objects as it goes (the lazy section below): a pointer parameter is an empty object,
 * a typed load of its uninitialized bytes draws the value, and a pointer drawn there is NULL, a pointer
 * into an object the call already has, a function, or a new empty object.  The call's line carries the
 * graph the call made; the gcc side rebuilds it (sw_load in tests/sweep_rt.h) and must produce the same
 * result: the return value (a returned pointer as a tag) followed by the final bytes of every object. */

enum { SA_SCALAR, SA_PTR, SA_REC, SA_STATE };
enum { SR_VOID, SR_SCALAR, SR_PTR };

#define SGUIDED 64              /* calls drawing from the dictionary */

typedef struct SArg {
    int kind;
    Ti ti;                      /* SA_SCALAR */
    uint64_t sptr;              /* SA_STATE: the pointer the builder returned (see SStates) */
    int si;                     /* ... and which state that is */
    TypeId ty;                  /* SA_REC: the record type (its size is in off) */
    uint64_t off;               /* SA_REC: its size in bytes */
} SArg;

typedef struct SFn {
    int rk;
    Ti rti;
    SArg a[16];
    bool ptrfn, byval;
    unsigned nstate;            /* parameters that are SA_STATE */
    const struct Snap *snap;    /* ... and what a call restores afterwards */
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

/* Does an object of type t hold a pointer? */
static bool sweep_hasptr(TypeTable *tt, TypeId t)
{
    TypeKind k = type_ckind(tt, t);
    if (k == TY_PTR)
        return true;
    if (k == TY_ARRAY)
        return sweep_hasptr(tt, type_base(tt, t));
    if (type_is_record(tt, t)) {
        Record *r = type_record(tt, t);
        uint32_t i, n = r->flags & RF_UNION && r->nfields > 1 ? 1 : r->nfields;
        for (i = 0; i < n; i++)
            if (sweep_hasptr(tt, tt->fields.data[r->fields + i].ty))
                return true;
    }
    return false;
}

/* Does a pointer parameter of type pt have a target the sweep can make and the gcc build can spell?  The
 * old layout's size limit stays so the set of swept functions does not move (PARKED.md). */
static bool sweep_pointee_ok(Ev *v, TypeId pt)
{
    TypeTable *tt = &v->c->tt;
    TypeId e = type_base(tt, pt);
    uint64_t n = 16, size = 0;
    bool ok = false;
    if (type_is_void(tt, e)) {
        size = 4 * n;
        ok = true;
    } else if (type_is_object(tt, e)) {
        uint64_t esz = type_size(tt, e, &ok);
        if (sweep_hasptr(tt, e))
            n = 2;
        if (esz == 1)
            n = 4 * 16;
        if (type_is_record(tt, e) && type_record(tt, e)->flags & RF_NOKEYWORD)
            ok = false;             /* __va_list_tag: no spelling to cast to */
        size = esz * n;
    }
    v->res.status = EV_OK;
    return ok && size;
}

static bool sweep_par(Ev *v, TypeId t, SArg *a)
{
    TypeTable *tt = &v->c->tt;
    TypeKind k = type_ckind(tt, t);
    bool ok = false;
    memset(a, 0, sizeof *a);
    if (sweep_scalar(k)) {
        a->kind = SA_SCALAR;
        ok = ev_tinfo(v, t, &a->ti);
        v->res.status = EV_OK;
        return ok;
    }
    if (type_is_record(tt, t)) {
        a->kind = SA_REC;
        a->ty = t;
        a->off = type_size(tt, t, &ok);
        ok = ok && !(type_record(tt, t)->flags & RF_NOKEYWORD);
        v->res.status = EV_OK;
        return ok && a->off;
    }
    if (k == TY_PTR) {
        a->kind = SA_PTR;
        return sweep_pointee_ok(v, t);
    }
    return false;
}

/* ---- state built by earlier calls (--eval-state) --------------------------------------
 * A constructor is a function with no parameters that returns a pointer to a record
 * (`luaL_newstate`, `ZSTD_createCCtx`).  It runs once, in the building phase (Ev.build), and the
 * objects it made are copied (Snap).  A later function with a parameter of that pointer type gets the
 * built object instead of a blob generated from the type, and every call puts the copy back, so
 * calls stay independent of each other. */

typedef struct Snap {
    size_t n;                   /* objects at the time */
    Obj *o;                     /* each one whole; a live one's bytes and init flags are private copies */
} Snap;

#define SMAXST 16               /* states built in all */
#define SMAXPER 4               /* ... of one type */
#define SMAXBP 4                /* parameters of a builder */

typedef struct SState {
    char key[96];               /* the spelled pointer type (qualifiers dropped) */
    uint64_t ptr;               /* what the builder returned: an object and an offset in it */
    size_t fi;                  /* the builder: its index in Ev.fn */
    int arg[SMAXBP];            /* per parameter: the state it took, -1: a scalar 0 */
    bool ext;                   /* external linkage: other files can call it */
} SState;

/* The builders in the order they ran: the gcc side replays the whole list per call. */
typedef struct SStates {
    SState st[SMAXST];
    unsigned n;
    Snap snap;
} SStates;

static uint8_t *dup_bytes(const uint8_t *p, uint64_t n)
{
    return memcpy(xmalloc(n ? n : 1), p, n);
}

static Snap snap_take(Ev *v)
{
    Snap s = {0};
    size_t i;
    s.n = v->obj.len;
    s.o = xcalloc(s.n + 1, sizeof *s.o);
    for (i = 0; i < s.n; i++) {
        s.o[i] = v->obj.data[i];
        s.o[i].sh = NULL;
        if (s.o[i].live) {
            s.o[i].b = dup_bytes(s.o[i].b, s.o[i].size);
            s.o[i].init = dup_bytes(s.o[i].init, s.o[i].size);
        }
    }
    return s;
}

/* Back to the snapshot: its objects return whole (a call may have freed one, or made another take its
 * id); what a call made is killed, except what is cached by id for the whole run (static objects,
 * string literals, labels, functions). */
static void snap_restore(Ev *v, const Snap *s)
{
    size_t i;
    while (v->obj.len < s->n) {
        Obj z;
        memset(&z, 0, sizeof z);
        vec_push(&v->obj, z);
    }
    for (i = 0; i < v->obj.len; i++) {
        Obj *o = &v->obj.data[i];
        if (i >= s->n) {
            if (o->live && o->kind == OK_NONE && !o->fn && !o->pin)
                ev_kill_obj(v, (uint32_t)i + 1);
            continue;
        }
        free(o->b);
        free(o->init);
        free(o->sh);
        *o = s->o[i];
        if (o->live) {
            o->b = dup_bytes(s->o[i].b, o->size);
            o->init = dup_bytes(s->o[i].init, o->size);
        }
    }
}

static void snap_free(Snap *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        free(s->o[i].b);
        free(s->o[i].init);
    }
    free(s->o);
}

/* The spelled record type of a pointer-to-record type, qualifiers dropped (a `const Ctx *` parameter
 * takes the `Ctx *` a constructor returns), "" for any other. */
static void state_key(Checker *c, TypeId pt, char *key, size_t cap)
{
    StrBuf sb = {0};
    TypeTable *tt = &c->tt;
    key[0] = 0;
    if (type_ckind(tt, pt) == TY_PTR && type_is_record(tt, type_base(tt, pt))) {
        sweep_typename(c, &sb, TYPE_UNQUAL(type_canon(tt, type_base(tt, pt))));
        snprintf(key, cap, "%s *", sb_cstr(&sb));
    }
    sb_free(&sb);
}

/* The shape of a builder's parameters: pk[k] is the state key of a record-pointer parameter, "" for an
 * integer one (passed 0); false for any other parameter, or too many. */
static bool builder_shape(const Fn *f, char pk[SMAXBP][96])
{
    size_t k;
    if (f->np > SMAXBP)
        return false;
    for (k = 0; k < f->np; k++) {
        TypeKind pkind = type_ckind(&f->c->tt, f->par[k].ty);
        state_key(f->c, f->par[k].ty, pk[k], sizeof pk[k]);
        if (!pk[k][0] && !(sweep_scalar(pkind) && pkind != TY_FLOAT && pkind != TY_DOUBLE))
            return false;
    }
    return true;
}

/* The nth (mod their number) state of a type, -1 if none; *cnt: how many there are. */
static int state_pick(const SStates *ss, const char *key, unsigned nth, unsigned *cnt)
{
    unsigned j, c = 0, seen = 0;
    int at = -1;
    for (j = 0; j < ss->n; j++)
        c += !strcmp(ss->st[j].key, key);
    if (cnt)
        *cnt = c;
    for (j = 0; c && j < ss->n; j++)
        if (!strcmp(ss->st[j].key, key) && seen++ == nth % c)
            at = (int)j;
    return at;
}

/* Run builder i on the states arg names (-1: a scalar 0) and keep what it built.  One that does not
 * finish is reported, not hidden. */
static void state_run(Ev *v, SStates *ss, size_t i, const int *arg)
{
    const Fn *f = &v->fn.data[i];
    uint64_t slot[SMAXBP] = {0}, x = 0;
    const char *nm = cident(f->c, csym(f->c, f->fsym)->name);
    uint32_t fsym = f->fsym;
    size_t np = f->np, k;
    size_t before = v->obj.len;
    Snap undo = snap_take(v);           /* a builder that yields no state is not replayed by the gcc side: undo what it did */
    SState t;
    EvRes r;
    for (k = 0; k < np; k++)
        if (arg[k] >= 0)
            slot[k] = ss->st[arg[k]].ptr;
    state_key(f->c, f->ret, t.key, sizeof t.key);
    t.ext = !(fsym & SYM_LOCAL) && csym(f->c, fsym)->linkage == LK_EXTERNAL;
    v->c = f->c;
    fprintf(stderr, "cereal: state: running %s\n", nm);
    v->build = true;
    r = ev_call(v, fsym, slot, np, &x);
    v->build = false;
    if (r.status != EV_OK || !pid(x) || (np && pid(x) <= before)) {   /* with parameters: a new object, not a getter's interior pointer */
        fprintf(stderr, "cereal: state: %s: %s %s\n", nm,
                r.status != EV_OK ? stop_name(r.status) : "returned NULL or an old object",
                r.status != EV_OK ? r.why : "");
        v->res.status = EV_OK;
        snap_restore(v, &undo);
        snap_free(&undo);
        return;
    }
    snap_free(&undo);
    t.ptr = x;
    t.fi = i;
    for (k = 0; k < SMAXBP; k++)
        t.arg[k] = k < np ? arg[k] : -1;
    fprintf(stderr, "cereal: state: %s built a %s\n", nm, t.key);
    ss->st[ss->n++] = t;
}

/* Build the states: first the builders without parameters (one per type), then, for two rounds, the
 * builders that take states (`lua_newthread(L)`) on each state the round began with.  Every state of a
 * type is another way to call a function that takes one.  Then keep a copy of everything they made. */
static void states_build(Ev *v, SStates *ss)
{
    unsigned round;
    for (round = 0; round < 3; round++) {
        unsigned n0 = ss->n;
        size_t i;
        for (i = 0; i < v->fn.len; i++) {
            const Fn *f = &v->fn.data[i];
            char pk[SMAXBP][96], rk[96];
            unsigned seed, k, first = 0;
            if (f->fsym == SYM_NONE || !f->user || f->variadic || !!f->np != !!round || !builder_shape(f, pk))
                continue;
            state_key(f->c, f->ret, rk, sizeof rk);
            if (!rk[0])
                continue;
            if (!round) {               /* one state of a type from a plain constructor */
                int arg[SMAXBP] = {-1, -1, -1, -1};
                unsigned cnt;
                state_pick(ss, rk, 0, &cnt);
                if (!cnt && ss->n < SMAXST)
                    state_run(v, ss, i, arg);
                continue;
            }
            while (first < f->np && !pk[first][0])
                first++;
            if (first < f->np && strcmp(pk[first], rk))
                continue;               /* only a state from a state of its own type: others are allocation helpers that disturb the first state */
            for (seed = 0; first < f->np && seed < n0; seed++) {
                int arg[SMAXBP] = {-1, -1, -1, -1};
                unsigned cnt, j;
                bool have = true, dup = false;
                if (strcmp(ss->st[seed].key, pk[first]))
                    continue;
                for (k = 0; k < f->np; k++)
                    if (pk[k][0]) {
                        arg[k] = k == first ? (int)seed : state_pick(ss, pk[k], 0, NULL);
                        have &= arg[k] >= 0;
                    }
                if (!have)
                    continue;
                for (j = 0; j < ss->n; j++)
                    dup |= ss->st[j].fi == i && !memcmp(ss->st[j].arg, arg, sizeof arg);
                state_pick(ss, rk, 0, &cnt);
                if (!dup && cnt < SMAXPER && ss->n < SMAXST)
                    state_run(v, ss, i, arg);
            }
        }
    }
    ss->snap = snap_take(v);
    {
        size_t m, live = 0;
        uint64_t bytes = 0, big = 0;
        for (m = 0; m < ss->snap.n; m++)
            if (ss->snap.o[m].live) {
                live++;
                bytes += ss->snap.o[m].size;
                if (ss->snap.o[m].size > big)
                    big = ss->snap.o[m].size;
            }
        fprintf(stderr, "cereal: state: %u states, snapshot of %zu objects, %llu bytes (largest %llu)\n", ss->n, live,
                (unsigned long long)bytes, (unsigned long long)big);
    }
}

/* Every builder the gcc side of a function in file c would replay is one it can call. */
static bool states_callable(Ev *v, const SStates *ss, Checker *c)
{
    unsigned j;
    for (j = 0; j < ss->n; j++)
        if (!ss->st[j].ext && v->fn.data[ss->st[j].fi].c != c)
            return false;
    return true;
}

/* A function the sweep calls, and the gcc side can name: a user function with a fixed, short parameter list. */
static bool sweep_cand(const Fn *f)
{
    return f->fsym != SYM_NONE && f->user && !f->variadic && f->np <= 16 &&
           strcmp(cident(f->c, csym(f->c, f->fsym)->name), "main");
}

/* Can f be swept: its result and parameters are of types the sweep can make and the gcc build can spell. */
static bool sweep_fn(Ev *v, const Fn *f, SFn *s, const SStates *sts, unsigned var)
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
        if (sts) {                      /* a pointer to a record some builder built: one of those objects */
            char key[96];
            unsigned cnt;
            int j;
            state_key(v->c, f->par[k].ty, key, sizeof key);
            j = key[0] ? state_pick(sts, key, var, &cnt) : -1;
            if (j >= 0) {
                memset(a, 0, sizeof *a);
                a->kind = SA_STATE;
                a->sptr = sts->st[j].ptr;
                a->si = j;
                s->nstate++;
                s->ptrfn = true;
                continue;
            }
        }
        if (!sweep_par(v, f->par[k].ty, a) || !sweep_named(v->c, f->par[k].ty))
            return false;
        if (a->kind != SA_SCALAR) {
            s->ptrfn |= a->kind == SA_PTR || sweep_hasptr(tt, a->ty);
            s->byval |= a->kind == SA_REC;
        }
    }
    return s->rk != SR_VOID || s->ptrfn;
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

/* Built state: the gcc side replays every builder, in order, as the locals q0, q1, ... */
static void sweep_replay(Ev *v, const SStates *sts, Checker *c, StrBuf *sb, const SFn *s)
{
    size_t i, k;
    for (i = 0; s->nstate && i < sts->n; i++) {
        const SState *t = &sts->st[i];
        const Fn *bf = &v->fn.data[t->fi];
        const char *nm = cident(bf->c, csym(bf->c, bf->fsym)->name);
        if (bf->c != c) {               /* another file: its declaration may not be visible, so go by symbol */
            sb_printf(sb, "extern void*c%zu() __asm__(\"%s\");", i, nm);
            sb_printf(sb, "void*q%zu=(void*)c%zu(", i, i);
        } else
            sb_printf(sb, "void*q%zu=(void*)%s(", i, nm);
        for (k = 0; k < bf->np; k++) {
            if (k)
                sb_putc(sb, ',');
            if (t->arg[k] >= 0)
                sb_printf(sb, "q%d", t->arg[k]);
            else {
                sb_putc(sb, '(');
                sweep_typename(bf->c, sb, bf->par[k].ty);
                sb_puts(sb, ")0");
            }
        }
        sb_puts(sb, ");");
    }
}

/* A scalar parameter k as an expression, read from its aligned copy a<k>. */
static void sweep_scalar_expr(StrBuf *sb, const SArg *a, size_t k)
{
    if (a->ti.flt)
        sb_printf(sb, "*(%s*)a%zu", a->ti.bytes == 4 ? "float" : "double", k);
    else if (a->ti.boolean)
        sb_printf(sb, "*(_Bool*)a%zu", k);
    else
        sb_printf(sb, "*(%s%s*)a%zu", a->ti.sg ? "" : "unsigned ",
                  a->ti.bytes == 1 ? "char" : a->ti.bytes == 2 ? "short" : a->ti.bytes == 4 ? "int" : "long long", k);
}

/* The gcc side of one function: the graph comes from the line (sw_load in tests/sweep_rt.h). */
static void sweep_thunk(Ev *v, const SStates *sts, Checker *c, StrBuf *sb, const char *tname, const char *name,
                             const Fn *f, const SFn *s)
{
    size_t k;
    unsigned ab = 0, off = 0;
    for (k = 0; k < f->np; k++)
        ab += s->a[k].kind == SA_SCALAR ? s->a[k].ti.bytes : s->a[k].kind == SA_STATE ? 0 : 8;
    sb_printf(sb, "thunk %s %s static unsigned t_%s(const unsigned char *in, unsigned char *out){"
                  "SwG g;const unsigned char *ap=sw_load(&g,in,%u);",
              tname, s->byval ? "byval" : "-", tname, ab);
    for (k = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        if (a->kind == SA_SCALAR) {
            sb_printf(sb, "unsigned char a%zu[8] __attribute__((aligned(16)));memcpy(a%zu,ap+%u,%u);", k, k, off,
                      a->ti.bytes);
            off += a->ti.bytes;
        } else if (a->kind != SA_STATE)
            off += 8;
    }
    sweep_replay(v, sts, c, sb, s);
    if (s->rk == SR_SCALAR)
        sb_puts(sb, "__auto_type r=");
    else if (s->rk == SR_PTR)
        sb_puts(sb, "void*r=(void*)");
    sb_printf(sb, "%s(", name);
    for (k = 0, off = 0; k < f->np; k++) {
        const SArg *a = &s->a[k];
        if (k)
            sb_putc(sb, ',');
        if (a->kind == SA_SCALAR) {
            sweep_scalar_expr(sb, a, k);
            off += a->ti.bytes;
        } else if (a->kind == SA_STATE) {
            sb_printf(sb, "q%d", a->si);
        } else if (a->kind == SA_PTR) {
            sb_printf(sb, "(void*)sw_ptr(&g,ap+%u)", off);
            off += 8;
        } else {
            sb_puts(sb, "*(");
            sweep_typename(c, sb, a->ty);
            sb_printf(sb, "*)sw_blob(&g,ap+%u)", off);
            off += 8;
        }
    }
    sb_puts(sb, ");");
    if (s->rk == SR_SCALAR)
        sb_puts(sb, "memcpy(out,&r,sizeof r);return sw_out(&g,out,sizeof r);");
    else if (s->rk == SR_PTR)
        sb_puts(sb, "{unsigned long long t=sw_tag(&g,r);memcpy(out,&t,8);}return sw_out(&g,out,8);");
    else
        sb_puts(sb, "return sw_out(&g,out,0);");
    sb_putc(sb, '}');
    sb_putc(sb, 10);
}

/* Tracking (--eval-solve): every byte the call draws, and every scalar parameter, is a symbol of the
 * solver's expressions (lazy_sym). */
static void track_begin(Ev *v)
{
    sx_clear(&v->sx);
    v->path.len = 0;
    v->path_full = false;
}

/* ---- lazy blobs (docs/LAZY_BLOBS_DESIGN.md) ---------------------------------------------------
 * A call makes its objects as it goes: a pointer parameter is an empty lazy object, a typed load of
 * its uninitialized bytes draws the value (Ev.lazy_fill), and a pointer drawn there is NULL, a pointer
 * into an object the call already has, or a new empty lazy object.  The call line carries the graph
 * the call made, which the gcc side rebuilds (sw_load in tests/sweep_rt.h):
 *   u32 n; u32 size[n]; the arguments (a scalar's bytes, or u64 (blob + 1) << 32 | offset);
 *   u32 nlinks, {blob, off, target + 1, target off} per pointer the call read;
 *   u32 nzero, {blob, off} per other word that held a pointer at the end;
 *   the initial bytes of every blob (zero where the call read none).
 * The result is the return value (a pointer as a tag over the blobs), then the final bytes of every
 * blob with the pointer words (links and zero cells) zeroed. */

#define LMAXBL 32               /* lazy objects per call */
#define LMAXBY 32768            /* bytes of lazy objects per call */
#define LHEAD 128               /* a pointer to a lazy object starts this far in: the program may look before it (container_of, p[-1]) */

/* Draw keys (docs/LAZY_BLOBS_SOLVER_PLAN.md): every value the call draws has a 64-bit key that does not
 * depend on the order of the draws: a parameter's is its number, a lazy blob's is the hash of the key
 * of the blob holding the pointer cell and the cell's offset, a byte's is the hash of its blob's key
 * and its offset. A call logs what it drew (rec) and consults a forced map (force) before it draws. */
typedef uint64_t LKey;

typedef struct LEnt {
    LKey key;
    uint64_t val;
} LEnt;

typedef VEC(LEnt) LLog;
typedef VEC(LKey) LKeys;

/* What a caller of sweep_call asks for beyond the call: values to force, the log of what the
 * final run drew, and, in a tracked run, the key of each symbol. */
typedef struct LOpt {
    const LLog *force;          /* sorted by key */
    LLog *rec;
    LKeys *syms;
} LOpt;

#define LZ_PARAM ((uint64_t)1 << 56)      /* what a derived key is the key of: tags in the top bits, which no offset reaches */
#define LZ_ALIAS ((uint64_t)2 << 56)
#define LZ_OUT ((uint64_t)3 << 56)
#define LZ_TGT ((uint64_t)4 << 56)
#define LZ_TOFF ((uint64_t)5 << 56)
#define LZ_FN ((uint64_t)6 << 56)
#define LFN 0x80000000u         /* LLink.tb1: the link names a function (index in Ev.fn), not a blob */
#define LMAXFN (1u << 16)       /* functions the gcc side can address (sw_fn) */

static LKey lz_key(LKey a, uint64_t b)
{
    uint64_t h = (a ^ 0x9e3779b97f4a7c15ull) + b * 0xbf58476d1ce4e5b9ull;
    h = (h ^ h >> 30) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ h >> 27) * 0x94d049bb133111ebull;
    return h ^ h >> 31;
}

static int lz_cmp(const void *a, const void *b)
{
    LKey x = ((const LEnt *)a)->key, y = ((const LEnt *)b)->key;
    return x < y ? -1 : x > y;
}

/* The entry of `key`; `log` must be sorted by key. */
static LEnt *llog_at(const LLog *log, LKey key)
{
    LEnt k = {key, 0};
    return log && log->len ? bsearch(&k, log->data, log->len, sizeof k, lz_cmp) : NULL;
}

typedef struct LBlob {
    uint32_t id;                /* the object */
    int par;                    /* the blob whose pointer made it, -1: a parameter's */
    uint32_t poff;              /* ... the offset of that pointer, or the parameter's number */
    uint8_t *ini;               /* the bytes the call read first, as drawn */
    uint64_t cap;
    LKey key;
} LBlob;

typedef struct LLink {
    uint32_t blob, off, tb1, toff;
} LLink;

#define LMAXHINT 4             /* retries of a call that subtracts pointers of two blobs */

typedef struct Lazy {
    uint64_t *rs;
    unsigned ndraw, nullat;     /* pointer draws so far; the one that is NULL (a call has at most one: a chain of
                                   pointers needs them all non-NULL to get anywhere) */
    VEC(LBlob) bl;
    VEC(LLink) lk;
    LLog rec;                   /* what this call drew */
    const LLog *force;          /* values to use instead of drawing (sorted), or NULL */
    LKeys syms;                 /* tracking: the key of each solver symbol, by id (dense, by first use) */
} Lazy;

/* A symbol for the byte drawn under `key`: ids are dense, so the solver's model maps back to keys. */
static uint32_t lazy_sym(Ev *v, Lazy *z, LKey key)
{
    vec_push(&z->syms, key);
    return sx_sym(&v->sx, (uint32_t)z->syms.len - 1);
}

/* The one place a value is drawn: the caller has drawn `drawn` from the PRNG (always, so the stream
 * is the same with or without a forced value); the forced value wins, and the value used is logged. */
static uint64_t lazy_draw(Lazy *z, LKey key, uint64_t drawn)
{
    LEnt e = {key, drawn}, *f = llog_at(z->force, key);
    if (f)
        e.val = f->val;
    vec_push(&z->rec, e);
    return e.val;
}

static int lazy_index(const Lazy *z, uint32_t id)
{
    size_t i;
    for (i = 0; i < z->bl.len; i++)
        if (z->bl.data[i].id == id)
            return (int)i;
    return -1;
}

/* A new lazy object of `size` bytes; its index, or -1 at the budget (the call then stops). */
static int lazy_new(Ev *v, Lazy *z, uint64_t size, int par, uint32_t poff)
{
    LBlob b = {0, par, poff, NULL, 0, 0};
    size_t i;
    b.key = par < 0 ? lz_key(LZ_PARAM, poff) : lz_key(z->bl.data[par].key, poff);
    uint64_t total = size;
    for (i = 0; i < z->bl.len; i++)
        total += v->obj.data[z->bl.data[i].id - 1].size;
    if (z->bl.len >= LMAXBL || total > LMAXBY) {
        UNSUP(v, "lazy blob budget");
        return -1;
    }
    b.id = ev_new_obj(v, size, false);
    if (!b.id)
        return -1;
    v->obj.data[b.id - 1].lazy = true;
    vec_push(&z->bl, b);
    return (int)z->bl.len - 1;
}

static void lazy_record(LBlob *b, uint64_t off, unsigned n, const uint8_t *src)
{
    if (off + n > b->cap) {
        uint64_t cap = off + n > 2 * b->cap ? off + n : 2 * b->cap;
        b->ini = xrealloc(b->ini, cap);
        memset(b->ini + b->cap, 0, cap - b->cap);
        b->cap = cap;
    }
    memcpy(b->ini + off, src, n);
}

/* A pointer to a function type fty, drawn as a function: the index in Ev.fn of one of the swept
 * functions whose type prints the same (return and parameter types, as the gcc build spells them), or -1
 * when none does. */
static long lazy_fn(Ev *v, Lazy *z, LKey ck, TypeId fty)
{
    TypeTable *tt = &v->c->tt;
    TypeId canon = type_canon(tt, fty);
    const TypeEnt *te = type_ent(tt, canon);
    VEC(uint32_t) cand = {0};
    StrBuf want = {0};
    uint64_t g;
    size_t i, k;
    long res = -1;
    if (te->flags & (TF_VARIADIC | TF_NOPROTO))
        return -1;
    type_print(tt, &want, canon);
    for (i = 0; i < v->fn.len && i < LMAXFN; i++) {
        const Fn *f = &v->fn.data[i];
        StrBuf have = {0};
        if (!sweep_cand(f) || f->np != te->n)
            continue;
        type_print(&f->c->tt, &have, type_canon(&f->c->tt, csym(f->c, f->fsym)->ty));
        if (!strcmp(sb_cstr(&have), sb_cstr(&want)))
            vec_push(&cand, (uint32_t)i);
        sb_free(&have);
    }
    if (cand.len) {
        g = lazy_draw(z, lz_key(ck, LZ_FN), cand.data[rnd(z->rs) % cand.len]);
        for (k = 0; k < cand.len; k++)          /* a forced function that is no candidate here: the first */
            if (cand.data[k] == g)
                break;
        res = (long)cand.data[k < cand.len ? k : 0];
    }
    vec_free(&cand);
    sb_free(&want);
    return res;
}

/* Ev.lazy_fill: the typed load of ti->bytes bytes at off of lazy object id finds some uninitialized.
 * Scalars are drawn as the generator draws them; a pointer is NULL (1/8), into an existing object
 * (1/8, at an offset of 0 to 64), or into a new one. */
/* The untouched byte at off of blob bi: its draw (dflt unless forced), stored, recorded for the call's graph, and
 * an input of the solver (a symbol, see sweep_track) when sym. */
static void lazy_byte(Ev *v, Lazy *z, int bi, uint64_t off, uint8_t dflt, bool sym)
{
    Obj *o = &v->obj.data[z->bl.data[bi].id - 1];
    LKey key = lz_key(z->bl.data[bi].key, off);
    uint8_t b = (uint8_t)lazy_draw(z, key, dflt);
    o->b[off] = b;
    o->init[off] = 1;
    if (v->track && sym) {
        if (!o->sh)
            o->sh = xcalloc(o->size ? o->size : 1, sizeof *o->sh);
        o->sh[off] = lazy_sym(v, z, key);
    }
    lazy_record(&z->bl.data[bi], off, 1, &b);
}

static bool lazy_fill(Ev *v, uint32_t id, uint64_t off, const Ti *ti)
{
    Lazy *z = v->lazy_ud;
    int bi = lazy_index(z, id);
    Obj *o = &v->obj.data[id - 1];
    uint64_t x;
    unsigned i, un = 0;
    uint8_t raw[8];
    if (bi < 0)
        return false;
    for (i = 0; i < ti->bytes; i++)
        un += !o->init[off + i];
    if (ti->ptr) {
        unsigned d = z->ndraw++;
        uint64_t r = d == z->nullat ? 0 : 1 + rnd(z->rs) % 7;
        LLink l = {(uint32_t)bi, (uint32_t)off, 0, 0};
        LKey ck = lz_key(z->bl.data[bi].key, off);
        if (un != ti->bytes)
            return false;               /* half a pointer was written: not a pointer to draw */
        r = lazy_draw(z, lz_key(ck, LZ_OUT), r > 2 ? 2 : r);       /* 0 NULL, 1 share, 2 new */
        long fi = r && ti->fty ? lazy_fn(v, z, ck, ti->fty) : -1;   /* a function pointer is NULL or a function */
        if (r == 0) {
            x = 0;
        } else if (fi >= 0) {
            Fn *g = &v->fn.data[fi];
            l.tb1 = LFN | (uint32_t)fi;
            x = mkptr(ev_function_obj(v, g->c, g->fsym), 0);
        } else if (r == 1) {
            int t = (int)(rnd(z->rs) % z->bl.len);
            uint64_t tk, to = LHEAD + (rnd(z->rs) % 9) * (ti->psz > 8 ? ti->psz : 8);   /* whole elements of the pointee: two such pointers subtract */
            size_t b;
            tk = lazy_draw(z, lz_key(ck, LZ_TGT), z->bl.data[t].key);
            to = lazy_draw(z, lz_key(ck, LZ_TOFF), to);
            for (b = 0; b < z->bl.len; b++)         /* a forced target the call has no blob for: the drawn one */
                if (z->bl.data[b].key == tk)
                    t = (int)b;
            l.tb1 = (uint32_t)t + 1;
            l.toff = (uint32_t)to;
            x = mkptr(z->bl.data[t].id, l.toff);
        } else {
            int nb = lazy_new(v, z, 0, bi, (uint32_t)off);
            if (nb < 0)
                return false;
            l.tb1 = (uint32_t)nb + 1;
            l.toff = LHEAD;
            x = mkptr(z->bl.data[nb].id, LHEAD);
        }
        if (x)
            vec_push(&z->lk, l);
        o = &v->obj.data[id - 1];       /* lazy_new may have moved the table */
        for (i = 0; i < ti->bytes; i++)
            o->b[off + i] = (uint8_t)(x >> (8 * i));
        memset(o->init + off, x ? 2 : 1, ti->bytes);   /* 2: a pointer (see load_n) */
        return true;
    }
    x = sweep_arg(v, ti, z->rs);
    for (i = 0; i < ti->bytes; i++)
        raw[i] = (uint8_t)(x >> (8 * i));
    for (i = 0; i < ti->bytes; i++)
        if (!o->init[off + i])
            lazy_byte(v, z, bi, off + i, raw[i], !ti->flt);
    return true;
}

/* Ev.lazy_zero: the untouched bytes of an untyped read are zero unless the solver forced them. */
static void lazy_zero(Ev *v, uint32_t id, uint64_t off, uint64_t n)
{
    Lazy *z = v->lazy_ud;
    int bi = lazy_index(z, id);
    uint64_t i;
    if (bi < 0)
        return;
    for (i = off; i < off + n; i++)
        if (!v->obj.data[id - 1].init[i])
            lazy_byte(v, z, bi, i, 0, true);
}

static void lazy_free(Lazy *z)
{
    size_t i;
    for (i = 0; i < z->bl.len; i++)
        free(z->bl.data[i].ini);
    vec_free(&z->bl);
    vec_free(&z->lk);
    vec_free(&z->rec);
    vec_free(&z->syms);
}

static void lazy_put32(StrBuf *sb, uint32_t x)
{
    sb_putc(sb, (char)(x & 255));
    sb_putc(sb, (char)(x >> 8 & 255));
    sb_putc(sb, (char)(x >> 16 & 255));
    sb_putc(sb, (char)(x >> 24 & 255));
}

/* Where the call stopped, for the miss report: the path of pointer fields from a parameter's object. */
static void lazy_where(Ev *v, const Lazy *z)
{
    int bi = v->f_obj ? lazy_index(z, v->f_obj) : -1;
    int path[8], np = 0, i;
    char *p = v->note;
    size_t left = sizeof v->note;
    int n;
    if (bi < 0) {
        v->note[0] = 0;
        return;
    }
    while (bi >= 0 && np < 8) {
        path[np++] = bi;
        bi = z->bl.data[bi].par;
    }
    n = snprintf(p, left, "lazy blob param %u", z->bl.data[path[np - 1]].poff + 1);
    for (i = np - 2; i >= 0 && n > 0 && (size_t)n < left; i--) {
        p += n;
        left -= (size_t)n;
        n = snprintf(p, left, " +%u", z->bl.data[path[i]].poff);
    }
    if (n > 0 && (size_t)n < left)
        snprintf(p + n, left - (size_t)n, " (+%d)", (int)v->f_off);
}

/* The draws that would make the stopped call go further, at most two: what the sweep forces on a rerun.
 * Each rule reads the stop and names the cells whose draw was wrong, and what to draw there instead. */
static unsigned lazy_redraw(const Ev *v, const Lazy *z, LEnt hint[2])
{
    unsigned n = 0;
    size_t j;
    if (v->res.status == EV_TRAP && v->sub_a) {         /* two blobs subtracted: the later shares the earlier, by its pointer cell or its parameter */
        int a = lazy_index(z, v->sub_a), b = lazy_index(z, v->sub_b), lo = a < b ? a : b, hi = a > b ? a : b;
        if (a >= 0 && b >= 0 && a != b) {
            const LBlob *h = &z->bl.data[hi];
            if (h->par >= 0) {
                hint[0] = (LEnt){lz_key(h->key, LZ_OUT), 1};
                hint[1] = (LEnt){lz_key(h->key, LZ_TGT), z->bl.data[lo].key};
                n = 2;
            } else if (z->bl.data[lo].par < 0) {
                hint[0] = (LEnt){lz_key(h->key, LZ_ALIAS), (uint64_t)(z->bl.data[lo].poff + 1) | (uint64_t)LHEAD << 16};
                n = 1;
            }
        }
    } else if (v->res.status == EV_UNSUPPORTED && v->pun_obj) {     /* a drawn pointer read as an integer: the cells that share the blob (not the one that made it) get a new blob of their own */
        int bi = lazy_index(z, v->pun_obj);
        for (j = 0; bi >= 0 && j < z->lk.len && n < 2; j++) {
            const LLink *l = &z->lk.data[j];
            const LBlob *src = &z->bl.data[l->blob], *t = &z->bl.data[bi];
            if (!(l->tb1 & LFN) && (int)l->tb1 - 1 == bi && !((int)l->blob == t->par && l->off == t->poff))
                hint[n++] = (LEnt){lz_key(lz_key(src->key, l->off), LZ_OUT), 2};
        }
    }
    return n;
}

/* One call of the sweep: the scalars are drawn from *rs,
 * the pointer parameters are empty objects, and the line is printed from what the call made. */
static int lazy_call1(Ev *v, StrBuf *body, const char *name, const Fn *f, const SFn *s, bool show_stop,
                      bool *bad, uint64_t *rs, bool may_retry, LEnt hint[2], unsigned *nhint, bool *retry,
                      const LOpt *op)
{
    Lazy z = {0};
    uint64_t slot[16] = {0}, x = 0, boff[LMAXBL];
    uint32_t ash[16] = {0};
    StrBuf pay = {0}, args = {0}, zc = {0};
    uint8_t *out;
    size_t k, i, j, n = 0, total = 8;
    uint32_t nz = 0;
    EvRes r;
    memset(&v->res, 0, sizeof v->res);
    z.rs = rs;
    z.force = op ? op->force : NULL;
    v->lazy_ud = &z;
    if (v->track)
        track_begin(v);
    v->lazy_fill = lazy_fill;
    v->lazy_zero = lazy_zero;
    for (k = 0; k < f->np && !v->res.status; k++) {
        const SArg *a = &s->a[k];
        if (a->kind == SA_SCALAR) {
            uint64_t raw = sweep_arg(v, &a->ti, rs), drawn = 0;
            uint32_t sh = 0;
            for (i = 0; i < a->ti.bytes; i++) {
                LKey key = lz_key(lz_key(LZ_PARAM, k), i);
                drawn |= (lazy_draw(&z, key, raw >> (8 * i) & 255) & 255) << (8 * i);
                if (v->track && !a->ti.flt) {   /* the scalar is an input: its bytes' symbols, joined (see sweep_track) */
                    uint32_t b = lazy_sym(v, &z, key);
                    sh = i ? sx_join(&v->sx, b, sh) : b;
                }
            }
            raw = a->ti.boolean ? drawn != 0 : drawn;
            if (sh)
                ash[k] = sx_ext(&v->sx, sh, a->ti.bytes, a->ti.sg, a->ti.boolean);
            slot[k] = ev_canon(&a->ti, raw);
            for (i = 0; i < a->ti.bytes; i++)
                sb_putc(&args, (char)(raw >> (8 * i) & 255));
        } else if (a->kind == SA_STATE) {
            slot[k] = a->sptr;
        } else {
            uint64_t w;
            int bi = -1;
            uint32_t off = LHEAD;
            if (a->kind == SA_PTR && k) {     /* into an earlier pointer parameter's object: (j + 1) | offset << 16, 0 none */
                uint64_t al = 0;
                if (rnd(rs) % 3 == 0) {
                    size_t np = 0, pick;
                    for (j = 0; j < k; j++)
                        np += s->a[j].kind == SA_PTR;
                    if (np) {
                        pick = (size_t)(rnd(rs) % np);
                        for (j = 0; j < k; j++)
                            if (s->a[j].kind == SA_PTR && !pick--)
                                break;
                        al = (uint64_t)(j + 1) | (uint64_t)(LHEAD + (rnd(rs) % 9) * 8) << 16;
                    }
                }
                al = lazy_draw(&z, lz_key(lz_key(LZ_PARAM, k), LZ_ALIAS), al);
                j = (size_t)(al & 0xffff);
                if (j >= 1 && j <= k && s->a[j - 1].kind == SA_PTR) {
                    bi = lazy_index(&z, pid(slot[j - 1]));
                    off = (uint32_t)(al >> 16);
                }
            }
            if (bi < 0)
                bi = lazy_new(v, &z, a->kind == SA_REC ? a->off : 0, -1, (uint32_t)k);
            if (bi < 0)
                break;
            slot[k] = mkptr(z.bl.data[bi].id, off);
            w = ((uint64_t)bi + 1) << 32 | off;
            for (i = 0; i < 8; i++)
                sb_putc(&args, (char)(w >> (8 * i) & 255));
        }
    }
    if (v->res.status) {
        r = v->res;
    } else {
        v->ld_obj = 0;
        z.nullat = rnd(rs) & 1 ? ~0u : (unsigned)(rnd(rs) % 32);
        v->sub_a = v->sub_b = v->pun_obj = 0;
        v->ash = v->track ? ash : NULL;
        r = ev_call(v, f->fsym, slot, f->np, &x);
        v->ash = NULL;
    }
    v->lazy_fill = NULL;
    v->lazy_zero = NULL;
    v->lazy_ud = NULL;
    if (r.status != EV_OK)
        lazy_where(v, &z);
    *nhint = may_retry && r.status != EV_OK ? lazy_redraw(v, &z, hint) : 0;
    *retry = *nhint != 0;
    for (i = 0; i < z.bl.len; i++)
        total += v->obj.data[z.bl.data[i].id - 1].size;
    out = xcalloc(total + 16, 1);
    if (r.status == EV_OK) {
        if (s->rk == SR_SCALAR) {
            x = ev_canon(&s->rti, x);
            memcpy(out, &x, s->rti.bytes);
            n = s->rti.bytes;
        } else if (s->rk == SR_PTR) {
            uint64_t tag = x ? ~0ull : 0;          /* a forged address (null + 24) is not NULL: gcc's tag is ~0 too */
            for (i = 0; i < z.bl.len; i++)
                if (pid(x) == z.bl.data[i].id && poff(x) >= 0 && (uint64_t)poff(x) <= v->obj.data[pid(x) - 1].size + 64 + LHEAD)
                    tag = (((uint64_t)i + 1) << 32) | (uint32_t)poff(x);
            if (tag == ~0ull)
                *bad = true;
            memcpy(out, &tag, 8);
            n = 8;
        }
        for (i = 0; i < z.bl.len; i++) {        /* the final bytes of every blob, pointer words zeroed */
            const Obj *o = &v->obj.data[z.bl.data[i].id - 1];
            uint64_t w;
            boff[i] = n;
            memcpy(out + n, o->b, o->size);
            for (w = 0; w + 8 <= o->size; w += 8) {
                uint64_t val;
                bool known = false;
                memcpy(&val, o->b + w, 8);
                if (!pid(val) && o->init[w] == 2 && o->init[w + 7] == 2 && (int32_t)val < 0) {   /* NULL minus a few bytes: gcc's address wraps to the top of the space */
                    val = (uint64_t)(int64_t)(int32_t)val;
                    memcpy(out + n + w, &val, 8);
                }
                if (!pid(val) || pid(val) > v->obj.len)
                    continue;
                for (j = 0; j < z.lk.len; j++)
                    known |= z.lk.data[j].blob == i && z.lk.data[j].off == w;
                if (!known) {
                    lazy_put32(&zc, (uint32_t)i);
                    lazy_put32(&zc, (uint32_t)w);
                    nz++;
                }
                memset(out + n + w, 0, 8);
            }
            n += o->size;
        }
        for (j = 0; j < z.lk.len; j++)
            memset(out + boff[z.lk.data[j].blob] + z.lk.data[j].off, 0, 8);
    }
    lazy_put32(&pay, (uint32_t)z.bl.len);
    for (i = 0; i < z.bl.len; i++)
        lazy_put32(&pay, (uint32_t)v->obj.data[z.bl.data[i].id - 1].size);
    sb_putn(&pay, args.data, args.len);
    lazy_put32(&pay, (uint32_t)z.lk.len);
    for (j = 0; j < z.lk.len; j++) {
        lazy_put32(&pay, z.lk.data[j].blob);
        lazy_put32(&pay, z.lk.data[j].off);
        lazy_put32(&pay, z.lk.data[j].tb1);
        lazy_put32(&pay, z.lk.data[j].toff);
    }
    lazy_put32(&pay, nz);
    sb_putn(&pay, zc.data, zc.len);
    for (i = 0; i < z.bl.len; i++) {
        uint64_t sz = v->obj.data[z.bl.data[i].id - 1].size, have = z.bl.data[i].cap < sz ? z.bl.data[i].cap : sz;
        if (have)
            sb_putn(&pay, (const char *)z.bl.data[i].ini, have);
        for (j = have; j < sz; j++)
            sb_putc(&pay, 0);
    }
    if (!*retry && (r.status == EV_OK || (show_stop && (!s->ptrfn || r.status == EV_UNSUPPORTED)))) {
        sb_printf(body, "call %s ", name);
        sweep_hex(body, (const uint8_t *)pay.data, pay.len);
        if (r.status == EV_OK) {
            sb_puts(body, " => ");
            sweep_hex(body, out, n);
            sb_putc(body, 10);
        } else
            sb_printf(body, " %s %s%c", stop_name(r.status), r.why, 10);
    }
    if (s->snap)
        snap_restore(v, s->snap);
    for (i = 0; i < z.bl.len; i++)
        ev_kill_obj(v, z.bl.data[i].id);
    ev_trim(v);
    free(out);
    sb_free(&args);
    sb_free(&pay);
    sb_free(&zc);
    if (op && op->rec) {        /* hand over what this attempt drew */
        vec_free(op->rec);
        *op->rec = z.rec;
        memset(&z.rec, 0, sizeof z.rec);
    }
    if (op && op->syms) {
        vec_free(op->syms);
        *op->syms = z.syms;
        memset(&z.syms, 0, sizeof z.syms);
    }
    lazy_free(&z);
    return r.status;
}

/* The log sorted by key, as `force` wants it. */
static void llog_sort(LLog *log)
{
    if (log->len)
        qsort(log->data, log->len, sizeof *log->data, lz_cmp);
}

/* The call, rerun each time it stops on a subtraction of pointers of two blobs with the
 * later blob's pointer cell forced to share the earlier blob (the program meant them as one object, such as
 * a saved pc and the code it points into): the same draws, so the rerun differs only there.  *rec gets what
 * the final run drew (unsorted); with `force` (sorted) the call takes those values instead of drawing them,
 * and the hints are put over them (a forced call that already shares as asked is not retried). */
static void hint_put(LLog *log, LEnt e)
{
    LEnt *f = llog_at(log, e.key);
    if (f)
        f->val = e.val;
    else
        vec_push(log, e);
    llog_sort(log);
}

static int sweep_call(Ev *v, StrBuf *body, const char *name, const Fn *f, const SFn *s, bool show_stop,
                           bool *bad, uint64_t *rs, const LOpt *op)
{
    LLog cur = {0};
    LOpt o = op ? *op : (LOpt){0};
    unsigned nh = 0;
    uint64_t saved = *rs;
    size_t i;
    if (op && op->force)
        for (i = 0; i < op->force->len; i++)
            vec_push(&cur, op->force->data[i]);
    for (;;) {
        LEnt hint[2];
        unsigned nhint, k;
        bool retry, asked = true;
        int st;
        *rs = saved;
        if (cur.len)
            o.force = &cur;
        st = lazy_call1(v, body, name, f, s, show_stop, bad, rs, nh < LMAXHINT, hint, &nhint, &retry, &o);
        if (retry) {
            for (k = 0; k < nhint; k++) {
                LEnt *e = llog_at(&cur, hint[k].key);
                asked &= e && e->val == hint[k].val;
            }
            retry = !asked;             /* already asked for: the stop is not about this alias */
            for (k = 0; retry && k < nhint; k++)
                hint_put(&cur, hint[k]);
        }
        if (!retry) {
            vec_free(&cur);
            return st;
        }
        nh++;
    }
}

static int cmp64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* ---- solving for inputs (docs/CONCOLIC_DESIGN.md) ------------------------------------
 * From a call that finished: rerun it tracked, walk its path keeping the prefix asserted, and at
 * each branch whose other edge no call has taken ask the solver for bytes that follow the prefix
 * and take it.  Each answer is one more call through sweep_call (printed only if it finishes,
 * compared with gcc like every other), so the solver may be wrong or incomplete. */

#define SQUERY 48               /* solver queries per function */
#define SSEEDS 4                /* finishing inputs kept as starting points */
#define SEXPAND 8               /* tracked runs per function */
#define SRLIMIT 5000000         /* the solver's resource limit per query: deterministic, not time */

typedef struct SolveStat {
    unsigned long queries, sat, unsat, unknown, calls, fresh;
    size_t edges, taken;            /* edges of the swept functions, and those taken by the end */
    size_t nodes;                   /* the most expression nodes one tracked call made */
    unsigned long seeded, tracked;  /* functions with a finishing input; tracked runs */
    unsigned long steps, open;      /* path steps (branches on symbolic values, pins); branches with an unseen edge */
    unsigned long branches;         /* every branch and switch the tracked runs took */
    unsigned long gain;             /* edges first taken by a solver round, in any function */
} SolveStat;

typedef struct STried {
    const Blk *blk;
    uint32_t edge;
} STried;

typedef VEC(STried) STriedVec;

static void blk_count(const Fn *f, size_t *edges, size_t *taken)
{
    size_t i;
    for (i = 0; i < f->nblk; i++) {
        const Blk *b = &f->blk[i];
        unsigned n = b->term == TK_BR ? 2 : b->term == TK_SWTCH ? (b->nc > 61 ? 62 : (unsigned)b->nc + 1) : 0, e;
        *edges += n;
        for (e = 0; e < n; e++)
            *taken += (unsigned)(b->seen >> (b->term == TK_SWTCH && e == n - 1 ? 62 : e) & 1);
    }
}

/* A corpus entry: the log of what the call drew (sorted by key) and the random state it started from,
 * which replays it (see lazy_draw). */
typedef struct SSeed {
    LLog log;
    uint64_t rs0;
} SSeed;

static void seed_free(SSeed *sd)
{
    vec_free(&sd->log);
}

/* One solved call: the seed with the model's values written over it; true if it finished and
 * took an edge nobody had (then it is worth tracking itself).  The model is in solver symbols;
 * `syms` (from the seed's tracked run) maps them back to the keys of the draws. */
static bool solve_run(Ev *v, const SxModel *m, const SSeed *seed, const LKeys *syms, const Fn *f, const SFn *s,
                      StrBuf *body, const char *name, bool *bad, bool *saw_ok, SSeed *child)
{
    SSeed c = {0};
    unsigned long before = v->newseen;
    size_t i;
    int st;
    {
        LLog force = {0};
        LOpt op = {&force, &c.log, NULL};
        uint64_t rs = seed->rs0;
        for (i = 0; i < seed->log.len; i++)
            vec_push(&force, seed->log.data[i]);
        for (i = 0; i < m->off.len; i++) {
            LEnt *e = m->off.data[i] < syms->len ? llog_at(&force, syms->data[m->off.data[i]]) : NULL;
            if (e)
                e->val = m->val.data[i];
        }
        c.rs0 = seed->rs0;
        st = sweep_call(v, body, name, f, s, false, bad, &rs, &op);
        vec_free(&force);
        llog_sort(&c.log);
    }
    *saw_ok |= st == EV_OK;
    if (st == EV_OK && v->newseen != before) {
        *child = c;
        return true;
    }
    seed_free(&c);
    return false;
}

static bool tried(const STriedVec *t, const Blk *b, uint32_t edge)
{
    size_t i;
    for (i = 0; i < t->len; i++)
        if (t->data[i].blk == b && t->data[i].edge == edge)
            return true;
    return false;
}

/* The condition for taking edge e of the switch step p, in cs (room for 64); the count. */
static size_t switch_conds(Ev *v, const PathEnt *p, uint32_t e, SxCond *cs, bool *whole)
{
    const Blk *b = p->blk;
    size_t k, n = 0;
    *whole = true;
    if (e < b->nc) {
        cs[0].a = sx_op(&v->sx, SX_EQ, p->expr, sx_const(&v->sx, 64, (uint64_t)b->cases[e].v));
        cs[0].want = true;
        return 1;
    }
    for (k = 0; k < b->nc; k++) {       /* the default: none of the cases */
        if (n == 64) {
            *whole = false;
            break;
        }
        cs[n].a = sx_op(&v->sx, SX_EQ, p->expr, sx_const(&v->sx, 64, (uint64_t)b->cases[k].v));
        cs[n++].want = false;
    }
    return n;
}

/* One function's search (the loop in eval_sweep): the corpus is the inputs that finished and are worth
 * mutating (the first few, then any that took a new edge); `next` is the one the solver tracks next. */
#define SCORPUS 16

typedef struct SSearch {
    VEC(SSeed) corpus;
    size_t next;
    STriedVec done;                     /* the edges already asked about */
    size_t queries, expanded;
} SSearch;

/* Keeps the call that just finished as a seed: the log it drew (moved out of *log) and the random state
 * it started from. */
static void search_keep(SSearch *q, LLog *log, uint64_t rs0, bool fresh)
{
    SSeed sd = {0};
    if (q->corpus.len >= SCORPUS || (q->corpus.len >= SSEEDS && !fresh))
        return;
    llog_sort(log);
    sd.log = *log;
    memset(log, 0, sizeof *log);
    sd.rs0 = rs0;
    vec_push(&q->corpus, sd);
}

static void search_free(SSearch *q)
{
    while (q->corpus.len) {
        SSeed sd = vec_pop(&q->corpus);
        seed_free(&sd);
    }
    vec_free(&q->corpus);
    vec_free(&q->done);
}

/* One function's sweep: what a mutation needs (a mutation is a round of the search). */
typedef struct SCtx {
    Ev *v;
    SxSolver *sv;                       /* NULL without --eval-solve */
    SolveStat *ss;
    const Fn *f;
    const SFn *s;
    const char *name;
    StrBuf *body;                       /* the calls that print */
    uint64_t rs;                        /* its random state */
    SSearch q;
    bool bad, saw_ok, saw_unsup;
    char why[112];                      /* the first reason a call of the generator stopped, "" if none did */
    char loc[112];                      /* ... and where it had read the pointer it stopped on */
} SCtx;

/* The solver mutation: track the next corpus input and, at each branch of its path whose other edge
 * nobody has taken, ask for bytes that take it; each answer is one more call, kept if it takes a new
 * edge.  False when the corpus or the budgets are used up. */
static bool solve_step1(SCtx *x)
{
    Ev *v = x->v;
    SxSolver *sv = x->sv;
    SolveStat *st = x->ss;
    SSearch *q = &x->q;
    const Fn *f = x->f;
    const SFn *s = x->s;
    StrBuf *body = x->body;
    const char *name = x->name;
    bool *bad = &x->bad, *saw_ok = &x->saw_ok;
    STriedVec *done = &q->done;
    SxModel m = {0};
    size_t i;
    LKeys syms = {0};
    {
        SSeed seed;                     /* a shallow copy: the corpus may grow (and move) below */
        StrBuf sink = {0};
        int r;
        if (q->next >= q->corpus.len || q->expanded >= SEXPAND || q->queries >= SQUERY)
            return false;
        seed = q->corpus.data[q->next++];
        v->track = true;
        {
            LOpt op = {&seed.log, NULL, &syms};
            uint64_t rs = seed.rs0;
            r = sweep_call(v, &sink, name, f, s, false, bad, &rs, &op);
        }
        v->track = false;
        sb_free(&sink);
        if (r != EV_OK) {
            vec_free(&syms);
            return true;
        }
        q->expanded++;
        st->tracked++;
        st->branches += v->nbr;
        v->nbr = 0;
        if (v->sx.n.len > st->nodes)
            st->nodes = v->sx.n.len;
        sx_begin(sv, &v->sx);
        for (i = 0; i < v->path.len && q->queries < SQUERY; i++) {
            PathEnt p = v->path.data[i];
            SxCond cs[64];
            st->steps++;
            if (p.blk) {
                const Blk *b = p.blk;
                uint32_t e, ne = b->term == TK_BR ? 2 : (uint32_t)b->nc + 1;
                for (e = 0; e < ne; e++)
                    if (e != p.edge && !(b->seen >> blk_edge_bit(b, e) & 1)) {
                        st->open++;
                        break;
                    }
                for (e = 0; e < ne && q->queries < SQUERY; e++) {
                    size_t n;
                    bool whole = true, sat;
                    SSeed child = {0};
                    if (e == p.edge || (b->seen >> blk_edge_bit(b, e) & 1) || tried(done, b, e))
                        continue;
                    if (b->term == TK_BR) {
                        cs[0].a = p.expr;
                        cs[0].want = !p.want;
                        n = 1;
                    } else {
                        n = switch_conds(v, &p, e, cs, &whole);
                    }
                    if (!whole)
                        continue;
                    {
                        STried t = { b, e };
                        vec_push(done, t);
                    }
                    q->queries++;
                    st->queries++;
                    r = sx_try(sv, cs, n, &m);
                    if (r == SX_ERROR) {
                        fprintf(stderr, "cereal: the solver stopped answering\n");
                        exit(2);
                    }
                    st->sat += r == SX_SAT;
                    st->unsat += r == SX_UNSAT;
                    st->unknown += r == SX_UNKNOWN;
                    if (r != SX_SAT)
                        continue;
                    st->calls++;
                    sat = solve_run(v, &m, &seed, &syms, f, s, body, name, bad, saw_ok, &child);
                    if (sat) {
                        st->fresh++;
                        if (q->corpus.len < SCORPUS)
                            vec_push(&q->corpus, child);
                        else
                            seed_free(&child);
                    }
                }
            }
            /* the step itself joins the prefix */
            if (!p.blk || p.blk->term == TK_BR) {
                sx_assert(sv, p.expr, p.want);
            } else {
                bool whole;
                size_t n = switch_conds(v, &p, p.edge, cs, &whole), k;
                for (k = 0; k < n; k++)
                    sx_assert(sv, cs[k].a, cs[k].want);
            }
        }
    }
    sx_model_free(&m);
    vec_free(&syms);
    return true;
}

static bool solve_step(SCtx *x)
{
    unsigned long seen = x->v->newseen;
    bool r = solve_step1(x);
    x->ss->gain += x->v->newseen - seen;
    return r;
}

/* The generator mutation: a fresh input (from the dictionary too, once it is on), run and kept. */
static void mut_gen(SCtx *x)
{
    Ev *v = x->v;
    unsigned long seen = v->newseen;
    int st;
    {
        LLog rec = {0};
        LOpt op = {NULL, &rec, NULL};
        uint64_t rs0 = x->rs;
        size_t at = x->body->len;
        st = sweep_call(v, x->body, x->name, x->f, x->s, !v->dict_use && !x->saw_unsup, &x->bad, &x->rs, &op);
        if (getenv("CEREAL_LAZY_REPLAY")) {     /* test only: the call, rerun from its log, prints the same line */
            StrBuf again = {0};
            uint64_t rs1 = rs0, rs2 = x->rs;
            bool bad = false, don = v->dict_on;
            LOpt again_op = {&rec, NULL, NULL};
            char why1[112];
            int st2;
            snprintf(why1, sizeof why1, "%s: %s", stop_name(st), v->res.why);
            llog_sort(&rec);
            v->dict_on = false;                 /* the replay must not feed the dictionary twice */
            st2 = sweep_call(v, &again, x->name, x->f, x->s, !v->dict_use && !x->saw_unsup, &bad, &rs1, &again_op);
            if (again.len != x->body->len - at || memcmp(again.data, x->body->data + at, again.len)) {
                fprintf(stderr, "cereal: lazy replay of %s differs: %s, then %s\n", x->name, why1,
                        st2 ? v->res.why : "it finished");
            }
            v->dict_on = don;
            sb_free(&again);
            x->rs = rs2;
        }
        if (x->sv && st == EV_OK)
            search_keep(&x->q, &rec, rs0, v->newseen != seen);
        vec_free(&rec);
    }
    x->saw_ok |= st == EV_OK;
    if (st != EV_OK && !x->why[0]) {
        snprintf(x->why, sizeof x->why, "%s: %s", stop_name(st), v->res.why);
        memcpy(x->loc, v->note, sizeof x->loc);
    }
    x->saw_unsup |= !v->dict_use && st == EV_UNSUPPORTED && x->s->ptrfn;
}

static void mut_solve(SCtx *x)
{
    if (x->sv)
        solve_step(x);
}

/* the rounds after the random ones, in turn */
static void (*const rotation[])(SCtx *) = { mut_gen, mut_solve };


/* The path of c's own source file, as spelled on the command line. */
static const char *main_path(Checker *c)
{
    uint32_t k;
    for (k = 0; k < srcmgr_nfiles(c->sm); k++)
        if (srcmgr_file(c->sm, k)->kind == SF_USER)
            return srcmgr_file(c->sm, k)->name;
    return "?";
}

/* Why an edge stays untaken, judged from its branch's condition (a survey: --eval-solve prints the counts). */
enum { MC_NOFIN, MC_SWITCH, MC_PNULL, MC_PCMP, MC_FLOAT, MC_ICMP, MC_IVAL, MC_CALL, MC_CONST, MC_CALL0, MC_OTHER, MC_N };
static const char *const miss_name[MC_N] = { "function never finished", "switch", "pointer null test",
    "pointer compare", "float", "integer compare", "integer value", "call result", "constant condition",
    "call without arguments", "other" };

static bool miss_const(const Fn *f, uint32_t n)
{
    while (f->nodes[n].op == TO_CAST)
        n = f->kids[f->nodes[n].kid0];
    return f->nodes[n].op == TO_CONST;
}

static int miss_class(Ev *v, const Fn *f, const Blk *b)
{
    uint32_t n = b->val;
    Ti ti;
    int c = MC_OTHER;
    if (b->term == TK_SWTCH)
        return MC_SWITCH;
    for (;;) {                                  /* look through casts, `!` and the comma */
        const TNode *t = &f->nodes[n];
        if (t->op == TO_CAST || (t->op == TO_UNOP && t->pun == P_BANG))
            n = f->kids[t->kid0];
        else if (t->op == TO_COMMA)
            n = f->kids[t->kid0 + 1];
        else
            break;
    }
    {
        const TNode *t = &f->nodes[n];
        if (t->op == TO_CMP) {
            uint32_t k0 = f->kids[t->kid0], k1 = f->kids[t->kid0 + 1];
            if (!ev_tinfo(v, f->nodes[k0].ty, &ti))
                c = MC_OTHER;
            else if (ti.flt)
                c = MC_FLOAT;
            else if (ti.ptr)
                c = miss_const(f, k0) || miss_const(f, k1) ? MC_PNULL : MC_PCMP;
            else
                c = MC_ICMP;
        } else if (t->op == TO_CONST) {
            c = MC_CONST;                       /* one edge can never be taken: `while (1)`, `do { } while (0)` */
        } else if (t->op == TO_CALL) {
            c = t->sym && !t->nkids ? MC_CALL0 : MC_CALL;   /* nothing of the input reaches it: MEM_32bits and the like */
        } else if (ev_tinfo(v, t->ty, &ti)) {
            c = ti.ptr ? MC_PNULL : ti.flt ? MC_FLOAT : MC_IVAL;
        }
    }
    v->res.status = EV_OK;
    return c;
}

typedef struct SweptFn {
    size_t i;
    bool finished;
    char why[112], loc[112];
} SweptFn;

static void miss_report(Ev *v, const SweptFn *sw, size_t n)
{
    size_t counts[MC_N] = {0}, total = 0, k, i;
    struct { char key[104]; size_t edges; const char *ex[4]; unsigned nex; } why[128];
    size_t nwhy = 0;
    struct AtEnt { char key[200]; size_t edges; } at[256];
    size_t nat = 0;
    for (k = 0; k < n; k++) {
        const Fn *f = &v->fn.data[sw[k].i];
        size_t fe = 0;
        v->c = f->c;
        for (i = 0; i < f->nblk; i++) {
            const Blk *b = &f->blk[i];
            unsigned ne = b->term == TK_BR ? 2 : b->term == TK_SWTCH ? (b->nc > 61 ? 62 : (unsigned)b->nc + 1) : 0, e;
            int c = -1;
            for (e = 0; e < ne; e++)
                if (!(b->seen >> (b->term == TK_SWTCH && e == ne - 1 ? 62 : e) & 1)) {
                    if (c < 0) {
                        c = sw[k].finished ? miss_class(v, f, b) : MC_NOFIN;
                    }
                    counts[c]++;
                    total++;
                    fe += c == MC_NOFIN;
                    if (c == MC_NOFIN) {            /* by the function's first stop, digits and addresses folded */
                        char key[104];
                        size_t m, j = 0, w;
                        for (m = 0; sw[k].why[m] && j + 1 < sizeof key; m++)
                            if (!(sw[k].why[m] >= '0' && sw[k].why[m] <= '9' && j && key[j - 1] == '#'))
                                key[j++] = sw[k].why[m] >= '0' && sw[k].why[m] <= '9' ? '#' : sw[k].why[m];
                        key[j] = 0;
                        for (w = 0; w < nwhy && strcmp(why[w].key, key); w++)
                            ;
                        if (w == nwhy && nwhy < 128) {
                            strcpy(why[nwhy].key, key);
                            why[nwhy].edges = why[nwhy].nex = 0;
                            nwhy++;
                        }
                        if (w < nwhy)
                        {
                            why[w].edges++;
                            if (why[w].nex < 4 && (!why[w].nex || why[w].ex[why[w].nex - 1] != cident(f->c, csym(f->c, f->fsym)->name)))
                                why[w].ex[why[w].nex++] = cident(f->c, csym(f->c, f->fsym)->name);
                        }
                    }
                }
        }
        if (fe) {                                   /* per function, and by the place the stop read its pointer */
            char key[200], *p;
            size_t w;
            fprintf(stderr, "cereal: stuck: %zu %s: %s%s%s\n", fe, cident(f->c, csym(f->c, f->fsym)->name), sw[k].why,
                    sw[k].loc[0] ? " @ " : "", sw[k].loc);
            snprintf(key, sizeof key, "%s%s%s", sw[k].why, sw[k].loc[0] ? " @ " : "", sw[k].loc);
            if ((p = strstr(key, " (+")) != NULL)
                *p = 0;
            for (w = 0; w < nat && strcmp(at[w].key, key); w++)
                ;
            if (w == nat && nat < 256) {
                strcpy(at[nat].key, key);
                at[nat++].edges = 0;
            }
            if (w < nat)
                at[w].edges += fe;
        }
    }
    for (k = 0; k < 25 && k < nat; k++) {           /* the biggest places first */
        size_t m = k, j;
        for (j = k + 1; j < nat; j++)
            if (at[j].edges > at[m].edges)
                m = j;
        if (m != k) {
            struct AtEnt t = at[k];
            at[k] = at[m];
            at[m] = t;
        }
        fprintf(stderr, "cereal: stuck at: %zu edges: %s\n", at[k].edges, at[k].key);
    }
    for (k = 0; k < nwhy; k++) {
        unsigned e;
        fprintf(stderr, "cereal: never finished: %zu edges: %s [", why[k].edges, why[k].key[0] ? why[k].key : "(nothing ran)");
        for (e = 0; e < why[k].nex; e++)
            fprintf(stderr, "%s%s", e ? " " : "", why[k].ex[e]);
        fputs("]\n", stderr);
    }
    fprintf(stderr, "cereal: untaken edges %zu:", total);
    for (i = 0; i < MC_N; i++)
        fprintf(stderr, " %s %zu;", miss_name[i], counts[i]);
    fputc(10, stderr);
}

/* Every function of the files of c0 (one file, or the whole link: c0 is then the first). */
/* How many ways there are to pick the built states for f's parameters (0: it takes none, or the
 * gcc side cannot call every builder from its file). */
static unsigned state_variants(const Fn *f, const SStates *ss, bool callable)
{
    unsigned nv = 0;
    size_t k;
    for (k = 0; callable && k < f->np; k++) {
        char key[96];
        unsigned cnt = 0;
        state_key(f->c, f->par[k].ty, key, sizeof key);
        if (key[0])
            state_pick(ss, key, 0, &cnt);
        nv = cnt > nv ? cnt : nv;
    }
    return nv;
}

/* A function is swept in variants: one per way to pick its built states (--eval-state; none when it
 * takes no state), then the plain one, whose parameters are generated from their types.  They share
 * the function's edges (Blk.seen), so a function counts as finished if any variant finished. */
static void eval_sweep(Checker *c0)
{
    Ev *v = ev_of(c0);
    FILE *out = c0->opt.tast;
    Checker *cur = NULL;
    size_t i, k;
    long j;
    SxSolver *sv = NULL;
    SolveStat ss = {0};
    VEC(SweptFn) swept = {0};
    SStates sts = {0};
    if (!out)
        return;
    if (c0->opt.eval_solve) {
        char err[256];
        sv = sx_open(c0->opt.eval_solve, err, sizeof err);
        if (!sv) {
            fprintf(stderr, "cereal: --eval-solve: %s\n", err);
            exit(2);
        }
        sx_limit(sv, SRLIMIT);
        v->track = false;
    }
    if (c0->opt.eval_state)
        states_build(v, &sts);
    for (i = 0; i < v->fn.len && i < LMAXFN; i++) {    /* the functions a drawn function pointer can name, per file (sw_fn) */
        const Fn *f = &v->fn.data[i];
        size_t b;
        if (!sweep_cand(f))
            continue;
        for (b = 0; b < i && !(v->fn.data[b].c == f->c && sweep_cand(&v->fn.data[b])); b++)
            ;
        if (b == i && c0->opt.link)     /* the file's first candidate: announce the file */
            fprintf(out, "file %s\n", main_path(f->c));
        fprintf(out, "fp %zu %s\n", i, cident(f->c, csym(f->c, f->fsym)->name));
    }
    for (i = 0; i < v->fn.len; i++) {
        Fn *f = &v->fn.data[i];
        Checker *c = f->c;
        const char *name;
        unsigned nv, vi;
        bool fin = false;               /* a state variant finished */
        if (!sweep_cand(f))
            continue;
        v->c = c;
        name = cident(c, csym(c, f->fsym)->name);
        nv = sts.n ? state_variants(f, &sts, states_callable(v, &sts, c)) : 0;
        for (vi = 0; vi <= nv; vi++) {
            bool st = vi < nv;          /* a state variant; the last one is the plain sweep */
            char tname[160];
            SFn s;
            SCtx x = {0};
            StrBuf head = {0}, body = {0};
            snprintf(tname, sizeof tname, "%s__st%u", name, vi);
            if (!sweep_fn(v, f, &s, st ? &sts : NULL, vi)) {
                v->res.status = EV_OK;
                continue;
            }
            s.snap = st ? &sts.snap : NULL;
            if (c0->opt.link && c != cur) {
                fprintf(out, "file %s\n", main_path(c));
                cur = c;
            }
            sb_printf(&head, "fn %s ", st ? tname : name);
            sweep_typename(c, &head, f->ret);
            for (k = 0; k < f->np; k++) {
                sb_putc(&head, '|');
                sweep_typename(c, &head, f->par[k].ty);
            }
            sb_putc(&head, 10);
            sweep_thunk(v, &sts, c, &head, st ? tname : name, name, f, &s);
            x.v = v;
            x.sv = sv;
            x.ss = &ss;
            x.f = f;
            x.s = &s;
            x.name = st ? tname : name;
            x.body = &body;
            x.rs = (uint64_t)c0->opt.eval_seed * 0x9e3779b97f4a7c15ull;
            for (k = 0; x.name[k]; k++)
                x.rs = (x.rs ^ (unsigned char)x.name[k]) * 1099511628211ull;
            x.rs |= 1;
            v->dict.len = 0;
            v->dict_on = true;
            for (j = 0; j < c0->opt.eval_sweep; j++)
                mut_gen(&x);
            /* guided calls: the same generator, now drawing integers from what the program compared its
             * inputs with; only calls that finish are kept */
            v->dict_on = false;
            if (v->dict.len) {
                size_t d, m = 0;
                qsort(v->dict.data, v->dict.len, sizeof *v->dict.data, cmp64);
                for (d = 0; d < v->dict.len; d++)
                    if (!d || v->dict.data[d] != v->dict.data[d - 1])
                        v->dict.data[m++] = v->dict.data[d];
                v->dict.len = m;
                v->dict_use = true;
                for (j = 0; j < SGUIDED; j++) {
                    size_t r;
                    for (r = 0; r < sizeof rotation / sizeof *rotation; r++)
                        rotation[r](&x);
                }
                v->dict_use = false;
            }
            if (sv) {
                size_t ed = 0, tk = 0;
                blk_count(f, &ed, &tk);
                ss.edges += st ? 0 : ed;
                ss.seeded += !st && x.q.corpus.len > 0;
                while (solve_step(&x))
                    ;
                tk = 0;
                ed = 0;
                blk_count(f, &ed, &tk);
                ss.taken += st ? 0 : tk;
                search_free(&x.q);
            }
            if (!st) {
                SweptFn sw = { i, x.saw_ok || fin, "", "" };
                memcpy(sw.why, x.why, sizeof sw.why);
                memcpy(sw.loc, x.loc, sizeof sw.loc);
                vec_push(&swept, sw);
            } else if (x.saw_ok) {
                fin = true;
            } else {
                fprintf(stderr, "cereal: state-pass: %s: %s\n", tname, x.why[0] ? x.why : "no call stopped");
            }
            if (!x.bad && (!s.ptrfn || x.saw_ok || x.saw_unsup)) {
                fputs(sb_cstr(&head), out);
                fputs(sb_cstr(&body), out);
            }
            sb_free(&head);
            sb_free(&body);
        }
    }
    if (c0->opt.eval_state)
        snap_free(&sts.snap);
    if (sv) {
        fprintf(stderr,
                "cereal: solve: %lu queries (%lu sat, %lu unsat, %lu unknown), %lu calls, %lu reached a new edge; "
                "edges taken %zu of %zu, %lu of them first by a solver round (callees included); at most %zu nodes in a call; "
                "%lu functions with a finishing input, %lu tracked runs, %lu symbolic steps of %lu branches, "
                "%lu with an unseen edge\n",
                ss.queries, ss.sat, ss.unsat, ss.unknown, ss.calls, ss.fresh, ss.taken, ss.edges, ss.gain,
                ss.nodes, ss.seeded, ss.tracked, ss.steps, ss.branches, ss.open);
        miss_report(v, swept.data, swept.len);
        vec_free(&swept);
        sx_close(sv);
    }
}

void eval_finish(Checker *c)
{
    if (c->opt.eval == EVAL_SWEEP && c->opt.link)
        vec_push(&c->opt.link->cs, c);     /* swept after the last file (evlink_end) */
    else if (c->opt.eval == EVAL_SWEEP)
        eval_sweep(c);
    else if (c->opt.eval == EVAL_RUN)
        eval_main(c);
}

/* --eval-link: after the last file, sweep them all; then free the files' checkers and the shared Ev.
 * Returns the exit status. */
int evlink_end(struct EvLink *lk)
{
    size_t i;
    int rc = 0;
    if (lk->cs.len) {
        Ev *v = ev_of(lk->cs.data[0]);
        if (ev_dup_def(v)) {
            fprintf(stderr, "cereal: --eval-link: multiple definition of '%s'\n", v->dup_def);
            rc = 2;
        } else {
            eval_sweep(lk->cs.data[0]);
        }
    }
    for (i = 0; i < lk->cs.len; i++)
        checker_free(lk->cs.data[i]);
    ev_free(lk->ev);
    vec_free(&lk->cs);
    return rc;
}
