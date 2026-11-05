/* cexpr_name.c - members, spelling suggestions, missing headers, literals, identifiers
 * (split from cexpr.c; see its header comment). */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- members ------------------------------------------------------------- */

/* Records with more members than this get a name index (Checker.fkey) on
 * their first lookup; smaller ones are searched in place. */
#define FIDX_MIN 16

/* rec's record if it is a struct or union whose members can be looked up
 * (complete, or being defined).  read: a read of its contents, reported
 * to the summary (type_record); else straight from the table. */
static const Record *member_rec(Checker *c, TypeId rec, bool read)
{
    const Record *r;
    if (!is_record(c, rec))
        return NULL;
    r = read ? type_record(TT, rec)
             : &TT->recs.data[type_ent(TT, type_canon(TT, rec))->extra];
    return (r->flags & (RF_COMPLETE | RF_DEFINING)) ? r : NULL;
}

/* The index entry for key: its slot, empty (fval UINT32_MAX) if absent. */
static uint32_t fidx_probe(Checker *c, uint64_t key)
{
    uint32_t i = (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (c->fcap - 1);
    while (c->fval[i] != UINT32_MAX && c->fkey[i] != key)
        i = (i + 1) & (c->fcap - 1);
    return i;
}

static uint32_t fidx_get(Checker *c, uint64_t key)
{
    return c->fcap ? c->fval[fidx_probe(c, key)] : UINT32_MAX;
}

/* Adds key -> val unless key is there (the first entry wins). */
static void fidx_put(Checker *c, uint64_t key, uint32_t val)
{
    uint32_t i;
    if (2 * (c->fn + 1) > c->fcap) {
        uint64_t *ok = c->fkey;
        uint32_t *ov = c->fval, oc = c->fcap;
        c->fcap = oc ? 2 * oc : 1024;
        c->fkey = xmalloc(c->fcap * sizeof *c->fkey);
        c->fval = xmalloc(c->fcap * sizeof *c->fval);
        memset(c->fval, 0xFF, c->fcap * sizeof *c->fval);
        for (i = 0; i < oc; i++)
            if (ov[i] != UINT32_MAX) {
                uint32_t j = fidx_probe(c, ok[i]);
                c->fkey[j] = ok[i];
                c->fval[j] = ov[i];
            }
        free(ok);
        free(ov);
    }
    i = fidx_probe(c, key);
    if (c->fval[i] == UINT32_MAX) {
        c->fkey[i] = key;
        c->fval[i] = val;
        c->fn++;
    }
}

/* Indexes the names anonymous member a (member k) holds at any depth;
 * *lo: the lowest record index among a and the anonymous members in it. */
static void fidx_anon(Checker *c, uint64_t base, const Record *a, uint32_t k,
                      uint32_t *lo)
{
    uint32_t j, ai = (uint32_t)(a - TT->recs.data);
    if (ai < *lo)
        *lo = ai;
    for (j = 0; j < a->nfields; j++) {
        const Field *f = &TT->fields.data[a->fields + j];
        const Record *n;
        if (f->name)
            fidx_put(c, base | f->name, k);
        else if ((n = member_rec(c, f->ty, false)) != NULL)
            fidx_anon(c, base, n, k, lo);
    }
}

/* The contents reads (summary) of a search for a name rec lacks. */
static void read_miss(Checker *c, TypeId rec)
{
    const Record *a = member_rec(c, rec, true);
    uint32_t j;
    for (j = 0; a && j < a->nfields; j++)
        if (!TT->fields.data[a->fields + j].name)
            read_miss(c, TT->fields.data[a->fields + j].ty);
}

int32_t cfield_slot(Checker *c, const Record *r, uint32_t name)
{
    const Field *f = TT->fields.data + r->fields;
    uint64_t base = (uint64_t)r->fields << 32;
    uint32_t n = r->nfields, k, lo;
    const Record *a;
    if (n <= FIDX_MIN || !name) {
        for (k = 0; k < n; k++)
            if (f[k].name == name)
                return (int32_t)k;
        for (k = 0; k < n; k++)
            if (!f[k].name && (a = member_rec(c, f[k].ty, true)) != NULL &&
                cfield_slot(c, a, name) >= 0)
                return (int32_t)k;
        return -1;
    }
    /* the marker entry: the lowest record index of the anonymous members
     * at any depth (UINT32_MAX - 1: none) */
    lo = fidx_get(c, base);
    if (lo == UINT32_MAX) {
        lo = UINT32_MAX - 1;
        for (k = 0; k < n; k++)
            if (f[k].name)
                fidx_put(c, base | f[k].name, k);
        for (k = 0; k < n; k++)
            if (!f[k].name && (a = member_rec(c, f[k].ty, false)) != NULL)
                fidx_anon(c, base, a, k, &lo);
        fidx_put(c, base, lo);
    }
    k = fidx_get(c, base | name);
    /* anonymous members of older units: the reads a search in place makes
     * of those before the one found (all if none) */
    if (lo < TT->unit_rec0 && (k == UINT32_MAX || !f[k].name)) {
        uint32_t j, end = k == UINT32_MAX ? n : k;
        for (j = 0; j < end; j++)
            if (!f[j].name)
                read_miss(c, f[j].ty);
    }
    return k == UINT32_MAX ? -1 : (int32_t)k;
}

/* The field name of record rec (anonymous members searched, depth first,
 * after the named ones); adds its offset to *off and the qualifiers of the
 * anonymous members on the way to *quals. */
const Field *find_field(Checker *c, TypeId rec, uint32_t name,
                               uint64_t *off, unsigned *quals)
{
    const Record *r;
    while ((r = member_rec(c, rec, true)) != NULL) {
        int32_t k = cfield_slot(c, r, name);
        const Field *f;
        if (k < 0)
            return NULL;
        f = &TT->fields.data[r->fields + k];
        *off += f->off_bits;
        if (f->name == name)
            return f;
        *quals |= tquals(c, f->ty);
        rec = f->ty;
    }
    return NULL;
}

/* ---- spelling suggestions (gcc's spellcheck.cc) ---------------------------- */


/* A misspelled member name: the fields of rec, anonymous members'
 * included. */
void fuzzy_fields(Checker *c, Best *b, TypeId rec)
{
    const Record *r;
    uint32_t k;
    if (!is_record(c, rec))
        return;
    r = type_record(TT, rec);
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (f->name)
            best_consider(b, cident(c, f->name));
        else
            fuzzy_fields(c, b, f->ty);
        r = type_record(TT, rec);
    }
}

/* The spelling suggestion for a misspelled member of rec (NULL: none). */
const char *cexpr_fuzzy_field(Checker *c, TypeId rec, uint32_t name)
{
    Best b;
    best_init(&b, cident(c, name), &c->fuzzy_work);
    fuzzy_fields(c, &b, rec);
    return best_get(&b);
}

/* ---- missing headers (gcc's known-headers.cc, C part) ---------------------- */

typedef struct StdName {
    const char *name, *header;
} StdName;

static const StdName std_names[] = {
    {"assert", "<assert.h>"}, {"errno", "<errno.h>"},
    {"CHAR_BIT", "<limits.h>"}, {"CHAR_MAX", "<limits.h>"},
    {"CHAR_MIN", "<limits.h>"}, {"INT_MAX", "<limits.h>"},
    {"INT_MIN", "<limits.h>"}, {"LLONG_MAX", "<limits.h>"},
    {"LLONG_MIN", "<limits.h>"}, {"LONG_MAX", "<limits.h>"},
    {"LONG_MIN", "<limits.h>"}, {"MB_LEN_MAX", "<limits.h>"},
    {"SCHAR_MAX", "<limits.h>"}, {"SCHAR_MIN", "<limits.h>"},
    {"SHRT_MAX", "<limits.h>"}, {"SHRT_MIN", "<limits.h>"},
    {"UCHAR_MAX", "<limits.h>"}, {"UINT_MAX", "<limits.h>"},
    {"ULLONG_MAX", "<limits.h>"}, {"ULONG_MAX", "<limits.h>"},
    {"USHRT_MAX", "<limits.h>"},
    {"DBL_MAX", "<float.h>"}, {"DBL_MIN", "<float.h>"},
    {"FLT_MAX", "<float.h>"}, {"FLT_MIN", "<float.h>"},
    {"LDBL_MAX", "<float.h>"}, {"LDBL_MIN", "<float.h>"},
    {"va_list", "<stdarg.h>"},
    {"NULL", "<stddef.h>"}, {"offsetof", "<stddef.h>"},
    {"size_t", "<stddef.h>"}, {"wchar_t", "<stddef.h>"},
    {"ptrdiff_t", "<stddef.h>"},
    {"BUFSIZ", "<stdio.h>"}, {"EOF", "<stdio.h>"}, {"FILE", "<stdio.h>"},
    {"FILENAME_MAX", "<stdio.h>"}, {"fopen", "<stdio.h>"},
    {"fpos_t", "<stdio.h>"}, {"getchar", "<stdio.h>"},
    {"printf", "<stdio.h>"}, {"snprintf", "<stdio.h>"},
    {"sprintf", "<stdio.h>"}, {"stderr", "<stdio.h>"},
    {"stdin", "<stdio.h>"}, {"stdout", "<stdio.h>"},
    {"EXIT_FAILURE", "<stdlib.h>"}, {"EXIT_SUCCESS", "<stdlib.h>"},
    {"abort", "<stdlib.h>"}, {"atexit", "<stdlib.h>"},
    {"calloc", "<stdlib.h>"}, {"exit", "<stdlib.h>"},
    {"free", "<stdlib.h>"}, {"getenv", "<stdlib.h>"},
    {"malloc", "<stdlib.h>"}, {"realloc", "<stdlib.h>"},
    {"memchr", "<string.h>"}, {"memcmp", "<string.h>"},
    {"memcpy", "<string.h>"}, {"memmove", "<string.h>"},
    {"memset", "<string.h>"}, {"strcat", "<string.h>"},
    {"strchr", "<string.h>"}, {"strcmp", "<string.h>"},
    {"strcpy", "<string.h>"}, {"strlen", "<string.h>"},
    {"strncat", "<string.h>"}, {"strncmp", "<string.h>"},
    {"strncpy", "<string.h>"}, {"strrchr", "<string.h>"},
    {"strspn", "<string.h>"}, {"strstr", "<string.h>"},
    {"PTRDIFF_MAX", "<stdint.h>"}, {"PTRDIFF_MIN", "<stdint.h>"},
    {"SIG_ATOMIC_MAX", "<stdint.h>"}, {"SIG_ATOMIC_MIN", "<stdint.h>"},
    {"SIZE_MAX", "<stdint.h>"}, {"WINT_MAX", "<stdint.h>"},
    {"INTPTR_MAX", "<stdint.h>"}, {"UINTPTR_MAX", "<stdint.h>"},
    {"WINT_MIN", "<stdint.h>"},
    {"INT8_MAX", "<stdint.h>"}, {"INT16_MAX", "<stdint.h>"},
    {"INT32_MAX", "<stdint.h>"}, {"INT64_MAX", "<stdint.h>"},
    {"INT8_MIN", "<stdint.h>"}, {"INT16_MIN", "<stdint.h>"},
    {"INT32_MIN", "<stdint.h>"}, {"INT64_MIN", "<stdint.h>"},
    {"UINT8_MAX", "<stdint.h>"}, {"UINT16_MAX", "<stdint.h>"},
    {"UINT32_MAX", "<stdint.h>"}, {"UINT64_MAX", "<stdint.h>"},
    {"int8_t", "<stdint.h>"}, {"int16_t", "<stdint.h>"},
    {"int32_t", "<stdint.h>"}, {"int64_t", "<stdint.h>"},
    {"intptr_t", "<stdint.h>"}, {"uint8_t", "<stdint.h>"},
    {"uint16_t", "<stdint.h>"}, {"uint32_t", "<stdint.h>"},
    {"uint64_t", "<stdint.h>"}, {"uintptr_t", "<stdint.h>"},
    {"asctime", "<time.h>"}, {"clock", "<time.h>"},
    {"clock_t", "<time.h>"}, {"ctime", "<time.h>"},
    {"difftime", "<time.h>"}, {"gmtime", "<time.h>"},
    {"localtime", "<time.h>"}, {"mktime", "<time.h>"},
    {"strftime", "<time.h>"}, {"time", "<time.h>"},
    {"time_t", "<time.h>"}, {"tm", "<time.h>"},
    {"WCHAR_MAX", "<wchar.h>"}, {"WCHAR_MIN", "<wchar.h>"},
    {"bool", "<stdbool.h>"}, {"true", "<stdbool.h>"},
    {"false", "<stdbool.h>"}
};

/* <inttypes.h>: gcc-13 knows PRI{d,i,o,u,x,X}{8,16,32,64,PTR} and
 * SCN{d,i,o,u,x}{8,16,32,64,PTR} (not the LEAST/FAST/MAX forms; probed). */
bool inttypes_fmt_macro(const char *n)
{
    bool pri = !strncmp(n, "PRI", 3);
    const char *conv = pri ? "dioxuX" : "dioxu", *sfx[] = {"8", "16", "32", "64", "PTR"};
    size_t k;
    if (!pri && strncmp(n, "SCN", 3))
        return false;
    if (!n[3] || !strchr(conv, n[3]))
        return false;
    for (k = 0; k < sizeof sfx / sizeof *sfx; k++)
        if (!strcmp(n + 4, sfx[k]))
            return true;
    return false;
}

const char *std_header(const char *name)
{
    size_t k;
    if (inttypes_fmt_macro(name))
        return "<inttypes.h>";
    for (k = 0; k < sizeof std_names / sizeof *std_names; k++)
        if (!strcmp(std_names[k].name, name))
            return std_names[k].header;
    return NULL;
}

static void header_note(Checker *c, Diagnostic *d, SrcLoc loc,
                        const char *name, const char *hdr)
{
    cheader_note(c, d, loc, hdr,
                 "'%s' is defined in header '%s'; did you forget to '#include %s'?",
                 name, hdr, hdr);
}

#include "cbuiltin_tab.h"
#include "cbuiltin_x86.h"

static const BTab *bt_bisect(const char *name)
{
    size_t lo = 0, hi = sizeof cbuiltin_tab / sizeof *cbuiltin_tab;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, cbuiltin_tab[mid].name);
        if (!r)
            return &cbuiltin_tab[mid];
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* The pure/nonnull/fmt columns of NAME or __builtin_NAME, zeros for any other name.  Only
 * the generic table has them (the target's rows carry none), so the target's linear search
 * is skipped: this runs on every call. */
const BTab *bt_attr(const char *name)
{
    static const BTab none;
    const BTab *bt;
    bt = bt_bisect(!strncmp(name, "__builtin_", 10) ? name + 10 : name);
    return bt ? bt : &none;
}

/* any: the __builtin_ spelling, which exists in every mode */
const BTab *bt_find(Checker *c, const char *name, bool any)
{
    const BTab *bt = bt_bisect(name);
    if (bt)
        return (bt->gnu == 1 && !c->opt.gnu && !any) || (bt->gnu >= 2 && !any) ? NULL : bt;
    if (any) {              /* the target's: only as __builtin_NAME */
        const IaBt *r = ia32_find(name);
        if (r)
            return &r->bt;
    }
    return NULL;
}

/* A type as gcc prints it ("const char *", "long unsigned int"). */
static TypeId bt_type(Checker *c, const char *s, size_t n)
{
    TypeId t;
    bool cst = false;
    size_t k;
    while (n && s[n - 1] == ' ')
        n--;
    while (n && *s == ' ')
        s++, n--;
    if (n > 6 && !strncmp(s + n - 6, " const", 6))   /* "char * const" */
        return type_qual(bt_type(c, s, n - 6), TQ_CONST);
    if (n > 6 && !strncmp(s, "const ", 6)) {
        cst = true;
        s += 6;
        n -= 6;
    }
    if (n > 10 && !strncmp(s, "__vector(", 9)) {        /* "__vector(16) char" */
        char *end;
        unsigned long bytes = strtoul(s + 9, &end, 10);
        if (end[0] == ')' && end[1] == ' ')
            return type_vector(TT, bt_type(c, end + 2, (size_t)(s + n - (end + 2))), bytes);
    }
    if (n && s[n - 1] == '*') {
        TypeId b = bt_type(c, s, n - 1);
        if (cst)
            b = type_qual(b, TQ_CONST);
        return type_ptr(TT, b);
    }
    {
        static const struct { const char *n; TypeKind k; } base[] = {
            {"void", TY_VOID}, {"int", TY_INT}, {"char", TY_CHAR}, {"short int", TY_SHORT},
            {"signed char", TY_SCHAR},
            {"long int", TY_LONG}, {"long long int", TY_LLONG},
            {"long unsigned int", TY_ULONG}, {"double", TY_DOUBLE},
            {"unsigned int", TY_UINT}, {"short unsigned int", TY_USHORT},
            {"long long unsigned int", TY_ULLONG}, {"unsigned char", TY_UCHAR},
            {"__int128 unsigned", TY_UINT128}, {"_Bool", TY_BOOL},
            {"_Float16", TY_FLOAT16}, {"_Float32", TY_FLOAT32},
            {"_Float64", TY_FLOAT64}, {"_Float32x", TY_FLOAT32X},
            {"_Float64x", TY_FLOAT64X}, {"_Float128", TY_FLOAT128},
            {"__bf16", TY_BF16}, {"_Decimal32", TY_DEC32},
            {"_Decimal64", TY_DEC64}, {"_Decimal128", TY_DEC128},
            {"float", TY_FLOAT}, {"long double", TY_LDOUBLE},
            {"__float128", TY_FLOAT128}
        };
        t = ERRT;
        for (k = 0; k < sizeof base / sizeof *base; k++)
            if (strlen(base[k].n) == n && !strncmp(base[k].n, s, n))
                t = TYPE_MK(base[k].k, 0);
        if (is_err(c, t)) {
            if (n > 9 && !strncmp(s, "_Complex ", 9))
                t = type_complex(TT, bt_type(c, s + 9, n - 9));
            else if (n == 13 && !strncmp(s, "__va_list_tag", 13))
                t = type_base(TT, type_canon(TT, TT->va_list));
        }
    }
    if (cst)
        t = type_qual(t, TQ_CONST);
    return t;
}

/* The function type of a table entry. */
TypeId bt_func_type(Checker *c, const BTab *b)
{
    const char *p = strchr(b->sig, '|'), *q;
    TypeId ps[8], ret;
    uint32_t n = 0, flags = 0;
    if (!p)
        return ERRT;
    ret = bt_type(c, b->sig, (size_t)(p - b->sig));
    p++;
    if (!strcmp(p, "?"))
        return type_func(TT, ret, NULL, 0, TF_NOPROTO);
    while (*p && n < 8) {
        q = strchr(p, '|');
        if (!q)
            q = p + strlen(p);
        if (q - p == 3 && !strncmp(p, "...", 3))
            flags |= TF_VARIADIC;
        else
            ps[n++] = bt_type(c, p, (size_t)(q - p));
        p = *q ? q + 1 : q;
    }
    return type_func(TT, ret, ps, n, flags);
}

/* __builtin_{s,u}{add,sub,mul}{,l,ll}_overflow: _Bool (T, T, T *). */
static TypeId overflow_func_type(Checker *c, const char *b)
{
    static const char *const op[] = {"add", "sub", "mul"};
    bool sg = *b == 's';
    TypeKind k;
    TypeId t, ps[3];
    size_t k2;
    int found = 0;
    if (*b != 's' && *b != 'u')
        return ERRT;
    for (k2 = 0; k2 < 3; k2++)
        if (!strncmp(b + 1, op[k2], 3))
            found = 1;
    if (!found)
        return ERRT;
    b += 4;
    if (!strncmp(b, "ll_overflow", 12))
        k = sg ? TY_LLONG : TY_ULLONG;
    else if (!strncmp(b, "l_overflow", 11))
        k = sg ? TY_LONG : TY_ULONG;
    else if (!strncmp(b, "_overflow", 10))
        k = sg ? TY_INT : TY_UINT;
    else
        return ERRT;
    t = TYPE_MK(k, 0);
    ps[0] = ps[1] = t;
    ps[2] = type_ptr(TT, t);
    return type_func(TT, TYPE_B(BOOL), ps, 3, 0);
}

/* gcc's builtin type declarations, candidates for misspelled names. */
static const char *const builtin_type_names[] = {
    "int", "char", "long int", "unsigned int", "long unsigned int",
    "long long int", "long long unsigned int", "short int",
    "short unsigned int", "signed char", "unsigned char", "__int128",
    "__int128 unsigned", "float", "double", "long double", "_Float16",
    "_Float32", "_Float64", "_Float128", "_Float32x", "_Float64x",
    "complex int", "complex float", "complex double", "complex long double",
    "void", "_Bool", "__bf16"
};

typedef struct {
    Best *b;
    bool res_ok;
} MacroCand;

static void macro_cand(void *arg, const char *s, size_t n)
{
    MacroCand *mc = arg;
    if (mc->res_ok || !reserved_name(s))
        best_consider_n(mc->b, s, n);
}

/* lookup_name_fuzzy: a visible name close to goal.  functions: only
 * functions and pointers to functions (an implicit declaration). */
static const char *fuzzy_name(Checker *c, const char *goal, bool functions)
{
    Best b;
    size_t k;
    char cand[1024], gbuf[1024];
    static __thread char sug[1024];
    const Ident *ident;
    bool res_ok;
    goal = cident_utf8_to(goal, gbuf, sizeof gbuf);     /* gcc compares UTF-8 */
    res_ok = goal[0] == '_';
    best_init(&b, goal, &c->fuzzy_work);
    for (k = c->log.len; k-- > 0;) {
        const Bind *bd = &c->log.data[k];
        const char *s;
        if (c->top[bd->ns][bd->ident] == 0)
            continue;
        if (c->fuzzy_work > SC_BUDGET)
            return NULL;
        /* not cident (): its rotating buffers hold the caller's goal */
        ident = ident_by_id(c->in, bd->ident);
        s = ident->ext ? cident_utf8_to(ident->str, cand, sizeof cand) : ident->str;
        if (!res_ok && reserved_name(s))
            continue;
        if (bd->ns == NS_ORD) {
            CSym *sym = csym(c, bd->ref);
            if (sym->flags & CSF_IMPLICIT)
                continue;
            if ((sym->flags & CSF_ERROR) && is_err(c, sym->ty))
                continue;
            if (functions) {
                if (sym->kind == CS_FUNC)
                    ;
                else if (sym->kind == CS_OBJ && is_ptr(c, sym->ty) &&
                         is_func(c, pointee(c, sym->ty)))
                    ;
                else
                    continue;
            }
        } else if (functions) {
            continue;
        }
        best_consider_n(&b, s, strlen(s));
        if (b.str == s && ident->ext) {        /* keep it past this buffer */
            snprintf(sug, sizeof sug, "%s", s);
            b.str = sug;
        }
    }
    if (c->opt.macro_names) {
        MacroCand mc;
        mc.b = &b;
        mc.res_ok = res_ok;
        c->opt.macro_names(c->opt.macro_names_ctx, macro_cand, &mc);
    }
    if (!functions)
        for (k = sizeof builtin_type_names / sizeof *builtin_type_names;
             k-- > 0;)
            if (res_ok || !reserved_name(builtin_type_names[k]))
                best_consider(&b, builtin_type_names[k]);
    return best_get(&b);
}

/* ---- literals -------------------------------------------------------------- */

static void lit_report(Checker *c, uint32_t i, const Lit *l)
{
    SrcLoc loc = cnode_loc(c, i);
    if (!l->msg[0])
        return;
    switch (l->level) {
    case 2: {
        size_t k = c->diag->all.len, lim = k > 64 ? k - 64 : 0;
        if (l->id == DW_INPUTLOC)
            loc = cinput_loc(c, c->nodes[i].tok);
        for (; k-- > lim;)      /* the parser reported it when it skipped the token */
            if (c->diag->all.data[k]->loc == loc &&
                !strcmp(c->diag->all.data[k]->msg, l->msg))
                return;
        cerror(c, loc, "%s", l->msg);
        break;
    }
    case 1:
        if (l->flags & (LIT_TOO_LARGE | LIT_UNSIGNED_WARN))
            cpedwarn(c, loc, l->id, "%s", l->msg);
        else
            cwarn(c, loc, l->id, "%s", l->msg);
        break;
    default:
        /* libcpp's pedwarns carry no option tag; interpret_float's
         * non-standard suffix pedwarn is c-family's: [-Wpedantic] at
         * input_location */
        if (l->id == DW_C2X && c->opt.std_year >= 2023)
            break;              /* standard in C23 */
        if (l->id == DW_C2X && !c->opt.pedantic) {
            /* libcpp: the compat option's own, shorter wording */
            const char *or_gcc = strstr(l->msg, " or GCC");
            if (diag_option_explicit(c->diag, DW_C11_C2X_COMPAT))
                cwarn(c, loc, DW_C11_C2X_COMPAT, "%.*s",
                      or_gcc ? (int)(or_gcc - l->msg) : (int)strlen(l->msg),
                      l->msg);
        } else if (c->opt.pedantic && !cexpr_in_extension(c, i)) {
            if (l->id == DW_INPUTLOC)
                cpedwarn(c, cinput_loc(c, c->nodes[i].tok), DW_PEDANTIC, "%s",
                         l->msg);
            else
                cpedwarn(c, loc, DW_NONE, "%s", l->msg);
        }
        break;
    }
    if (l->flags & LIT_FIXED)
        cerror(c, loc, "fixed-point types not supported for this target");
    if ((l->flags & LIT_UNSIGNED_WARN) && !cin_system(c, loc))
        cwarn(c, cinput_loc(c, c->nodes[i].tok), DW_TRADITIONAL,
              "this decimal constant would be unsigned in ISO C90");
}

/* libcpp's -Wtraditional: a floating constant's suffix, or an integer
 * constant's unsigned or imaginary one, as spelled. */
static void traditional_suffix(Checker *c, uint32_t i, const char *s,
                               size_t len, bool flt)
{
    size_t k = 0;
    bool hex = len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    char suf[16];
    if (!diag_enabled(c->diag, DW_TRADITIONAL))
        return;
    if (len > 1 && s[0] == '0' && (hex || s[1] == 'b' || s[1] == 'B'))
        k = 2;
    while (k < len) {
        char ch = s[k];
        if (isdigit((unsigned char)ch) || ch == '.' ||
            (hex && isxdigit((unsigned char)ch)))
            k++;
        else if (ch == (hex ? 'p' : 'e') || ch == (hex ? 'P' : 'E')) {
            k++;
            if (k < len && (s[k] == '+' || s[k] == '-'))
                k++;
        } else
            break;
    }
    if (k >= len || len - k >= sizeof suf)
        return;
    memcpy(suf, s + k, len - k);
    suf[len - k] = 0;
    if (!flt && !strpbrk(suf, "uUiIjJ"))
        return;
    /* cpp_sys_macro_p: a token of a system header's macro */
    if (!cin_system(c, cnode_loc(c, i)) &&
        !cin_system(c, c->u->toks[c->nodes[i].tok].t.loc))
        cwarn(c, cnode_loc(c, i), DW_TRADITIONAL,
              "traditional C rejects the \"%s\" suffix", suf);
}

/* Whether a decimal literal that overflowed the host long double (x87,
 * max ~1.18973149535723176502e4932) still fits _Float128, whose maximum is
 * slightly larger: compare the value scaled by 1e-2 with the maximum. */
static bool f128_decimal_fits(const char *s, size_t len)
{
    char buf[128];
    size_t k = 0, e = 0;
    long ex;
    if (len >= sizeof buf - 8)
        return false;
    while (e < len && s[e] != 'e' && s[e] != 'E')
        e++;
    if (e == len || (len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')))
        return false;
    memcpy(buf, s, e);
    ex = strtol(s + e + 1, NULL, 10);
    k = e + (size_t)snprintf(buf + e, sizeof buf - e, "e%ld", ex - 2);
    buf[k] = 0;
    return strtold(buf, NULL) <= 1.18973149535723176508575932662800702e4930L;
}

void e_number(Checker *c, uint32_t i)
{
    size_t len;
    const char *s = ttext(c, c->nodes[i].tok, &len);
    Lit l;
    TypeId t;
    int kw = tckw(c, c->nodes[i].tok);
    if (kw == CK_TRUE || kw == CK_FALSE) {      /* C23 true, false */
        c->ty[i] = TYPE_MK(TY_BOOL, 0);
        c->ck[i] = K_ICE;
        c->cv[i] = kw == CK_TRUE;
        c->ef[i] = EF_INTOPS;
        return;
    }
    if (kw == CK_NULLPTR) {             /* C23 nullptr: a null pointer constant of its own type */
        c->ty[i] = TYPE_B(NULLPTR);
        c->ck[i] = K_ADDR;
        c->cb[i] = 0;
        c->cv[i] = 0;
        c->ef[i] = EF_NPC;
        return;
    }
    lit_number(c->tgt, s, len, &l);
    traditional_suffix(c, i, s, len, l.flags & LIT_FLOAT);
    lit_report(c, i, &l);
    if (l.flags & LIT_BAD) {
        set_err(c, i);
        return;
    }
    t = TYPE_MK(l.ty, 0);
    if (l.flags & LIT_IMAGINARY) {
        long double im = 0;
        bool ok = false;
        c->ty[i] = type_complex(TT, t);
        if (l.flags & LIT_FLOAT) {
            ok = !(l.ty >= TY_DEC32 && l.ty <= TY_DEC128);
            im = fround(c, t, l.f);
        } else if (int_bits(c, t) <= 64) {
            ok = true;
            im = cplx_ld(c, t, cexpr_trunc(c, t, l.v));
        }
        if (ok) {
            c->ef[i] = 0;
            cplx_set(c, i, 0, im);
        }
        return;
    }
    c->ty[i] = t;
    if (l.flags & LIT_FLOAT) {
        long double v = fround(c, t, l.f);
        size_t k;
        bool nonzero = false, hex = len > 1 && s[0] == '0' &&
                                    (s[1] == 'x' || s[1] == 'X');
        if (l.ty >= TY_DEC32 && l.ty <= TY_DEC128)
            return;
        if (!hex && len && !isalpha((unsigned char)s[len - 1]))
            cwarn(c, cinput_loc(c, c->nodes[i].tok),
                  DW_UNSUFFIXED_FLOAT_CONSTANTS, "unsuffixed floating constant");
        for (k = hex ? 2 : 0; k < len; k++) {
            char ch = s[k];
            if ((!hex && (ch == 'e' || ch == 'E')) ||
                (hex && (ch == 'p' || ch == 'P')))
                break;
            if ((ch >= '1' && ch <= '9') ||
                (hex && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))))
                nonzero = true;
        }
        if (isinf(v) && !(tkind(c, t) == TY_FLOAT128 && f128_decimal_fits(s, len)))
            cwarn(c, cinput_loc(c, c->nodes[i].tok), DW_OVERFLOW,
                  "floating constant exceeds range of %s", type_q(TT, t));
        else if (v == 0 && nonzero)
            cwarn(c, cinput_loc(c, c->nodes[i].tok), DW_OVERFLOW,
                  "floating constant truncated to zero");
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, v);
        c->ef[i] = EF_REALCST;
        return;
    }
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, t, l.v);
    c->ef[i] = EF_INTOPS;
    if (len && s[0] >= '1' && s[0] <= '9')
        c->ef[i] |= EF_DECIMAL;
    else if (len == 1 || (len > 1 && s[1] != 'x' && s[1] != 'X' &&
                          s[1] != 'b' && s[1] != 'B' && s[1] != '\'' &&
                          !(s[1] >= '0' && s[1] <= '9')))
        c->ef[i] |= EF_DECIMAL;   /* 0, 0u */
}

/* libcpp: \a and \x mean something else in traditional C. */
static void traditional_escapes(Checker *c, uint32_t tok, const char *s,
                                size_t len)
{
    size_t k;
    SrcLoc loc;
    loc = ctok_loc(c, tok);
    if (cin_system(c, loc))
        return;
    for (k = 0; k + 1 < len; k++)
        if (s[k] == '\\') {
            if (diag_enabled(c->diag, DW_TRADITIONAL) &&
                (s[k + 1] == 'a' || s[k + 1] == 'x'))
                cwarn(c, loc, DW_TRADITIONAL, "the meaning of '\\%c' is "
                      "different in traditional C", s[k + 1]);
            k++;
        }
}

typedef struct EscCtx {
    Checker *c;
    SrcLoc loc;
} EscCtx;

static void esc_emit(void *ctx, int level, const char *msg)
{
    EscCtx *e = ctx;
    if (level == 2)
        cerror(e->c, e->loc, "%s", msg);
    else
        cpedwarn(e->c, e->loc, DW_NONE, "%s", msg);
}

/* libcpp's convert_escape and friends: bad hex/octal/unknown escapes. */
static void escape_literal(Checker *c, const char *s, size_t len, SrcLoc loc)
{
    EscCtx e;
    if (cin_system(c, loc))
        return;
    e.c = c;
    e.loc = loc;
    lit_escape_diags(s, len, c->opt.pedantic, esc_emit, &e);
}

/* libcpp's _cpp_valid_ucn for the escapes of a literal token: the -pedantic
 * notes on delimited and named forms, then the errors for a malformed or
 * invalid one.  Reported once per token, at loc (the token after a string,
 * a character constant's own start). */
static void ucn_literal(Checker *c, uint32_t tok, const char *s, size_t len,
                        SrcLoc loc)
{
    size_t k = 0, e = len ? len - 1 : 0, j;
    bool any = false, ped = c->opt.pedantic;
    for (j = 0; j + 1 < len; j++)
        if (s[j] == '\\') {
            any |= s[j + 1] == 'u' || s[j + 1] == 'U' || s[j + 1] == 'N';
            j++;
        }
    if (!any || cin_system(c, loc))
        return;
    for (j = c->ucn_seen.len; j-- > 0;)     /* the token's spelling place */
        if (c->ucn_seen.data[j] == ctok_loc(c, tok))
            return;
    vec_push(&c->ucn_seen, ctok_loc(c, tok));
    while (k < e && s[k] != '"' && s[k] != '\'')
        k++;
    for (k = 0; k < e && s[k] != '"' && s[k] != '\''; k++)
        ;
    for (k++; k < e; k++) {
        size_t b, p;
        char kind;
        uint64_t v = 0;
        unsigned nd = 0;
        if (s[k] != '\\') {
            continue;
        }
        b = k++;
        kind = k < e ? s[k] : 0;
        if (kind != 'u' && kind != 'U' && kind != 'N')
            continue;
        p = k + 1;
        if (kind == 'N') {
            size_t ns;
            if (p >= e || s[p] != '{') {
                cerror(c, loc, "'\\N' not followed by '{'");
                cerror(c, loc, "incomplete universal character name \\N");
                k = p - 1;
                continue;
            }
            ns = ++p;
            while (p < e && (isalnum((unsigned char)s[p]) || s[p] == ' ' ||
                             s[p] == '-'))
                p++;
            if (p >= e || s[p] != '}') {
                cerror(c, loc, "'\\N{' not terminated with '}' after "
                       "%.*s", (int)(p - b), s + b);
                cerror(c, loc, "%.*s is not a valid universal character",
                       (int)(p - b), s + b);
                k = p - 1;
                continue;
            }
            if (p == ns)
                cerror(c, loc, "empty named universal character escape "
                       "sequence");
            else if (ped)
                cpedwarn(c, loc, DW_NONE, "named universal character escapes are "
                         "only valid in C++23");
            /* a name is not looked up: the Unicode database is not here */
            k = p;
            continue;
        }
        if (p < e && s[p] == '{') {
            size_t ds;
            ds = ++p;
            while (p < e && isxdigit((unsigned char)s[p])) {
                v = v << 4 | (uint64_t)(isdigit((unsigned char)s[p])
                                        ? s[p] - '0' : (s[p] | 32) - 'a' + 10);
                if (v > 0xFFFFFFFFu)
                    v = 0xFFFFFFFFu;
                p++;
            }
            if (p >= e || s[p] != '}') {
                cerror(c, loc, "'\\%c{' not terminated with '}' after %.*s",
                       kind, (int)(p - b), s + b);
                k = p - 1;
                continue;
            }
            if (p == ds)
                cerror(c, loc, "empty delimited escape sequence");
            else if (ped)
                cpedwarn(c, loc, DW_NONE, "delimited escape sequences are only "
                         "valid in C++23");
            p++;
        } else {
            unsigned want = kind == 'u' ? 4 : 8;
            while (nd < want && p < e && isxdigit((unsigned char)s[p])) {
                v = v << 4 | (uint64_t)(isdigit((unsigned char)s[p])
                                        ? s[p] - '0' : (s[p] | 32) - 'a' + 10);
                p++;
                nd++;
            }
            if (nd < want) {
                cerror(c, loc, "incomplete universal character name %.*s",
                       (int)(p - b), s + b);
                k = p - 1;
                continue;
            }
        }
        if (v > 0x10FFFF && v < 0x80000000u)
            cpedwarn(c, loc, DW_NONE, "%.*s is outside the UCS codespace",
                     (int)(p - b), s + b);
        else if ((v < 0xA0 && v != 0x24 && v != 0x40 && v != 0x60) ||
                 v >= 0x80000000u || (v >= 0xD800 && v <= 0xDFFF))
            cerror(c, loc, "%.*s is not a valid universal character",
                   (int)(p - b), s + b);
        k = p - 1;
    }
}

void e_char(Checker *c, uint32_t i)
{
    size_t len;
    const char *s = ttext(c, c->nodes[i].tok, &len);
    Lit l;
    ucn_literal(c, c->nodes[i].tok, s, len, ctok_loc(c, c->nodes[i].tok));
    escape_literal(c, s, len, ctok_loc(c, c->nodes[i].tok));
    lit_char(c->tgt, s, len, &l);
    traditional_escapes(c, c->nodes[i].tok, s, len);
    lit_report(c, i, &l);
    if (l.flags & LIT_BAD) {
        set_err(c, i);
        return;
    }
    c->ty[i] = TYPE_MK(l.ty, 0);
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, c->ty[i], l.v);
    c->ef[i] = EF_INTOPS;
}

/* gcc parses the strings of an asm (template, constraints, clobbers, label)
 * apart from expressions: no -Woverlength-strings. */
static bool in_asm_string(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    return p != NO_NODE &&
           (ntag(c, p) == N_TOP_ASM || ntag(c, p) == N_ASM ||
            ntag(c, p) == N_ASM_SECTION || ntag(c, p) == N_ASM_OPERAND ||
            ntag(c, p) == N_ASM_LABEL);
}

void e_string(Checker *c, uint32_t i)
{
    uint32_t np = node_pieces(c, i), k;
    int prefix = 0;
    uint64_t units = 0;
    unsigned width;
    TypeKind ek = TY_CHAR;
    if (np > 1 && diag_enabled(c->diag, DW_TRADITIONAL)) {
        SrcLoc il = cinput_loc(c, c->nodes[i].tok + 1);
        if (!cin_system(c, il))
            cwarn(c, il, DW_TRADITIONAL, "traditional C rejects string "
                  "constant concatenation");
    }
    for (k = 0; k < np; k++) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        int p = lit_str_prefix(s, len);
        traditional_escapes(c, c->nodes[i].tok + k, s, len);
        ucn_literal(c, c->nodes[i].tok + k, s, len,
                    tloc(c, c->nodes[i].tok + np));
        escape_literal(c, s, len, tloc(c, c->nodes[i].tok + np));
        if (p && prefix && p != prefix) {
            /* at the lookahead's line, once per piece whose prefix differs
             * from the first prefixed one (gcc's lexer), and once more when
             * the second piece differs from the first (the interpreter) */
            SrcLoc il = cinput_loc(c, c->nodes[i].tok + np);
            unsigned e, errs = 0;
            int first = 0, q0 = 0, q1 = 0;
            for (e = 0; e < np; e++) {
                size_t l2;
                const char *s2 = ttext(c, c->nodes[i].tok + e, &l2);
                int q = lit_str_prefix(s2, l2);
                if (e == 0)
                    q0 = q;
                else if (e == 1)
                    q1 = q;
                if (q && !first)
                    first = q;
                else if (q && q != first)
                    errs++;
            }
            if (q0 && q1 && q0 != q1)
                errs++;
            while (errs--)
                cerror(c, il, "unsupported non-standard concatenation of "
                       "string literals");
            set_err(c, i);
            return;
        }
        if (p)
            prefix = p;
    }
    if (prefix == '8')
        prefix = 0;
    switch (prefix) {
    case 'L': ek = c->tgt->wchar_type; break;
    case 'u': ek = c->tgt->char16_type; break;
    case 'U': ek = c->tgt->char32_type; break;
    default: break;
    }
    width = c->tgt->size[ek];
    for (k = 0; k < np; k++) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        lit_str_units(s, len, width, &units);
    }
    if (units > 4095 && !cexpr_in_extension(c, i) && !in_asm_string(c, i))
        cpedwarn(c, cinput_loc(c, c->nodes[i].tok + np), DW_OVERLENGTH_STRINGS,
                 "string length '%llu' is greater than the length '%d' ISO "
                 "C99 compilers are required to support",
                 (unsigned long long)units, 4095);
    /* -Wwrite-strings: the literal is an array of const char */
    c->ty[i] = type_array(TT, TYPE_MK(ek, diag_enabled(c->diag, DW_WRITE_STRINGS)
                                          ? TQ_CONST : 0), units + 1);
    c->ef[i] = EF_LVALUE | EF_STRING | EF_ADDRLV;
    c->cb[i] = CB_NODE | i;
    c->ck[i] = K_ADDR;
}

/* ---- identifiers ------------------------------------------------------------- */

/* The __builtin___ names gcc defines: the fortify family and clear_cache. */
static bool builtin3_known(const char *s)
{
    size_t n = strlen(s);
    return !strcmp(s, "clear_cache") || (n > 4 && !strcmp(s + n - 4, "_chk"));
}

static bool builtin_name(const char *s)
{
    return !strncmp(s, "__builtin_", 10) || !strncmp(s, "__sync_", 7) ||
           !strncmp(s, "__atomic_", 9);
}

/* Is node i the callee of a call (not parenthesized)? */
static bool is_callee(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    return p != NO_NODE && ntag(c, p) == N_CALL && first_child(c, p) == i;
}

static void old_decl_note(Checker *c, Diagnostic *d, const CSym *s)
{
    const char *name = cident(c, s->name);
    if (s->flags & CSF_DEFINED)
        cnote(c, d, s->def_loc ? s->def_loc : s->loc,
              "previous definition of '%s' with type %s", name,
              type_q(TT, s->ty));
    else if (s->flags & CSF_IMPLICIT)
        cnote(c, d, s->loc, "previous implicit declaration of '%s' with type %s",
              name, type_q(TT, s->ty));
    else
        cnote(c, d, s->loc, "previous declaration of '%s' with type %s", name,
              type_q(TT, s->ty));
}

/* "incompatible implicit declaration of built-in function": the declaration
 * int() of a library built-in whose type differs. */
static void builtin_mismatch(Checker *c, SrcLoc loc, const BTab *bt)
{
    Diagnostic *d = cwarn_d(c, DL_WARNING, loc, DW_BUILTIN_DECLARATION_MISMATCH,
                            "incompatible implicit declaration of built-in "
                            "function '%s'", bt->name);
    if (d && bt->hdr[0])
        cheader_note(c, d, loc, bt->hdr,
                     "include '%s' or provide a declaration of '%s'",
                     bt->hdr, bt->name);
}

/* An implicit function declaration (C90; a C99 pedwarn). */
static void implicit_decl(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    uint32_t ref = id < c->nidents && c->ext[id] ? c->ext[id] - 1 : SYM_NONE;
    const BTab *bt = bt_find(c, name, false);
    csum_touch(c, SUM_EXT, id);
    CSym s;
    Diagnostic *d;
    if (ref != SYM_NONE && csym(c, ref)->kind == CS_FUNC) {
        CSym *o = csym(c, ref);
        if (!(o->flags & CSF_IMPLICIT)) {
            d = cpedwarn(c, loc, DW_IMPLICIT_FUNCTION_DECLARATION,
                         "implicit declaration of function '%s'", name);
            old_decl_note(c, d, o);
            o->flags |= CSF_IMPLICIT;
        } else if (bt && bt->mismatch)
            builtin_mismatch(c, loc, bt);
        cbind(c, NS_ORD, id, ref);
        o = csym(c, ref);
        c->ty[i] = o->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        if (c->sx)
            csx_sym(c, ref, c->nodes[i].tok, CIX_REF | CIX_CALL);
        return;
    }
    if (bt) {
        d = cpedwarn(c, loc, DW_IMPLICIT_FUNCTION_DECLARATION,
                     "implicit declaration of function '%s'", name);
        if (d && bt->hdr[0])
            cheader_note(c, d, loc, bt->hdr, "include '%s' or provide a "
                         "declaration of '%s'", bt->hdr, name);
        if (bt->mismatch)
            builtin_mismatch(c, loc, bt);
    } else {
        const char *hdr = std_header(name);
        const char *sug = hdr ? NULL : fuzzy_name(c, name, true);
        if (sug) {
            d = cpedwarn(c, loc, DW_IMPLICIT_FUNCTION_DECLARATION,
                         "implicit declaration of function '%s'; did you mean "
                         "'%s'?", name, sug);
            cfix_tok(c, d, 0, c->nodes[i].tok, sug, FIX_MAYBE);
        } else
            d = cpedwarn(c, loc, DW_IMPLICIT_FUNCTION_DECLARATION,
                         "implicit declaration of function '%s'", name);
        if (hdr)
            header_note(c, d, loc, name, hdr);
    }
    memset(&s, 0, sizeof s);
    s.name = id;
    s.kind = CS_FUNC;
    s.sc = SC_EXTERN;
    s.linkage = LK_EXTERNAL;
    s.flags = CSF_IMPLICIT | CSF_USED | CSF_CUSED;
    s.ty = bt && bt->mismatch ? bt_func_type(c, bt)
                              : type_func(TT, TYPE_B(INT), NULL, 0, TF_NOPROTO);
    s.loc = loc;
    ref = csym_new(c, true, &s);
    if (id < c->nidents && !c->ext[id])
        c->ext[id] = ref + 1;
    cbind(c, NS_ORD, id, ref);
    c->ty[i] = s.ty;
    c->ck[i] = K_ADDR;
    c->cb[i] = ref + 1;
    if (c->sx)
        csx_sym(c, ref, c->nodes[i].tok, CIX_REF | CIX_CALL);
}

/* Whether id was already reported undeclared in the current function. */
bool cexpr_undeclared_here(Checker *c, uint32_t id)
{
    uint64_t key = ((uint64_t)c->u->first_tok * UINT64_C(0x100000001)) ^
                   c->func_sym;
    size_t k;
    if (key != c->undecl_key)
        return false;
    for (k = 0; k < c->undecl.len; k++)
        if (c->undecl.data[k] == id)
            return true;
    return false;
}

static void undeclared_(Checker *c, uint32_t i, uint32_t id);

/* gcc diagnoses an undeclared name when it parses it, so one before the
 * unit's first syntax error is reported even though the unit is quiet. */
void undeclared(Checker *c, uint32_t i, uint32_t id)
{
    bool quiet = quiet_lift_before(c, c->nodes[i].tok);
    undeclared_(c, i, id);
    quiet_restore(c, quiet);
}

static void undeclared_(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    const char *hdr, *sug;
    Diagnostic *d;
    set_err(c, i);
    if (!in_function(c)) {
        CSym s;
        hdr = std_header(name);
        sug = hdr ? NULL : fuzzy_name(c, name, false);
        if (sug) {
            d = cerror_d(c, loc, "'%s' undeclared here (not in a function); "
                         "did you mean '%s'?", name, sug);
            cfix_tok(c, d, 0, c->nodes[i].tok, sug, FIX_MAYBE);
        } else
            d = cerror_d(c, loc, "'%s' undeclared here (not in a function)",
                         name);
        if (hdr)
            header_note(c, d, loc, name, hdr);
        memset(&s, 0, sizeof s);
        s.name = id;
        s.kind = CS_OBJ;
        s.flags = CSF_ERROR;
        s.ty = ERRT;
        s.loc = loc;
        cbind(c, NS_ORD, id, csym_new(c, cat_file_scope(c), &s));
        return;
    }
    {
        uint64_t key = ((uint64_t)c->u->first_tok * UINT64_C(0x100000001)) ^
                       c->func_sym;
        size_t k;
        if (key != c->undecl_key) {
            c->undecl_key = key;
            c->undecl.len = 0;
        }
        for (k = 0; k < c->undecl.len; k++)
            if (c->undecl.data[k] == id)
                return;
        vec_push(&c->undecl, id);
    }
    hdr = std_header(name);
    sug = hdr ? NULL : fuzzy_name(c, name, false);
    if (sug) {
        d = cerror_d(c, loc, "'%s' undeclared (first use in this function); "
                     "did you mean '%s'?", name, sug);
        cfix_tok(c, d, 0, c->nodes[i].tok, sug, FIX_MAYBE);
    } else
        d = cerror_d(c, loc, "'%s' undeclared (first use in this function)",
                     name);
    if (hdr)
        header_note(c, d, loc, name, hdr);
    if (d && !c->undecl_noted) {
        c->undecl_noted = true;
        cnote(c, d, loc, "each undeclared identifier is reported only once "
              "for each function it appears in");
    }
}

/* gcc's reject_gcc_builtin: a built-in function without a library fallback
 * (__builtin_trap, __sync_*, ...) may only be called, cast to void or
 * evaluated as a statement; anything that would take its address is an error.
 * The consumer of the value (through parentheses and '*') decides, and where
 * gcc reports: input_location, except an arithmetic, relational or equality
 * operand (the operand's own place for the left, the operator for the right)
 * and the right side of && and ||. */
static void reject_builtin(Checker *c, uint32_t i, const char *name)
{
    uint32_t n = i, p, k[2];
    SrcLoc loc = cinput_loc(c, c->nodes[i].tok);
    int op;
    if (!strncmp(name, "__builtin_", 10)) {
        const BTab *bt = bt_find(c, name + 10, true);
        if (bt && bt->gnu != 2)
            return;         /* has a library fallback */
    }
    for (;;) {
        p = c->par[n];
        if (p == NO_NODE)
            return;
        if (ntag(c, p) == N_PAREN ||
            (ntag(c, p) == N_UNARY && npunct(c, p) == P_STAR)) {
            n = p;
            continue;
        }
        break;
    }
    switch (ntag(c, p)) {
    case N_CALL:
        if (first_child(c, p) == n)
            return;
        break;
    case N_CAST:
        if (nkids(c, p, k, 2) < 2 || k[1] != n || is_void(c, type_of_typename(c, k[0])))
            return;
        break;
    case N_BINARY:
        op = npunct(c, p);
        if (op == P_COMMA || nkids(c, p, k, 2) < 2)
            return;
        if (k[0] != n)
            loc = cnode_loc(c, p);
        else if (op != P_ANDAND && op != P_OROR)
            loc = cnode_loc(c, n);
        break;
    case N_ASSIGN:
        if (nkids(c, p, k, 2) < 2 || k[1] != n)
            return;
        if (npunct(c, p) != P_ASSIGN)
            loc = cnode_loc(c, n);
        break;
    case N_UNARY: case N_COND: case N_RETURN: case N_IF: case N_WHILE:
    case N_DO: case N_SWITCH: case N_FOR: case N_INIT_DECL:
    case N_INIT_LIST: case N_DESIGNATED:
        break;
    default:
        return;
    }
    if (!strcmp(name, "__builtin_complex"))
        cerror(c, ctok_loc(c, last_tok(c, i) + 1), "cannot take address of '%s'", name);
    else
        cerror(c, loc, "built-in function '%s' must be directly called", name);
}

/* An operand of __builtin_has_attribute names a function without using it. */

static bool in_has_attr(const Checker *c, uint32_t i)
{
    unsigned depth;
    for (depth = 0; depth < 8; depth++) {
        uint32_t p = c->par[i];
        if (p == NO_NODE)
            return false;
        switch (ntag(c, p)) {
        case N_HAS_ATTR:
            return true;
        case N_PAREN: case N_MEMBER_EXPR: case N_INDEX: case N_UNARY:
        case N_CAST: case N_BINARY:
            i = p;
            break;
        default:
            return false;
        }
    }
    return false;
}

/* Whether the arguments of attribute item p are expressions (cleanup's
 * function name too: a use for the symbol index). */
static bool attr_takes_expr(Checker *c, uint32_t p, bool cleanup)
{
    size_t al, q;
    const char *an = ttext(c, c->nodes[p].tok, &al);
    static const char *const ex[] = {
        "nonnull", "aligned", "vector_size", "warn_if_not_aligned",
        "alloc_size", "alloc_align", "assume_aligned", "malloc",
        "fallthrough", "constructor", "destructor", "format_arg"};
    if (al > 4 && !strncmp(an, "__", 2) && !strncmp(an + al - 2, "__", 2)) {
        an += 2;
        al -= 4;
    }
    if (cleanup && al == 7 && !strncmp(an, "cleanup", 7))
        return true;
    for (q = 0; q < sizeof ex / sizeof *ex; q++)
        if (strlen(ex[q]) == al && !strncmp(an, ex[q], al))
            return true;
    return false;
}


void e_ident(Checker *c, uint32_t i)
{
    uint32_t id = cnode_ident(c, i), ref = lookup_ord(c, id);
    const char *name = cident(c, id);
    CSym *s;
    c->sym[i] = ref;
    if (ref == SYM_NONE) {
        uint32_t p = c->par[i];
        if (!strcmp(name, "__func__") || !strcmp(name, "__FUNCTION__") ||
            !strcmp(name, "__PRETTY_FUNCTION__")) {
            size_t n = 0;
            if (strcmp(name, "__func__"))
                cpedantic(c, cnode_loc(c, i), "ISO C does not support '%s' "
                          "predefined identifier", name);
            if (in_function(c))
                n = strlen(cident(c, csym(c, c->func_sym)->name));
            else if (!strcmp(name, "__func__"))
                cpedwarn(c, cnode_loc(c, i), DW_NONE,
                         "'%s' is not defined outside of function scope",
                         name);
            c->ty[i] = type_array(TT, type_qual(TYPE_B(CHAR), TQ_CONST), n + 1);
            c->ef[i] = EF_LVALUE | EF_ADDRLV;
            c->cb[i] = CB_NODE | i;
            c->ck[i] = K_ADDR;
            return;
        }
        if (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM &&
            strncmp(name, "__builtin_", 10) && attr_takes_expr(c, p, false)) {
            undeclared(c, i, id);
            return;
        }
        if (!strncmp(name, "__builtin_", 10)) {
            /* a built-in with a library counterpart has that function's type;
             * one without (gnu = 2) only when called, since any other use is
             * rejected below */
            const BTab *bt = bt_find(c, name + 10, true);
            TypeId ft = bt && (bt->gnu != 2 || is_callee(c, i))
                            ? bt_func_type(c, bt)
                            : overflow_func_type(c, name + 10);
            /* the table lists no parameters for a built-in gcc checks by hand
             * (__builtin_classify_type, __atomic_is_lock_free, ...) */
            if (bt && bt->gnu == 2 && !is_err(c, ft) &&
                !*(strchr(bt->sig, '|') + 1))
                ft = type_func(TT, type_base(TT, ft), NULL, 0, TF_NOPROTO);
            if (!is_err(c, ft)) {
                c->ty[i] = ft;
                c->ck[i] = K_ADDR;
                c->ef[i] = EF_ADDRLV;
                return;
            }
        }
        if (is_callee(c, i) && !strncmp(name, "__builtin___", 12) &&
            !builtin3_known(name + 12)) {     /* any other is implicit */
            implicit_decl(c, i, id);
            return;
        }
        if (builtin_name(name) || (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM)) {
            if (builtin_name(name))
                reject_builtin(c, i, name);
            set_err(c, i);
            return;
        }
        if (is_callee(c, i)) {
            implicit_decl(c, i, id);
            return;
        }
        undeclared(c, i, id);
        return;
    }
    s = csym(c, ref);
    if (s->auto_pend) {
        cerror(c, cnode_loc(c, i), "underspecified '%s' referenced in its "
               "initializer", name);
        set_err(c, i);
        return;
    }
    if (c->sx && (c->par[i] == NO_NODE || ntag(c, c->par[i]) != N_ATTR_ITEM ||
                  attr_takes_expr(c, c->par[i], true)))
        csx_sym(c, ref, c->nodes[i].tok,
                CIX_REF | (call_callee(c, i) ? CIX_CALL : 0));
    /* gcc marks a variable named in __builtin_has_attribute used; a function
     * stays unused ("declared static but never defined") */
    if ((s->kind != CS_FUNC || !in_has_attr(c, i)) &&
        !(s->kind == CS_OBJ && is_err(c, s->ty))) {   /* gcc: an erroneous type is no use */
        s->flags |= CSF_USED;
        if (s->kind == CS_FUNC && sizeof_outer(c, i) == NO_NODE)
            s->flags |= CSF_CUSED;
    }
    if (c->func_sym != SYM_NONE && !(ref & SYM_LOCAL) &&
        (s->kind == CS_OBJ || s->kind == CS_FUNC) && s->linkage == LK_INTERNAL)
        cdecl_record_inline_static(c, ctok_loc(c, c->nodes[i].tok), s->name,
                                   false);
    cdep_use(c, cinput_loc(c, c->nodes[i].tok), s, &s->loc);
    switch (s->kind) {
    case CS_TYPEDEF:
        set_err(c, i);
        return;
    case CS_ENUMCONST:
        if ((s->flags & CSF_IN_STRUCT) && !c->recs.len)
            cxx_in_struct_use(c, ctok_loc(c, c->nodes[i].tok),
                              "enum constant", "enum constant", s->loc);
        c->ty[i] = s->vty ? s->vty : s->ty;
        if (is_err(c, c->ty[i])) {
            set_err(c, i);
            return;
        }
        c->ck[i] = K_ICE;
        c->cv[i] = cexpr_trunc(c, c->ty[i], s->val);
        c->ef[i] = EF_INTOPS;
        return;
    case CS_FUNC:
        c->ty[i] = cbind_type(c, id) ? cbind_type(c, id) - 1 : s->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        c->ef[i] = EF_ADDRLV;
        return;
    default:
        break;
    }
    c->ty[i] = cbind_type(c, id) ? cbind_type(c, id) - 1 : s->ty;
    if (is_err(c, s->ty)) {
        set_err(c, i);
        return;
    }
    if (s->cx == 2 && value_use(c, i)) {
        constexpr_read(c, i, s);
        return;
    }
    c->ef[i] = EF_LVALUE;
    if (s->sc == SC_REGISTER) {
        c->ef[i] |= EF_REGISTER;
        if (type_ckind(TT, c->ty[i]) == TY_ARRAY) {
            /* c_mark_addressable on the array-to-pointer conversion */
            uint32_t p = c->par[i];
            while (p != NO_NODE && ntag(c, p) == N_PAREN)
                p = c->par[p];
            if (p == NO_NODE ||
                !(ntag(c, p) == N_SIZEOF_EXPR || ntag(c, p) == N_ALIGNOF_EXPR ||
                  ntag(c, p) == N_TYPEOF || ntag(c, p) == N_INDEX ||
                  (ntag(c, p) == N_UNARY && npunct(c, p) == P_AMP)))
                cerror(c, cinput_loc(c, last_tok(c, i) + 1),
                       "address of register variable '%s' requested",
                       cident(c, id));
        }
    }
    if (tquals(c, s->ty) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (!(s->flags & CSF_THREAD) &&
        (!(ref & SYM_LOCAL) || s->sc == SC_STATIC || s->sc == SC_EXTERN ||
         s->linkage != LK_NONE)) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = ref + 1;
        addr_rvalue(c, i);
    }
}

