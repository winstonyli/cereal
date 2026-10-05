/* cinit.c - initializers (P2c).
 *
 * A port of gcc 13's initializer machinery: c-typeck.cc (digest_init,
 * process_init_element, push_init_level, pop_init_level, set_init_index,
 * set_init_label, output_init_element, the pending-element tree) and the
 * initializer productions of c-parser.cc (c_parser_initelt/_initval).
 *
 * The walk is post-order, so a braced initializer arrives as a stream of
 * nodes: cinit_pre opens the INIT_LISTs that start at a node (gcc's
 * start_init / push_init_level), cinit_post feeds designators and elements
 * and closes lists (pop_init_level).  Each initializer keeps a context
 * (CCtx) with gcc's constructor stack; the values themselves are not built,
 * only what gcc's diagnostics and the array sizes need.  The elements are
 * kept in a per-level sorted set only when the initializer has designators
 * (gcc's pending-element tree); positional initializers just count.
 *
 * gcc reports at input_location, the parser's lookahead; the walk has no
 * parser, so the lookahead token at each event is recorded (x->lam, x->lav)
 * and turned into a location lazily with cdecl_iloc.  See docs/TYPES.md. */
#include "c/check_int.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)
#define NOB 0xFFFFFFFFu
#define LT_IN 0xFFFFFFFFu        /* a location: gcc's input_location */

enum { LV_NULL, LV_ERR, LV_REC, LV_UNI, LV_ARR, LV_VEC, LV_SCAL };
enum { V_NONE, V_ERR, V_EXPR, V_CTOR, V_ZERO };
enum { LA_TOK, LA_AFTER, LA_CLOSE };
enum { PV_SCAL, PV_CTOR, PV_STR };

struct PSet;
struct Lvl;

typedef struct PEnt {
    int64_t key;             /* array index / field index */
    uint64_t strn;           /* PV_STR: units, including the NUL */
    struct PSet *sub;        /* PV_CTOR: the elements of the sub-object */
    bool side, zero;
    uint8_t vk;
} PEnt;

typedef struct PSet {
    PEnt *e;
    uint32_t n, cap;
    struct PSet *next;       /* the context's allocation list */
} PSet;

/* A value reaching process_init_element (gcc's c_expr). */
typedef struct IVal {
    uint8_t kind;
    bool strict;             /* an unparenthesized string literal */
    bool str;                /* a string literal, parentheses stripped */
    bool cl;                 /* a compound literal */
    bool izero;              /* integer_zerop */
    bool side;               /* TREE_SIDE_EFFECTS */
    bool digested;           /* already converted: a scalar level's result */
    bool decayed;            /* array converted to pointer */
    uint32_t node;
    TypeId type;
    uint64_t strn;           /* str: units including the NUL */
    PSet *ps;                /* V_CTOR: the elements */
} IVal;

typedef struct RS {
    struct RS *prev, *next, *alloc_next;
    uint32_t fi;
    int64_t start, index, end;
    bool has_end;
    struct Lvl *stack;
} RS;

/* One level of gcc's constructor stack. */
typedef struct Lvl {
    struct Lvl *up;
    uint8_t kind;
    bool implicit, designated, erroneous, varsize, inc, hasmax, flex, fhn;
    bool has_repl, lag, onlyzero, lastside, anyside, eldes;
    bool fpend;              /* a flexible array's string is pending */
    TypeId type;             /* as declared, qualifiers included */
    TypeId elem;
    int64_t maxidx, idx, ui, lagmax;
    uint32_t fbase, nf, fi, uf;
    uint64_t nel;
    int depth, dd_saved;
    RS *rs_saved;
    IVal repl, e0;
    PSet *ps;
} Lvl;

typedef struct CCtx {
    struct CCtx *prev;
    uint32_t list, lo, hi;   /* the INIT_LIST and its node range */
    bool reqc, dm, varroot;
    bool varerr;             /* "variable-sized object" was reported */
    uint32_t init_loc;       /* token of the brace (or the type name) */
    SrcLoc trad_loc;         /* -Wtraditional's location, 0 if none */
    SrcLoc decl_loc;         /* the declarator, 0 if none */
    Lvl *stk;
    StrBuf path;
    size_t *dl;
    int dlcap, sdepth;
    RS *rs, *rsall;
    PSet *psall;
    int dd, derr;
    uint8_t lam;
    uint32_t lav;
    int64_t rtop;            /* the element count the root ended with */
    bool found_mb_unused;
    char *dwpath;            /* a positional-init warning waiting for the next element */
} CCtx;

typedef struct CInit {
    CCtx *top;
    bool zeroinit, found_mb;
    Lvl *freel;
    uint32_t *tmp;
    size_t tmpcap;
} CInit;

/* ---- tokens -------------------------------------------------------------- */

static int tpunct(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return P_NONE;
    t = &c->u->toks[tok].t;
    return t->kind == TK_PUNCT ? t->punct : P_NONE;
}

static unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static uint32_t first_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = c->nodes[i].tok;
    for (k = cfirst(c, i); k < i; k++)
        if (c->nodes[k].tok < m)
            m = c->nodes[k].tok;
    return m;
}

static uint32_t last_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = 0;
    for (k = cfirst(c, i); k <= i; k++) {
        uint32_t t = c->nodes[k].tok;
        if (c->nodes[k].tag == N_STRING)
            t += node_pieces(c, k) - 1u;
        if (t > m)
            m = t;
    }
    return m;
}

static uint32_t after_tok(const Checker *c, uint32_t i)
{
    uint32_t f = first_tok(c, i), l = last_tok(c, i), k;
    int depth = 0;
    for (k = f; k <= l && k < c->u->ntoks; k++)
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE: depth++; break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE: depth--; break;
        default: break;
        }
    k = l + 1;
    while (depth > 0 && k < c->u->ntoks) {
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE: depth++; break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE: depth--; break;
        default: break;
        }
        k++;
    }
    return k;
}

/* The first child of i (NOB if none). */
static uint32_t first_child(const Checker *c, uint32_t i)
{
    uint32_t k, f = cfirst(c, i);
    if (c->nodes[i].size <= 1)
        return NOB;
    k = i - 1;
    while (cfirst(c, k) > f)
        k = cfirst(c, k) - 1;
    return k;
}

/* ---- types ------------------------------------------------------------------ */

static TypeId mainv(Checker *c, TypeId t)
{
    return TYPE_UNQUAL(type_canon(TT, t));
}

static TypeKind ck_(Checker *c, TypeId t)
{
    return type_ckind(TT, t);
}

static bool is_err(Checker *c, TypeId t)
{
    return ck_(c, t) == TY_ERROR;
}

static bool is_arr(Checker *c, TypeId t)
{
    TypeKind k = ck_(c, t);
    return k == TY_ARRAY || k == TY_VLA;
}

static bool is_aggr(Checker *c, TypeId t)
{
    TypeKind k = ck_(c, t);
    return k == TY_ARRAY || k == TY_VLA || k == TY_STRUCT || k == TY_UNION;
}

static bool incomplete_arr(Checker *c, TypeId t)
{
    TypeId cn = type_canon(TT, t);
    return ck_(c, cn) == TY_ARRAY && (type_ent(TT, cn)->flags & TF_INCOMPLETE);
}

static bool is_varsize(Checker *c, TypeId t)
{
    TypeKind k = ck_(c, t);
    if (k == TY_VLA)
        return true;
    if (k == TY_ARRAY)
        return is_varsize(c, type_base(TT, type_canon(TT, t)));
    if (k == TY_STRUCT || k == TY_UNION) {      /* a VLA member (GNU) */
        const Record *r = type_record(TT, type_canon(TT, t));
        uint32_t f;
        if (r && (r->flags & RF_COMPLETE))
            for (f = 0; f < r->nfields; f++)
                if (is_varsize(c, TT->fields.data[r->fields + f].ty))
                    return true;
    }
    return false;
}

static bool compat(Checker *c, TypeId a, TypeId b)
{
    return type_compatible(TT, a, b);
}

static bool char_like(Checker *c, TypeId t)   /* t: main variant */
{
    return t == TYPE_B(CHAR) || t == TYPE_B(SCHAR) || t == TYPE_B(UCHAR);
}

static bool wide_like(Checker *c, TypeId t)
{
    return type_compatible(TT, t, TYPE_MK(c->tgt->wchar_type, 0)) ||
           type_compatible(TT, t, TYPE_MK(c->tgt->char16_type, 0)) ||
           type_compatible(TT, t, TYPE_MK(c->tgt->char32_type, 0));
}

static int lkind(Checker *c, TypeId t)
{
    switch (ck_(c, t)) {
    case TY_ERROR: return LV_ERR;
    case TY_STRUCT: return LV_REC;
    case TY_UNION: return LV_UNI;
    case TY_ARRAY: case TY_VLA: return LV_ARR;
    case TY_VECTOR: return LV_VEC;
    default: return LV_SCAL;
    }
}

/* ---- the context ------------------------------------------------------------ */

static CInit *ci_get(Checker *c)
{
    if (!c->ci)
        c->ci = xcalloc(1, sizeof *c->ci);
    return c->ci;
}

static void sp_push(CCtx *x, const char *s)
{
    x->sdepth++;
    if (x->sdepth + 2 > x->dlcap) {
        x->dlcap = x->dlcap ? x->dlcap * 2 : 16;
        while (x->sdepth + 2 > x->dlcap)
            x->dlcap *= 2;
        x->dl = xrealloc(x->dl, (size_t)x->dlcap * sizeof *x->dl);
    }
    sb_puts(&x->path, s);
    x->dl[x->sdepth] = x->path.len;
}

static void sp_restore(CCtx *x, int d)
{
    if (d < 0)
        d = 0;
    if (d > x->sdepth)
        return;
    x->sdepth = d;
    x->path.len = d ? x->dl[d] : 0;
    if (x->path.data)
        x->path.data[x->path.len] = 0;
}

static SrcLoc iloc_now(Checker *c, CCtx *x)
{
    uint32_t t;
    switch (x->lam) {
    case LA_AFTER: t = after_tok(c, x->lav); break;
    case LA_CLOSE: t = after_tok(c, x->lav) - 1; break;
    default: t = x->lav; break;
    }
    return cdecl_iloc(c, t);
}

static SrcLoc rloc(Checker *c, CCtx *x, uint32_t lt)
{
    if (lt == LT_IN && x)
        return iloc_now(c, x);
    return tloc(c, lt);
}

/* pedwarn_init / warning_init: a token spelled in a system header macro is
 * reported where the macro was expanded, or the warning would be dropped. */
bool cin_system(Checker *c, SrcLoc loc);
static SrcLoc wloc(Checker *c, CCtx *x, uint32_t lt)
{
    SrcLoc loc = rloc(c, x, lt);
    if (lt != LT_IN && lt < c->u->ntoks && cin_system(c, loc) &&
        c->u->toks[lt].exp)
        return c->u->toks[lt].exp;
    return loc;
}

static void la_set(CCtx *x, int mode, uint32_t v)
{
    x->lam = (uint8_t)mode;
    x->lav = v;
}

/* ---- diagnostics (gcc's error_init, pedwarn_init, warning_init) ------------------ */

static void near_note(Checker *c, CCtx *x, Diagnostic *d, SrcLoc loc)
{
    if (d && x && x->path.len)
        cnote(c, d, loc, "(near initialization for '%s')", sb_cstr(&x->path));
}

static void ierr(Checker *c, CCtx *x, uint32_t lt, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    SrcLoc loc = rloc(c, x, lt);
    Diagnostic *d;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    d = cerror_d(c, loc, "%s", buf);
    near_note(c, x, d, loc);
}

/* A pedwarn that is always on (id ""). */
static void iped(Checker *c, CCtx *x, uint32_t lt, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    SrcLoc loc = wloc(c, x, lt);
    Diagnostic *d;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    d = cpedwarn(c, loc, "", "%s", buf);
    near_note(c, x, d, loc);
}

/* A pedwarn under -Wpedantic. */
static void ipdt(Checker *c, CCtx *x, uint32_t lt, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    SrcLoc loc;
    Diagnostic *d;
    if (!c->opt.pedantic)
        return;
    loc = rloc(c, x, lt);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    d = cpedantic(c, loc, "%s", buf);
    near_note(c, x, d, loc);
}

/* A warning with option id (NULL / "": unconditional). */
static void iwarn(Checker *c, CCtx *x, uint32_t lt, const char *id,
                  const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    SrcLoc loc = wloc(c, x, lt);
    Diagnostic *d;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    d = cwarn_d(c, DL_WARNING, loc, id ? id : "", "%s", buf);
    near_note(c, x, d, loc);
}

/* ---- pending-element sets ----------------------------------------------------- */

static PSet *ps_new(CCtx *x)
{
    PSet *p = xcalloc(1, sizeof *p);
    p->next = x->psall;
    x->psall = p;
    return p;
}

static PEnt *ps_find(PSet *s, int64_t key)
{
    uint32_t lo = 0, hi;
    if (!s)
        return NULL;
    hi = s->n;
    while (lo < hi) {
        uint32_t m = lo + (hi - lo) / 2;
        if (s->e[m].key < key)
            lo = m + 1;
        else
            hi = m;
    }
    return lo < s->n && s->e[lo].key == key ? &s->e[lo] : NULL;
}

/* The slot for key (new slots are zeroed). */
static PEnt *ps_slot(CCtx *x, Lvl *L, int64_t key, bool *existed)
{
    PSet *s = L->ps ? L->ps : (L->ps = ps_new(x));
    uint32_t lo = 0, hi = s->n;
    *existed = false;
    if (s->n && s->e[s->n - 1].key < key)
        lo = s->n;
    else {
        while (lo < hi) {
            uint32_t m = lo + (hi - lo) / 2;
            if (s->e[m].key < key)
                lo = m + 1;
            else
                hi = m;
        }
        if (lo < s->n && s->e[lo].key == key) {
            *existed = true;
            return &s->e[lo];
        }
    }
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->e = xrealloc(s->e, (size_t)s->cap * sizeof *s->e);
    }
    if (lo < s->n)
        memmove(&s->e[lo + 1], &s->e[lo], (size_t)(s->n - lo) * sizeof *s->e);
    s->n++;
    memset(&s->e[lo], 0, sizeof s->e[lo]);
    s->e[lo].key = key;
    return &s->e[lo];
}

/* ---- levels --------------------------------------------------------------------- */

static const Field *lf(Checker *c, const Lvl *L, uint32_t k)
{
    return &TT->fields.data[L->fbase + k];
}

static bool unnamed_bf(const Field *f)
{
    return !f->name && (f->flags & FF_BITFIELD);
}

static uint32_t firstf(Checker *c, const Lvl *L)
{
    uint32_t k = 0;
    while (k < L->nf && unnamed_bf(lf(c, L, k)))
        k++;
    return k;
}

static uint32_t nextf(Checker *c, const Lvl *L, uint32_t k)
{
    k++;
    while (k < L->nf && unnamed_bf(lf(c, L, k)))
        k++;
    return k;
}

static Lvl *lvl_new(CInit *ci)
{
    Lvl *L = ci->freel;
    if (L)
        ci->freel = L->up;
    else
        L = xmalloc(sizeof *L);
    memset(L, 0, sizeof *L);
    return L;
}

static bool use_ps(const CCtx *x, const Lvl *L)
{
    return x->dm && (L->kind == LV_REC || L->kind == LV_UNI ||
                     L->kind == LV_ARR);
}

static uint64_t nelems(const CCtx *x, const Lvl *L)
{
    if (use_ps(x, L))
        return L->ps ? L->ps->n : 0;
    return L->nel;
}

/* The type's own state: gcc's constructor_type, _fields, _max_index. */
static void setup_kind(Checker *c, Lvl *N, TypeId t)
{
    N->type = t;
    N->kind = (uint8_t)lkind(c, t);
    N->maxidx = -1;
    N->lagmax = -1;
    switch (N->kind) {
    case LV_REC:
    case LV_UNI: {
        Record *r = type_record(TT, type_canon(TT, t));
        N->fbase = r ? r->fields : 0;
        N->nf = r ? r->nfields : 0;
        N->fi = N->uf = firstf(c, N);
        break;
    }
    case LV_ARR: {
        TypeId cn = type_canon(TT, t);
        const TypeEnt *e = type_ent(TT, cn);
        /* the array's own element type keeps a typedef name for messages */
        N->elem = type_base(TT, type_ent(TT, TYPE_UNQUAL(t))->kind == e->kind
                                    ? t : cn);
        if (e->kind == TY_VLA) {
            N->hasmax = true;
            N->maxidx = -1;
        } else if (e->flags & TF_INCOMPLETE)
            N->hasmax = false;
        else {
            N->hasmax = true;
            N->maxidx = (int64_t)e->n - 1;
        }
        N->varsize = is_varsize(c, t);
        break;
    }
    case LV_VEC: {
        TypeId cn = type_canon(TT, t);
        bool ok;
        uint64_t es;
        N->elem = type_base(TT, cn);
        es = type_size(TT, N->elem, &ok);
        N->hasmax = true;
        N->maxidx = ok && es ? (int64_t)(type_ent(TT, cn)->n / es) - 1 : -1;
        break;
    }
    default:
        break;
    }
}

/* gcc's set_nonincremental_init: elements already stored move to the
 * pending tree; here they are in the set already. */
static void nonincr(Checker *c, Lvl *L)
{
    if (L->kind != LV_REC && L->kind != LV_ARR)
        return;
    L->inc = false;
    if (L->kind == LV_REC)
        L->uf = firstf(c, L);
    else
        L->ui = 0;
}

/* Elements that became next in sequence (output_pending_init_elements). */
static void flush0(Checker *c, CCtx *x, Lvl *L)
{
    if (!use_ps(x, L) || !L->ps)
        return;
    if (L->kind == LV_ARR) {
        while (ps_find(L->ps, L->ui))
            L->ui++;
    } else if (L->kind == LV_REC) {
        while (L->uf < L->nf && ps_find(L->ps, L->uf))
            L->uf = nextf(c, L, L->uf);
    }
}

/* output_pending_init_elements (1): the gaps are skipped. */
static void flush_all(Checker *c, CCtx *x, Lvl *L)
{
    int64_t mk = -1;
    if (L->kind != LV_ARR && L->kind != LV_REC)
        return;
    if (use_ps(x, L)) {
        if (L->ps && L->ps->n)
            mk = L->ps->e[L->ps->n - 1].key;
    } else if (L->lag)
        mk = L->lagmax;
    if (L->kind == LV_ARR) {
        if (mk >= 0 && mk + 1 > L->ui)
            L->ui = mk + 1;
    } else if (mk >= 0 && mk >= (int64_t)L->uf)
        L->uf = nextf(c, L, (uint32_t)mk);
}

/* ---- the path (gcc's spelling stack) --------------------------------------------- */

static void push_member_name(Checker *c, CCtx *x, const Lvl *L, uint32_t k)
{
    char buf[512];
    uint32_t name = lf(c, L, k)->name;
    snprintf(buf, sizeof buf, ".%s", name ? cident(c, name) : "<anonymous>");
    sp_push(x, buf);
}

static void push_array_bounds(CCtx *x, int64_t i)
{
    char buf[40];
    snprintf(buf, sizeof buf, "[%" PRId64 "]", i);
    sp_push(x, buf);
}

/* ---- forward declarations --------------------------------------------------------- */

static void process_element(Checker *c, CCtx *x, uint32_t lt, IVal v,
                            bool implicit);
static IVal pop_level(Checker *c, CCtx *x, uint32_t lt, int implicit);
static void push_level(Checker *c, CCtx *x, uint32_t lt, int implicit);

/* ---- values ------------------------------------------------------------------------ */

static bool node_int_zero(Checker *c, uint32_t e)
{
    if (c->ck[e] == K_ICE || c->ck[e] == K_FOLD)
        return c->cv[e] == 0 && type_is_integer(TT, c->ty[e]);
    if (c->ck[e] == K_ADDR)
        return c->cb[e] == 0 && c->cv[e] == 0;
    return false;
}

static void ival_from(Checker *c, uint32_t e, IVal *v)
{
    uint32_t s = e;
    memset(v, 0, sizeof *v);
    v->node = e;
    if (is_err(c, c->ty[e])) {
        v->kind = V_ERR;
        return;
    }
    while (ntag(c, s) == N_PAREN && c->nodes[s].size > 1)
        s = s - 1;
    v->kind = V_EXPR;
    {
        uint32_t u = e;     /* __extension__ does not parenthesize */
        while (cexpr_is_extension(c, u) && first_child(c, u) != NO_NODE)
            u = first_child(c, u);
        v->strict = ntag(c, u) == N_STRING;
    }
    v->str = (c->ef[e] & EF_STRING) && is_arr(c, c->ty[e]);
    v->cl = ntag(c, s) == N_COMPOUND_LIT;
    v->side = (c->ef[e] & EF_SIDE) != 0;
    v->type = (v->str || v->cl) ? c->ty[e] : cexpr_rvalue_type(c, e);
    v->izero = node_int_zero(c, e);
    if (v->str)
        v->strn = type_ent(TT, type_canon(TT, c->ty[e]))->n;
}

static bool node_err(Checker *c, uint32_t n)
{
    return n != NOB && (is_err(c, c->ty[n]) || c->ck[n] == K_ERR);
}

/* gcc folds a read of a const object with a constant initializer (in_init
 * decl_constant_value): an expression built only from such reads and
 * constants is a constant. */
/* gcc folds calls of the library functions it knows as builtins (atan,
 * nan, ...) when the arguments are constant; an initializer made of them
 * is accepted with a pedwarn. */
static bool g_pedw;

static bool foldable_libcall(Checker *c, uint32_t n, int depth);

static bool is_icelike(Checker *c, uint32_t n)
{
    return c->ck[n] == K_ICE || c->ck[n] == K_FOLD;
}

/* An object pointer plus or minus integer constants: the variable's
 * symbol (gcc folds the difference of two such expressions). */
static uint32_t ptr_off_base(Checker *c, uint32_t n, int depth)
{
    uint32_t e2, e1, r;
    int op;
    if (depth > 20)
        return SYM_NONE;
    switch (ntag(c, n)) {
    case N_PAREN:
        return n > 0 ? ptr_off_base(c, n - 1, depth + 1) : SYM_NONE;
    case N_IDENT: {
        uint32_t ref = lookup_ord(c, cnode_ident(c, n));
        CSym *s;
        if (ref == SYM_NONE)
            return SYM_NONE;
        s = csym(c, ref);
        if (s->kind != CS_OBJ || is_err(c, s->ty) || ck_(c, s->ty) != TY_PTR ||
            (TYPE_QUALS(s->ty) & TQ_VOLATILE))
            return SYM_NONE;
        return ref;
    }
    case N_BINARY:
        op = tpunct(c, c->nodes[n].tok);
        e2 = n - 1;
        e1 = e2 - c->nodes[e2].size;
        if (op == P_PLUS || op == P_MINUS) {
            if (is_icelike(c, e2))
                return ptr_off_base(c, e1, depth + 1);
            if (op == P_PLUS && is_icelike(c, e1) &&
                (r = ptr_off_base(c, e2, depth + 1)) != SYM_NONE)
                return r;
        }
        return SYM_NONE;
    default:
        return SYM_NONE;
    }
}

/* A read of an element of a static const aggregate with a constant
 * initializer (`a[27]`, `e[1].c[4]`): gcc folds it to the stored value. */
static bool const_agg_read(Checker *c, uint32_t n, int depth)
{
    while (ntag(c, n) == N_PAREN && n > 0)
        n--;
    if (depth > 40)
        return false;
    switch (ntag(c, n)) {
    case N_IDENT: {
        uint32_t ref = lookup_ord(c, cnode_ident(c, n));
        CSym *sy;
        if (ref == SYM_NONE)
            return false;
        sy = csym(c, ref);
        return sy->kind == CS_OBJ && (sy->flags & CSF_CONST_INIT) &&
               !is_err(c, sy->ty) &&
               (is_arr(c, sy->ty) || is_aggr(c, sy->ty));
    }
    case N_INDEX: {
        uint32_t b2 = n - 1, b1 = b2 - c->nodes[b2].size;
        return is_icelike(c, b2) && !(c->ef[b2] & EF_OVERFLOW) &&
               const_agg_read(c, b1, depth + 1);
    }
    case N_MEMBER_EXPR:
        return !(c->nodes[n].flags & NF_ARROW) &&
               const_agg_read(c, n - 1, depth + 1);
    default:
        return false;
    }
}

static bool const_varlike(Checker *c, uint32_t n, int depth)
{
    uint32_t e2, e1;
    int op;
    if (depth > 40)
        return false;
    if (c->ck[n] == K_ICE || c->ck[n] == K_FOLD || c->ck[n] == K_FLOAT ||
        (c->ef[n] & EF_CPLXCST))
        return true;
    switch (ntag(c, n)) {
    case N_PAREN:
    case N_CAST:
        return n > 0 && const_varlike(c, n - 1, depth + 1);
    case N_NUMBER:              /* e.g. an imaginary constant */
        return true;
    case N_CALL:
        return foldable_libcall(c, n, depth);
    case N_IDENT: {
        uint32_t id = cnode_ident(c, n), ref = lookup_ord(c, id);
        CSym *s;
        TypeKind k;
        if (ref == SYM_NONE)
            return false;
        s = csym(c, ref);
        if (s->kind != CS_OBJ || !(s->flags & CSF_CONST_INIT) ||
            is_err(c, s->ty))
            return false;
        if ((TYPE_QUALS(s->ty) & (TQ_CONST | TQ_VOLATILE)) != TQ_CONST)
            return false;
        k = ck_(c, s->ty);
        return k != TY_ARRAY && k != TY_STRUCT && k != TY_UNION &&
               k != TY_VLA;
    }
    case N_COND: {          /* constant operands: gcc folds the choice */
        uint32_t f3 = n - 1, t2, c1;
        if (c->nodes[n].flags & NF_OMITTED)
            return false;
        t2 = f3 - c->nodes[f3].size;
        c1 = t2 - c->nodes[t2].size;
        return const_varlike(c, c1, depth + 1) &&
               const_varlike(c, t2, depth + 1) &&
               const_varlike(c, f3, depth + 1);
    }
    case N_INDEX: {         /* "str"[constant] */
        uint32_t b2 = n - 1, b1 = b2 - c->nodes[b2].size;
        uint32_t s, ix;
        int64_t v;
        while (ntag(c, b1) == N_PAREN && b1 > 0)
            b1--;
        if (ntag(c, b1) == N_STRING) {
            s = b1;
            ix = b2;
        } else if (ntag(c, b2) == N_STRING) {   /* 1["bar"] */
            s = b2;
            ix = b1;
        } else {
            return !is_aggr(c, c->ty[n]) && !is_arr(c, c->ty[n]) &&
                   const_agg_read(c, n, depth);
        }
        if (!is_icelike(c, ix) || (c->ef[ix] & EF_OVERFLOW))
            return false;
        v = cexpr_sval(c, ix);  /* gcc: only an index inside the string */
        return v >= 0 && (uint64_t)v < type_ent(TT, c->ty[s])->n;
    }
    case N_MEMBER_EXPR:
        return !is_aggr(c, c->ty[n]) && !is_arr(c, c->ty[n]) &&
               const_agg_read(c, n, depth);
    case N_UNARY:
        op = tpunct(c, c->nodes[n].tok);
        return (op == P_PLUS || op == P_MINUS || op == P_TILDE ||
                op == P_BANG) && const_varlike(c, n - 1, depth + 1);
    case N_BINARY:
        op = tpunct(c, c->nodes[n].tok);
        if (op == P_COMMA)
            return false;
        e2 = n - 1;
        e1 = e2 - c->nodes[e2].size;
        if (op == P_MINUS) {    /* &&a - &&b: a link-time constant */
            uint32_t a = e1, b = e2;
            while (ntag(c, a) == N_PAREN || ntag(c, a) == N_CAST)
                a--;
            while (ntag(c, b) == N_PAREN || ntag(c, b) == N_CAST)
                b--;
            if (ntag(c, a) == N_ADDR_LABEL && ntag(c, b) == N_ADDR_LABEL)
                return true;
        }
        if (op == P_MINUS) {    /* p + a - (p + b): the same variable */
            uint32_t ba = ptr_off_base(c, e1, 0);
            if (ba != SYM_NONE && ba == ptr_off_base(c, e2, 0))
                return true;
        }
        if ((op == P_SLASH || op == P_PERCENT) &&
            (c->ck[e2] == K_ICE || c->ck[e2] == K_FOLD) && c->cv[e2] == 0 &&
            (c->ck[e1] == K_ICE || c->ck[e1] == K_FOLD))
            return false;
        return const_varlike(c, e1, depth + 1) &&
               const_varlike(c, e2, depth + 1);
    default:
        return false;
    }
}

static bool foldable_libcall(Checker *c, uint32_t n, int depth)
{
    static const char *const fns[] = {"atan", "nan", "sin", "cos", "tan",
        "exp", "log", "sqrt", "fabs", "floor", "ceil", "pow", "fmod",
        "atan2", "asin", "acos", "sinh", "cosh", "tanh", "log10", "exp2",
        "cbrt", "trunc", "round", "copysign", "fmin", "fmax", "hypot"};
    uint32_t k[9], nk, j, id, ref;
    char name[32];
    size_t len, m;
    if (ntag(c, n) != N_CALL)
        return false;
    nk = node_children(c->nodes, n, k, 9);
    if (!nk || nk > 8 || ntag(c, k[0]) != N_IDENT)
        return false;
    id = cnode_ident(c, k[0]);
    snprintf(name, sizeof name, "%s", cident(c, id));
    if (!strncmp(name, "__builtin_", 10)) {
        /* gcc folds a __builtin_ call of constants to a true constant */
        static const char *const bfns[] = {"inf", "huge_val", "abs", "labs",
            "llabs", "strlen", "ffs", "clz", "ctz", "popcount", "parity"};
        const char *b = name + 10;
        size_t q, bl = strlen(b);
        for (q = 0; q < sizeof fns / sizeof *fns; q++)
            if (!strcmp(b, fns[q]) ||
                (bl > 1 && (b[bl - 1] == 'f' || b[bl - 1] == 'l') &&
                 !strncmp(b, fns[q], bl - 1) && !fns[q][bl - 1]))
                break;
        if (q == sizeof fns / sizeof *fns) {
            for (q = 0; q < sizeof bfns / sizeof *bfns; q++)
                if (!strncmp(b, bfns[q], strlen(bfns[q])) &&
                    (!b[strlen(bfns[q])] || !strcmp(b + strlen(bfns[q]), "f") ||
                     !strcmp(b + strlen(bfns[q]), "l")))
                    break;
            if (q == sizeof bfns / sizeof *bfns)
                return false;
        }
        for (j = 1; j < nk; j++)
            if (ntag(c, k[j]) != N_STRING && !const_varlike(c, k[j], depth + 1))
                return false;
        return true;
    }
    ref = lookup_ord(c, id);
    if (ref == SYM_NONE || csym(c, ref)->kind != CS_FUNC)
        return false;
    len = strlen(name);
    for (m = 0; m < sizeof fns / sizeof *fns; m++)
        if (!strcmp(name, fns[m]) ||
            (len > 1 && (name[len - 1] == 'f' || name[len - 1] == 'l') &&
             !strncmp(name, fns[m], len - 1) && !fns[m][len - 1]))
            break;
    if (m == sizeof fns / sizeof *fns)
        return false;
    for (j = 1; j < nk; j++)
        if (ntag(c, k[j]) != N_STRING && !const_varlike(c, k[j], depth + 1))
            return false;
    g_pedw = true;
    return true;
}

/* A cast to a variably modified type whose size expression has side
 * effects, a call or a comma (a volatile read does not count): gcc keeps it wrapped in a
 * C_MAYBE_CONST_EXPR (a pedwarn in an initializer, never an ICE). */
static bool cast_bound_side(Checker *c, uint32_t n)
{
    uint32_t k, op;
    while (ntag(c, n) == N_PAREN && n > 0)
        n--;
    if (ntag(c, n) != N_CAST || n == 0)
        return false;
    /* a typeof's side effects vanish when its type is not variably modified */
    if (!type_is_vm(TT, c->ty[n]))
        return false;
    op = n - 1;
    for (k = cfirst(c, n); k < cfirst(c, op); k++)
        switch (ntag(c, k)) {
        case N_CALL: case N_ASSIGN: case N_POSTFIX: case N_STMT_EXPR:
            return true;
        case N_UNARY:
            if (tpunct(c, c->nodes[k].tok) == P_INC ||
                tpunct(c, c->nodes[k].tok) == P_DEC)
                return true;
            break;
        case N_BINARY:
            if (tpunct(c, c->nodes[k].tok) == P_COMMA)
                return true;
            break;
        default:
            break;
        }
    return false;
}

/* 3: constant after gcc's folding of a library call (pedwarn); 2: a
 * constant gcc can emit; 1: constant but not computable at load time;
 * 0: not constant.  vt: the value's type after array decay. */
static int const_class(Checker *c, uint32_t n, TypeId vt, TypeId target);
static int const_class0(Checker *c, uint32_t n, TypeId vt, TypeId target)
{
    int cls;
    TypeKind tk, vk;
    {
        /* a comma expression stays a COMPOUND_EXPR: constant only when its
         * left operand is too */
        uint32_t s = n;
        while (ntag(c, s) == N_PAREN && s > 0)
            s--;
        while (ntag(c, s) == N_BINARY && tpunct(c, c->nodes[s].tok) == P_COMMA) {
            uint32_t e2 = s - 1, e1 = e2 - c->nodes[e2].size;
            if (c->ck[e1] == K_NONE || c->ck[e1] == K_ADDR)
                return 0;
            s = e2;
            while (ntag(c, s) == N_PAREN && s > 0)
                s--;
        }
    }
    switch (c->ck[n]) {
    case K_FOLD:        /* an integer expression ISO C does not call constant */
        {
            uint32_t s = n;
            while (ntag(c, s) == N_PAREN && s > 0)
                s--;
            if (ntag(c, s) == N_COND && !(c->nodes[s].flags & NF_OMITTED)) {
                /* only the chosen arm counts (the other is not evaluated) */
                uint32_t f3 = s - 1, t2 = f3 - c->nodes[f3].size;
                uint32_t c1 = t2 - c->nodes[t2].size;
                if (c->ck[c1] == K_ICE || c->ck[c1] == K_FOLD)
                    return const_class0(c, c->cv[c1] ? t2 : f3, vt, target);
            }
        }
        return (c->ef[n] & EF_INTOPS) && !(c->ef[n] & EF_CST) &&
               type_is_integer(TT, vt) ? 3 : 2;
    case K_ICE: case K_FLOAT: case K_ERR:
        return 2;
    case K_NONE:
        g_pedw = false;
        if (!const_varlike(c, n, 0))
            return 0;
        if (!g_pedw && type_is_integer(TT, vt) &&
            type_is_integer(TT, target)) {
            /* an address difference converted to a wider integer */
            bool ok1, ok2;
            uint64_t vs = type_size(TT, vt, &ok1);
            uint64_t ts = type_size(TT, target, &ok2);
            if (ok1 && ok2 && vs >= c->tgt->ptr_size && ts > vs)
                return 1;
        }
        return g_pedw ? 3 : 2;
    default:
        break;
    }
    if (c->cb[n] == 0)
        return 2;
    {
        /* an integer address value (a label difference) converted to a
         * wider integer cannot be emitted */
        uint32_t s = n;
        while (ntag(c, s) == N_PAREN && s > 0)
            s--;
        if (ntag(c, s) == N_CAST && s > 0 && c->ck[s - 1] == K_ADDR &&
            type_is_integer(TT, vt) && type_is_integer(TT, c->ty[s - 1])) {
            bool ok1, ok2;
            uint64_t a1 = type_size(TT, vt, &ok1);
            uint64_t a2 = type_size(TT, c->ty[s - 1], &ok2);
            if (ok1 && ok2 && a1 > a2)
                return 1;
        }
    }
    if (!(c->cb[n] & CB_NODE)) {
        /* a nested function's address needs a trampoline: never constant */
        const CSym *fs = csym(c, c->cb[n] - 1);
        if (((c->cb[n] - 1) & SYM_LOCAL) && fs->kind == CS_FUNC &&
            (fs->flags & CSF_DEFINED))
            return 0;
    }
    cls = 2;
    vk = ck_(c, vt);
    if (vk != TY_PTR) {
        bool ok;
        uint64_t sz = type_size(TT, vt, &ok);
        if (!(type_is_integer(TT, vt) && ok && sz >= c->tgt->ptr_size))
            cls = 1;
    }
    if (cls == 2) {
        bool ok;
        uint64_t sz;
        tk = ck_(c, target);
        if (tk == TY_PTR)
            return 2;
        if (tk == TY_BOOL)
            return 2;
        sz = type_size(TT, target, &ok);
        if (type_is_integer(TT, target)) {
            bool ok2;
            uint64_t vs = type_size(TT, vt, &ok2);
            if (ok && ok2 && vk != TY_PTR && sz > vs)
                return 1;       /* a wider integer than the address value */
            return ok && sz >= c->tgt->ptr_size ? 2 : 1;
        }
        return 0;
    }
    return cls;
}

static int const_class(Checker *c, uint32_t n, TypeId vt, TypeId target)
{
    int r = const_class0(c, n, vt, target);
    return r == 2 && cast_bound_side(c, n) ? 3 : r;
}

/* The value after conversion to target is an integer zero (null pointer). */
static bool post_zero(Checker *c, const IVal *v, TypeId target)
{
    TypeKind tk;
    uint32_t n = v->node;
    if (v->kind == V_ZERO)
        return true;
    if (v->kind != V_EXPR || v->str || v->cl || n == NOB)
        return false;
    tk = ck_(c, target);
    if (!(type_is_integer(TT, target) || tk == TY_PTR))
        return false;
    if (node_int_zero(c, n))
        return true;
    if (c->ck[n] == K_FLOAT)
        return c->fv.data[c->cv[n]] == 0;
    return false;
}

/* ---- digest_init --------------------------------------------------------------------- */

/* gcc's maybe_warn_string_init. */
static void maybe_warn_string(Checker *c, CCtx *x, uint32_t lt, TypeId type,
                              const IVal *v)
{
    if (c->opt.pedantic && is_arr(c, type) && v->str && !v->strict)
        ipdt(c, x, lt, "array initialized from parenthesized string constant");
}

/* Returns false after an error.  x is NULL for the top level of a
 * brace-less initializer (no spelling stack). */
/* set by the caller of digest while the target is a bit-field member */
static bool digest_bitfield;

static bool digest(Checker *c, CCtx *x, uint32_t lt, bool top, bool reqc,
                   TypeId type, IVal *v)
{
    TypeKind tk = ck_(c, type);
    TypeId vt = v->type;
    if (tk == TY_ERROR || v->kind == V_ERR || node_err(c, v->node))
        return false;
    if (v->node != NOB && !v->str)
        vt = cexpr_builtin_ptr_type(c, v->node, vt);
    if (v->kind == V_CTOR || v->kind == V_ZERO || v->digested)
        return true;

    if ((tk == TY_ARRAY || tk == TY_VLA) && v->str && !v->decayed) {
        TypeId cn = type_canon(TT, type);
        TypeId typ1 = mainv(c, type_base(TT, cn));
        bool char_array = char_like(c, typ1);
        bool wide = wide_like(c, typ1);
        if (char_array || wide) {
            TypeId typ2 = mainv(c, type_base(TT, type_canon(TT, vt)));
            bool incompat = false;
            if (!v->strict)
                maybe_warn_string(c, x, lt, type, v);
            if ((!top && incomplete_arr(c, type)) ||
                (tk == TY_ARRAY && !incomplete_arr(c, type) &&
                 type_ent(TT, cn)->n == 0))     /* a zero-length array too */
                ipdt(c, x, lt, "initialization of a flexible array member");
            if (compat(c, mainv(c, vt), mainv(c, type)))
                return true;
            if (char_array) {
                if (typ2 != TYPE_B(CHAR))
                    incompat = true;
            } else if (!compat(c, typ1, typ2))
                incompat = true;
            if (incompat) {
                char a[128];
                snprintf(a, sizeof a, "%s", type_q(TT, typ1));
                ierr(c, x, lt, "cannot initialize array of %s from a string "
                     "literal with type array of %s", a, type_q(TT, typ2));
                return false;
            }
            if (tk == TY_ARRAY && !incomplete_arr(c, type)) {
                uint64_t n = type_ent(TT, cn)->n;
                if (n + 1 < v->strn)
                    iped(c, x, lt, "initializer-string for array of %s is "
                         "too long", type_q(TT, typ1));
                else if (n < v->strn && cexpr_cxx_compat(c, v->node))
                    cwarn(c, rloc(c, x, lt), "c++-compat", "initializer-string "
                          "for array of %s is too long for C++",
                          type_q(TT, typ1));
            }
            return true;
        } else if (type_is_integer(TT, typ1)) {
            ierr(c, x, lt, "array of inappropriate type initialized from "
                 "string constant");
            return false;
        }
    }

    /* any type can be initialized from an expression of the same type */
    if ((compat(c, mainv(c, vt), mainv(c, type)) &&
         !(tk == TY_PTR && cexpr_cxx_compat(c, v->node) &&
           cexpr_enum_int_mix(c, vt, type, 0))) ||
        (tk == TY_ARRAY && compat(c, vt, type)) ||
        (tk == TY_VECTOR && compat(c, vt, type)) ||
        (tk == TY_PTR && is_arr(c, vt) &&
         compat(c, type_base(TT, type_canon(TT, vt)),
                type_base(TT, type_canon(TT, type))))) {
        if (tk == TY_PTR && v->node != NOB)
            cexpr_packed_check(c, v->node, type);
        if (tk == TY_PTR && is_arr(c, vt)) {
            /* string and compound literal decay */
            v->decayed = true;
            vt = v->type = type_ptr(TT, type_base(TT, type_canon(TT, vt)));
        }
        if (reqc && v->cl && !v->decayed) {
            if (tk != TY_VECTOR)
                ipdt(c, x, lt, "initializer element is not constant");
            return true;
        }
        if (tk == TY_ARRAY && !v->str) {
            ierr(c, x, lt, "array initialized from non-constant array "
                 "expression");
            return false;
        }
        if (reqc && v->node != NOB) {
            int cc = const_class(c, v->node, vt, type);
            if (cc == 3) {
                ipdt(c, x, lt, "initializer element is not a constant "
                     "expression");
            } else if (cc != 2) {
                ierr(c, x, lt, "initializer element is not constant");
                return false;
            }
        }
        return true;
    }

    /* scalars, including conversions */
    if (tk != TY_STRUCT && tk != TY_UNION && tk != TY_ARRAY && tk != TY_VLA &&
        tk != TY_FUNC && tk != TY_VOID) {
        ConvInfo ci;
        char pbuf[1];
        TypeId at = vt;
        (void)pbuf;
        if (is_arr(c, vt) && (v->str || v->cl))
            at = type_ptr(TT, type_base(TT, type_canon(TT, vt)));
        memset(&ci, 0, sizeof ci);
        ci.context = CONV_INIT;
        ci.loc = rloc(c, x, lt);
        ci.lhs_bitfield = digest_bitfield;
        ci.near = x && x->path.len ? sb_cstr(&x->path) : NULL;
        if (!cexpr_assign_check(c, v->node, type, &ci))
            return false;
        if (reqc && v->node != NOB) {
            int cls = const_class(c, v->node, at, type);
            if (cls == 0) {
                ierr(c, x, lt, "initializer element is not constant");
                return false;
            }
            if (cls == 3)
                ipdt(c, x, lt, "initializer element is not a constant "
                     "expression");
            if (cls == 1) {
                ierr(c, x, lt, "initializer element is not computable at "
                     "load time");
                return false;
            }
        }
        return true;
    }

    if (tk != TY_ERROR && is_varsize(c, type)) {
        ierr(c, x, lt, "variable-sized object may not be initialized except "
             "with an empty initializer");
        if (x)
            x->varerr = true;
    }
    else
        ierr(c, x, lt, "invalid initializer");
    return false;
}

/* ---- output_init_element -------------------------------------------------------------- */

static void override_warn(Checker *c, CCtx *x, uint32_t lt, bool side)
{
    if (side)
        iwarn(c, x, lt, "override-init-side-effects",
              "initialized field with side-effects overwritten");
    else
        iwarn(c, x, lt, "override-init", "initialized field overwritten");
}

static void fill_ent(PEnt *e, const IVal *v, bool zero, uint64_t strn)
{
    e->side = v->side;
    e->zero = zero;
    if (v->kind == V_CTOR) {
        e->vk = PV_CTOR;
        e->sub = v->ps;
    } else if (strn) {
        e->vk = PV_STR;
        e->strn = strn;
        e->sub = NULL;
    } else {
        e->vk = PV_SCAL;
        e->sub = NULL;
    }
}

static void add_pending(Checker *c, CCtx *x, Lvl *L, uint32_t lt, int64_t key,
                        const IVal *v, bool zero, uint64_t strn, bool implicit)
{
    if (use_ps(x, L)) {
        bool ex;
        PEnt *e = ps_slot(x, L, key, &ex);
        if (ex && !implicit)
            override_warn(c, x, lt, e->side);
        fill_ent(e, v, zero, strn);
    } else {
        L->lag = true;
        if (key > L->lagmax)
            L->lagmax = key;
        L->nel++;
        if (L->nel == 1)
            L->onlyzero = zero;
    }
    if (v->side)
        L->anyside = true;
    /* an out-of-order string for a flexible array is digested again when the
     * pending elements are output, at the declarator */
    if (v->str && L->kind == LV_REC && key >= 0 && (uint32_t)key < L->nf &&
        incomplete_arr(c, lf(c, L, (uint32_t)key)->ty))
        L->fpend = true;
}

/* -Woverflow for a constant stored into a bit-field: gcc converts to the
 * bit-field's own (narrower) type, which prints as 'unsigned char:3'. */
static void bitfield_overflow(Checker *c, const Lvl *L, const IVal *v,
                              TypeId ft)
{
    const Field *f;
    TypeId rt;
    uint32_t n = v->node;
    if ((L->kind != LV_REC && L->kind != LV_UNI) || L->fi >= L->nf ||
        n == NOB || v->kind != V_EXPR || v->str || v->cl)
        return;
    f = lf(c, L, L->fi);
    if (!(f->flags & FF_BITFIELD) || !f->width || f->width >= 64)
        return;
    if (!type_is_integer(TT, ft) || ck_(c, ft) == TY_BOOL)
        return;
    if (c->ck[n] != K_ICE && c->ck[n] != K_FOLD)
        return;
    if (c->ef[n] & EF_OVERFLOW)
        return;
    rt = v->type;
    cexpr_bf_overflow(c, cnode_loc(c, n), n, ft, rt, f->width);
}

/* gcc's constant_expression_warning: the constant (after conversion to
 * type) carries TREE_OVERFLOW. */
static bool const_overflowed(Checker *c, uint32_t n, TypeId type)
{
    if (n == NOB)
        return false;
    if ((c->ck[n] == K_ICE || c->ck[n] == K_FOLD) && (c->ef[n] & EF_OVERFLOW))
        return true;
    if (c->ck[n] == K_FLOAT && type_is_integer(TT, type) &&
        ck_(c, type) != TY_BOOL) {
        unsigned bits = type_int_bits(TT, type);
        long double f = c->fv.data[c->cv[n]], lo, hi;
        if (bits == 0 || bits > 64)
            return false;
        if (f != f)
            return true;
        if (type_is_signed(TT, type)) {
            hi = (long double)(1ULL << (bits - 1));
            lo = -hi;
            return f >= hi || f <= lo - 1;
        }
        hi = bits == 64 ? 18446744073709551616.0L : (long double)(1ULL << bits);
        return f >= hi || f <= -1;
    }
    return false;
}

static void out_elem(Checker *c, CCtx *x, uint32_t lt, IVal v, TypeId type,
                     int64_t key, bool implicit)
{
    Lvl *L = x->stk;
    int k = L->kind;
    bool zero, rq = x->reqc, des = L->eldes;
    uint64_t strn = 0;

    L->eldes = false;
    if (x->dwpath && !implicit) {
        /* gcc reports a positional element of an elided-brace level at the
         * next element it reads (input_location), with that level's path */
        Diagnostic *d = cwarn_d(c, DL_WARNING, wloc(c, x, lt), "designated-init",
                                "positional initialization of field in "
                                "'struct' declared with 'designated_init' "
                                "attribute");
        if (d && *x->dwpath)
            cnote(c, d, rloc(c, x, lt), "(near initialization for '%s')",
                  x->dwpath);
        free(x->dwpath);
        x->dwpath = NULL;
    }
    if (is_err(c, type) || v.kind == V_ERR) {
        L->erroneous = true;
        return;
    }
    if (k == LV_REC && !des && ck_(c, L->type) == TY_STRUCT &&
        (type_record(TT, type_canon(TT, L->type))->flags & RF_DESIGNATED)) {
        char *pp = xstrdup(sb_cstr(&x->path));
        if (strrchr(pp, '.'))
            *strrchr(pp, '.') = 0;     /* gcc names the struct, not the field */
        if (L->implicit) {
            x->dwpath = pp;
            pp = NULL;
        } else {
            Diagnostic *d = cwarn_d(c, DL_WARNING, wloc(c, x, lt),
                                    "designated-init", "positional "
                                    "initialization of field in 'struct' "
                                    "declared with 'designated_init' attribute");
            if (d && *pp)
                cnote(c, d, wloc(c, x, lt),
                      "(near initialization for '%s')", pp);
        }
        free(pp);
    }
    if (v.kind == V_EXPR && !v.digested && (v.str || v.cl) && is_arr(c, v.type)
        && !(v.str && is_arr(c, type) &&
             type_is_integer(TT, type_base(TT, type_canon(TT, type)))) &&
        !compat(c, mainv(c, v.type), mainv(c, type))) {
        v.type = type_ptr(TT, type_base(TT, type_canon(TT, v.type)));
        v.decayed = true;
    }
    if (v.kind == V_EXPR && v.cl && !v.decayed && x->reqc) {
        ipdt(c, x, lt, "initializer element is not constant");
        if (is_aggr(c, v.type) && !compat(c, mainv(c, v.type), mainv(c, type))) {
            /* the list's CONSTRUCTOR still has the literal's type: digest
             * reports the mismatch */
            v.cl = false;
            rq = false;
        } else {
            v.kind = V_CTOR;
            v.ps = NULL;
            v.izero = false;
        }
    }
    bool pre_zero = v.kind == V_EXPR && v.node != NOB &&
                    (v.izero || (c->ck[v.node] == K_FLOAT &&
                                 c->fv.data[c->cv[v.node]] == 0));
    if (v.kind == V_EXPR && !v.digested) {
        bool bf = false, had = false, ok;
        uint32_t bn = v.node;
        if (bn != NOB && (k == LV_REC || k == LV_UNI) && L->fi < L->nf &&
            (lf(c, L, L->fi)->flags & FF_BITFIELD)) {
            had = (c->ef[bn] & EF_OVERFLOW) != 0;
            bf = true;
            c->ef[bn] |= EF_OVERFLOW;   /* the bit-field type reports it */
        }
        digest_bitfield = (k == LV_REC || k == LV_UNI) && L->fi < L->nf &&
                          (lf(c, L, L->fi)->flags & FF_BITFIELD);
        ok = digest(c, x, lt, false, rq, type, &v);
        digest_bitfield = false;
        if (ok && rq && k == LV_REC && v.node != NOB &&
            (type_record(TT, type_canon(TT, L->type))->flags & RF_SSO) &&
            ck_(c, type) == TY_PTR && c->ck[v.node] == K_ADDR) {
            /* a reverse-order pointer cannot be an address constant */
            ierr(c, NULL, x->init_loc, "initializer element is not constant");
            ok = false;
        }
        if (!ok) {
            if (bf && !had)
                c->ef[bn] &= ~(uint64_t)EF_OVERFLOW;
            L->erroneous = true;
            return;
        }
        if (bf && !had) {
            c->ef[bn] &= ~(uint64_t)EF_OVERFLOW;
            bitfield_overflow(c, L, &v, type);
        }
        if (rq && const_overflowed(c, v.node, type))
            cconst_overflow(c, cinput_loc(c, last_tok(c, v.node) + 1));
        zero = post_zero(c, &v, type);
        if (v.str && !v.decayed && is_arr(c, type)) {
            strn = v.strn;
            if (ck_(c, type) == TY_ARRAY && !incomplete_arr(c, type)) {
                uint64_t n = type_ent(TT, type_canon(TT, type))->n;
                if (n < strn)
                    strn = n;
            }
        }
    } else
        zero = v.kind == V_ZERO ? true : (v.kind == V_EXPR ? v.izero : false);

    /* an empty field that is not the last one needs nothing stored */
    if ((k == LV_REC || k == LV_UNI) && !v.side && L->fi + 1 < L->nf) {
        bool ok;
        uint64_t sz = type_size(TT, lf(c, L, L->fi)->ty, &ok);
        if (ok && sz == 0)
            return;
    }

    if (k == LV_ARR && (!L->inc || key != L->ui)) {
        if (L->inc && key < L->ui)
            nonincr(c, L);
        add_pending(c, x, L, lt, key, &v, zero, strn, implicit);
        return;
    } else if (k == LV_REC && (!L->inc || (uint32_t)key != L->uf)) {
        if (L->inc) {
            if (L->uf >= L->nf || key < (int64_t)L->uf)
                nonincr(c, L);
        }
        add_pending(c, x, L, lt, key, &v, zero, strn, implicit);
        return;
    } else if (k == LV_UNI && nelems(x, L) > 0) {
        if (!implicit)
            override_warn(c, x, lt, L->lastside);
        if (use_ps(x, L)) {
            if (L->ps)
                L->ps->n = 0;
        } else
            L->nel = 0;
    }

    /* process_init_element: a union's nonzero, undesignated initializer */
    if (k == LV_UNI && x->trad_loc && !des && (v.kind == V_CTOR || !zero) &&
        !pre_zero &&
        !cin_system(c, x->trad_loc))
        cwarn(c, x->trad_loc, "traditional", "traditional C rejects "
              "initialization of unions");
    /* output the element */
    if (use_ps(x, L)) {
        bool ex;
        PEnt *e = ps_slot(x, L, key, &ex);
        fill_ent(e, &v, zero, strn);
    } else {
        L->nel++;
        if (L->nel == 1)
            L->onlyzero = zero;
        if (k == LV_SCAL) {
            L->e0 = v;
            L->e0.kind = V_EXPR;
            L->e0.digested = true;
            L->e0.izero = zero;
            L->e0.type = L->type;
        }
    }
    L->lastside = v.side;
    if (v.side)
        L->anyside = true;
    if (k == LV_ARR)
        L->ui++;
    else if (k == LV_REC)
        L->uf = nextf(c, L, L->uf);
    else if (k == LV_UNI)
        L->uf = L->nf;
    flush0(c, x, L);
}

/* ---- push_init_level / pop_init_level ----------------------------------------------------- */

static PEnt *find_init_member(Checker *c, CCtx *x, Lvl *P)
{
    if (P->kind == LV_ARR) {
        if (P->inc && P->idx < P->ui)
            nonincr(c, P);
        return x->dm ? ps_find(P->ps, P->idx) : NULL;
    }
    if (P->kind == LV_REC) {
        if (P->inc && (P->uf >= P->nf || (uint32_t)P->fi < P->uf))
            nonincr(c, P);
        return x->dm ? ps_find(P->ps, P->fi) : NULL;
    }
    if (P->kind == LV_UNI && x->dm && P->ps && P->ps->n &&
        P->ps->e[P->ps->n - 1].key == (int64_t)P->fi)
        return &P->ps->e[P->ps->n - 1];
    return NULL;
}

static void push_level(Checker *c, CCtx *x, uint32_t lt, int implicit)
{
    CInit *ci = c->ci;
    Lvl *P = x->stk, *N;
    PEnt *value = NULL;
    TypeId t = P->type;
    bool comp = false;
    int before = x->sdepth;

    if (implicit) {
        if ((P->kind == LV_REC || P->kind == LV_UNI) && P->fi < P->nf)
            value = find_init_member(c, x, P);
        else if (P->kind == LV_ARR)
            value = find_init_member(c, x, P);
    }

    N = lvl_new(ci);
    N->up = P;
    N->implicit = implicit != 0;
    N->inc = true;
    N->designated = P->designated;
    N->dd_saved = x->dd;
    N->rs_saved = NULL;
    if (!implicit) {
        N->rs_saved = x->rs;
        x->rs = NULL;
        x->dd = 0;
        x->derr = 0;
    }
    N->depth = before;
    x->stk = N;

    /* the new level's type and path */
    N->kind = LV_SCAL;
    if (P->kind == LV_NULL)
        t = P->type, N->kind = LV_NULL;
    else if (P->kind == LV_REC || P->kind == LV_UNI) {
        if (P->fi >= P->nf)
            N->kind = LV_NULL;
        else {
            t = lf(c, P, P->fi)->ty;
            push_member_name(c, x, P, P->fi);
            comp = true;
        }
    } else if (P->kind == LV_ARR) {
        t = P->elem;
        push_array_bounds(x, P->idx);
        comp = true;
    }
    if (comp)
        N->depth = before + 1;

    if (P->kind == LV_NULL || N->kind == LV_NULL) {
        N->kind = LV_NULL;
        N->type = t;
        N->fbase = N->nf = N->fi = N->uf = 0;
        N->maxidx = -1;
        N->lagmax = -1;
        ierr(c, x, lt, "extra brace group at end of initializer");
        return;
    }

    if (P->kind == LV_ERR)
        t = P->type;
    setup_kind(c, N, t);

    if (value && value->vk == PV_CTOR) {
        N->ps = value->sub;
        if (N->ps && N->ps->n && (N->kind == LV_REC || N->kind == LV_ARR))
            nonincr(c, N);
    }
    if (implicit == 1)
        c->ci->found_mb = true;
    if (value && value->vk == PV_STR && N->kind == LV_ARR) {
        uint64_t n = value->strn, k;
        if (N->hasmax && (uint64_t)(N->maxidx + 1) < n)
            n = (uint64_t)(N->maxidx + 1);
        if (n) {
            N->ps = ps_new(x);
            N->ps->e = xmalloc((size_t)n * sizeof *N->ps->e);
            N->ps->cap = (uint32_t)n;
            for (k = 0; k < n; k++) {
                PEnt *e = &N->ps->e[k];
                memset(e, 0, sizeof *e);
                e->key = (int64_t)k;
                e->zero = k + 1 == value->strn;
            }
            N->ps->n = (uint32_t)n;
        }
        N->inc = false;
    }
    if (N->kind == LV_SCAL) {
        if (!is_err(c, t))
            iwarn(c, x, LT_IN, "", "braces around scalar initializer");
    }
    if (N->kind == LV_ARR && P->kind != LV_ARR && P->kind != LV_NULL &&
        (P->kind == LV_REC || P->kind == LV_UNI) && incomplete_arr(c, t)) {
        N->flex = true;
        N->fhn = P->fi + 1 < P->nf;
    }
}

static IVal pop_level(Checker *c, CCtx *x, uint32_t lt, int implicit)
{
    CInit *ci = c->ci;
    Lvl *L, *up;
    IVal ret;
    uint64_t nel;
    memset(&ret, 0, sizeof ret);
    ret.node = NOB;

    if (implicit == 0) {
        while (x->stk->implicit) {
            IVal r = pop_level(c, x, lt, 1);
            process_element(c, x, LT_IN, r, true);
        }
    }
    L = x->stk;
    flush_all(c, x, L);
    if (L->fpend && x->decl_loc && c->opt.pedantic) {
        Diagnostic *d = cpedantic(c, x->decl_loc,
                                  "initialization of a flexible array member");
        near_note(c, x, d, x->decl_loc);
    }

    /* a flexible array member in an inappropriate context */
    if (L->kind == LV_ARR && L->flex) {
        if (L->ui == 0)
            L->kind = LV_NULL;
        else {
            if (L->depth > 2)
                ierr(c, x, lt, "initialization of flexible array member in a "
                     "nested context");
            else
                ipdt(c, x, lt, "initialization of a flexible array member");
            if (L->fhn)
                L->kind = LV_NULL;
        }
    }

    nel = nelems(x, L);
    if (nel == 0)
        ci->zeroinit = true;
    else if (nel == 1) {
        bool z = use_ps(x, L) ? (L->ps && L->ps->e[0].zero) : L->onlyzero;
        if (z)
            ci->zeroinit = true;
    } else
        ci->zeroinit = false;

    if (!implicit && ci->found_mb && !ci->zeroinit)
        iwarn(c, NULL, x->init_loc, "missing-braces",
              "missing braces around initializer");

    if (L->kind == LV_REC) {
        while (L->uf < L->nf) {
            bool ok;
            uint64_t sz = type_size(TT, lf(c, L, L->uf)->ty, &ok);
            if (!ok || sz == 0)
                L->uf++;
            else
                break;
        }
        if (L->uf < L->nf && !L->designated && !ci->zeroinit) {
            const Field *f = lf(c, L, L->uf);
            Diagnostic *d = cwarn_d(c, DL_WARNING, iloc_now(c, x),
                                    "missing-field-initializers",
                                    "missing initializer for field '%s' of %s",
                                    f->name ? cident(c, f->name) : "({anonymous})",
                                    type_q(TT, L->type));
            if (d)
                cnote(c, d, f->loc, "'%s' declared here",
                      f->name ? cident(c, f->name) : "({anonymous})");
        }
    }

    if (L->has_repl)
        ret = L->repl;
    else if (L->kind == LV_NULL)
        ret.kind = V_NONE;
    else if (L->kind == LV_ERR)
        ret.kind = V_ERR;
    else if (L->kind == LV_SCAL) {
        if (L->nel == 0) {
            if (L->erroneous || is_err(c, L->type))
                ret.kind = V_ERR;
            else if (ck_(c, L->type) == TY_FUNC) {
                ierr(c, x, lt, "invalid initializer");
                ret.kind = V_ERR;
            } else {
                ret.kind = V_ZERO;
                ret.izero = true;
                ret.type = L->type;
                ret.digested = true;
            }
        } else {
            if (L->nel != 1)
                ierr(c, x, lt, "extra elements in scalar initializer");
            ret = L->e0;
        }
    } else {
        if (L->erroneous)
            ret.kind = V_ERR;
        else {
            ret.kind = V_CTOR;
            ret.type = L->type;
            ret.ps = L->ps;
            ret.side = L->anyside;
        }
    }
    x->rtop = L->ui;

    x->dd = L->dd_saved;
    if (!implicit)
        x->rs = L->rs_saved;
    up = L->up;
    x->stk = up;
    sp_restore(x, up ? up->depth : 0);
    L->up = ci->freel;
    ci->freel = L;
    if (ret.kind == V_NONE && !up)
        ret.kind = V_ERR;
    return ret;
}

static void finish_implicit_inits(Checker *c, CCtx *x, uint32_t lt)
{
    for (;;) {
        Lvl *L = x->stk;
        if (!L->implicit)
            break;
        if (((L->kind == LV_REC || L->kind == LV_UNI) && L->fi >= L->nf) ||
            (L->kind == LV_ARR && L->hasmax && L->maxidx < L->idx)) {
            IVal r = pop_level(c, x, lt, 1);
            process_element(c, x, LT_IN, r, true);
        } else
            break;
    }
}

/* ---- process_init_element ------------------------------------------------------------------- */

static bool elementwise_p(Checker *c, TypeId ft, const IVal *v)
{
    TypeKind tk = ck_(c, ft);
    if (tk == TY_ERROR || v->kind == V_ERR || is_err(c, v->type))
        return false;
    if (tk == TY_VECTOR)
        return ck_(c, v->type) != TY_VECTOR;
    if (is_aggr(c, ft))
        return mainv(c, ft) != mainv(c, v->type);
    return false;
}

static RS *rs_new(CCtx *x)
{
    RS *p = xcalloc(1, sizeof *p);
    p->alloc_next = x->rsall;
    x->rsall = p;
    return p;
}

static void push_range_stack(CCtx *x, bool has_end, int64_t end)
{
    Lvl *L = x->stk;
    RS *p = rs_new(x);
    p->prev = x->rs;
    p->fi = L->fi;
    p->start = p->index = L->idx;
    p->stack = L;
    p->has_end = has_end;
    p->end = end;
    if (x->rs)
        x->rs->next = p;
    x->rs = p;
}

enum { ST_PUSHED, ST_BREAK, ST_DONE };

static int element_step(Checker *c, CCtx *x, uint32_t lt, IVal *v,
                        bool implicit, bool string_flag)
{
    Lvl *L = x->stk;
    bool has = v->kind != V_NONE;
    TypeId ft;
    switch (L->kind) {
    case LV_REC:
    case LV_UNI: {
        bool rec = L->kind == LV_REC;
        const Field *f;
        if (L->fi >= L->nf) {
            iped(c, x, lt, rec ? "excess elements in struct initializer"
                               : "excess elements in union initializer");
            return ST_BREAK;
        }
        f = lf(c, L, L->fi);
        ft = is_err(c, f->ty) ? f->ty : mainv(c, f->ty);
        if (rec && incomplete_arr(c, ft) && !x->reqc && L->fi + 1 == L->nf) {
            ierr(c, x, lt, "non-static initialization of a flexible array "
                 "member");
            return ST_BREAK;
        }
        if (rec && string_flag && incomplete_arr(c, ft) && L->depth > 1 &&
            L->fi + 1 == L->nf) {
            Lvl *a;
            bool in_array = false;
            for (a = L->up; a && a->kind != LV_NULL; a = a->up)
                if (a->kind == LV_ARR) {
                    in_array = true;
                    break;
                }
            if (in_array) {
                ierr(c, x, lt, "initialization of flexible array member in a "
                     "nested context");
                return ST_BREAK;
            }
        }
        if (has && is_arr(c, ft) && string_flag &&
            type_is_integer(TT, type_base(TT, type_canon(TT, ft))))
            ;
        else if (has && elementwise_p(c, ft, v)) {
            push_level(c, x, lt, 1);
            return ST_PUSHED;
        }
        if (has) {
            push_member_name(c, x, L, L->fi);
            out_elem(c, x, lt, *v, ft, L->fi, implicit);
            sp_restore(x, L->depth);
        } else if (rec && L->uf == L->fi)
            L->uf = nextf(c, L, L->fi);
        if (rec)
            L->fi = nextf(c, L, L->fi);
        else {
            if (!has)
                L->uf = L->nf;
            L->fi = L->nf;
        }
        return ST_DONE;
    }
    case LV_ARR: {
        TypeId et = mainv(c, L->elem);
        if (has && is_arr(c, et) && string_flag &&
            type_is_integer(TT, type_base(TT, type_canon(TT, et))))
            ;
        else if (has && elementwise_p(c, et, v)) {
            push_level(c, x, lt, 1);
            return ST_PUSHED;
        }
        if (L->hasmax && (L->maxidx < L->idx || L->maxidx == -1)) {
            if (!x->varerr)
                iped(c, x, lt, "excess elements in array initializer");
            return ST_BREAK;
        }
        if (has) {
            push_array_bounds(x, L->idx);
            out_elem(c, x, lt, *v, et, L->idx, implicit);
            sp_restore(x, L->depth);
        }
        L->idx++;
        if (!has)
            L->ui = L->idx;
        return ST_DONE;
    }
    case LV_VEC: {
        TypeId et = mainv(c, L->elem);
        if (L->maxidx < L->idx) {
            iped(c, x, lt, "excess elements in vector initializer");
            return ST_BREAK;
        }
        if (has)
            out_elem(c, x, lt, *v, et, L->idx, implicit);
        L->idx++;
        if (!has)
            L->ui = L->idx;
        return ST_DONE;
    }
    default:
        if (L->fi == 1) {
            iped(c, x, lt, "excess elements in scalar initializer");
            return ST_BREAK;
        }
        if (has)
            out_elem(c, x, lt, *v, L->type, 0, implicit);
        L->fi = 1;
        return ST_DONE;
    }
}

static void process_element(Checker *c, CCtx *x, uint32_t lt, IVal v,
                            bool implicit)
{
    CInit *ci = c->ci;
    Lvl *L;
    bool was_des = x->dd != 0;
    bool string_flag = v.kind == V_EXPR && v.str;

    x->dd = 0;
    x->derr = 0;
    if (!implicit && v.kind != V_NONE && !v.izero)
        ci->zeroinit = false;

    L = x->stk;
    if (string_flag && L->kind == LV_ARR && !was_des &&
        type_is_integer(TT, type_base(TT, type_canon(TT, L->type))) &&
        L->ui == 0) {
        if (L->has_repl)
            ierr(c, x, lt, "excess elements in 'char' array initializer");
        L->repl = v;
        L->has_repl = true;
        return;
    }
    if (L->has_repl) {
        ierr(c, x, lt, "excess elements in struct initializer");
        return;
    }
    if (L->kind == LV_NULL || L->kind == LV_ERR)
        return;
    if (L->varsize)
        return;

    /* exhausted levels that had no braces */
    for (;;) {
        L = x->stk;
        if (!L->implicit)
            break;
        if (((L->kind == LV_REC || L->kind == LV_UNI) && L->fi >= L->nf) ||
            ((L->kind == LV_ARR || L->kind == LV_VEC) && L->hasmax &&
             L->maxidx < L->idx)) {
            IVal r = pop_level(c, x, lt, 1);
            process_element(c, x, lt, r, true);
        } else
            break;
    }

    for (;;) {
        int st = element_step(c, x, lt, &v, implicit, string_flag);
        if (st == ST_PUSHED)
            continue;
        if (st == ST_BREAK)
            break;
        /* range initializers, at this level or higher in the stack */
        if (x->rs) {
            RS *p, *range_stack = x->rs;
            bool finish = false;
            x->rs = NULL;
            while (x->stk != range_stack->stack) {
                IVal r = pop_level(c, x, lt, 1);
                process_element(c, x, lt, r, true);
            }
            for (p = range_stack; !p->has_end || p->index == p->end;
                 p = p->prev) {
                IVal r = pop_level(c, x, lt, 1);
                process_element(c, x, lt, r, true);
            }
            p->index++;
            if (p->index == p->end && !p->prev)
                finish = true;
            for (;;) {
                Lvl *T = x->stk;
                T->idx = p->index;
                T->fi = p->fi;
                if (finish && p->has_end && p->index == p->start) {
                    finish = false;
                    p->prev = NULL;
                }
                p = p->next;
                if (!p)
                    break;
                finish_implicit_inits(c, x, lt);
                push_level(c, x, lt, 2);
                p->stack = x->stk;
                if (p->has_end && p->index == p->end)
                    p->index = p->start;
            }
            if (!finish)
                x->rs = range_stack;
            continue;
        }
        break;
    }
    x->rs = NULL;
}

/* ---- designators -------------------------------------------------------------------------------- */

/* Returns true if the designator cannot be applied (an error was given or
 * the level is unusable). */
static bool set_designator(Checker *c, CCtx *x, uint32_t lt, bool array)
{
    Lvl *L = x->stk;
    TypeId sub;
    TypeKind sk;
    if (L->kind == LV_NULL || L->kind == LV_ERR)
        return true;
    if (x->derr)
        return true;
    if (L->varsize)
        return true;
    if (!x->dd) {
        while (x->stk->implicit) {
            IVal r = pop_level(c, x, lt, 1);
            process_element(c, x, LT_IN, r, true);
        }
        x->stk->designated = true;
        x->stk->eldes = true;
        return false;
    }
    if (L->kind == LV_REC || L->kind == LV_UNI) {
        if (L->fi >= L->nf)
            return true;
        sub = lf(c, L, L->fi)->ty;
    } else
        sub = L->elem;
    sub = is_err(c, sub) ? sub : mainv(c, sub);
    sk = ck_(c, sub);
    if (array && sk != TY_ARRAY && sk != TY_VLA) {
        ierr(c, x, lt, "array index in non-array initializer");
        return true;
    } else if (!array && sk != TY_STRUCT && sk != TY_UNION) {
        ierr(c, x, lt, "field name not in record or union initializer");
        return true;
    }
    L->designated = true;
    L->eldes = true;
    finish_implicit_inits(c, x, lt);
    push_level(c, x, lt, 2);
    x->stk->eldes = true;           /* the level's first element is the designated one */
    return false;
}

static bool int_node(Checker *c, uint32_t n)
{
    return !node_err(c, n) && type_is_integer(TT, c->ty[n]);
}

static void set_init_index(Checker *c, CCtx *x, uint32_t lt, uint32_t e1,
                           uint32_t e2)
{
    Lvl *L;
    int64_t first = 0, last = 0;
    bool neg, big, has_last = e2 != NOB;
    if (set_designator(c, x, lt, true))
        return;
    x->derr = 1;
    if (!int_node(c, e1) || (has_last && !int_node(c, e2))) {
        ierr(c, x, lt, "array index in initializer not of integer type");
        return;
    }
    if (c->ck[e1] == K_FOLD && !(c->ef[e1] & EF_CST))
        ipdt(c, x, lt, "array index in initializer is not an integer "
             "constant expression");
    if (has_last && c->ck[e2] == K_FOLD && !(c->ef[e2] & EF_CST))
        ipdt(c, x, lt, "array index in initializer is not an integer "
             "constant expression");
    if ((c->ck[e1] != K_ICE && c->ck[e1] != K_FOLD) ||
        (has_last && c->ck[e2] != K_ICE && c->ck[e2] != K_FOLD)) {
        ierr(c, x, lt, "nonconstant array index in initializer");
        return;
    }
    L = x->stk;
    if (L->kind != LV_ARR) {
        ierr(c, x, lt, "array index in non-array initializer");
        return;
    }
    /* constant_expression_warning on the index (and range end) */
    if (c->ef[e1] & EF_OVERFLOW)
        cconst_overflow(c, cinput_loc(c, last_tok(c, e1) + 1));
    if (has_last && (c->ef[e2] & EF_OVERFLOW))
        cconst_overflow(c, cinput_loc(c, last_tok(c, e2) + 1));
    first = cexpr_sval(c, e1);
    neg = type_is_signed(TT, c->ty[e1]) ? first < 0 : false;
    big = !type_is_signed(TT, c->ty[e1]) && first < 0;
    if (neg || big || (L->hasmax && L->maxidx < first)) {
        ierr(c, x, lt, "array index in initializer exceeds array bounds");
        return;
    }
    L->idx = first;
    if (has_last) {
        bool lneg, lbig;
        last = cexpr_sval(c, e2);
        lneg = type_is_signed(TT, c->ty[e2]) ? last < 0 : false;
        lbig = !type_is_signed(TT, c->ty[e2]) && last < 0;
        if (!lneg && !lbig && last == first)
            has_last = false;
        else if (lneg || (!lbig && last < first)) {
            ierr(c, x, lt, "empty index range in initializer");
            has_last = false;
        } else if (lbig || (L->hasmax && L->maxidx < last)) {
            ierr(c, x, lt, "array index range in initializer exceeds array "
                 "bounds");
            has_last = false;
        }
    }
    x->dd++;
    x->derr = 0;
    if (x->rs || has_last)
        push_range_stack(x, has_last, last);
}

/* The path of fields (anonymous members searched) to the named member. */
static bool lookup_path(Checker *c, TypeId rec, uint32_t name, uint32_t *path,
                        int *n, int max)
{
    Record *r = type_record(TT, type_canon(TT, rec));
    uint32_t k;
    if (!r || *n >= max)
        return false;
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &TT->fields.data[r->fields + k];
        TypeKind fk;
        if (!f->name) {
            fk = ck_(c, f->ty);
            if (fk == TY_STRUCT || fk == TY_UNION) {
                path[(*n)++] = k;
                if (lookup_path(c, f->ty, name, path, n, max))
                    return true;
                (*n)--;
            }
        }
        f = &TT->fields.data[r->fields + k];
        if (f->name == name) {
            path[(*n)++] = k;
            return true;
        }
    }
    return false;
}

static void set_init_label(Checker *c, CCtx *x, uint32_t lt, uint32_t name,
                           uint32_t name_tok)
{
    Lvl *L;
    uint32_t path[32];
    int n = 0, j;
    if (set_designator(c, x, lt, false))
        return;
    x->derr = 1;
    L = x->stk;
    if (L->kind != LV_REC && L->kind != LV_UNI) {
        ierr(c, x, lt, "field name not in record or union initializer");
        return;
    }
    if (!lookup_path(c, L->type, name, path, &n, 32)) {
        const char *g = cexpr_fuzzy_field(c, L->type, name);
        if (g)
            cerror(c, tloc(c, name_tok), "%s has no member named '%s'; did "
                   "you mean '%s'?", type_q(TT, L->type), cident(c, name), g);
        else
            cerror(c, tloc(c, name_tok), "%s has no member named '%s'",
                   type_q(TT, L->type), cident(c, name));
        return;
    }
    for (j = 0; j < n; j++) {
        x->stk->fi = path[j];
        x->dd++;
        x->derr = 0;
        if (x->rs)
            push_range_stack(x, false, 0);
        if (j + 1 < n && set_designator(c, x, lt, false))
            return;
    }
}

/* ---- contexts ----------------------------------------------------------------------------------- */

static bool has_designated(Checker *c, uint32_t lo, uint32_t hi)
{
    uint32_t k;
    for (k = lo; k <= hi; k++)
        if (ntag(c, k) == N_DESIGNATED)
            return true;
    return false;
}

static CCtx *ctx_open(Checker *c, uint32_t list, bool reqc, const char *label,
                      uint32_t init_loc, bool varroot)
{
    CInit *ci = ci_get(c);
    CCtx *x = xcalloc(1, sizeof *x);
    x->prev = ci->top;
    x->list = list;
    x->lo = cfirst(c, list);
    x->hi = list;
    x->reqc = reqc;
    x->dm = has_designated(c, x->lo, x->hi);
    x->varroot = varroot;
    x->init_loc = init_loc;
    ci->top = x;
    sp_push(x, label);
    return x;
}

static void ctx_close(Checker *c, CCtx *x)
{
    CInit *ci = c->ci;
    PSet *p;
    RS *r;
    Lvl *L;
    while ((L = x->stk) != NULL) {
        x->stk = L->up;
        L->up = ci->freel;
        ci->freel = L;
    }
    for (p = x->psall; p;) {
        PSet *n = p->next;
        free(p->e);
        free(p);
        p = n;
    }
    for (r = x->rsall; r;) {
        RS *n = r->alloc_next;
        free(r);
        r = n;
    }
    sb_free(&x->path);
    free(x->dwpath);
    free(x->dl);
    ci->top = x->prev;
    free(x);
}

/* gcc's really_start_incremental_init. */
static void start_root(Checker *c, CCtx *x, TypeId type)
{
    CInit *ci = c->ci;
    Lvl *L = lvl_new(ci);
    L->inc = true;
    L->depth = x->sdepth;
    setup_kind(c, L, type);
    x->stk = L;
    ci->zeroinit = true;
    ci->found_mb = false;
    x->dd = x->derr = 0;
    x->rs = NULL;
}

void cinit_reset(Checker *c)
{
    CInit *ci = c->ci;
    if (!ci)
        return;
    while (ci->top)
        ctx_close(c, ci->top);
}

void cinit_free(Checker *c)
{
    CInit *ci = c->ci;
    Lvl *L;
    if (!ci)
        return;
    cinit_reset(c);
    while ((L = ci->freel) != NULL) {
        ci->freel = L->up;
        free(L);
    }
    free(ci->tmp);
    free(ci);
    c->ci = NULL;
}

void cinit_declared(Checker *c, uint32_t declared)
{
    uint32_t idecl = c->par[declared], list;
    CSym *s = NULL;
    TypeId type = ERRT;
    bool reqc = false;
    CCtx *x;
    const char *label = "(anonymous)";
    if (idecl == NOB || idecl >= c->nn || ntag(c, idecl) != N_INIT_DECL)
        return;
    list = idecl - 1;
    if (list == declared || ntag(c, list) != N_INIT_LIST)
        return;
    if (c->cb[declared]) {
        s = csym(c, c->cb[declared] - 1);
        if ((c->cv[declared] & 2) && !(s->flags & CSF_AUTO_TYPE))
            type = s->ty;   /* incomplete struct: digested, all excess */
        else if ((c->cv[declared] & 1) && !(s->flags & CSF_AUTO_TYPE)) {
            type = s->ty;
            reqc = (s->flags & CSF_TREE_STATIC) != 0;
        }
        label = cident(c, s->name);
    }
    x = ctx_open(c, list, reqc, label, c->nodes[list].tok, false);
    if (c->cb[declared])
        x->decl_loc = cdecl_tag_loc(c, declared)
                          ? cdecl_tag_loc(c, declared)
                          : csym(c, c->cb[declared] - 1)->loc;
    if (diag_enabled(c->diag, "traditional"))
        x->trad_loc = ctrad_decl_loc(c, declared, c->nodes[list].tok);
    x->varroot = !is_err(c, type) && is_varsize(c, type);
    x->rtop = 0;
    (void)type;
    /* the root level is created when the list's first node arrives */
    x->stk = NULL;
    /* remember the type in the (not yet used) level slot */
    {
        Lvl *L = lvl_new(c->ci);
        L->type = type;
        L->up = NULL;
        x->stk = L;
    }
}

/* ---- hooks: before a node ----------------------------------------------------------------------- */

static void empty_braces(Checker *c, CCtx *x, uint32_t list)
{
    if (c->nodes[list].size == 1)
        ipdt(c, NULL, c->nodes[list].tok, "ISO C forbids empty initializer "
             "braces before C2X");
    (void)x;
}

void cinit_pre(Checker *c, uint32_t i)
{
    CInit *ci = c->ci;
    CCtx *x = ci ? ci->top : NULL;
    uint32_t p, n = 0, k;
    if (!x || i < x->lo || i > x->hi)
        return;
    for (p = i; p != NOB && p < c->nn && p <= x->hi && cfirst(c, p) == i;
         p = c->par[p]) {
        if (ntag(c, p) == N_INIT_LIST) {
            if ((size_t)n >= ci->tmpcap) {
                ci->tmpcap = ci->tmpcap ? ci->tmpcap * 2 : 16;
                ci->tmp = xrealloc(ci->tmp, ci->tmpcap * sizeof *ci->tmp);
            }
            ci->tmp[n++] = p;
        }
        if (p == x->list)
            break;
    }
    for (k = n; k-- > 0;) {
        uint32_t list = ci->tmp[k];
        uint32_t lbrace = c->nodes[list].tok;
        la_set(x, LA_TOK, lbrace);
        if (list == x->list) {
            Lvl *pre = x->stk;
            TypeId type = pre ? pre->type : ERRT;
            if (pre) {
                x->stk = NULL;
                pre->up = ci->freel;
                ci->freel = pre;
            }
            start_root(c, x, type);
            if (x->varroot && c->nodes[list].size > 1)
            {
                cerror(c, tloc(c, lbrace), "variable-sized object may not be "
                       "initialized except with an empty initializer");
                x->varerr = true;
            }
            empty_braces(c, x, list);
        } else {
            uint32_t par = c->par[list];
            if (par == NOB || (ntag(c, par) != N_INIT_LIST &&
                               ntag(c, par) != N_DESIGNATED))
                continue;
            finish_implicit_inits(c, x, lbrace);
            push_level(c, x, lbrace, 0);
            empty_braces(c, x, list);
        }
    }
}

/* ---- hooks: after a node ------------------------------------------------------------------------ */

static void finalize_root(Checker *c, CCtx *x)
{
    uint32_t list = x->list;
    uint32_t lbrace = c->nodes[list].tok;
    IVal r;
    Lvl *rl = x->stk;
    TypeId rtype;
    la_set(x, LA_CLOSE, list);
    while (rl && rl->up)
        rl = rl->up;
    rtype = rl ? rl->type : ERRT;
    r = pop_level(c, x, lbrace, 0);
    if (r.kind == V_ERR || r.kind == V_NONE)
        c->ck[list] = K_ERR;
    else if (r.kind == V_CTOR) {
        c->ck[list] = K_ICE;
        c->cv[list] = is_arr(c, rtype) ? (uint64_t)x->rtop : 0;
    } else if (r.kind == V_EXPR && r.str && !r.digested && is_arr(c, rtype)) {
        IVal v = r;
        v.strict = true;
        /* in a compound literal gcc's input_location is the literal's '(' */
        uint32_t cl = c->par[list];
        maybe_warn_string(c, NULL, cl != NOB && cl < c->nn && ntag(c, cl) == N_COMPOUND_LIT
                          ? c->nodes[cl].tok : x->init_loc, rtype, &r);
        if (digest(c, NULL, x->init_loc, true, x->reqc, rtype, &v)) {
            c->ck[list] = K_ICE;
            c->cv[list] = r.strn;
        } else
            c->ck[list] = K_ERR;
    } else {
        c->ck[list] = K_ICE;
        c->cv[list] = 0;
    }
    {
        uint32_t cl = c->par[list];
        if (x->trad_loc && cl != NOB && cl < c->nn &&
            ntag(c, cl) == N_COMPOUND_LIT && !cat_file_scope(c) &&
            (is_aggr(c, rtype) || is_arr(c, rtype)) &&
            !cin_system(c, x->trad_loc))
            cwarn(c, x->trad_loc, "traditional", "traditional C rejects "
                  "automatic aggregate initialization");
    }
    ctx_close(c, x);
}

static void open_complit(Checker *c, uint32_t tn)
{
    uint32_t par = c->par[tn], list;
    CInit *ci;
    CCtx *x;
    TypeId t = c->ty[tn];
    SrcLoc tl;
    uint32_t tt_;
    if (par == NOB || par >= c->nn || ntag(c, par) != N_COMPOUND_LIT)
        return;
    list = par - 1;
    if (ntag(c, list) != N_INIT_LIST)
        return;
    ci = ci_get(c);
    tt_ = first_tok(c, tn);
    tl = tloc(c, tt_ > 0 ? tt_ - 1 : tt_);
    if (!is_err(c, t)) {
        if (is_varsize(c, t)) {
            cerror(c, tl, "compound literal has variable size");
            t = ERRT;
        } else if (ck_(c, t) == TY_FUNC) {
            cerror(c, tl, "compound literal has function type");
            t = ERRT;
        }
    }
    c->ty[tn] = t;
    x = ctx_open(c, list, cat_file_scope(c), "(anonymous)", c->nodes[list].tok, false);
    if (diag_enabled(c->diag, "traditional"))
        x->trad_loc = ctrad_loc(c, ctrad_tag(c, tt_, c->nodes[list].tok),
                                c->nodes[list].tok);
    x->varroot = false;
    {
        Lvl *L = lvl_new(ci);
        L->type = t;
        x->stk = L;
    }
}

/* The token of the first designator of a DESIGNATED node (the '.' or '['). */
static uint32_t des_start(Checker *c, uint32_t p)
{
    uint32_t d = first_child(c, p);
    if (d == NOB)
        return c->nodes[p].tok;
    if (ntag(c, d) == N_DESIG_FIELD)
        return c->nodes[d].tok ? c->nodes[d].tok - 1 : c->nodes[d].tok;
    if (ntag(c, d) == N_DESIG_INDEX)
        return first_tok(c, d - 1) - 1;
    return first_tok(c, d - 1 - c->nodes[d - 1].size) - 1;
}

static void designator(Checker *c, CCtx *x, uint32_t i, uint32_t par)
{
    unsigned tag = ntag(c, i);
    uint32_t value = par - 1, lastdes = cfirst(c, value) - 1;
    bool single = cfirst(c, i) == cfirst(c, par);
    uint32_t vtok;
    if (tag == N_DESIG_FIELD) {
        uint32_t tok = c->nodes[i].tok;
        uint32_t name = cnode_ident(c, i);
        if (tok > 0 && tpunct(c, tok - 1) == P_DOT) {
            la_set(x, LA_TOK, tok);
            set_init_label(c, x, des_start(c, par), name, tok);
        } else {
            la_set(x, LA_TOK, tok + 1);
            set_init_label(c, x, tok, name, tok);
            ipdt(c, NULL, tok + 1, "obsolete use of designated initializer "
                 "with ':'");
        }
        return;
    }
    if (tag == N_DESIG_INDEX) {
        uint32_t e = i - 1;
        la_set(x, LA_AFTER, e);
        set_init_index(c, x, first_tok(c, e), e, NOB);
    } else {
        uint32_t e2 = i - 1, e1 = i - 1 - c->nodes[i - 1].size;
        la_set(x, LA_AFTER, e2);
        set_init_index(c, x, first_tok(c, e1), e1, e2);
        ipdt(c, NULL, first_tok(c, e2) - 1, "ISO C forbids specifying range of "
             "elements to initialize");
    }
    vtok = first_tok(c, value);
    if (single && i == lastdes && vtok > 0 && tpunct(c, vtok - 1) != P_ASSIGN)
        ipdt(c, NULL, vtok, "obsolete use of designated initializer without "
             "'='");
}

void cinit_post(Checker *c, uint32_t i)
{
    CInit *ci;
    CCtx *x;
    unsigned tag = ntag(c, i);
    uint32_t par;
    if (tag == N_TYPE_NAME) {
        open_complit(c, i);
        return;
    }
    ci = c->ci;
    x = ci ? ci->top : NULL;
    if (!x || i < x->lo || i > x->hi)
        return;
    par = c->par[i];
    if (tag == N_INIT_LIST) {
        if (i == x->list) {
            finalize_root(c, x);
            return;
        }
        if (par != NOB && (ntag(c, par) == N_INIT_LIST ||
                           ntag(c, par) == N_DESIGNATED)) {
            uint32_t lb = c->nodes[i].tok;
            IVal r;
            la_set(x, LA_CLOSE, i);
            r = pop_level(c, x, lb, 0);
            la_set(x, LA_CLOSE, i);
            process_element(c, x, lb, r, false);
        }
        return;
    }
    if (par == NOB || par >= c->nn)
        return;
    if (tag == N_DESIG_FIELD || tag == N_DESIG_INDEX || tag == N_DESIG_RANGE) {
        if (ntag(c, par) == N_DESIGNATED && par <= x->hi) {
            if (i == first_child(c, par))
                cc90(c, tloc(c, des_start(c, par)), NULL, "ISO C90 forbids "
                     "specifying subobject to initialize");
            designator(c, x, i, par);
        }
        return;
    }
    if (tag == N_DESIGNATED)
        return;
    if (ntag(c, par) == N_INIT_LIST ||
        (ntag(c, par) == N_DESIGNATED && i == par - 1)) {
        IVal v;
        if (par > x->hi)
            return;
        ival_from(c, i, &v);
        la_set(x, LA_AFTER, i);
        process_element(c, x, first_tok(c, i), v, false);
    }
}

/* ---- brace-less initializers ---------------------------------------------------------------------- */

/* A static const aggregate: reads of its elements fold (a[27]). */
static void mark_const_agg(Checker *c, CSym *s, TypeId type)
{
    TypeId et = type;
    if ((!is_aggr(c, type) && !is_arr(c, type)) ||
        !(s->flags & CSF_TREE_STATIC))
        return;
    while (is_arr(c, et))
        et = type_base(TT, type_canon(TT, et));
    if ((TYPE_QUALS(et) & (TQ_CONST | TQ_VOLATILE)) == TQ_CONST)
        s->flags |= CSF_CONST_INIT;
}

void cinit_decl_done(Checker *c, uint32_t idecl)
{
    uint32_t declared = idecl, init;
    CSym *s;
    TypeId type;
    IVal v;
    uint32_t ft;
    /* find the DECLARED child */
    init = idecl - 1;
    if (init >= c->nn || ntag(c, init) == N_DECLARED)
        return;
    {
        uint32_t k = init;
        declared = NOB;
        /* the initializer is the last child; the DECLARED precedes it */
        k = cfirst(c, init) - 1;
        if (ntag(c, k) == N_DECLARED)
            declared = k;
    }
    if (declared == NOB || !c->cb[declared] || !(c->cv[declared] & 1))
        return;
    s = csym(c, c->cb[declared] - 1);
    if (ntag(c, init) == N_INIT_LIST) {
        if (!is_err(c, s->ty)) {
            mark_const_agg(c, s, s->ty);
            cexpr_note_strinit(c, s, init);
        }
        return;
    }
    if (s->flags & CSF_AUTO_TYPE)
        return;
    type = s->ty;
    if (is_err(c, type))
        return;
    ft = first_tok(c, init);
    if (is_varsize(c, type)) {
        cerror(c, tloc(c, ft), "variable-sized object may not be initialized "
               "except with an empty initializer");
        c->ck[init] = K_ERR;
        return;
    }
    ival_from(c, init, &v);
    if (v.kind == V_ERR)
        return;
    maybe_warn_string(c, NULL, ft, type, &v);
    v.strict = true;
    if (!digest(c, NULL, ft, true, (s->flags & CSF_TREE_STATIC) != 0, type,
                &v))
        c->ck[init] = K_ERR;
    else {
        uint32_t dp = c->par[idecl];
        if ((s->flags & CSF_TREE_STATIC) && const_overflowed(c, init, type) &&
            dp != NOB && dp < c->nn)
            cconst_overflow(c, tloc(c, first_tok(c, dp)));
        if ((TYPE_QUALS(type) & (TQ_CONST | TQ_VOLATILE)) == TQ_CONST &&
            c->opt.opt_level && c->opt.opt_level != '0' &&
            type_is_integer(TT, type) && type_is_integer(TT, c->ty[init]) &&
            (c->ck[init] == K_ICE ||
             (c->ck[init] == K_FOLD && (c->ef[init] & EF_CST))) &&
            !(c->ef[init] & EF_OVERFLOW)) {
            /* -O: reads of it fold to the value (cexpr.c fold_const_var) */
            s->val = cexpr_trunc(c, type, c->cv[init]);
            s->flags |= CSF_CONST_VAL;
        }
        if ((TYPE_QUALS(type) & (TQ_CONST | TQ_VOLATILE)) == TQ_CONST &&
            !is_aggr(c, type) && !is_arr(c, type) && v.kind == V_EXPR &&
            !v.str && !v.cl && const_class(c, init, v.type, type) == 2)
            s->flags |= CSF_CONST_INIT;
        else
            mark_const_agg(c, s, type);
        cexpr_note_strinit(c, s, init);
    }
}
