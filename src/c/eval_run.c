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
            !strcmp(ev_fn_name(c, v->fn.data[i].fsym), "main"))
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
 * its inputs with), a third of the integers are one of its entries, or one either side.  A field of a lazy blob
 * (`field`) is mostly kept small: a drawn size that pairs with a lazy array is scanned up to the array's end. */
static uint64_t sweep_arg(Ev *v, const Ti *ti, uint64_t *rs, bool field)
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
    if (field && rnd(rs) % 16)
        return ev_canon(ti, rnd(rs) % 4096);
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

/* Does a pointer parameter of type pt have a target the sweep can make (non-zero size) and the gcc build
 * can spell? */
static bool sweep_pointee_ok(Ev *v, TypeId pt)
{
    TypeTable *tt = &v->c->tt;
    TypeId e = type_base(tt, pt);
    bool ok = false;
    if (type_is_void(tt, e)) {
        ok = true;
    } else if (type_is_object(tt, e)) {
        uint64_t esz = type_size(tt, e, &ok);
        ok = ok && esz;
        if (type_is_record(tt, e) && type_record(tt, e)->flags & RF_NOKEYWORD)
            ok = false;             /* __va_list_tag: no spelling to cast to */
    }
    v->res.status = EV_OK;
    return ok;
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
    const char *nm = ev_fn_name(f->c, f->fsym);
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
    t.ext = ev_fn_external(f->c, fsym);
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
           strcmp(ev_fn_name(f->c, f->fsym), "main");
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
        const char *nm = ev_fn_name(bf->c, bf->fsym);
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
    v->dom.len = 0;
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
#define LCHAIN 8                /* a pointer cell that extends a chain of this many self-links (the root plus this many blobs) is NULL (lazy_chain); 4 ended the six-element walk of lazy_list.c */

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
/* A solver symbol: the key of the draw it stands for, and whether that draw is a pointer cell's outcome (NULL or not),
 * which the solver only chooses between NULL and the outcome the seed had (solve_run). */
typedef struct LSym {
    LKey key;
    bool out;
} LSym;

typedef VEC(LSym) LKeys;

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
#define LZ_SZ ((uint64_t)7 << 56)         /* the pointee size of a pointer cell: the key of the hint shared by every cell of that size */
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

#define LMAXHINT 8             /* retries of a call that subtracts pointers of two blobs */

typedef struct Lazy {
    uint64_t *rs;
    unsigned ndraw, nullat;     /* pointer draws so far; the one that is NULL (a call has at most one: a chain of
                                   pointers needs them all non-NULL to get anywhere) */
    VEC(LBlob) bl;
    VEC(LLink) lk;
    LLog rec;                   /* what this call drew */
    const LLog *force;          /* values to use instead of drawing (sorted), or NULL */
    LKeys syms;                 /* tracking: the draw of each solver symbol, by id (dense, by first use) */
} Lazy;

/* A symbol for the byte drawn under `key`: ids are dense, so the solver's model maps back to keys. */
static uint32_t lazy_sym(Ev *v, Lazy *z, LKey key, bool out)
{
    LSym y = {key, out};
    vec_push(&z->syms, y);
    return sx_sym(&v->sx, (uint32_t)z->syms.len - 1);
}

/* How far a pointer offset the solver picks may go: the first elements of the blob, as the draws stay near its start. */
#define LOFFMAX (LHEAD + 4096)

static uint64_t lazy_draw(Lazy *z, LKey key, const uint64_t *alt, uint64_t drawn);

/* An offset into the blob `id`, drawn as three bytes (keys derived from `key`) so that the solver can choose it.  With
 * tracking, *wsh is the pointer word (id, offset) as an expression over those bytes, and the offset's range (from LHEAD
 * to LOFFMAX, a whole number of `step` elements from LHEAD, as the plain draws are) joins Ev.dom. */
static uint32_t lazy_off(Ev *v, Lazy *z, LKey key, uint64_t drawn, uint32_t id, uint32_t step, uint32_t *wsh)
{
    uint32_t off = 0, sh = 0, o;
    unsigned i;
    for (i = 0; i < 3; i++) {
        LKey k = lz_key(key, i);
        off |= (uint32_t)(lazy_draw(z, k, NULL, drawn >> (8 * i) & 255) & 255) << (8 * i);
        if (v->track) {
            uint32_t b = lazy_sym(v, z, k, false);
            sh = i ? sx_join(&v->sx, b, sh) : b;
        }
    }
    *wsh = 0;
    if (!v->track)
        return off;
    o = sx_ext(&v->sx, sh, 3, false, false);
    vec_push(&v->dom, sx_op(&v->sx, SX_ULE, o, sx_const(&v->sx, 64, LOFFMAX)));
    vec_push(&v->dom, sx_op(&v->sx, SX_ULE, sx_const(&v->sx, 64, LHEAD), o));
    if (step > 1) {
        uint32_t rem = sx_op(&v->sx, SX_UREM, sx_op(&v->sx, SX_SUB, o, sx_const(&v->sx, 64, LHEAD)), sx_const(&v->sx, 64, step));
        vec_push(&v->dom, sx_op(&v->sx, SX_EQ, rem, sx_const(&v->sx, 64, 0)));
    }
    *wsh = sx_op(&v->sx, SX_OR, sx_const(&v->sx, 64, (uint64_t)id << 32), o);
    return off;
}

/* The one place a value is drawn: the caller has drawn `drawn` from the PRNG (always, so the stream
 * is the same with or without a forced value); the forced value wins, and the value used is logged.  A cell with no forced
 * entry of its own takes `*alt` (NULL: none), logged under `key` like any other. */
static uint64_t lazy_draw(Lazy *z, LKey key, const uint64_t *alt, uint64_t drawn)
{
    LEnt e = {key, drawn}, *f = llog_at(z->force, key);
    if (f)
        e.val = f->val;
    else if (alt)
        e.val = *alt;
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

/* The index of the blob with draw key `key`, or -1. */
static int lazy_blob_key(const Lazy *z, LKey key)
{
    size_t i;
    for (i = 0; i < z->bl.len; i++)
        if (z->bl.data[i].key == key)
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
    vec_push(&z->bl, b);
    v->obj.data[b.id - 1].lazy = (uint32_t)z->bl.len;
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
        g = lazy_draw(z, lz_key(ck, LZ_FN), NULL, cand.data[rnd(z->rs) % cand.len]);
        for (k = 0; k < cand.len; k++)          /* a forced function that is no candidate here: the first */
            if (cand.data[k] == g)
                break;
        res = (long)cand.data[k < cand.len ? k : 0];
    }
    vec_free(&cand);
    sb_free(&want);
    return res;
}

/* The untouched byte at off of blob bi: its draw (dflt unless forced), stored, recorded for the call's graph, and
 * an input of the solver (a symbol, see sweep_track) when sym. */
static void lazy_byte(Ev *v, Lazy *z, int bi, uint64_t off, uint8_t dflt, bool sym)
{
    Obj *o = &v->obj.data[z->bl.data[bi].id - 1];
    LKey key = lz_key(z->bl.data[bi].key, off);
    uint8_t b = (uint8_t)lazy_draw(z, key, NULL, dflt);
    o->b[off] = b;
    o->init[off] = 1;
    if (v->track && sym) {
        if (!o->sh)
            o->sh = xcalloc(o->size ? o->size : 1, sizeof *o->sh);
        o->sh[off] = lazy_sym(v, z, key, false);
    }
    lazy_record(&z->bl.data[bi], off, 1, &b);
}

/* How many blobs in a row, ending at blob bi, were made by a pointer at offset off of their parent: the length of a chain of
 * self-links (next, prev, previous) that a pointer cell at off of bi would extend.  Keyed by the creation path, so it does not depend on the draw order. */
static unsigned lazy_chain(const Lazy *z, int bi, uint64_t off)
{
    unsigned depth = 0;
    while (z->bl.data[bi].par >= 0 && z->bl.data[bi].poff == off) {
        depth++;
        bi = z->bl.data[bi].par;
    }
    return depth;
}

/* Ev.lazy_fill: the typed load of ti->bytes bytes at off of lazy object id finds some uninitialized.
 * Scalars are drawn as the generator draws them; a pointer cell is NULL at the one draw a call picks
 * (z->nullat; half of the calls have none) and where it would extend a chain of LCHAIN self-links (the root plus LCHAIN blobs)
 * (a walk down one then ends before the blob budget), else shares an existing blob (1 in 7) or is a new one. */
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
        if (lazy_chain(z, bi, off) >= LCHAIN)
            r = 0;
        LLink l = {(uint32_t)bi, (uint32_t)off, 0, 0};
        LKey ck = lz_key(z->bl.data[bi].key, off);
        /* The hint of the cell's pointee size (lazy_redraw): share the blob it names, if the call has made it (the first cell of the size
         * is read before that blob exists, and a share of the unmade blob would be of its own); the call's one NULL keeps to its draw. */
        const LEnt *se = ti->psz && d != z->nullat ? llog_at(z->force, lz_key(LZ_SZ, ti->psz)) : NULL;
        uint64_t one = 1;
        uint32_t psh = 0;              /* the pointer word as an expression, when its offset is the solver's */
        uint32_t osh;                   /* the draw of the outcome (0 NULL, else not), when tracking */
        if (un != ti->bytes)
            return false;               /* half a pointer was written: not a pointer to draw */
        if (se && lazy_blob_key(z, se->val) < 0)
            se = NULL;
        /* 0 NULL, 1 share, 2 new */
        r = lazy_draw(z, lz_key(ck, LZ_OUT), se ? &one : NULL, r > 2 ? 2 : r);
        osh = v->track ? sx_ext(&v->sx, lazy_sym(v, z, lz_key(ck, LZ_OUT), true), 1, false, false) : 0;
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
            int b;
            tk = lazy_draw(z, lz_key(ck, LZ_TGT), se ? &se->val : NULL, z->bl.data[t].key);
            b = lazy_blob_key(z, tk);               /* a forced target the call has no blob for: the drawn one */
            if (b >= 0)
                t = b;
            to = lazy_off(v, z, lz_key(ck, LZ_TOFF), to, z->bl.data[t].id, ti->psz ? (uint32_t)ti->psz : 1, &psh);
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
        if (v->track) {                 /* the word, or NULL when the solver draws the outcome 0: a stand-in word for a NULL cell */
            uint32_t w = psh ? psh : sx_const(&v->sx, 64, x ? x : (uint64_t)1 << 32 | LHEAD);
            psh = sx_op(&v->sx, SX_MASK, w, osh);
        }
        if (psh) {
            if (!o->sh)
                o->sh = xcalloc(o->size ? o->size : 1, sizeof *o->sh);
            for (i = 0; i < 8; i++)
                o->sh[off + i] = sx_byte(&v->sx, psh, i);
        }
        return true;
    }
    x = sweep_arg(v, ti, z->rs, true);
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

/* The draws that would make the stopped call go further, at most three: what the sweep forces on a rerun.
 * Each rule reads the stop and names the cells whose draw was wrong, and what to draw there instead.  A cell is
 * named twice, by itself (a rerun of a seed holds an entry per cell) and by its pointee size (every cell of that size, such as
 * the later elements of a list or the fields of a stack, takes the share without a retry each). */
static unsigned lazy_redraw(const Ev *v, const Lazy *z, LEnt hint[3])
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
                if (v->sub_sz) {
                    hint[2] = (LEnt){lz_key(LZ_SZ, v->sub_sz), z->bl.data[lo].key};
                    n = 3;
                }
            } else if (z->bl.data[lo].par < 0) {
                hint[0] = (LEnt){lz_key(h->key, LZ_ALIAS), (uint64_t)(z->bl.data[lo].poff + 1)};
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
                      bool *bad, uint64_t *rs, bool may_retry, LEnt hint[3], unsigned *nhint, bool *retry,
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
    v->asm_ran = false;
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
            uint64_t raw = sweep_arg(v, &a->ti, rs, false), drawn = 0;
            uint32_t sh = 0;
            for (i = 0; i < a->ti.bytes; i++) {
                LKey key = lz_key(lz_key(LZ_PARAM, k), i);
                drawn |= (lazy_draw(&z, key, NULL, raw >> (8 * i) & 255) & 255) << (8 * i);
                if (v->track && !a->ti.flt) {   /* the scalar is an input: its bytes' symbols, joined (see sweep_track) */
                    uint32_t b = lazy_sym(v, &z, key, false);
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
            uint32_t off = LHEAD, osh = 0;
            uint64_t doff = LHEAD;
            if (a->kind == SA_PTR && k) {     /* into an earlier pointer parameter's object: j + 1, 0 none; then the offset */
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
                        al = (uint64_t)(j + 1);
                        doff = LHEAD + (rnd(rs) % 9) * 8;
                    }
                }
                al = lazy_draw(&z, lz_key(lz_key(LZ_PARAM, k), LZ_ALIAS), NULL, al);
                j = (size_t)(al & 0xffff);
                if (j >= 1 && j <= k && s->a[j - 1].kind == SA_PTR) {
                    bi = lazy_index(&z, pid(slot[j - 1]));
                    if (bi >= 0)
                        off = lazy_off(v, &z, lz_key(lz_key(LZ_PARAM, k), LZ_TOFF), doff, z.bl.data[bi].id,
                                       a->ti.psz ? (uint32_t)a->ti.psz : 1, &osh);
                }
            }
            if (bi < 0)
                bi = lazy_new(v, &z, a->kind == SA_REC ? a->off : 0, -1, (uint32_t)k);
            if (bi < 0)
                break;
            slot[k] = mkptr(z.bl.data[bi].id, off);
            ash[k] = osh;
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
        v->sub_a = v->sub_b = v->sub_sz = v->pun_obj = 0;
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
    if (!*retry && !(r.status == EV_OK && v->asm_ran) && (r.status == EV_OK || (show_stop && (!s->ptrfn || r.status == EV_UNSUPPORTED)))) {
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
        sort_n(log->data, log->len, sizeof *log->data, lz_cmp);
}

/* Put e over the log: the entry with its key takes e's value, else e is added (the log stays sorted). */
static void hint_put(LLog *log, LEnt e)
{
    LEnt *f = llog_at(log, e.key);
    if (f)
        f->val = e.val;
    else
        vec_push(log, e);
    llog_sort(log);
}

/* The call, rerun each time it stops on a subtraction of pointers of two blobs with the
 * later blob's pointer cell, and the cells of its pointee size, forced to share the earlier blob (the program meant them as one object, such as
 * a saved pc and the code it points into): the same draws, so the rerun differs only there.  *rec gets what
 * the final run drew (unsorted); with `force` (sorted) the call takes those values instead of drawing them,
 * and the hints are put over them (a forced call that already shares as asked is not retried). */
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
        LEnt hint[3];
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

/* ---- solving for inputs (docs/CONCOLIC_DESIGN.md) ------------------------------------
 * From a call that finished: rerun it tracked, walk its path keeping the prefix asserted, and at
 * each branch whose other edge no call has taken ask the solver for bytes that follow the prefix
 * and take it.  Each answer is one more call through sweep_call (printed only if it finishes,
 * compared with gcc like every other), so the solver may be wrong or incomplete. */

#define SQUERY 48               /* solver queries per function */
#define SSEEDS 4                /* finishing inputs kept as starting points */
#define SEXPAND 8               /* tracked runs per function */
#define SRLIMIT 5000000         /* the solver's resource limit per query: deterministic, not time */
#define STRAP 4                 /* calls that stopped (trap, unsupported, limit), kept as starting points of their own */
#define STBASE 4                /* tracked runs of stopped calls per function, plus one per STDIV untaken edges, at most STMAX */
#define STDIV 8
#define STMAX 256

typedef struct SolveStat {
    unsigned long queries, sat, unsat, unknown, calls, fresh;
    size_t edges, taken;            /* edges of the swept functions, and those taken by the end */
    size_t nodes;                   /* the most expression nodes one tracked call made */
    unsigned long seeded, tracked;  /* functions with a finishing input; tracked runs */
    unsigned long steps, open;      /* path steps (branches on symbolic values, pins); branches with an unseen edge */
    unsigned long branches;         /* every branch and switch the tracked runs took */
    unsigned long gain;             /* edges first taken by a solver round, in any function */
    unsigned long stopped;          /* tracked runs of stopped calls (the queue of their own) */
    unsigned long unsat_nopin;      /* --eval-miss: unsat queries that are sat with every pin dropped (an upper bound: a model may not replay) */
    unsigned long unsat_kind[PK_N]; /* ... and with only the pins of that kind dropped */
    unsigned long tq, tsat;         /* g1 spike: table-element queries, and those with a model */
} SolveStat;

/* What came of asking the solver for an edge (--eval-miss reports the edges still untaken by it). */
enum { AK_NOREACH, AK_UNTRK, AK_CONC, AK_OPEN, AK_UNKNOWN, AK_UNSAT, AK_STOP, AK_MISS, AK_N };
static const char *const ask_name[AK_N] = { "no run reached it", "only untracked runs reached it", "reached, condition concrete",
    "symbolic step, not asked", "unknown", "unsat", "sat, run stopped",
    "sat, run finished without it" };

typedef struct STried {
    const Blk *blk;
    uint32_t edge;
    uint8_t ask;                        /* AK_* of the one query */
} STried;

typedef VEC(STried) STriedVec;

/* A branch on a literal constant (`while (1)`, `do { } while (0)`, a macro's `if (0)`): one of its two edges can
 * never be taken, so the sweep counts it as one edge. */
static uint32_t cond_node(const Fn *f, const Blk *b)
{
    uint32_t n = b->val;
    for (;;) {                                  /* look through casts, `!` and the comma */
        const TNode *t = &f->nodes[n];
        if (t->op == TO_CAST || (t->op == TO_UNOP && t->pun == P_BANG))
            n = f->kids[t->kid0];
        else if (t->op == TO_COMMA)
            n = f->kids[t->kid0 + 1];
        else
            return n;
    }
}

static bool blk_const(const Fn *f, const Blk *b)
{
    return b->term == TK_BR && f->nodes[cond_node(f, b)].op == TO_CONST;
}

static void blk_count(const Fn *f, size_t *edges, size_t *taken)
{
    size_t i;
    for (i = 0; i < f->nblk; i++) {
        const Blk *b = &f->blk[i];
        if (blk_const(f, b)) {
            *edges += 1;
            *taken += (b->seen & 3) != 0;
            continue;
        }
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

/* What a solved call came to: nothing worth keeping (a finish with nothing new, an exit), a finish that took a new edge, or a stop. */
enum { SR_NONE, SR_EDGE, SR_STOP };

/* A call that stopped before its end (a trap, an unsupported construct, the step limit): its path up to the stop is a seed. */
static bool call_stopped(int st)
{
    return st == EV_TRAP || st == EV_UNSUPPORTED || st == EV_FUEL;
}

/* One solved call: the seed with the model's values written over it; SR_EDGE if it finished and
 * took an edge nobody had (then it is worth tracking itself), SR_STOP if it stopped (a stopped call is a seed
 * of the queue of its own, see stop_step).  The model is in solver symbols;
 * `syms` (from the seed's tracked run) maps them back to the keys of the draws. */
static int solve_run(Ev *v, const SxModel *m, const SSeed *seed, const LKeys *syms, const Fn *f, const SFn *s,
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
            const LSym *y = m->off.data[i] < syms->len ? &syms->data[m->off.data[i]] : NULL;
            LEnt *e = y ? llog_at(&force, y->key) : NULL;
            if (e)      /* an outcome: NULL or not; not NULL is the seed's outcome, or a new blob where the seed had NULL */
                e->val = y->out ? (m->val.data[i] ? (e->val ? e->val : 2) : 0) : m->val.data[i];
        }
        c.rs0 = seed->rs0;
        st = sweep_call(v, body, name, f, s, false, bad, &rs, &op);
        vec_free(&force);
        llog_sort(&c.log);
    }
    *saw_ok |= st == EV_OK;
    if ((st == EV_OK && v->newseen != before) || call_stopped(st)) {
        *child = c;
        return call_stopped(st) ? SR_STOP : SR_EDGE;
    }
    seed_free(&c);
    return SR_NONE;
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
    size_t tqueries;                    /* table-index queries (g1 spike): a budget of their own, STABQ */
    VEC(SSeed) stopped;                 /* calls that stopped: the queue of their own, popped by stop_step */
    size_t texpanded;
} SSearch;

/* The call that just ran as a seed: the log it drew (moved out of *log) and the random state it started from. */
static SSeed seed_take(LLog *log, uint64_t rs0)
{
    SSeed sd = {0};
    llog_sort(log);
    sd.log = *log;
    memset(log, 0, sizeof *log);
    sd.rs0 = rs0;
    return sd;
}

/* Keeps the call that just finished as a seed. */
static void search_keep(SSearch *q, LLog *log, uint64_t rs0, bool fresh)
{
    if (q->corpus.len >= SCORPUS || (q->corpus.len >= SSEEDS && !fresh))
        return;
    vec_push(&q->corpus, seed_take(log, rs0));
}

/* Keeps a call that stopped for the queue of its own; sd is consumed. */
static void stop_push(SSearch *q, SSeed sd)
{
    if (q->stopped.len >= STRAP)
        seed_free(&sd);
    else
        vec_push(&q->stopped, sd);
}

static void search_free(SSearch *q)
{
    while (q->corpus.len) {
        SSeed sd = vec_pop(&q->corpus);
        seed_free(&sd);
    }
    vec_free(&q->corpus);
    vec_free(&q->done);
    while (q->stopped.len) {
        SSeed sd = vec_pop(&q->stopped);
        seed_free(&sd);
    }
    vec_free(&q->stopped);
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

/* The conditions that take edge e of the branch step p (n of them; whole: all could be built). */
static size_t flip_conds(Ev *v, const PathEnt *p, uint32_t e, SxCond *cs, bool *whole)
{
    *whole = true;
    if (p->blk->term == TK_BR) {
        cs[0].a = p->expr;
        cs[0].want = !p->want;
        return 1;
    }
    return switch_conds(v, p, e, cs, whole);
}

/* The step itself joins the prefix. */
static void assert_step(Ev *v, SxSolver *sv, const PathEnt *p)
{
    if (!p->blk || p->blk->term == TK_BR) {
        sx_assert(sv, p->expr, p->want);
    } else {
        SxCond cs[64];
        bool whole;
        size_t n = switch_conds(v, p, p->edge, cs, &whole), k;
        for (k = 0; k < n; k++)
            sx_assert(sv, cs[k].a, cs[k].want);
    }
}

/* Is edge e of path step i taken by some input when the pins of the kinds in drop (a PinKind mask) are left out of
 * the prefix?  Only for the --eval-miss census of what the pins cost. */
static bool probe_sat(Ev *v, SxSolver *sv, size_t i, uint32_t e, unsigned drop)
{
    SxCond cs[64];
    SxModel m = {0};
    bool whole;
    size_t j, n;
    int r;
    sx_begin(sv, &v->sx);
    for (j = 0; j < v->dom.len; j++)
        sx_assert(sv, v->dom.data[j], true);
    for (j = 0; j < i; j++)
        if (v->path.data[j].blk || !(drop >> v->path.data[j].edge & 1))
            assert_step(v, sv, &v->path.data[j]);
    n = flip_conds(v, &v->path.data[i], e, cs, &whole);
    r = whole ? sx_try(sv, cs, n, &m) : SX_ERROR;
    sx_model_free(&m);
    return r == SX_SAT;
}

/* One query: do the bytes exist that satisfy cs (n conditions) on top of the prefix asserted so far?  If so, run them
 * as one more call, kept as a seed if it takes a new edge (or, if it stops, in the queue of its own).  The AK_* class
 * of the answer. */
static int flip_ask(SCtx *x, const SSeed *seed, const LKeys *syms, const SxCond *cs, size_t n, SxModel *m, size_t *budget)
{
    Ev *v = x->v;
    SolveStat *st = x->ss;
    SSearch *q = &x->q;
    SSeed child = {0};
    int r, sat;
    ++*budget;
    st->queries++;
    r = sx_try(x->sv, cs, n, m);
    if (r == SX_ERROR) {
        fprintf(stderr, "cereal: the solver stopped answering\n");
        exit(2);
    }
    st->sat += r == SX_SAT;
    st->unsat += r == SX_UNSAT;
    st->unknown += r == SX_UNKNOWN;
    if (r != SX_SAT)
        return r == SX_UNSAT ? AK_UNSAT : AK_UNKNOWN;
    st->calls++;
    sat = solve_run(v, m, seed, syms, x->f, x->s, x->body, x->name, &x->bad, &x->saw_ok, &child);
    if (sat == SR_STOP) {
        stop_push(q, child);
    } else if (sat == SR_EDGE) {
        st->fresh++;
        if (q->corpus.len < SCORPUS)
            vec_push(&q->corpus, child);
        else
            seed_free(&child);
    }
    return sat == SR_STOP ? AK_STOP : AK_MISS;
}

static unsigned tab_cap(const char *name, unsigned def)        /* g1 spike: the caps are env-tunable while measuring */
{
    const char *s = getenv(name);
    return s ? (unsigned)strtoul(s, NULL, 10) : def;
}
#define STAB tab_cap("CEREAL_STAB", 8)  /* table elements asked per walk (g1 spike) */
#define STABQ tab_cap("CEREAL_STABQ", 48) /* table-index queries per function (g1 spike); the branch flips keep SQUERY */

/* g1 spike: the pin p is the index of a pointer into the read-only table p.tobj.  Ask for indices this index site has
 * not taken yet: the pin's word equal to each. */
static void table_flips(SCtx *x, const SSeed *seed, const LKeys *syms, const PathEnt *p, SxModel *m, unsigned *left)
{
    Ev *v = x->v;
    uint32_t sh = sx_at(&v->sx, p->expr)->a;
    uint64_t k;
    for (k = 0; k < p->tn && *left && x->q.tqueries < STABQ; k++) {
        size_t i;
        SxCond c;
        for (i = 0; i < v->tseen.len; i++)
            if (v->tseen.data[i].key == p->tkey && v->tseen.data[i].val == k)
                break;
        if (i < v->tseen.len)
            continue;
        {
            TSeen t = { p->tkey, k };
            vec_push(&v->tseen, t);
        }
        c.a = sx_op(&v->sx, SX_EQ, sh, sx_const(&v->sx, 64, k));
        c.want = true;
        (*left)--;
        x->ss->tq++;
        x->ss->tsat += flip_ask(x, seed, syms, &c, 1, m, &x->q.tqueries) >= AK_STOP;
    }
}


/* The walk over the tracked path of seed (just rerun, its symbols in syms): at each branch whose other edge
 * nobody has taken, ask for bytes that take it, the steps before it as the prefix; each answer is one more
 * call, kept if it takes a new edge (or, if it stops, in the queue of its own). */
static void flip_walk(SCtx *x, const SSeed *seed, const LKeys *syms)
{
    Ev *v = x->v;
    SxSolver *sv = x->sv;
    SolveStat *st = x->ss;
    SSearch *q = &x->q;
    STriedVec *done = &q->done;
    SxModel m = {0};
    struct { size_t i; uint32_t e; } un[SQUERY];
    size_t i, nun = 0;
    bool tflip = getenv("CEREAL_TFLIP") != NULL;
    unsigned tleft = STAB;
    sx_begin(sv, &v->sx);
    for (i = 0; i < v->dom.len; i++)
        sx_assert(sv, v->dom.data[i], true);
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
                bool whole = true;
                int ask;
                if (e == p.edge || (b->seen >> blk_edge_bit(b, e) & 1) || tried(done, b, e))
                    continue;
                n = flip_conds(v, &p, e, cs, &whole);
                if (!whole)
                    continue;
                {
                    STried t = { b, e, AK_UNKNOWN };
                    vec_push(done, t);
                }
                ask = flip_ask(x, seed, syms, cs, n, &m, &q->queries);
                done->data[done->len - 1].ask = ask;
                if (ask == AK_UNSAT && v->c->opt.eval_miss && nun < SQUERY) {
                    un[nun].i = i;
                    un[nun++].e = e;
                }
            }
        } else if (p.tobj && tflip) {
            table_flips(x, seed, syms, &p, &m, &tleft);
        }
        assert_step(v, sv, &p);
    }
    for (i = 0; i < nun; i++) {                 /* --eval-miss: would the unsat queries be sat without their pins? */
        unsigned present = 0, k;
        size_t j;
        for (j = 0; j < un[i].i; j++)
            if (!v->path.data[j].blk)
                present |= 1u << v->path.data[j].edge;
        if (!present || !probe_sat(v, sv, un[i].i, un[i].e, present))
            continue;
        st->unsat_nopin++;
        for (k = 0; k < PK_N; k++)
            if (present >> k & 1)
                st->unsat_kind[k] += probe_sat(v, sv, un[i].i, un[i].e, 1u << k);
    }
    sx_model_free(&m);
}

/* The solver mutation: track the next corpus input and flip the branches of its path (flip_walk).
 * False when the corpus or the budgets are used up. */
static bool solve_step1(SCtx *x)
{
    Ev *v = x->v;
    SolveStat *st = x->ss;
    SSearch *q = &x->q;
    LKeys syms = {0};
    SSeed seed;                         /* a shallow copy: the corpus may grow (and move) below */
    StrBuf sink = {0};
    int r;
    if (q->next >= q->corpus.len || q->expanded >= SEXPAND || q->queries >= SQUERY)
        return false;
    seed = q->corpus.data[q->next++];
    v->track = true;
    {
        LOpt op = {&seed.log, NULL, &syms};
        uint64_t rs = seed.rs0;
        r = sweep_call(v, &sink, x->name, x->f, x->s, false, &x->bad, &rs, &op);
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
    flip_walk(x, &seed, &syms);
    vec_free(&syms);
    return true;
}

/* Tracked runs of stopped calls per function: STBASE, plus one for each STDIV edges still untaken, at most STMAX. */
static size_t stop_budget(const SCtx *x)
{
    size_t edges = 0, taken = 0, b;
    blk_count(x->f, &edges, &taken);
    b = STBASE + (edges - taken) / STDIV;
    return b < STMAX ? b : STMAX;
}

/* The stopped-call mutation: a call that stopped is a seed like a finished one, but from the queue of its own
 * (a stop is not an input to keep as a corpus entry).  Rerun tracked, its path up to the stop is walked like
 * any other, and each answer that stops again joins the queue (a chain).  False when the queue or the budget is used up. */
static bool stop_step(SCtx *x)
{
    Ev *v = x->v;
    SSearch *q = &x->q;
    LKeys syms = {0};
    SSeed seed;
    StrBuf sink = {0};
    int r;
    if (!q->stopped.len || q->texpanded >= stop_budget(x) || q->queries >= SQUERY)
        return false;
    seed = vec_pop(&q->stopped);
    v->track = true;
    {
        LOpt op = {&seed.log, NULL, &syms};
        uint64_t rs = seed.rs0;
        r = sweep_call(v, &sink, x->name, x->f, x->s, false, &x->bad, &rs, &op);
    }
    v->track = false;
    sb_free(&sink);
    q->texpanded++;
    x->ss->stopped++;
    v->nbr = 0;
    if (call_stopped(r))
        flip_walk(x, &seed, &syms);
    vec_free(&syms);
    seed_free(&seed);
    return true;
}

static bool solve_step(SCtx *x)
{
    unsigned long seen = x->v->newseen;
    bool r = stop_step(x) || solve_step1(x) || stop_step(x);
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
        if (v->c->opt.eval_only)                /* --eval-only: each call's outcome */
            fprintf(stderr, "cereal: call %s: %s%s%s\n", x->name, st ? stop_name(st) : "finished", st ? ": " : "", st ? v->res.why : "");
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
        else if (x->sv && call_stopped(st))
            stop_push(&x->q, seed_take(&rec, rs0));
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
enum { MC_NOFIN, MC_SWITCH, MC_PNULL, MC_PCMP, MC_FLOAT, MC_ICMP, MC_IVAL, MC_CALL, MC_CALL0, MC_OTHER, MC_N };
static const char *const miss_name[MC_N] = { "function never finished", "switch", "pointer null test",
    "pointer compare", "float", "integer compare", "integer value", "call result",
    "call without arguments", "other" };

static bool miss_const(const Fn *f, uint32_t n)
{
    while (f->nodes[n].op == TO_CAST)
        n = f->kids[f->nodes[n].kid0];
    return f->nodes[n].op == TO_CONST;
}

static int miss_class(Ev *v, const Fn *f, const Blk *b)
{
    uint32_t n = cond_node(f, b);
    Ti ti;
    int c = MC_OTHER;
    if (b->term == TK_SWTCH)
        return MC_SWITCH;
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
    bool has_ask;                       /* the solver ran: ask counts the untaken edges by class and by what asking came to */
    size_t ask[MC_N][AK_N];
    size_t fr[MC_N][AK_N];              /* the same for the frontier: untaken edges of a reached block into a block nothing reached */
} SweptFn;

/* Is name one of the comma-separated names of list? */
static bool name_listed(const char *list, const char *name)
{
    size_t n = strlen(name);
    while (*list) {
        const char *e = strchr(list, ',');
        size_t l = e ? (size_t)(e - list) : strlen(list);
        if (l == n && !memcmp(list, name, n))
            return true;
        list += l + (e != NULL);
    }
    return false;
}

/* The untaken edges of f by class (cls), as the sweep counts edges (blk_count); the total.  ids: one `cereal: edge FN
 * BLOCK.EDGE CLASS` line per edge on stderr (--eval-miss), so runs with other seeds can be intersected. */
static size_t fn_miss(Ev *v, const Fn *f, bool finished, size_t cls[MC_N], bool ids)
{
    size_t i, total = 0;
    v->c = f->c;
    for (i = 0; i < f->nblk; i++) {
        const Blk *b = &f->blk[i];
        unsigned ne = b->term == TK_BR ? 2 : b->term == TK_SWTCH ? (b->nc > 61 ? 62 : (unsigned)b->nc + 1) : 0, e;
        int c = -1;
        if (blk_const(f, b)) {                  /* its one live edge, untaken only if the block was never reached */
            if (!(b->seen & 3)) {
                cls[finished ? MC_OTHER : MC_NOFIN]++;
                if (ids)
                    fprintf(stderr, "cereal: edge %s %zu.0 %s\n", ev_fn_name(f->c, f->fsym), i, miss_name[finished ? MC_OTHER : MC_NOFIN]);
                total++;
            }
            continue;
        }
        for (e = 0; e < ne; e++)
            if (!(b->seen >> (b->term == TK_SWTCH && e == ne - 1 ? 62 : e) & 1)) {
                if (c < 0)
                    c = finished ? miss_class(v, f, b) : MC_NOFIN;
                cls[c]++;
                if (ids)
                    fprintf(stderr, "cereal: edge %s %zu.%u %s\n", ev_fn_name(f->c, f->fsym), i,
                            b->term == TK_SWTCH && e == ne - 1 ? 62u : e, miss_name[c]);
                total++;
            }
    }
    return total;
}

/* The untaken edges of the finished function f by class and by what asking the solver came to (done: the edges asked);
 * the same edges as fn_miss counts, at the moment the function's search ends (later callers may take more).  Caveats:
 * `done` holds the queries made while sweeping f's plain variant, callee blocks included, and none of the state variants';
 * Blk.trk is set by any tracked run of any function; "unsat" is relative to the first seed's path prefix; "symbolic step,
 * not asked" lumps the query and run budgets, a path cap and a tracked run that did not finish. */
static void fn_ask(Ev *v, const Fn *f, const STriedVec *done, size_t ask[MC_N][AK_N], size_t fr[MC_N][AK_N])
{
    size_t i;
    v->c = f->c;
    for (i = 0; i < f->nblk; i++) {
        const Blk *b = &f->blk[i];
        unsigned ne = b->term == TK_BR ? 2 : b->term == TK_SWTCH ? (b->nc > 61 ? 62 : (unsigned)b->nc + 1) : 0, e;
        int c = -1;
        if (blk_const(f, b)) {
            ask[MC_OTHER][AK_NOREACH] += !(b->seen & 3);
            continue;
        }
        for (e = 0; e < ne; e++)
            if (!(b->seen >> (b->term == TK_SWTCH && e == ne - 1 ? 62 : e) & 1)) {
                uint32_t fe = b->term == TK_SWTCH && e == ne - 1 ? (uint32_t)b->nc : e;
                size_t j;
                int a = b->trk & 2 ? AK_OPEN : b->trk ? AK_CONC : b->seen ? AK_UNTRK : AK_NOREACH;
                if (c < 0)
                    c = miss_class(v, f, b);
                for (j = 0; j < done->len; j++)
                    if (done->data[j].blk == b && done->data[j].edge == fe)
                        a = done->data[j].ask;
                ask[c][a]++;
                if (b->seen) {          /* a reached block: is this edge the way into one nothing reached? */
                    uint32_t to = b->term == TK_BR ? (fe ? b->b : b->a) : fe < b->nc ? b->cases[fe].to : b->a;
                    size_t hop;     /* only branch blocks record `seen`: follow jumps to the next one */
                    for (hop = 0; hop < f->nblk && to < f->nblk && f->blk[to].term == TK_JMP; hop++)
                        to = f->blk[to].a;
                    fr[c][a] += to < f->nblk && (f->blk[to].term == TK_BR || f->blk[to].term == TK_SWTCH) && !f->blk[to].seen;
                }
            }
    }
}

typedef struct Gate {
    uint32_t blk, ed;                   /* the block and the edge (as the `cereal: edge` lines give it) */
    int cls;
    uint8_t *reach;                     /* the blocks its edge leads to without passing a reached branch */
} Gate;

static size_t blk_nsucc(const Blk *b)
{
    return b->term == TK_BR ? 2 : b->term == TK_SWTCH ? b->nc + 1 : b->term == TK_JMP ? 1 : 0;
}

static uint32_t blk_succ(const Blk *b, size_t i)
{
    return b->term == TK_BR ? (i ? b->b : b->a) : b->term == TK_SWTCH && i < b->nc ? b->cases[i].to : b->a;
}

static bool blk_reached_branch(const Blk *b)
{
    return (b->term == TK_BR || b->term == TK_SWTCH) && b->seen;
}

/* --eval-miss: the untaken edges of branch blocks no run reached, traced to their gates: the untaken edges of reached
 * blocks that lead into them (the frontier).  Greedy cover, per function: the gate with the most still uncovered edges
 * behind it first.  Prints the picks, then the function's totals (edges behind no gate: unreachable here, or only
 * through a computed goto). */
static void fn_gates(Ev *v, const Fn *f)
{
    size_t n = f->nblk, i, e, tot = 0, toti = 0, left = 0, picks = 0, lab = 0;
    unsigned *wt = calloc(n, sizeof *wt), *wi = calloc(n, sizeof *wi);
    uint8_t *cov = calloc(n, 1);
    uint32_t *stk = malloc(n * sizeof *stk);
    VEC(Gate) gs = {0};
    v->c = f->c;
    if (!wt || !wi || !cov || !stk)
        goto out;
    for (i = 0; i < n; i++) {
        const Blk *b = &f->blk[i];
        if ((b->term != TK_BR && b->term != TK_SWTCH) || b->seen)
            continue;
        wt[i] = blk_const(f, b) ? 1 : b->term == TK_BR ? 2 : (b->nc > 61 ? 62 : (unsigned)b->nc + 1);
        if (!blk_const(f, b)) {
            int c = miss_class(v, f, b);
            wi[i] = c == MC_ICMP || c == MC_IVAL ? wt[i] : 0;
        }
        tot += wt[i];
        toti += wi[i];
    }
    for (i = 0; i < n && tot; i++) {
        const Blk *b = &f->blk[i];
        unsigned ne = b->term == TK_BR ? 2 : b->term == TK_SWTCH ? (b->nc > 61 ? 62 : (unsigned)b->nc + 1) : 0;
        int c = -1;
        if (!b->seen || !ne || blk_const(f, b))
            continue;
        for (e = 0; e < ne; e++) {
            uint32_t ed = b->term == TK_SWTCH && e == ne - 1 ? 62u : (uint32_t)e;
            uint32_t t = blk_succ(b, ed == 62u ? b->nc : e);
            size_t sp = 0, hit = 0;
            uint8_t *r;
            if (b->seen >> ed & 1 || t >= n || blk_reached_branch(&f->blk[t]))
                continue;
            if (!(r = calloc(n, 1)))
                goto out;
            r[t] = 1;
            stk[sp++] = t;
            while (sp) {
                uint32_t x = stk[--sp];
                const Blk *y = &f->blk[x];
                size_t k, ns = blk_nsucc(y);
                hit += wt[x];
                for (k = 0; k < ns; k++) {
                    uint32_t s = blk_succ(y, k);
                    if (s < n && !r[s] && !blk_reached_branch(&f->blk[s])) {
                        r[s] = 1;
                        stk[sp++] = s;
                    }
                }
            }
            if (!hit) {
                free(r);
                continue;
            }
            if (c < 0)
                c = miss_class(v, f, b);
            {
                Gate g = { (uint32_t)i, ed, c, r };
                vec_push(&gs, g);
            }
        }
    }
    while (tot) {
        size_t best = 0, bw = 0, bi = 0, g, x;
        for (g = 0; g < gs.len; g++) {
            size_t w = 0, wj = 0;
            for (x = 0; x < n; x++)
                if (gs.data[g].reach[x] && !cov[x]) {
                    w += wt[x];
                    wj += wi[x];
                }
            if (w > bw) {
                best = g;
                bw = w;
                bi = wj;
            }
        }
        if (!bw)
            break;
        if (picks++ < 12)
            fprintf(stderr, "cereal: gate %s %u.%u %s opens %zu edges (%zu integer)\n", ev_fn_name(f->c, f->fsym),
                    gs.data[best].blk, gs.data[best].ed, miss_name[gs.data[best].cls], bw, bi);
        for (x = 0; x < n; x++)
            if (gs.data[best].reach[x] && !cov[x]) {
                cov[x] = 1;
                left += wt[x];
            }
    }
    if (tot && f->nlab) {                       /* of the edges behind no gate, those behind a label of a computed goto */
        uint8_t *lr = calloc(n, 1);
        size_t j, sp = 0;
        if (lr) {
            for (j = 0; j < f->nlab; j++)
                if (f->lab[j].blk < n && !lr[f->lab[j].blk] && !blk_reached_branch(&f->blk[f->lab[j].blk])) {
                    lr[f->lab[j].blk] = 1;
                    stk[sp++] = f->lab[j].blk;
                }
            while (sp) {
                uint32_t x = stk[--sp];
                const Blk *y = &f->blk[x];
                size_t k, ns = blk_nsucc(y);
                for (k = 0; k < ns; k++) {
                    uint32_t s = blk_succ(y, k);
                    if (s < n && !lr[s] && !blk_reached_branch(&f->blk[s])) {
                        lr[s] = 1;
                        stk[sp++] = s;
                    }
                }
            }
            for (j = 0; j < n; j++)
                lab += lr[j] && !cov[j] ? wt[j] : 0;
            free(lr);
        }
    }
    if (tot)
        fprintf(stderr, "cereal: gated %s: %zu edges (%zu integer) in unreached blocks, %zu behind no gate (%zu of them behind a label), %zu gates, %zu picks\n",
                ev_fn_name(f->c, f->fsym), tot, toti, tot - left, lab, gs.len, picks);
out:
    while (gs.len)
        free(vec_pop(&gs).reach);
    vec_free(&gs);
    free(wt);
    free(wi);
    free(cov);
    free(stk);
}

static void miss_report(Ev *v, const SweptFn *sw, size_t n)
{
    size_t counts[MC_N] = {0}, total = 0, k, i;
    size_t ask[MC_N][AK_N] = {{0}}, fr[MC_N][AK_N] = {{0}};
    struct { char key[104]; size_t edges; const char *ex[4]; unsigned nex; } why[128];
    size_t nwhy = 0;
    struct AtEnt { char key[200]; size_t edges; } at[256];
    size_t nat = 0;
    for (k = 0; k < n; k++) {
        const Fn *f = &v->fn.data[sw[k].i];
        size_t cls[MC_N] = {0}, ft = fn_miss(v, f, sw[k].finished, cls, v->c->opt.eval_miss), fe = cls[MC_NOFIN];
        for (i = 0; i < MC_N; i++)
            counts[i] += cls[i];
        total += ft;
        if (v->c->opt.eval_miss && sw[k].finished)
            fn_gates(v, f);
        if (fe) {                                   /* by the function's first stop, digits and addresses folded */
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
            if (w < nwhy) {
                why[w].edges += fe;
                if (why[w].nex < 4 && (!why[w].nex || why[w].ex[why[w].nex - 1] != ev_fn_name(f->c, f->fsym)))
                    why[w].ex[why[w].nex++] = ev_fn_name(f->c, f->fsym);
            }
        }
        if (v->c->opt.eval_miss) {                  /* --eval-miss: this function's untaken edges by class */
            fprintf(stderr, "cereal: miss %s: %zu untaken:", ev_fn_name(f->c, f->fsym), ft);
            for (i = 0; i < MC_N; i++)
                if (cls[i])
                    fprintf(stderr, " %s %zu;", miss_name[i], cls[i]);
            fputc(10, stderr);
        }
        if (sw[k].has_ask)
            for (i = 0; i < MC_N; i++) {
                unsigned a;
                for (a = 0; a < AK_N; a++) {
                    ask[i][a] += sw[k].ask[i][a];
                    fr[i][a] += sw[k].fr[i][a];
                }
            }
        if (fe) {                                   /* per function, and by the place the stop read its pointer */
            char key[200], *p;
            size_t w;
            fprintf(stderr, "cereal: stuck: %zu %s: %s%s%s\n", fe, ev_fn_name(f->c, f->fsym), sw[k].why,
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
    if (v->c->opt.eval_miss)                        /* the finished functions' untaken edges by class, by what asking came to */
        for (i = 0; i < MC_N; i++) {
            unsigned a;
            size_t t = 0;
            for (a = 0; a < AK_N; a++)
                t += ask[i][a];
            if (!t)
                continue;
            fprintf(stderr, "cereal: asked %s: %zu:", miss_name[i], t);
            for (a = 0; a < AK_N; a++)
                fprintf(stderr, " %s %zu;", ask_name[a], ask[i][a]);
            fputc(10, stderr);
            for (t = 0, a = 0; a < AK_N; a++)
                t += fr[i][a];
            fprintf(stderr, "cereal: frontier %s: %zu:", miss_name[i], t);
            for (a = 0; a < AK_N; a++)
                fprintf(stderr, " %s %zu;", ask_name[a], fr[i][a]);
            fputc(10, stderr);
        }
}

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

/* Every function of the files of c0 (one file, or the whole link: c0 is then the first) is swept.
 * A function is swept in variants: one per way to pick its built states (--eval-state; none when it
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
        fprintf(out, "fp %zu %s\n", i, ev_fn_name(f->c, f->fsym));
    }
    for (i = 0; i < v->fn.len; i++) {
        Fn *f = &v->fn.data[i];
        Checker *c = f->c;
        const char *name;
        unsigned nv, vi;
        bool fin = false;               /* a state variant finished */
        bool has_ask = false;           /* the plain variant's untaken edges by class and by the solver's answer (fn_ask) */
        size_t ask[MC_N][AK_N] = {{0}}, fr[MC_N][AK_N] = {{0}};
        if (!sweep_cand(f))
            continue;
        v->c = c;
        name = ev_fn_name(c, f->fsym);
        if (c0->opt.eval_only && !name_listed(c0->opt.eval_only, name))
            continue;
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
                sort_n(v->dict.data, v->dict.len, sizeof *v->dict.data, u64_cmp);
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
                if (!st && (x.saw_ok || fin) && c0->opt.eval_miss) {
                    memset(ask, 0, sizeof ask);
                    memset(fr, 0, sizeof fr);
                    fn_ask(v, f, &x.q.done, ask, fr);
                    has_ask = true;
                }
                search_free(&x.q);
            }
            if (!st) {
                SweptFn sw = { i, x.saw_ok || fin, "", "", has_ask, {{0}}, {{0}} };
                memcpy(sw.ask, ask, sizeof sw.ask);
                memcpy(sw.fr, fr, sizeof sw.fr);
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
                "%lu functions with a finishing input, %lu tracked runs (%lu of stopped calls), %lu symbolic steps of %lu branches, "
                "%lu with an unseen edge\n",
                ss.queries, ss.sat, ss.unsat, ss.unknown, ss.calls, ss.fresh, ss.taken, ss.edges, ss.gain,
                ss.nodes, ss.seeded, ss.tracked, ss.stopped, ss.steps, ss.branches, ss.open);
        if (getenv("CEREAL_TFLIP"))
            fprintf(stderr, "cereal: table flips: %lu queries, %lu with a model\n", ss.tq, ss.tsat);
        if (c0->opt.eval_miss) {
            static const char *const pk[PK_N] = { "load", "store", "field", "index", "shift or divide", "goto" };
            unsigned pk_i;
            fprintf(stderr, "cereal: pins recorded:");
            for (pk_i = 0; pk_i < PK_N; pk_i++)
                fprintf(stderr, " %s %lu;", pk[pk_i], v->npin[pk_i]);
            fprintf(stderr, "\ncereal: pins: %lu of the %lu unsat queries are sat with every pin dropped; with only one kind dropped:",
                    ss.unsat_nopin, ss.unsat);
            for (pk_i = 0; pk_i < PK_N; pk_i++)
                fprintf(stderr, " %s %lu;", pk[pk_i], ss.unsat_kind[pk_i]);
            fputc(10, stderr);
        }
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
