/* pcfam.c - the family table: OS and compiler leaves and their macros. */
#include "pcfam.h"

#include <string.h>

enum { AX_OS, AX_CC, NAX };

static const char *const axis_name[NAX] = {"os", "cc"};
#define NLEAF 5
static const char *const leaf_name[NAX][NLEAF] = {
    {"linux", "android", "windows", "apple", "other"},
    {"gcc", "clang", "msvc", "clang-cl", "other"},
};
enum { L_LINUX, L_ANDROID, L_WINDOWS, L_APPLE };
enum { C_GCC, C_CLANG, C_MSVC, C_CLANGCL };

#define B(i) (1u << (i))

/* the leaves of one axis that define the macro, and those that do not; the
 * others (an "other" leaf, a compiler that also defines it) leave it free */
typedef struct Row {
    const char *name;
    int axis;
    unsigned must, never;
} Row;

#define LINUXISH (B(L_LINUX) | B(L_ANDROID))
static const Row rows[] = {
    {"__linux__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)},
    {"__linux", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)},
    {"__gnu_linux__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)},
    {"__unix__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)},
    {"__unix", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)},
    {"__ELF__", AX_OS, LINUXISH, B(L_WINDOWS) | B(L_APPLE)},
    {"__ANDROID__", AX_OS, B(L_ANDROID),
     B(L_LINUX) | B(L_WINDOWS) | B(L_APPLE)},
    {"_WIN32", AX_OS, B(L_WINDOWS), LINUXISH | B(L_APPLE)},
    {"__APPLE__", AX_OS, B(L_APPLE), LINUXISH | B(L_WINDOWS)},
    {"__MACH__", AX_OS, B(L_APPLE), LINUXISH | B(L_WINDOWS)},
    {"__GNUC__", AX_CC, B(C_GCC) | B(C_CLANG), B(C_MSVC) | B(C_CLANGCL)},
    {"__clang__", AX_CC, B(C_CLANG) | B(C_CLANGCL), B(C_GCC) | B(C_MSVC)},
    {"_MSC_VER", AX_CC, B(C_MSVC) | B(C_CLANGCL), B(C_GCC) | B(C_CLANG)},
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

const char *pc_leaf_name(const PcAtom *at)
{
    return leaf_name[strcmp(at->text, "os") ? AX_CC : AX_OS][at->c];
}

static Bdd leaf(PcCtx *cx, int axis, int i)
{
    return pc_atom(cx, PA_LEAF, NULL, i, axis_name[axis]);
}

Bdd pc_family(PcCtx *cx, Bdd f)
{
    BddMgr *m = &cx->m;
    BddVars vs = {0};
    size_t i, r;
    int a, l, l2;
    bool touched[NAX] = {false, false};
    Bdd g = f;
    bdd_vars(m, f, &vs);
    for (i = 0; i < vs.len; i++) {
        const PcAtom *at = pc_atom_at(cx, vs.data[i]);
        if (at && at->kind == PA_LEAF)
            touched[strcmp(at->text, "os") ? AX_CC : AX_OS] = true;
        else if (at && at->id && row_of(at->id->str))
            touched[row_of(at->id->str)->axis] = true;
    }
    if (!touched[AX_OS] && !touched[AX_CC])
        return f;
    touched[AX_OS] = true;      /* the cross-axis clauses need both */
    touched[AX_CC] = true;
    for (a = 0; a < NAX; a++) {
        Bdd any = BDD_FALSE;
        for (l = 0; l < NLEAF; l++) {
            any = bdd_or(m, any, leaf(cx, a, l));
            for (l2 = l + 1; l2 < NLEAF; l2++)
                g = bdd_and(m, g, bdd_not(m, bdd_and(m, leaf(cx, a, l),
                                                     leaf(cx, a, l2))));
        }
        g = bdd_and(m, g, any);
    }
    /* clang-cl and MSVC build for Windows */
    for (l = C_MSVC; l <= C_CLANGCL; l++)
        g = bdd_and(m, g, bdd_or(m, bdd_not(m, leaf(cx, AX_CC, l)),
                                 leaf(cx, AX_OS, L_WINDOWS)));
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
        for (l = 0; l < NLEAF; l++) {
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
    if (!strcmp(at->text, "os"))
        host = defd(h, "__ANDROID__") ? L_ANDROID
             : defd(h, "__linux__") ? L_LINUX
             : defd(h, "_WIN32") ? L_WINDOWS
             : defd(h, "__APPLE__") ? L_APPLE : 4;
    else
        host = defd(h, "_MSC_VER") ? (defd(h, "__clang__") ? C_CLANGCL : C_MSVC)
             : defd(h, "__clang__") ? C_CLANG
             : defd(h, "__GNUC__") ? C_GCC : 4;
    return host == (int)at->c;
}
