/* pcfam.c - the family table: OS, compiler and data-model leaves (the
 * target), the language standard and its mode (the language field), and
 * their macros. */
#include "pcfam.h"

#include <string.h>

enum { AX_OS, AX_CC, AX_DM, AX_STD, AX_MODE, AX_END, NAX };

#define MAXLEAF 5
static const char *const axis_name[NAX] = {"os", "cc", "dm", "std", "mode", "endian"};
static const int axis_n[NAX] = {5, 5, 3, 5, 2, 2};
static const char *const leaf_name[NAX][MAXLEAF] = {
    {"linux", "android", "windows", "apple", "other"},
    {"gcc", "clang", "msvc", "clang-cl", "other"},
    {"lp64", "ilp32", "llp64"},
    {"c89", "c99", "c11", "c17", "c23"},
    {"gnu", "iso"},
    {"little", "big"},
};
enum { L_LINUX, L_ANDROID, L_WINDOWS, L_APPLE };
enum { C_GCC, C_CLANG, C_MSVC, C_CLANGCL };
enum { D_LP64, D_ILP32, D_LLP64 };
enum { S_C89, S_C99, S_C11, S_C17, S_C23 };
enum { M_GNU, M_ISO };
enum { E_LITTLE, E_BIG };

#define B(i) (1u << (i))

/* A macro of the table: the leaves of one axis that define it, and those that
 * do not (the others, an "other" leaf or a compiler that also defines it,
 * leave it free); and, for some, its value when defined, under the leaves of
 * an axis that fix it. */
typedef struct Row {
    const char *name;
    int axis;
    unsigned must, never;
    int vaxis;
    unsigned has;           /* leaves of vaxis with a known value */
    intmax_t val[MAXLEAF];
    intmax_t range;         /* its value is below this (a version digit), or 0 */
} Row;

#define LINUXISH (B(L_LINUX) | B(L_ANDROID))
#define CLANGISH (B(C_CLANG) | B(C_CLANGCL))
#define GNUISH (B(C_GCC) | CLANGISH)
#define ALL 31u

/* defined or not, by the leaves of one axis; no value */
#define FLAG(n, ax, must, never) {n, ax, must, never, AX_OS, 0u, {0}, 0}
/* defined by gcc and clang, with the same value everywhere */
#define GNU_CONST(n, v) \
    {n, AX_CC, GNUISH, B(C_MSVC), AX_CC, ALL, {v, v, v, v, v}, 0}
/* defined by gcc and clang, valued by the data model */
#define GNU_DM(n, lp64, ilp32, llp64) \
    {n, AX_CC, GNUISH, B(C_MSVC), AX_DM, 7u, {lp64, ilp32, llp64}, 0}
/* defined by gcc and clang (not clang-cl), with a known value under clang
 * alone; rng: a digit of a version number */
#define GNU_CLANG(n, v, rng) \
    {n, AX_CC, B(C_GCC) | B(C_CLANG), B(C_MSVC) | B(C_CLANGCL), AX_CC, \
     B(C_CLANG), {0, v}, rng}
/* defined by clang and clang-cl, a digit of the version number */
#define CLANG_DIGIT(n, rng) \
    {n, AX_CC, CLANGISH, B(C_GCC) | B(C_MSVC), AX_OS, 0u, {0}, rng}
/* defined by gcc and clang, valued by the byte order */
#define GNU_END(n)     {n, AX_CC, GNUISH, B(C_MSVC), AX_END, 3u, {1234, 4321}, 0}
#define I64 9223372036854775807
#define I32 2147483647

static const Row rows[] = {
    FLAG("__linux__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)),
    FLAG("__linux", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)),
    FLAG("__gnu_linux__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)),
    FLAG("__unix__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)),
    FLAG("__unix", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)),
    FLAG("__ELF__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)),
    FLAG("__ANDROID__", AX_OS, B(L_ANDROID),
         B(L_LINUX) | B(L_WINDOWS) | B(L_APPLE)),
    FLAG("_WIN32", AX_OS, B(L_WINDOWS), LINUXISH | B(L_APPLE)),
    FLAG("__APPLE__", AX_OS, B(L_APPLE), LINUXISH | B(L_WINDOWS)),
    FLAG("__MACH__", AX_OS, B(L_APPLE), LINUXISH | B(L_WINDOWS)),
    GNU_CLANG("__GNUC__", 4, 0),
    GNU_CLANG("__GNUC_MINOR__", 2, 100),
    GNU_CLANG("__GNUC_PATCHLEVEL__", 1, 100),
    CLANG_DIGIT("__clang_major__", 0),
    CLANG_DIGIT("__clang_minor__", 100),
    CLANG_DIGIT("__clang_patchlevel__", 100),
    FLAG("__clang__", AX_CC, B(C_CLANG) | B(C_CLANGCL), B(C_GCC) | B(C_MSVC)),
    FLAG("_MSC_VER", AX_CC, B(C_MSVC) | B(C_CLANGCL), B(C_GCC) | B(C_CLANG)),
    /* the data model */
    FLAG("__LP64__", AX_DM, B(D_LP64), B(D_ILP32) | B(D_LLP64)),
    FLAG("_LP64", AX_DM, B(D_LP64), B(D_ILP32) | B(D_LLP64)),
    FLAG("_WIN64", AX_DM, B(D_LLP64), B(D_LP64) | B(D_ILP32)),
    /* the language: C89 has no __STDC_VERSION__ (an MSVC without /std lacks it
     * too, which the table leaves out); ISO mode sets __STRICT_ANSI__ */
    {"__STDC_VERSION__", AX_STD, B(S_C99) | B(S_C11) | B(S_C17) | B(S_C23),
     B(S_C89), AX_STD, B(S_C99) | B(S_C11) | B(S_C17) | B(S_C23),
     {0, 199901, 201112, 201710, 202311}, 0},
    FLAG("__STRICT_ANSI__", AX_MODE, B(M_ISO), B(M_GNU)),
    /* the feature-test macros: clang has them all, MSVC has none of these
     * (gcc has them from 5, 10 or 14 on, so it stays free) */
    FLAG("__has_attribute", AX_CC, CLANGISH, B(C_MSVC)),
    FLAG("__has_builtin", AX_CC, CLANGISH, B(C_MSVC)),
    FLAG("__has_feature", AX_CC, CLANGISH, B(C_MSVC)),
    FLAG("__has_extension", AX_CC, CLANGISH, B(C_MSVC)),
    FLAG("__has_warning", AX_CC, CLANGISH, B(C_MSVC)),
    FLAG("__has_include", AX_CC, CLANGISH, 0),
    FLAG("__has_include_next", AX_CC, CLANGISH, 0),
    /* limits and sizes of the types */
    GNU_CONST("__CHAR_BIT__", 8),
    GNU_CONST("__SCHAR_MAX__", 127),
    GNU_CONST("__SHRT_MAX__", 32767),
    GNU_CONST("__INT_MAX__", I32),
    GNU_CONST("__LONG_LONG_MAX__", I64),
    GNU_CONST("__SIZEOF_SHORT__", 2),
    GNU_CONST("__SIZEOF_INT__", 4),
    GNU_CONST("__SIZEOF_LONG_LONG__", 8),
    GNU_CONST("__SIZEOF_FLOAT__", 4),
    GNU_CONST("__SIZEOF_DOUBLE__", 8),
    GNU_CONST("__FLT_RADIX__", 2),
    GNU_CONST("__FLT_MANT_DIG__", 24),
    GNU_CONST("__DBL_MANT_DIG__", 53),
    GNU_CONST("__FLT_DIG__", 6),
    GNU_CONST("__DBL_DIG__", 15),
    GNU_DM("__LONG_MAX__", I64, I32, I32),
    GNU_DM("__SIZEOF_LONG__", 8, 4, 4),
    GNU_DM("__SIZEOF_POINTER__", 8, 4, 8),
    GNU_DM("__SIZEOF_SIZE_T__", 8, 4, 8),
    GNU_DM("__SIZEOF_PTRDIFF_T__", 8, 4, 8),
    GNU_DM("__INTPTR_MAX__", I64, I32, I64),
    GNU_DM("__PTRDIFF_MAX__", I64, I32, I64),
    /* the byte order */
    GNU_END("__BYTE_ORDER__"),
    GNU_CONST("__ORDER_LITTLE_ENDIAN__", 1234),
    GNU_CONST("__ORDER_BIG_ENDIAN__", 4321),
    GNU_CONST("__ORDER_PDP_ENDIAN__", 3412),
};
#define NROWS (sizeof rows / sizeof rows[0])

static const Row *row_of(const char *name)
{
    size_t i;
    for (i = 0; i < NROWS; i++)
        if (!strcmp(rows[i].name, name))
            return &rows[i];
    return NULL;
}

bool pc_fam_macro(const char *name)
{
    return row_of(name) != NULL;
}

intmax_t pc_fam_range(const char *name)
{
    const Row *row = row_of(name);
    return row ? row->range : 0;
}

static int axis_of(const PcAtom *at)
{
    int a;
    for (a = 0; a < NAX; a++)
        if (!strcmp(at->text, axis_name[a]))
            return a;
    return AX_OS;
}

const char *pc_leaf_name(const PcAtom *at)
{
    return leaf_name[axis_of(at)][at->c];
}

int pc_leaf_axis(const PcAtom *at)
{
    return axis_of(at);
}

bool pc_leaf_residual(const PcAtom *at)
{
    return !strcmp(pc_leaf_name(at), "other");
}

PcClass pc_axis_class(const char *axis)
{
    return !strcmp(axis, "std") || !strcmp(axis, "mode") ? PC_LANG : PC_TARGET;
}

/* the blocks of the axes come in table order wherever the first use is */
static Bdd leaf(PcCtx *cx, int axis, int i)
{
    int a;
    for (a = 0; a < axis; a++)
        pc_atom(cx, PA_LEAF, NULL, 0, axis_name[a]);
    return pc_atom(cx, PA_LEAF, NULL, i, axis_name[axis]);
}

bool pc_fam_values(PcCtx *cx, Ident *id, Bdd def, PcEntries *out)
{
    const Row *row = row_of(id->str);
    BddMgr *m = &cx->m;
    Bdd rest = def;
    PcEntry e = {BDD_FALSE, true, 0, NULL, NULL, NULL};
    int l, n;
    bool uniform;
    if (!row || !row->has)
        return false;
    n = axis_n[row->vaxis];
    uniform = row->has == (1u << n) - 1;
    for (l = 1; l < n; l++)
        uniform &= row->val[l] == row->val[0];
    if (uniform) {                      /* no leaf decides it */
        e.cond = def;
        e.n = row->val[0];
        vec_push(out, e);
        return true;
    }
    for (l = 0; l < n; l++) {
        if (!(row->has & B(l)))
            continue;
        e.cond = bdd_and(m, def, leaf(cx, row->vaxis, l));
        e.n = row->val[l];
        vec_push(out, e);
        rest = bdd_and(m, rest, bdd_not(m, leaf(cx, row->vaxis, l)));
    }
    if (row->has != (1u << n) - 1) {    /* the other leaves: the macro's own value */
        e.cond = rest;
        e.known = false;
        e.n = 0;
        vec_push(out, e);
    }
    return true;
}

Bdd pc_family(PcCtx *cx, Bdd f)
{
    BddMgr *m = &cx->m;
    BddVars vs = {0};
    size_t i, r;
    int a, l, l2;
    bool touched[NAX] = {false};
    Bdd g = f;
    bdd_vars(m, f, &vs);
    for (i = 0; i < vs.len; i++) {
        const PcAtom *at = pc_atom_at(cx, vs.data[i]);
        const Row *row;
        if (at && at->kind == PA_LEAF)
            touched[axis_of(at)] = true;
        else if (at && at->id && (row = row_of(at->id->str)))
            touched[row->axis] = true;
    }
    if (touched[AX_DM])         /* the data model is tied to the OS */
        touched[AX_OS] = true;
    if (touched[AX_END])        /* Windows is little-endian */
        touched[AX_OS] = true;
    if (touched[AX_OS] || touched[AX_CC])
        touched[AX_OS] = touched[AX_CC] = true;     /* msvc builds for windows */
    for (a = 0; a < NAX && !touched[a]; a++)
        ;
    if (a == NAX) {
        vec_free(&vs);
        return f;
    }
    for (a = 0; a < NAX; a++) {
        Bdd any = BDD_FALSE;
        if (!touched[a])
            continue;
        for (l = 0; l < axis_n[a]; l++) {
            any = bdd_or(m, any, leaf(cx, a, l));
            for (l2 = l + 1; l2 < axis_n[a]; l2++)
                g = bdd_and(m, g, bdd_not(m, bdd_and(m, leaf(cx, a, l),
                                                     leaf(cx, a, l2))));
        }
        g = bdd_and(m, g, any);
    }
    /* clang-cl and MSVC build for Windows */
    if (touched[AX_CC])
        for (l = C_MSVC; l <= C_CLANGCL; l++)
            g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, AX_CC, l)),
                                     leaf(cx, AX_OS, L_WINDOWS)));
    if (touched[AX_DM]) {       /* Windows is not LP64, LLP64 is Windows */
        g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, AX_DM, D_LP64)),
                                 bdd_not(m, leaf(cx, AX_OS, L_WINDOWS))));
        g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, AX_DM, D_LLP64)),
                                 leaf(cx, AX_OS, L_WINDOWS)));
    }
    if (touched[AX_END])
        g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, AX_END, E_BIG)),
                                 bdd_not(m, leaf(cx, AX_OS, L_WINDOWS))));
    for (i = 0; i < vs.len; i++) {
        const PcAtom *at = pc_atom_at(cx, vs.data[i]);
        const Row *row;
        Bdd d;
        bool seen = false;
        if (!at || !at->id || !(row = row_of(at->id->str)))
            continue;
        for (r = 0; r < i && !seen; r++) {
            const PcAtom *b = pc_atom_at(cx, vs.data[r]);
            seen = b && b->id == at->id;
        }
        if (seen)
            continue;
        d = pc_atom(cx, PA_DEF, at->id, 0, NULL);
        for (l = 0; l < axis_n[row->axis]; l++) {
            if (row->must & B(l))
                g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, row->axis, l)),
                                         d));
            if (row->never & B(l))
                g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, row->axis, l)),
                                         bdd_not(m, d)));
        }
    }
    vec_free(&vs);
    return g;
}

static bool defd(const PcKeyHost *h, const char *s)
{
    return h->defined(h->ctx, s, strlen(s));
}

bool pc_leaf_host(const PcAtom *at, const PcKeyHost *h)
{
    int host;
    switch (axis_of(at)) {
    case AX_OS:
        host = defd(h, "__ANDROID__") ? L_ANDROID
             : defd(h, "__linux__") ? L_LINUX
             : defd(h, "_WIN32") ? L_WINDOWS
             : defd(h, "__APPLE__") ? L_APPLE : 4;
        break;
    case AX_CC:
        host = defd(h, "_MSC_VER") ? (defd(h, "__clang__") ? C_CLANGCL : C_MSVC)
             : defd(h, "__clang__") ? C_CLANG
             : defd(h, "__GNUC__") ? C_GCC : 4;
        break;
    case AX_STD: {
        intmax_t v = 0;
        if (!h->value(h->ctx, "__STDC_VERSION__", 16, &v))
            v = 0;
        host = v >= 202000 ? S_C23 : v >= 201700 ? S_C17 : v >= 201100 ? S_C11
             : v >= 199900 ? S_C99 : S_C89;
        break;
    }
    case AX_MODE:
        host = defd(h, "__STRICT_ANSI__") ? M_ISO : M_GNU;
        break;
    case AX_END: {
        intmax_t v = 0;
        if (!h->value(h->ctx, "__BYTE_ORDER__", 14, &v))
            v = 1234;
        host = v == 4321 ? E_BIG : E_LITTLE;
        break;
    }
    default:
        host = defd(h, "_WIN64") ? D_LLP64
             : defd(h, "__LP64__") || defd(h, "_LP64") ? D_LP64 : D_ILP32;
        break;
    }
    return host == (int)at->c;
}
