/* ccall.c - calls and what they pull in: built-in function signatures and
 * nonnull/restrict/atomic/tgmath argument checks, e_call, e_index, e_member
 * (c-typeck.cc build_function_call_vec, convert_arguments, ...).  Split out
 * of cexpr.c; shares its helpers through cexpr_int.h. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* gcc 13's built-in library functions that carry the nonnull attribute:
 * the argument numbers, one digit each (probed from gcc itself). */
static uint64_t builtin_nonnull(const char *name)
{
    static const struct { const char *n, *pos; } t[] = {
        {"bcmp", "12"}, {"bcopy", "12"}, {"bzero", "1"}, {"fprintf", "12"},
        {"fputc", "2"}, {"fputs", "12"}, {"fscanf", "2"}, {"fwrite", "14"},
        {"index", "1"}, {"memchr", "1"}, {"memcmp", "12"}, {"memcpy", "12"},
        {"memmove", "12"}, {"mempcpy", "12"}, {"memset", "1"}, {"nan", "1"},
        {"nanf", "1"}, {"nanl", "1"}, {"nans", "1"}, {"nansf", "1"},
        {"nansl", "1"}, {"printf", "1"}, {"putc", "2"},
        {"puts_unlocked", "1"}, {"fputc_unlocked", "2"},
        {"fputs_unlocked", "12"}, {"fwrite_unlocked", "14"},
        {"printf_unlocked", "1"}, {"fprintf_unlocked", "12"},
        {"putc_unlocked", "2"},
        {"puts", "1"}, {"rindex", "1"}, {"scanf", "1"}, {"snprintf", "3"},
        {"sprintf", "12"}, {"sscanf", "2"}, {"stpcpy", "12"},
        {"stpncpy", "12"}, {"strcasecmp", "12"}, {"strcat", "12"},
        {"strchr", "1"}, {"strcmp", "12"}, {"strcpy", "12"},
        {"strcspn", "12"}, {"strdup", "1"}, {"strftime", "3"},
        {"strlen", "1"}, {"strncasecmp", "12"}, {"strncat", "12"},
        {"strncmp", "12"}, {"strncpy", "12"}, {"strndup", "1"},
        {"strpbrk", "12"}, {"strrchr", "1"}, {"strspn", "12"},
        {"strstr", "12"}};
    size_t k;
    uint64_t m = 0;
    const char *p;
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    /* {add,sub,mul}_overflow: the result pointer */
    if ((!strncmp(name, "add", 3) || !strncmp(name, "sub", 3) ||
         !strncmp(name, "mul", 3)) && !strcmp(name + 3, "_overflow"))
        return 4;
    /* {s,u}{add,sub,mul}{,l,ll}_overflow: the result pointer */
    if ((*name == 's' || *name == 'u') &&
        (!strncmp(name + 1, "add", 3) || !strncmp(name + 1, "sub", 3) ||
         !strncmp(name + 1, "mul", 3))) {
        const char *q = name + 4;
        if (!strncmp(q, "ll", 2))
            q += 2;
        else if (*q == 'l')
            q++;
        if (!strcmp(q, "_overflow"))
            return 4;
    }
    for (k = 0; k < sizeof t / sizeof *t; k++)
        if (*name == *t[k].n && !strcmp(name, t[k].n)) {
            for (p = t[k].pos; *p; p++)
                m |= (uint64_t)1 << (*p - '1');
            break;
        }
    return m;
}

/* gcc's match_builtin_function_types, as probed from gcc 13.  Compare the
 * declared type n with the built-in's o in one position (ret: the return
 * type): 0 equal, 1 a mismatch gcc only warns about ("mismatch in argument N
 * type"), 2 a conflict.  A FILE * parameter of the table is a void * that
 * accepts any pointer. */
static int bt_cmp(Checker *c, TypeId o, TypeId n, bool ret, bool file)
{
    TypeId uo = unqual(c, o), un = unqual(c, n);
    if (is_ptr(c, o) || is_ptr(c, n)) {
        TypeId po, pn;
        if (!is_ptr(c, o) || !is_ptr(c, n))
            return 2;
        if (file)
            return 0;
        po = pointee(c, o);
        pn = pointee(c, n);
        if (type_compatible(TT, po, pn))
            return 0;
        if (ret || type_compatible(TT, unqual(c, po), unqual(c, pn)))
            return 1;
        return 2;
    }
    if (type_compatible(TT, uo, un))
        return 0;
    if (is_int(c, o) && is_int(c, n) && tkind(c, uo) != TY_BOOL &&
        tkind(c, un) != TY_BOOL) {
        bool ok1 = true, ok2 = true;
        return type_size(TT, uo, &ok1) == type_size(TT, un, &ok2) ? 1 : 2;
    }
    return 2;
}

/* Is parameter j (0-based) of the library built-in a FILE * or struct tm *
 * (a void * to gcc until the type is declared)? */
bool extra_on(Checker *c);
static bool bt_file_param(const char *name, uint32_t j)
{
    static const struct { const char *n; unsigned char j; } t[] = {
        {"fprintf", 0}, {"fscanf", 0}, {"vfprintf", 0}, {"vfscanf", 0},
        {"fputc", 1}, {"fputs", 1}, {"putc", 1}, {"fwrite", 3},
        {"fprintf_unlocked", 0}, {"fputc_unlocked", 1},
        {"fputs_unlocked", 1}, {"fwrite_unlocked", 3},
        {"putc_unlocked", 1}, {"strftime", 3}};
    size_t k;
    for (k = 0; k < sizeof t / sizeof *t; k++)
        if (t[k].j == j && !strcmp(t[k].n, name))
            return true;
    return false;
}

typedef struct BtMatch {
    bool conflict;           /* gcc: "conflicting types for built-in function" */
    int soft;                /* the first soft mismatch: 0 none, 1 return, j + 2 */
    TypeId bft, dft;
} BtMatch;

static BtMatch bt_match(Checker *c, const BTab *bt, TypeId declty)
{
    BtMatch m;
    uint32_t n, j;
    const TypeId *bp, *dp;
    int r;
    memset(&m, 0, sizeof m);
    m.dft = type_canon(TT, declty);
    m.bft = type_canon(TT, bt_func_type(c, bt));
    if (type_ent(TT, m.dft)->kind != TY_FUNC ||
        type_ent(TT, m.bft)->kind != TY_FUNC)
        return m;
    r = bt_cmp(c, type_base(TT, m.bft), type_base(TT, m.dft), true, false);
    if (r == 2) {
        m.conflict = true;
        return m;
    }
    if (r)
        m.soft = 1;
    if (type_ent(TT, m.dft)->flags & TF_NOPROTO) {
        m.conflict = (type_ent(TT, m.bft)->flags & TF_VARIADIC) != 0;
        bp = type_params(TT, m.bft);
        for (j = 0; j < (uint32_t)type_ent(TT, m.bft)->n; j++) {
            TypeId pj = unqual(c, bp[j]);
            if ((is_flt(c, pj) && tkind(c, pj) == TY_FLOAT) ||
                (is_int(c, pj) && mainv(c, type_int_promote(TT, pj)) != mainv(c, pj)))
                m.conflict = true;     /* the argument would be promoted */
        }
        return m;
    }
    n = (uint32_t)type_ent(TT, m.dft)->n;
    if (n != (uint32_t)type_ent(TT, m.bft)->n ||
        (type_ent(TT, m.dft)->flags & TF_VARIADIC) !=
            (type_ent(TT, m.bft)->flags & TF_VARIADIC)) {
        m.conflict = true;
        return m;
    }
    dp = type_params(TT, m.dft);
    bp = type_params(TT, m.bft);
    for (j = 0; j < n; j++) {
        r = bt_cmp(c, bp[j], dp[j], false, bt_file_param(bt->name, j));
        if (r == 2) {
            m.conflict = true;
            return m;
        }
        if (r && !m.soft)
            m.soft = (int)j + 2;
    }
    return m;
}

static const BTab *bt_for_decl(Checker *c, const CSym *s)
{
    const char *n = cident(c, s->name);
    return bt_find(c, !strncmp(n, "__builtin_", 10) ? n + 10 : n,
                   !strncmp(n, "__builtin_", 10));
}

static bool builtin_decl_ok(Checker *c, const CSym *s)
{
    const BTab *bt = bt_for_decl(c, s);
    if (!bt || !strcmp(strchr(bt->sig, '|') + 1, "?") || (s->flags & CSF_IMPLICIT))
        return true;
    return !bt_match(c, bt, s->ty).conflict;
}

/* gcc prints a built-in's type: 'ret(a, b)', with a second space after an
 * argument that is not a pointer. */
static void bt_sig_print(StrBuf *sb, const char *sig)
{
    const char *p = strchr(sig, '|'), *q;
    bool first = true, prev_ptr = true;
    sb_putn(sb, sig, (size_t)(p - sig));
    sb_putc(sb, '(');
    for (p++; *p; p = *q ? q + 1 : q) {
        q = strchr(p, '|');
        if (!q)
            q = p + strlen(p);
        if (!first)
            sb_puts(sb, prev_ptr ? ", " : ",  ");
        sb_putn(sb, p, (size_t)(q - p));
        prev_ptr = q > p && q[-1] == '*';
        first = false;
    }
    if (first)
        sb_puts(sb, "void");
    sb_putc(sb, ')');
}

/* Whether name is a library built-in, spelled plain or __builtin_. */
bool ccall_is_builtin(Checker *c, const char *n)
{
    bool pre = !strncmp(n, "__builtin_", 10);
    return bt_find(c, pre ? n + 10 : n, pre) != NULL;
}

/* A variable named like a library built-in (`int printf;`). */
void cexpr_builtin_nonfn(Checker *c, const CSym *s)
{
    const BTab *bt = bt_for_decl(c, s);
    if (bt && !strcmp(strchr(bt->sig, '|') + 1, "?"))
        return;
    if (bt && diag_enabled(c->diag, "builtin-declaration-mismatch"))
        cwarn(c, s->loc, "builtin-declaration-mismatch", "built-in function "
              "'%s' declared as non-function", cident(c, s->name));
}

/* -Wbuiltin-declaration-mismatch for the first declaration of a library
 * built-in (gcc's diagnose_mismatched_decls on the undeclared built-in). */
void cexpr_builtin_decl(Checker *c, const CSym *s)
{
    const BTab *bt = bt_for_decl(c, s);
    const char *dn = cident(c, s->name);
    BtMatch m;
    StrBuf sb;
    const char *p;
    Diagnostic *d = NULL;
    if (!bt || !strcmp(strchr(bt->sig, '|') + 1, "?") || (s->flags & CSF_IMPLICIT) ||
        !diag_enabled(c->diag, "builtin-declaration-mismatch"))
        return;
    m = bt_match(c, bt, s->ty);
    memset(&sb, 0, sizeof sb);
    if (m.conflict) {
        bt_sig_print(&sb, bt->sig);
        d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                    "conflicting types for built-in function '%s'; expected "
                    "'%s'", dn, sb_cstr(&sb));
    } else if (m.soft && extra_on(c)) {
        uint32_t j = (uint32_t)m.soft - 2;
        p = strchr(bt->sig, '|');
        if (m.soft == 1) {
            sb_putn(&sb, bt->sig, (size_t)(p - bt->sig));
            d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                        "mismatch in return type of built-in function '%s'; "
                        "expected '%s'", dn, sb_cstr(&sb));
        } else {
            const char *q;
            for (p++; j; j--)
                p = strchr(p, '|') + 1;
            q = strchr(p, '|');
            sb_putn(&sb, p, q ? (size_t)(q - p) : strlen(p));
            d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                        "mismatch in argument %d type of built-in function "
                        "'%s'; expected '%s'", m.soft - 1, dn,
                        sb_cstr(&sb));
        }
    } else if ((type_ent(TT, m.dft)->flags & TF_NOPROTO) && extra_on(c) &&
               type_ent(TT, m.dft)->kind == TY_FUNC && type_ent(TT, m.bft)->n) {
        bt_sig_print(&sb, bt->sig);
        d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                    "declaration of built-in function '%s' without a "
                    "prototype; expected '%s'", dn, sb_cstr(&sb));
    }
    if (d && bt->hdr[0])
        cnote(c, d, header_note_loc(c, s->loc, bt->hdr), "'%s' is declared in "
              "header '%s'", bt->name, bt->hdr);
    sb_free(&sb);
}

/* The v* printf/scanf built-ins carry a 'format' attribute whose first-argument
 * is 0; a declaration without a prototype cannot take it (gcc's
 * handle_format_attribute on the redeclaration). */
void cexpr_builtin_noproto_fmt(Checker *c, const CSym *s, SrcLoc loc)
{
    static const char *const vf[] = {"vprintf", "vfprintf", "vsprintf",
                                     "vsnprintf", "vscanf", "vsscanf",
                                     "vfscanf"};
    const char *dn = cident(c, s->name);
    size_t i;
    if (s->sc == SC_STATIC || (s->flags & CSF_IMPLICIT) || c->ext[s->name] ||
        !(type_ent(TT, s->ty)->flags & TF_NOPROTO) ||
        type_ent(TT, s->ty)->kind != TY_FUNC)
        return;
    for (i = 0; i < sizeof vf / sizeof *vf; i++)
        if (!strcmp(dn, vf[i])) {
            cwarn(c, loc, "attributes", "'format' attribute cannot be applied "
                  "to a function that does not take variable arguments");
            return;
        }
}

/* A constant null pointer as gcc's integer_zerop sees it: argument a of a
 * nonnull parameter, looking into the arms of ?: and the value of a comma. */
static void nonnull_arg(Checker *c, uint32_t a, uint32_t parm, bool ptr, SrcLoc loc)
{
    uint32_t k[3], n;
    a = strip_paren(c, a);
    if (a == NO_NODE || node_err(c, a))
        return;
    if (((c->ef[a] & EF_NPC) && (ptr || is_ptr(c, c->ty[a]))) ||
        (c->ck[a] == K_ADDR && !c->cb[a] && c->cv[a] == 0 && is_ptr(c, c->ty[a])) ||
        (ptr && has_ival(c, a) && c->cv[a] == 0 &&
         !(ntag(c, a) == N_BINARY && npunct(c, a) == P_COMMA))) {
        cwarn(c, loc, "nonnull", "argument %u null where non-null expected",
              parm);
        return;
    }
    switch (ntag(c, a)) {
    case N_COND:
        n = nkids(c, a, k, 3);
        if (n < 2)
            return;
        if (has_ival(c, k[0])) {
            if (c->cv[k[0]] != 0)
                nonnull_arg(c, n == 3 ? k[1] : k[0], parm, ptr, loc);
            else
                nonnull_arg(c, k[n - 1], parm, ptr, loc);
        } else {
            nonnull_arg(c, n == 3 ? k[1] : k[0], parm, ptr, loc);
            nonnull_arg(c, k[n - 1], parm, ptr, loc);
        }
        break;
    case N_BINARY:
        if (npunct(c, a) == P_COMMA && nkids(c, a, k, 2) == 2)
            nonnull_arg(c, k[1], parm, ptr && is_ptr(c, c->ty[a]), loc);
        break;
    case N_CAST:
        if (is_ptr(c, c->ty[a]) && nkids(c, a, k, 2) == 2)
            nonnull_arg(c, k[1], parm, false, loc);
        break;
    default:
        break;
    }
}

/* -Wnonnull for a call's arguments kv[1..nk) against the mask (a declared
 * attribute or a built-in's); pt/nparm: the prototype, if any. */
static void check_nonnull(Checker *c, const uint32_t *kv, uint32_t nk,
                          uint64_t mask, const TypeId *pt, uint32_t nparm,
                          bool proto, bool builtin, SrcLoc loc)
{
    uint32_t j;
    for (j = 0; j + 1 < nk && j < 63; j++) {
        uint32_t a = kv[j + 1];
        bool ptr;
        if (node_err(c, a))
            continue;
        ptr = builtin || (proto && j < nparm ? is_ptr(c, pt[j]) : is_ptr(c, rvt(c, a)));
        if (!((mask >> j) & 1) && !((mask & NN_ALL) && ptr))
            continue;
        nonnull_arg(c, a, j + 1, ptr, loc);
    }
}

/* convert_arguments: the arguments of call i (callee node fn, of pointer to
 * function type ft) against the prototype.  False if the call is erroneous. */
static bool call_args(Checker *c, uint32_t i, uint32_t fn, TypeId ft)
{
    uint32_t buf[32], *kv = buf, nk = nkids(c, i, buf, 32), j;
    TypeId fty = type_canon(TT, pointee(c, ft));
    const TypeEnt *fe = type_ent(TT, fty);
    bool proto = !(fe->flags & TF_NOPROTO), variadic = (fe->flags & TF_VARIADIC) != 0;
    bool bad = false, too_many = false, builtin_few = false;
    uint32_t fnode = strip_paren(c, fn), fref = SYM_NONE, nparm = (uint32_t)fe->n;
    TypeId ufty = pointee(c, ft);
    const TypeId *pt, *bpt = NULL;
    uint32_t bn = NO_NODE;
    char fname[256];
    SrcLoc loc = call_loc(c, fn);
    /* the declared parameter types keep their typedef spellings */
    while (type_ent(TT, ufty)->kind == TY_TYPEDEF)
        ufty = type_ent(TT, ufty)->base;
    pt = type_params(TT, type_ent(TT, ufty)->kind == TY_FUNC ? ufty : fty);
    if (nk > 32) {
        kv = xmalloc(nk * sizeof *kv);
        nkids(c, i, kv, nk);
    }
    if (fnode != NO_NODE && ntag(c, fnode) == N_IDENT && c->ck[fnode] == K_ADDR &&
        c->cb[fnode] && !(c->cb[fnode] & CB_NODE) &&
        csym(c, c->cb[fnode] - 1)->kind == CS_FUNC)
        fref = c->cb[fnode] - 1;
    {
        uint32_t show = fnode;
        if (show != NO_NODE && ntag(c, show) == N_UNARY &&
            npunct(c, show) == P_AMP && first_child(c, show) != NO_NODE &&
            is_func(c, c->ty[first_child(c, show)]))
            show = strip_paren(c, first_child(c, show));
        snprintf(fname, sizeof fname, "%s", show == NO_NODE ? "" : estr(c, show));
    }
    if (!proto && fref != SYM_NONE) {
        bool impl = (csym(c, fref)->flags & CSF_IMPLICIT) != 0;
        const BTab *bt = bt_find(c, cident(c, csym(c, fref)->name), false);
        if (bt && (impl ? !bt->mismatch : builtin_decl_ok(c, csym(c, fref))) &&
            strcmp(strchr(bt->sig, '|') + 1, "?")) {
            TypeId bft = type_canon(TT, bt_func_type(c, bt));
            bn = (uint32_t)type_ent(TT, bft)->n;
            bpt = type_params(TT, bft);
        }
    }
    for (j = 0; j + 1 < nk; j++) {
        uint32_t a = kv[j + 1];
        bool have = proto && j < nparm;
        if (!have && proto && !variadic) {
            Diagnostic *d = cerror_d(c, loc, "too many arguments to function "
                                     "'%s'", fname);
            if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
                cnote(c, d, csym(c, fref)->loc, "declared here");
            too_many = true;
            break;
        }
        if (node_err(c, a)) {
            bad = true;
            continue;
        }
        if (!rvalue_ok(c, a)) {
            bad = true;
            continue;
        }
        if (!have && !is_void(c, rvt(c, a)) &&
            !(bn != NO_NODE && bn != 0xFFFFFFFEu && j < bn))
            double_promo(c, a, expr_loc(c, a), rvt(c, a), TYPE_B(DOUBLE),
                         "when passing argument to function");
        if (have) {
            ConvInfo ci;
            if (!complete(c, pt[j])) {
                cerror(c, expr_loc(c, a), "type of formal parameter %u is "
                       "incomplete", j + 1);
                continue;
            }
            memset(&ci, 0, sizeof ci);
            ci.context = CONV_ARG;
            ci.fname = fname;
            ci.parmnum = (int)j + 1;
            if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
                ci.fsym = fref + 1;
            if (!cexpr_assign_check(c, a, pt[j], &ci))
                bad = true;
        } else if (is_void(c, rvt(c, a))) {
            cerror(c, expr_loc(c, a), "invalid use of void expression");
            bad = true;
        } else if (bn != NO_NODE && bn != 0xFFFFFFFEu) {
            /* a built-in declared without a prototype: the arguments are
             * matched against the built-in's own parameters */
            if (j >= bn) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc,
                                        "builtin-declaration-mismatch", "too "
                                        "many arguments to built-in function "
                                        "'%s' expecting %u", fname, bn);
                if (d && fref != SYM_NONE &&
                    !(csym(c, fref)->flags & CSF_IMPLICIT))
                    cnote(c, d, csym(c, fref)->loc, "declared here");
                bn = 0xFFFFFFFEu;
            } else {
                ConvInfo ci;
                memset(&ci, 0, sizeof ci);
                ci.context = CONV_ARG;
                ci.fname = fname;
                ci.parmnum = (int)j + 1;
                ci.warnopt = "builtin-declaration-mismatch";
                if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
                    ci.note_loc = csym(c, fref)->loc;
                (void)cexpr_assign_check(c, a, bpt[j], &ci);
            }
        }
    }
    /* check_builtin_function_arguments fails: no nonnull / format checks */
    if (!too_many && !proto && bn != NO_NODE && bn != 0xFFFFFFFEu &&
        nk - 1 < bn)
    {
        builtin_few = true;
        Diagnostic *d = cwarn_d(c, DL_WARNING, loc,
                                "builtin-declaration-mismatch", "too few "
                                "arguments to built-in function '%s' expecting "
                                "%u", fname, bn);
        if (d && fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
            cnote(c, d, csym(c, fref)->loc, "declared here");
    }
    if (!too_many && !bad && !builtin_few && fref != SYM_NONE) {
        uint64_t mask = csym(c, fref)->nonnull;
        if (builtin_decl_ok(c, csym(c, fref)))
            mask |= builtin_nonnull(cident(c, csym(c, fref)->name));
        /* (types made meanwhile may have moved the parameter arrays) */
        pt = type_params(TT, type_ent(TT, ufty)->kind == TY_FUNC ? ufty : fty);
        if (mask)
            check_nonnull(c, kv, nk, mask, pt, nparm, proto, false, loc);
        if (csym(c, fref)->fmt || builtin_decl_ok(c, csym(c, fref)))
            check_format_literal(c, kv, nk, csym(c, fref),
                                 cident(c, csym(c, fref)->name), loc);
        if (builtin_decl_ok(c, csym(c, fref)) &&
            !strcmp(cident(c, csym(c, fref)->name), "strlen"))
            check_strlen(c, kv, nk);
    }
    /* a call through a pointer declared with 'nonnull' */
    if (!too_many && !bad && !builtin_few && fref == SYM_NONE &&
        fnode != NO_NODE && ntag(c, fnode) == N_IDENT) {
        uint32_t ps = lookup_ord(c, cnode_ident(c, fnode));
        uint32_t tb = c->top[NS_ORD][cnode_ident(c, fnode)];
        uint64_t nn = ps != SYM_NONE && csym(c, ps)->kind == CS_OBJ
                          ? csym(c, ps)->nonnull | (tb ? c->log.data[tb - 1].nn : 0)
                          : 0;
        if (nn)
            check_nonnull(c, kv, nk, nn, pt, nparm, proto, false, loc);
    }
    if (!too_many && !bad && proto && nparm > 1 && fref != SYM_NONE &&
        csym(c, fref)->parms)
        check_restrict(c, kv, nk, csym(c, fref)->parms, nparm, loc,
                       builtin_decl_ok(c, csym(c, fref)) &&
                       zero_size_ok(cident(c, csym(c, fref)->name)));
    if (!too_many && fnode != NO_NODE && ntag(c, fnode) == N_IDENT &&
        ((!strncmp(fname, "__builtin_", 10) && fref == SYM_NONE &&
          strncmp(fname, "__builtin___", 12)) ||
         (fref != SYM_NONE && bt_for_decl(c, csym(c, fref)) &&
          builtin_decl_ok(c, csym(c, fref)))))
        { memset_args(c, kv, nk, fname); sizeof_memaccess(c, kv, nk, fname); }
    if (!too_many && proto && nk - 1 < nparm) {
        Diagnostic *d = cerror_d(c, loc, "too few arguments to function '%s'",
                                 fname);
        if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
            cnote(c, d, csym(c, fref)->loc, "declared here");
        bad = true;
    }
    if (kv != buf)
        free(kv);
    return !bad;
}

/* handle_cleanup_attribute: the call fn(&var) of the cleanup function fsym on
 * an automatic variable of type vty.  dloc is the declarator, iloc gcc's
 * input_location. */
void cexpr_cleanup_call(Checker *c, uint32_t fsym, TypeId vty, SrcLoc dloc,
                        SrcLoc il)
{
    CSym *f = csym(c, fsym);
    TypeId fty = type_canon(TT, f->ty), at, pt, cp, ca;
    const TypeEnt *fe;
    char fname[256];
    Diagnostic *d;
    if (tkind(c, fty) != TY_FUNC)
        return;
    fe = type_ent(TT, fty);
    if (fe->flags & TF_NOPROTO)
        return;
    snprintf(fname, sizeof fname, "%s", cident(c, f->name));
    if (fe->n == 0 || (fe->n > 1 && !(fe->flags & TF_VARIADIC))) {
        d = cerror_d(c, dloc, fe->n ? "too few arguments to function '%s'"
                                    : "too many arguments to function '%s'",
                     fname);
        cnote(c, d, f->loc, "declared here");
        return;
    }
    pt = type_params(TT, fty)[0];
    at = type_ptr(TT, vty);
    cp = type_canon(TT, unqual(c, pt));
    ca = type_canon(TT, at);
    if (mainv(c, cp) == mainv(c, ca))
        return;
    if (tkind(c, cp) == TY_PTR) {
        TypeId tl = type_canon(TT, type_base(TT, cp));
        if (is_void(c, tl) || type_compatible(TT, mvt(c, tl),
                mvt(c, type_canon(TT, type_base(TT, ca)))))
            return;
        d = cwarn_d(c, DL_WARNING, il, "incompatible-pointer-types", "passing "
                    "argument 1 of '%s' from incompatible pointer type", fname);
    } else if (gcc_integer(c, cp))
        d = cwarn_d(c, DL_WARNING, il, "int-conversion", "passing argument 1 "
                    "of '%s' makes integer from pointer without a cast", fname);
    else
        return;
    if (d)
        cnote(c, d, param_loc(c, fsym, 0, NO_NODE, il),
              "expected %s but argument is of type %s", type_q(TT, pt),
              type_q(TT, at));
}

/* ARG_LOCATION: the expression's location, at the macro use when its first
 * token comes from an expansion. */
static SrcLoc arg_loc(Checker *c, uint32_t a)
{
    uint32_t t = first_tok(c, a);
    return c->u->toks[t].exp ? c->u->toks[t].exp : expr_loc(c, a);
}

/* The argument count of an __atomic_* built-in (sized variants _1.._16
 * included); 0 for the others. */
static uint32_t atomic_argc(const char *name)
{
    static const struct { const char *n; uint32_t argc; } t[] = {
        {"load_n", 2}, {"load", 3}, {"store_n", 3}, {"store", 3},
        {"exchange_n", 3}, {"exchange", 4}, {"compare_exchange_n", 6},
        {"compare_exchange", 6}, {"add_fetch", 3}, {"sub_fetch", 3},
        {"and_fetch", 3}, {"xor_fetch", 3}, {"or_fetch", 3},
        {"nand_fetch", 3}, {"fetch_add", 3}, {"fetch_sub", 3},
        {"fetch_and", 3}, {"fetch_xor", 3}, {"fetch_or", 3},
        {"fetch_nand", 3}, {"test_and_set", 2}, {"clear", 2},
        {"thread_fence", 1}, {"signal_fence", 1}, {"is_lock_free", 2},
        {"always_lock_free", 2}};
    char b[32];
    size_t n, len;
    if (strncmp(name, "__atomic_", 9))
        return 0;
    snprintf(b, sizeof b, "%s", name + 9);
    len = strlen(b);
    while (len && b[len - 1] >= '0' && b[len - 1] <= '9')
        b[--len] = 0;
    if (len < strlen(name + 9) && len && b[len - 1] == '_')
        b[--len] = 0;
    for (n = 0; n < sizeof t / sizeof *t; n++)
        if (!strcmp(b, t[n].n))
            return t[n].argc;
    return 0;
}

/* The __atomic_ and __sync_ built-ins gcc resolves itself: 1 the generic
 * (void *-based) __atomic_load/store/exchange/compare_exchange, 2 the _n and
 * __sync ones sized by the first argument, 3 those same for fetch operations
 * (a _Bool operand is refused); 0 the others. */
static int atomic_kind(const char *name)
{
    static const char *const ops[] = {"add", "sub", "and", "nand", "xor", "or"};
    const char *b;
    size_t k, n;
    if (!strncmp(name, "__sync_", 7)) {
        b = name + 7;
        if (!strcmp(b, "bool_compare_and_swap") ||
            !strcmp(b, "val_compare_and_swap") ||
            !strcmp(b, "lock_test_and_set") || !strcmp(b, "lock_release"))
            return 2;
        for (k = 0; k < sizeof ops / sizeof *ops; k++) {
            n = strlen(ops[k]);
            if (!strncmp(b, "fetch_and_", 10) && !strcmp(b + 10, ops[k]))
                return 3;
            if (!strncmp(b, ops[k], n) && !strcmp(b + n, "_and_fetch"))
                return 3;
        }
        return 0;
    }
    if (strncmp(name, "__atomic_", 9))
        return 0;
    b = name + 9;
    if (!strcmp(b, "load") || !strcmp(b, "store") || !strcmp(b, "exchange") ||
        !strcmp(b, "compare_exchange"))
        return 1;
    if (!strcmp(b, "load_n") || !strcmp(b, "store_n") ||
        !strcmp(b, "exchange_n") || !strcmp(b, "compare_exchange_n"))
        return 2;
    for (k = 0; k < sizeof ops / sizeof *ops; k++) {
        n = strlen(ops[k]);
        if (!strncmp(b, "fetch_", 6) && !strcmp(b + 6, ops[k]))
            return 3;
        if (!strncmp(b, ops[k], n) && !strcmp(b + n, "_fetch"))
            return 3;
    }
    return 0;
}

static bool vla_pointee(Checker *c, TypeId e)
{
    e = type_canon(TT, e);
    while (tkind(c, e) == TY_ARRAY)
        e = type_canon(TT, type_base(TT, e));
    return tkind(c, e) == TY_VLA ||
           ((tkind(c, e) == TY_STRUCT || tkind(c, e) == TY_UNION) &&
            (type_record(TT, e)->flags & RF_VLA));
}

/* sync_resolve_size / get_atomic_generic_size: the argument checks gcc makes
 * while resolving an overloaded atomic or sync built-in.  a: the
 * arguments, n their count.  False after an error. */
static bool atomic_args_ok(Checker *c, uint32_t i, const uint32_t *a,
                           uint32_t n, const char *name, SrcLoc loc)
{
    int kind = atomic_kind(name);
    uint32_t k;
    TypeId t0, e0;
    bool ok;
    uint64_t sz0;
    if (!kind || n == 0)
        return true;
    for (k = 0; k < n; k++)
        if (node_err(c, a[k]))
            return true;
    t0 = rvt(c, a[0]);
    if (kind != 1) {
        TypeId e = is_ptr(c, t0) ? pointee(c, t0) : ERRT;
        uint64_t sz = 0;
        bool good = is_ptr(c, t0) &&
                    (is_int(c, e) || is_ptr(c, e)) &&
                    type_is_complete(TT, e) &&
                    !(kind == 3 && tkind(c, e) == TY_BOOL);
        if (good) {
            sz = type_size(TT, e, &ok);
            good = ok && (sz == 1 || sz == 2 || sz == 4 || sz == 8 || sz == 16);
        }
        if (!good) {
            cerror(c, cdecl_iloc(c, last_tok(c, i) + 1), "operand type %s is incompatible with "
                   "argument 1 of '%s'", type_q(TT, t0), name);
            return false;
        }
        return true;
    }
    /* the generic functions */
    if (!is_ptr(c, t0) || is_void(c, pointee(c, t0))) {
        cerror(c, loc, "argument 1 of '%s' must be a non-void pointer type",
               name);
        return false;
    }
    e0 = pointee(c, t0);
    if (!type_is_complete(TT, e0)) {
        cerror(c, loc, "argument 1 of '%s' must be a pointer to a complete "
               "type", name);
        return false;
    }
    if (vla_pointee(c, e0)) {
        cerror(c, loc, "argument 1 of '%s' must be a pointer to a constant "
               "size type", name);
        return false;
    }
    sz0 = type_size(TT, e0, &ok);
    if (!sz0) {
        cerror(c, loc, "argument 1 of '%s' must be a pointer to a nonzero "
               "size object", name);
        return false;
    }
    {
        const char *b = name + 9;
        unsigned nparam = !strcmp(b, "exchange") ? 4 :
                          !strcmp(b, "compare_exchange") ? 6 : 3;
        unsigned nmodel = nparam == 6 ? 2 : 1;
        unsigned outputs = !strcmp(b, "exchange") ? 5 :
                           !strcmp(b, "load") ? 2 :
                           !strcmp(b, "store") ? 1 : 3;
        unsigned x;
        for (x = 0; x < nparam - nmodel; x++) {
            TypeId t = rvt(c, a[x]), e;
            uint64_t sz;
            unsigned q;
            if (nparam == 6 && x == 3)
                continue;
            if (!is_ptr(c, t)) {
                cerror(c, loc, "argument %u of '%s' must be a pointer type",
                       x + 1, name);
                return false;
            }
            e = pointee(c, t);
            if (vla_pointee(c, e)) {
                cerror(c, loc, "argument %u of '%s' must be a pointer to a "
                       "constant size type", x + 1, name);
                return false;
            }
            if (is_func(c, e)) {
                cerror(c, loc, "argument %u of '%s' must not be a pointer to "
                       "a function", x + 1, name);
                return false;
            }
            sz = type_is_complete(TT, e) ? type_size(TT, e, &ok) : 0;
            if (sz != sz0) {
                cerror(c, loc, "size mismatch in argument %u of '%s'", x + 1,
                       name);
                return false;
            }
            q = tquals(c, e);
            if ((outputs & (1u << x)) && (q & TQ_CONST))
                cwarn(c, loc, "incompatible-pointer-types", "argument %u of "
                      "'%s' discards 'const' qualifier", x + 1, name);
            if (x > 0 && (q & TQ_VOLATILE))
                cwarn(c, loc, "incompatible-pointer-types", "argument %u of "
                      "'%s' discards 'volatile' qualifier", x + 1, name);
        }
        for (x = nparam - nmodel; x < nparam; x++) {
            if (!is_int(c, rvt(c, a[x]))) {
                cerror(c, loc, "non-integer memory model argument %u of '%s'",
                       x + 1, name);
                return false;
            }
            if (c->ck[a[x]] == K_ICE && (c->cv[a[x]] & 0xffff) >= 6)
                cwarn(c, loc, "invalid-memory-model", "invalid memory model "
                      "argument %u of '%s'", x + 1, name);
        }
    }
    return true;
}

/* check_builtin_function_arguments, for the built-ins gcc validates itself:
 * argument counts and the argument kinds.  False after an error. */
static bool builtin_args_ok(Checker *c, uint32_t i, uint32_t fn,
                            const char *name)
{
    uint32_t all[16], *a = all + 1, n = nkids(c, i, all, 16), k, want = 0;
    const char *b = name + 10;
    SrcLoc loc = call_loc(c, fn);
    bool ovf = false, ovfp = false, fp1 = false, cmp = false;
    if (n == 0 || n > 16)
        return true;
    n--;
    if ((want = atomic_argc(name)) != 0) {
        if (n != want) {
            if (atomic_kind(name) == 1)
                cerror(c, loc, "incorrect number of arguments to function "
                       "'%s'", name);
            else
                cerror(c, loc, n < want ? "too few arguments to function "
                       "'%s'" : "too many arguments to function '%s'",
                       name);
            return false;
        }
        if (strstr(name, "lock_free") && !node_err(c, a[1])) {
            TypeId t = rvt(c, a[1]);
            if (is_int(c, t) && !(c->ef[a[1]] & EF_NPC)) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, arg_loc(c, a[1]),
                                        "int-conversion", "passing argument 2 "
                                        "of '%s' makes pointer from integer "
                                        "without a cast", name);
                cnote(c, d, arg_loc(c, a[1]), "expected 'const volatile void "
                      "*' but argument is of type %s", type_q(TT, t));
            }
            if (!is_ptr(c, t) && !is_int(c, t)) {     /* is/always_lock_free */
                Diagnostic *d = cerror_d(c, arg_loc(c, a[1]), "incompatible "
                                         "type for argument 2 of '%s'", name);
                cnote(c, d, arg_loc(c, a[1]), "expected 'const volatile void "
                      "*' but argument is of type %s", type_q(TT, t));
                return false;
            }
            return true;
        }
        return atomic_args_ok(c, i, a, n, name, loc);
    }
    if (!strncmp(name, "__sync_", 7))
        return atomic_args_ok(c, i, a, n, name, loc);
    if (strncmp(name, "__builtin_", 10))
        return true;
    if (!strcmp(b, "constant_p")) {
        want = 1;
    } else if (!strcmp(b, "alloca_with_align")) {
        want = 2;
    } else if (!strcmp(b, "alloca_with_align_and_max")) {
        want = 3;
    } else if (!strcmp(b, "assume_aligned")) {
        want = n > 2 ? 3 : 2;
    } else if (!strcmp(b, "fpclassify")) {
        want = 6;
    } else if (!strcmp(b, "clear_padding")) {
        want = 1;
    } else if (!strcmp(b, "speculation_safe_value")) {
        want = n < 1 ? 1 : n > 2 ? 2 : n;
        if (!n)         /* gcc reports this one at input_location */
            loc = after_loc(c, i);
    } else if (!strcmp(b, "va_start")) {
        if (n == 0) {
            cerror(c, loc, "too few arguments to function '%s'", name);
            return false;
        }
        return true;
    } else if (!strcmp(b, "isfinite") || !strcmp(b, "isinf_sign") ||
               !strcmp(b, "isinf") || !strcmp(b, "isnan") ||
               !strcmp(b, "isnormal") || !strcmp(b, "signbit")) {
        want = 1;
        fp1 = true;
    } else if (!strcmp(b, "isgreater") || !strcmp(b, "isgreaterequal") ||
               !strcmp(b, "isless") || !strcmp(b, "islessequal") ||
               !strcmp(b, "islessgreater") || !strcmp(b, "isunordered")) {
        want = 2;
        cmp = true;
    } else if (!strcmp(b, "add_overflow") || !strcmp(b, "sub_overflow") ||
               !strcmp(b, "mul_overflow")) {
        want = 3;
        ovf = true;
    } else if (!strcmp(b, "add_overflow_p") || !strcmp(b, "sub_overflow_p") ||
               !strcmp(b, "mul_overflow_p")) {
        want = 3;
        ovfp = true;
    } else {
        return true;
    }
    if (n != want) {
        cerror(c, loc, n < want ? "too few arguments to function '%s'"
                                : "too many arguments to function '%s'", name);
        return false;
    }
    for (k = 0; k < n; k++)
        if (node_err(c, a[k]))
            return true;
    if (!strcmp(b, "alloca_with_align")) {
        bool ok = c->ck[a[1]] == K_ICE && !(c->ef[a[1]] & EF_OVERFLOW);
        uint64_t v = c->cv[a[1]];
        if (ok && (v < 8 || v > 2147483648u || (v & (v - 1))))
            ok = false;
        if (!ok) {
            cerror(c, arg_loc(c, a[1]), "second argument to function '%s' "
                   "must be a constant integer power of 2 between '8' and "
                   "'2147483648' bits", name);
            return false;
        }
    } else if (!strcmp(b, "assume_aligned")) {
        if (n == 3 && !is_int(c, rvt(c, a[2]))) {
            cerror(c, arg_loc(c, a[2]), "non-integer argument 3 in call to "
                   "function '%s'", name);
            return false;
        }
    } else if (!strcmp(b, "clear_padding")) {
        TypeId t = rvt(c, a[0]), e;
        const char *why = NULL;
        if (!is_ptr(c, t))
            why = "does not have pointer type";
        else if (!type_is_complete(TT, e = pointee(c, t)))
            why = "points to incomplete type";
        if (why) {
            cerror(c, arg_loc(c, a[0]), "argument 1 in call to function '%s' "
                   "%s", name, why);
            return false;
        }
        if (tquals(c, e) & TQ_CONST) {
            cerror(c, arg_loc(c, a[0]), "argument 1 in call to function '%s' "
                   "has pointer to 'const' type (%s)", name, type_q(TT, t));
            return false;
        }
    } else if (!strcmp(b, "fpclassify")) {
        for (k = 0; k < 5; k++)
            if (c->ck[a[k]] != K_ICE) {
                cerror(c, arg_loc(c, a[k]), "non-const integer argument %u "
                       "in call to function '%s'", k + 1, name);
                return false;
            }
        if (!is_flt(c, rvt(c, a[5]))) {
            cerror(c, arg_loc(c, a[5]), "non-floating-point argument in call "
                   "to function '%s'", name);
            return false;
        }
    } else if (fp1) {
        if (!is_flt(c, rvt(c, a[0]))) {
            cerror(c, arg_loc(c, a[0]), "non-floating-point argument in call "
                   "to function '%s'", name);
            return false;
        }
    } else if (cmp) {
        TypeId t0 = rvt(c, a[0]), t1 = rvt(c, a[1]);
        if (!(is_flt(c, t0) || is_flt(c, t1)) ||
            !(is_flt(c, t0) || is_int(c, t0)) ||
            !(is_flt(c, t1) || is_int(c, t1))) {
            cerror(c, loc, "non-floating-point arguments in call to function "
                   "'%s'", name);
            return false;
        }
    } else if (ovf || ovfp) {
        for (k = 0; k < (ovf ? 2u : 3u); k++)
            if (!is_int(c, rvt(c, a[k]))) {
                cerror(c, arg_loc(c, a[k]), "argument %u in call to function "
                       "'%s' does not have integral type", k + 1, name);
                return false;
            }
        if (ovf) {
            TypeId t = rvt(c, a[2]), e;
            if (!is_ptr(c, t) || !is_int(c, e = pointee(c, t))) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' does not have pointer to integral type", name);
                return false;
            }
            if (tkind(c, e) == TY_ENUM) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to enumerated type", name);
                return false;
            }
            if (tkind(c, e) == TY_BOOL) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to boolean type", name);
                return false;
            }
            if (tquals(c, e) & TQ_CONST) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to 'const' type (%s)", name,
                       type_q(TT, t));
                return false;
            }
            if (tquals(c, e) & TQ_ATOMIC) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to '_Atomic' type (%s)", name,
                       type_q(TT, t));
                return false;
            }
        } else {
            TypeId t = rvt(c, a[2]);
            if (tkind(c, t) == TY_ENUM || tkind(c, t) == TY_BOOL) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has %s type", name,
                       tkind(c, t) == TY_ENUM ? "enumerated" : "boolean");
                return false;
            }
        }
    }
    return true;
}

/* A function designator or array operand takes the builtin's location when
 * converted to a pointer. */
static SrcLoc tg_loc(Checker *c, uint32_t n, SrcLoc loc)
{
    TypeId t = type_canon(TT, c->ty[n]);
    return is_func(c, t) || type_ckind(TT, t) == TY_ARRAY ? loc : cnode_loc(c, n);
}

/* check_tgmath_function: the parameter count of function-pointer argument
 * a (position pos), or 0 after an error. */
static uint32_t tgmath_function(Checker *c, uint32_t a, unsigned pos,
                                SrcLoc loc)
{
    TypeId t = rvt(c, a), f = 0;
    SrcLoc l = tg_loc(c, a, loc);
    const char *why;
    if (!is_ptr(c, t) || !is_func(c, pointee(c, t)))
        why = "is not a function pointer";
    else if ((type_ent(TT, f = type_canon(TT, pointee(c, t)))->flags &
              TF_NOPROTO))
        why = "is unprototyped";
    else if (type_ent(TT, f)->flags & TF_VARIADIC)
        why = "has variable arguments";
    else if (!type_ent(TT, f)->n)
        why = "has no arguments";
    else
        return (uint32_t)type_ent(TT, f)->n;
    cerror(c, l, "argument %u of '__builtin_tgmath' %s", pos, why);
    return 0;
}

/* common_type of two real floating types: the wider; of equal value sets,
 * _FloatN over _FloatNx over the standard type.  True if a wins over b. */
static int tg_rank(Checker *c, TypeId t)
{
    switch (tkind(c, t)) {
    case TY_FLOAT16: case TY_FLOAT32: case TY_FLOAT64: case TY_FLOAT128:
        return 2;
    case TY_FLOAT32X: case TY_FLOAT64X:
        return 1;
    default:
        return 0;
    }
}

static bool tg_better(Checker *c, TypeId a, TypeId b)
{
    int pa = float_prec(c, tkind(c, a)), pb = float_prec(c, tkind(c, b));
    return pa > pb || (pa == pb && tg_rank(c, a) > tg_rank(c, b));
}

static TypeId tg_mv(Checker *c, TypeId t)
{
    return unqual(c, type_canon(TT, t));
}

static bool tg_fl(Checker *c, TypeId t)
{
    return is_flt(c, t) ||
           (is_complex(c, t) && is_flt(c, type_base(TT, type_canon(TT, t))));
}

/* The checks of c_parser_postfix_expression on how the functions' return and
 * parameter types vary; false after an error. */
static bool tgmath_variation(Checker *c, const uint32_t *a, const TypeId *ft,
                             uint32_t nf, uint32_t nargs, SrcLoc loc,
                             int *kind, TypeId *tg)
{
    TypeId pf[17];
    bool pcx[17] = {false}, pvar[17] = {false};
    int maxv = 0, tgarg = 0;
    uint32_t j, m, u;
    pf[0] = tg_mv(c, type_base(TT, ft[0]));
    pcx[0] = is_complex(c, pf[0]);
    for (m = 0; m < nargs; m++) {
        pf[m + 1] = tg_mv(c, type_params(TT, ft[0])[m]);
        pcx[m + 1] = is_complex(c, pf[m + 1]);
    }
    for (j = 1; j < nf; j++) {
        TypeId ret = tg_mv(c, type_base(TT, ft[j]));
        if (ret != pf[0]) {
            pvar[0] = true;
            if (!tg_fl(c, pf[0])) {
                cerror(c, tg_loc(c, a[0], loc), "invalid type-generic return "
                       "type for argument 1 of '__builtin_tgmath'");
                return false;
            }
            if (!tg_fl(c, ret)) {
                cerror(c, tg_loc(c, a[j], loc), "invalid type-generic return "
                       "type for argument %u of '__builtin_tgmath'", j + 1);
                return false;
            }
        }
        if (is_complex(c, ret))
            pcx[0] = true;
        for (m = 0; m < nargs; m++) {
            TypeId t = tg_mv(c, type_params(TT, ft[j])[m]);
            if (t != pf[m + 1]) {
                pvar[m + 1] = true;
                if (!tg_fl(c, pf[m + 1])) {
                    cerror(c, tg_loc(c, a[0], loc), "invalid type-generic type "
                           "for argument %u of argument %u of "
                           "'__builtin_tgmath'", m + 1, 1);
                    return false;
                }
                if (!tg_fl(c, t)) {
                    cerror(c, tg_loc(c, a[j], loc), "invalid type-generic type "
                           "for argument %u of argument %u of "
                           "'__builtin_tgmath'", m + 1, j + 1);
                    return false;
                }
            }
            if (is_complex(c, t))
                pcx[m + 1] = true;
        }
    }
    for (j = 0; j <= nargs; j++) {          /* 0 fixed, 1 real, 2 complex */
        if (!pvar[j])
            kind[j] = 0;
        else if (pcx[j])
            maxv = kind[j] = 2;
        else {
            kind[j] = 1;
            if (maxv != 2)
                maxv = 1;
        }
    }
    if (!maxv) {
        cerror(c, loc, "function arguments of '__builtin_tgmath' all have "
               "the same type");
        return false;
    }
    for (j = 1; j <= nargs && !tgarg; j++)
        if (kind[j] == maxv)
            tgarg = (int)j;
    if (!tgarg) {
        cerror(c, loc, "function arguments of '__builtin_tgmath' lack "
               "type-generic parameter");
        return false;
    }
    for (j = 0; j < nf; j++) {
        tg[j] = tg_mv(c, type_params(TT, ft[j])[tgarg - 1]);
        for (u = 0; u < j; u++)
            if (tg[u] == tg[j]) {
                cerror(c, tg_loc(c, a[j], loc), "duplicate type-generic "
                       "parameter type for function argument %u of "
                       "'__builtin_tgmath'", j + 1);
                return false;
            }
    }
    for (j = 0; j < nf; j++) {
        TypeId et = tg[j], er = is_complex(c, et)
                    ? tg_mv(c, type_base(TT, type_canon(TT, et))) : et;
        TypeId ret = tg_mv(c, type_base(TT, ft[j]));
        if ((kind[0] == 2 && ret != et) || (kind[0] == 1 && ret != er)) {
            cerror(c, tg_loc(c, a[j], loc), "bad return type for function "
                   "argument %u of '__builtin_tgmath'", j + 1);
            return false;
        }
        for (m = 0; m < nargs; m++) {
            TypeId t = tg_mv(c, type_params(TT, ft[j])[m]);
            if ((kind[m + 1] == 2 && t != et) ||
                (kind[m + 1] == 1 && t != er)) {
                cerror(c, tg_loc(c, a[j], loc), "bad type for argument %u of "
                       "function argument %u of '__builtin_tgmath'", m + 1,
                       j + 1);
                return false;
            }
        }
    }
    for (m = 0; m < nargs; m++) {
        uint32_t ar = a[nf + m];
        TypeId t;
        if (!kind[m + 1])
            continue;
        t = rvt(c, ar);
        if (!is_int(c, t) && !is_flt(c, t) && !is_complex(c, t)) {
            cerror(c, cnode_loc(c, ar), "invalid type of argument %u of "
                   "type-generic function", m + 1);
            return false;
        }
    }
    return true;
}

/* __builtin_tgmath (functions..., arguments...): the call of the function
 * whose generic parameter type fits the arguments.  Sets i's type to its
 * return type; false (no diagnostic) if the call is malformed. */
static bool e_tgmath(Checker *c, uint32_t i)
{
    uint32_t all[16], n = nkids(c, i, all, 16), na, nf, nargs, j, m, sel;
    uint32_t *a = all + 1;
    TypeId rt, ft[16], tg[16], areal = 0;
    int kind[17];
    bool arg_cx, floatnx = false;
    SrcLoc loc;
    if (n < 1 || n > 16)
        return false;
    loc = cnode_loc(c, all[0]);
    na = n - 1;
    for (j = 0; j < na; j++)
        if (node_err(c, a[j]))
            return false;
    if (na < 3) {
        cerror(c, loc, "too few arguments to '__builtin_tgmath'");
        return false;
    }
    nargs = tgmath_function(c, a[0], 1, loc);
    if (!nargs)
        return false;
    if (na < nargs || na - nargs < 2) {
        cerror(c, loc, "too few arguments to '__builtin_tgmath'");
        return false;
    }
    nf = na - nargs;
    sel = nf;
    ft[0] = type_canon(TT, pointee(c, rvt(c, a[0])));
    for (j = 1; j < nf; j++) {
        uint32_t tn = tgmath_function(c, a[j], j + 1, loc);
        if (!tn)
            return false;
        if (tn != nargs) {
            cerror(c, tg_loc(c, a[j], loc), "argument %u of '__builtin_tgmath' "
                   "has wrong number of arguments", j + 1);
            return false;
        }
        ft[j] = type_canon(TT, pointee(c, rvt(c, a[j])));
    }
    if (!tgmath_variation(c, a, ft, nf, nargs, loc, kind, tg))
        return false;
    arg_cx = true;
    for (j = 0; j < nf; j++)
        if (!is_complex(c, tg[j]))
            arg_cx = false;
    for (m = 0; m < nargs; m++) {       /* integers become _Float32x if any
                                         * generic argument is _FloatNx */
        TypeId t;
        if (!kind[m + 1])
            continue;
        t = tg_mv(c, rvt(c, a[nf + m]));
        if (is_complex(c, t))
            t = tg_mv(c, type_base(TT, type_canon(TT, t)));
        if (tkind(c, t) == TY_FLOAT32X || tkind(c, t) == TY_FLOAT64X)
            floatnx = true;
    }
    for (m = 0; m < nargs; m++) {
        TypeId t;
        if (!kind[m + 1])
            continue;
        t = tg_mv(c, rvt(c, a[nf + m]));
        if (is_complex(c, t)) {
            arg_cx = true;
            t = tg_mv(c, type_base(TT, type_canon(TT, t)));
        }
        if (is_int(c, t))
            t = floatnx ? TYPE_B(FLOAT32X) : TYPE_B(DOUBLE);
        if (!areal || tg_better(c, t, areal))
            areal = t;
    }
    for (j = 0; j < nf && sel == nf; j++) {
        TypeId t = is_complex(c, tg[j]) ? tg_mv(c, type_base(TT, type_canon(TT, tg[j])))
                                        : tg[j];
        if (is_complex(c, tg[j]) == arg_cx && t == areal)
            sel = j;
    }
    if (sel == nf && !kind[0] && is_flt(c, tg_mv(c, type_base(TT, ft[0]))))
        for (j = 0; j < nf && sel == nf; j++) {
            TypeId t = is_complex(c, tg[j])
                       ? tg_mv(c, type_base(TT, type_canon(TT, tg[j]))) : tg[j];
            if (is_complex(c, tg[j]) == arg_cx && areal && is_flt(c, t) &&
                float_prec(c, tkind(c, areal)) <= float_prec(c, tkind(c, t)))
                sel = j;
        }
    if (sel == nf) {
        cerror(c, loc, "no matching function for type-generic call");
        return false;
    }
    rt = unqual(c, type_base(TT, ft[sel]));
    c->ty[i] = rt;
    c->ef[i] = EF_SIDE;
    return true;
}

/* The type of a call of an overloaded atomic or sync built-in; ERRT for the
 * others. */
static TypeId atomic_result(Checker *c, uint32_t i, const char *name)
{
    int kind = atomic_kind(name);
    const char *b = !strncmp(name, "__sync_", 7) ? name + 7 : name + 9;
    uint32_t all[16], n = nkids(c, i, all, 16);
    TypeId t;
    if (!kind || n < 2 || n > 16 || node_err(c, all[1]))
        return ERRT;
    if (kind == 1)
        return !strcmp(b, "compare_exchange") ? TYPE_B(BOOL) : TYPE_B(VOID);
    t = rvt(c, all[1]);
    if (!is_ptr(c, t))
        return ERRT;
    if (!strcmp(b, "bool_compare_and_swap") ||
        !strcmp(b, "compare_exchange_n"))
        return TYPE_B(BOOL);
    if (!strcmp(b, "lock_release") || !strcmp(b, "store_n"))
        return TYPE_B(VOID);
    return unqual(c, pointee(c, t));
}

/* -Wabsolute-value: warn_for_abs of c-parser.cc, for a call of a library
 * absolute value function with one argument of an unsuitable type. */
static void warn_for_abs(Checker *c, uint32_t i, const uint32_t *k, uint32_t n)
{
    uint32_t fn = strip_paren(c, k[0]), a = n == 2 ? k[1] : NO_NODE;
    const CSym *sy = NULL;
    const BTab *bt = NULL;
    const char *nm, *base;
    TypeId at, ft = 0, bft;
    int fam;
    bool integ, flt, cpx;
    if (a == NO_NODE || fn == NO_NODE || ntag(c, fn) != N_IDENT ||
        !diag_enabled(c->diag, "absolute-value") || node_err(c, a) ||
        inhibited(c, i, false))
        return;
    if (c->ck[fn] == K_ADDR && c->cb[fn] && !(c->cb[fn] & CB_NODE)) {
        sy = csym(c, c->cb[fn] - 1);
        if (sy->kind != CS_FUNC)
            return;
        nm = cident(c, sy->name);
    } else
        nm = cident(c, cnode_ident(c, fn));
    if (strncmp(nm, "__builtin_", 10) && !sy)
        return;
    base = !strncmp(nm, "__builtin_", 10) ? nm + 10 : nm;
    if (!strcmp(base, "abs") || !strcmp(base, "labs") ||
        !strcmp(base, "llabs") || !strcmp(base, "imaxabs"))
        fam = 0;
    else if (!strcmp(base, "fabs") || !strcmp(base, "fabsf") ||
             !strcmp(base, "fabsl"))
        fam = 1;
    else if (!strcmp(base, "cabs") || !strcmp(base, "cabsf") ||
             !strcmp(base, "cabsl")) {
        fam = 2;   /* not in the built-in table: the parameter is a complex */
        ft = !strcmp(base, "cabs") ? TYPE_B(DOUBLE)
           : !strcmp(base, "cabsf") ? TYPE_B(FLOAT) : TYPE_B(LDOUBLE);
    } else
        return;
    if (fam != 2) {
        bt = sy ? bt_for_decl(c, sy)
                : bt_find(c, nm + 10, true);
        if (!bt || (sy && !builtin_decl_ok(c, sy)))
            return;
    }
    at = unqual(c, rvt(c, a));
    if (is_err(c, at))
        return;
    integ = is_int(c, at);
    flt = is_flt(c, at);
    cpx = is_complex(c, at);
    if (!integ && !flt && !cpx)
        return;
    if (fam == 0 && !integ) {
        cwarn(c, call_loc(c, k[0]), "absolute-value", "using integer absolute "
              "value function '%s' when argument is of %s type %s", nm,
              flt ? "floating-point" : "complex", type_q(TT, at));
        return;
    }
    if (fam == 1 && !flt) {
        cwarn(c, call_loc(c, k[0]), "absolute-value", "using floating-point "
              "absolute value function '%s' when argument is of %s type %s",
              nm, integ ? "integer" : "complex", type_q(TT, at));
        return;
    }
    if (fam == 2 && !cpx) {
        cwarn(c, call_loc(c, k[0]), "absolute-value", "using complex absolute "
              "value function '%s' when argument is of %s type %s", nm,
              integ ? "integer" : "floating-point", type_q(TT, at));
        return;
    }
    if (fam == 0 && integ && !type_is_signed(TT, at) && tkind(c, at) != TY_BOOL)
        cwarn(c, call_loc(c, k[0]), "absolute-value", "taking the absolute "
              "value of unsigned type %s has no effect", type_q(TT, at));
    if (fam != 2) {
        bft = type_canon(TT, bt_func_type(c, bt));
        if (!type_ent(TT, bft)->n)
            return;
        ft = type_params(TT, bft)[0];
    }
    if (cpx) {
        at = type_canon(TT, type_base(TT, type_canon(TT, at)));
        if (fam != 2)
            ft = type_canon(TT, type_base(TT, type_canon(TT, ft)));
    }
    {
        bool ok1, ok2;
        uint64_t sa = type_size(TT, at, &ok1), sf = type_size(TT, ft, &ok2);
        if (ok1 && ok2 && sf < sa)
            cwarn(c, call_loc(c, k[0]), "absolute-value", "absolute value "
                  "function '%s' given an argument of type %s but has "
                  "parameter of type %s which may cause truncation of value",
                  nm, type_q(TT, at), type_q(TT, ft));
    }
}

size_t asm_string(Checker *c, uint32_t n, char *out, size_t cap);

/* __builtin_shufflevector (v0, v1, index...): a vector of the indices'
 * count whose elements come from the two vectors. */
static void e_shufflevector(Checker *c, uint32_t i, const char *name)
{
    uint32_t av[66], an = nkids(c, i, av, 66), j;
    SrcLoc bl = call_loc(c, av[0]);
    TypeId t0, t1, e0, e1;
    unsigned nidx, nsub;
    if (an < 4 || an > 66) {
        cerror(c, bl, "wrong number of arguments to '__builtin_shuffle'");
        set_err(c, i);
        return;
    }
    for (j = 1; j < an; j++)
        if (node_err(c, av[j])) {
            set_err(c, i);
            return;
        }
    t0 = rvt(c, av[1]);
    t1 = rvt(c, av[2]);
    nidx = an - 3;
    if (tkind(c, t0) != TY_VECTOR || tkind(c, t1) != TY_VECTOR)
        cerror(c, bl, "'%s' arguments must be vectors", name);
    else if (!type_compatible(TT, e0 = unqual(c, vec_elem(c, t0)),
                              e1 = unqual(c, vec_elem(c, t1))))
        cerror(c, bl, "'%s' argument vectors must have the same element type",
               name);
    else if (nidx & (nidx - 1))
        cerror(c, bl, "'%s' must specify a result with a power of two number "
               "of elements", name);
    else {
        nsub = (type_ent(TT, type_canon(TT, t0))->n +
                type_ent(TT, type_canon(TT, t1))->n) / vec_esize(c, e0);
        for (j = 3; j < an; j++) {
            uint32_t x = av[j];
            bool cst = is_int(c, rvt(c, x)) &&
                       (c->ck[x] == K_ICE ||
                        (c->ck[x] == K_FOLD && (c->ef[x] & EF_CST)));
            int64_t v = cst ? cexpr_sval(c, x) : 0;
            if (!cst || v < -1 || v >= (int64_t)nsub) {
                cerror(c, bl, "invalid element index '%s' to '%s'", estr(c, x),
                       name);
                set_err(c, i);
                return;
            }
        }
        c->ty[i] = type_vector(TT, e0, (uint64_t)nidx * vec_esize(c, e0));
        c->ef[i] = 0;
        for (j = 1; j < an; j++)
            c->ef[i] |= c->ef[av[j]] & EF_PROP;
        return;
    }
    set_err(c, i);
}

/* The line loc has after the #line directives before it (found by scanning
 * the text back; the checker has no line map). */
static uint32_t presumed_line(Checker *c, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    uint32_t line, col, l, len;
    if (!f)
        return 0;
    srcmgr_linecol(f, loc, &line, &col);
    for (l = line; l-- > 1;) {
        const char *t = srcmgr_line_text(f, l, &len), *e = t + len;
        unsigned long v;
        char *end;
        while (t < e && (*t == ' ' || *t == '\t'))
            t++;
        if (t >= e || *t++ != '#')
            continue;
        while (t < e && (*t == ' ' || *t == '\t'))
            t++;
        if (e - t > 4 && !strncmp(t, "line", 4))
            t += 4;
        while (t < e && (*t == ' ' || *t == '\t'))
            t++;
        if (t >= e || *t < '0' || *t > '9')
            continue;
        v = strtoul(t, &end, 10);
        return (uint32_t)(v + (line - (l + 1)));
    }
    return line;
}

/* A call of __builtin_FILE () or __builtin_FUNCTION (). */
static bool loc_builtin(Checker *c, uint32_t i)
{
    uint32_t k[2], f;
    const char *nm;
    if (ntag(c, i) != N_CALL || nkids(c, i, k, 2) != 1)
        return false;
    f = strip_paren(c, k[0]);
    if (f == NO_NODE || ntag(c, f) != N_IDENT)
        return false;
    nm = cident(c, cnode_ident(c, f));
    return !strcmp(nm, "__builtin_FILE") || !strcmp(nm, "__builtin_FUNCTION");
}

/* __func__, __FUNCTION__ or __PRETTY_FUNCTION__: the function name. */
static bool predef_ident(Checker *c, uint32_t n)
{
    const char *nm = cident(c, cnode_ident(c, n));
    return lookup_ord(c, cnode_ident(c, n)) == SYM_NONE &&
           (!strcmp(nm, "__func__") || !strcmp(nm, "__FUNCTION__") ||
            !strcmp(nm, "__PRETTY_FUNCTION__"));
}

/* Two address operands that gcc folds to the same address: equal string
 * literals, or the same __builtin_FILE/FUNCTION call. */
bool same_addr_const(Checker *c, uint32_t a, uint32_t b)
{
    char x[512], y[512];
    size_t lx, ly;
    a = strip_paren(c, a);
    b = strip_paren(c, b);
    if (a == NO_NODE || b == NO_NODE)
        return false;
    if (loc_builtin(c, a) && loc_builtin(c, b))
        return cnode_ident(c, strip_paren(c, first_child(c, a))) ==
               cnode_ident(c, strip_paren(c, first_child(c, b)));
    if (ntag(c, a) == N_IDENT && ntag(c, b) == N_IDENT)
        return predef_ident(c, a) && predef_ident(c, b);
    if (ntag(c, a) != N_STRING || ntag(c, b) != N_STRING)
        return false;
    lx = asm_string(c, a, x, sizeof x);
    ly = asm_string(c, b, y, sizeof y);
    return lx < sizeof x - 1 && lx == ly && !memcmp(x, y, lx);
}

/* __builtin_complex (re, im) */
static void e_builtin_complex(Checker *c, uint32_t i, SrcLoc bl)
{
    uint32_t av[8], an = nkids(c, i, av, 8);
    TypeId t0, t1;
    cc90(c, bl, NULL, "ISO C90 does not support complex types");
    if (an != 3) {
        cerror(c, bl, "wrong number of arguments to '__builtin_complex'");
        set_err(c, i);
        return;
    }
    if (node_err(c, av[1]) || node_err(c, av[2])) {
        set_err(c, i);
        return;
    }
    t0 = rvt(c, av[1]);
    t1 = rvt(c, av[2]);
    if (!is_flt(c, t0) || is_decimal_flt(c, t0) || !is_flt(c, t1) ||
        is_decimal_flt(c, t1)) {
        cerror(c, bl, "'__builtin_complex' operand not of real binary "
               "floating-point type");
        set_err(c, i);
        return;
    }
    if (mainv(c, t0) != mainv(c, t1)) {
        cerror(c, bl, "'__builtin_complex' operands of different types");
        set_err(c, i);
        return;
    }
    c->ty[i] = type_complex(TT, mainv(c, t0));
    c->ef[i] = (c->ef[av[1]] | c->ef[av[2]]) & EF_PROP;
    if (c->ck[av[1]] == K_FLOAT && c->ck[av[2]] == K_FLOAT)
        cplx_set(c, i, c->fv.data[c->cv[av[1]]], c->fv.data[c->cv[av[2]]]);
}

#include "c/cbuiltin_pure.h"

/* Whether some entry of cbuiltin_pure starts with ch: most calls are to
 * names that do not, and skip the search. */
static bool pure_first_char(char ch)
{
    static unsigned char seen[256], ready;
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {
        size_t k;
        for (k = 0; k < sizeof cbuiltin_pure / sizeof *cbuiltin_pure; k++)
            seen[(unsigned char)cbuiltin_pure[k][0]] = 1;
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    return seen[(unsigned char)ch];
}

/* A call gcc builds without TREE_SIDE_EFFECTS: a const or pure function (or a
 * library built-in that is one) whose arguments have none either. */
static bool call_pure(Checker *c, uint32_t i, uint32_t callee)
{
    uint32_t f = strip_paren(c, callee), av[32], an, j;
    const char *name;
    uint32_t ref;
    size_t lo = 0, hi = sizeof cbuiltin_pure / sizeof *cbuiltin_pure;
    bool pure = false;
    if (f == NO_NODE || ntag(c, f) != N_IDENT)
        return false;
    an = nkids(c, i, av, 32);
    for (j = 1; j < an && j < 32; j++)
        if (c->ef[av[j]] & EF_SIDE)
            return false;
    name = cident(c, cnode_ident(c, f));
    ref = lookup_ord(c, cnode_ident(c, f));
    if (ref != SYM_NONE) {
        if (csym(c, ref)->kind != CS_FUNC)
            return false;
        pure = csym(c, ref)->aset &&
               (cdecl_aset_has(c, csym(c, ref)->aset, "const", NULL) ||
               cdecl_aset_has(c, csym(c, ref)->aset, "pure", NULL));
    }
    if (!pure) {
        if (ref == SYM_NONE && strncmp(name, "__builtin_", 10))
            return false;
        if (!strncmp(name, "__builtin_", 10))
            name += 10;
        if (!pure_first_char(*name))
            return false;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            int r = strcmp(name, cbuiltin_pure[mid]);
            if (!r) {
                pure = true;
                break;
            }
            if (r < 0)
                hi = mid;
            else
                lo = mid + 1;
        }
    }
    return pure;
}

void e_call(Checker *c, uint32_t i)
{
    uint32_t k[3], n = nkids(c, i, k, 3), f;
    TypeId t;
    if (n == 0) {
        set_err(c, i);
        return;
    }
    f = strip_paren(c, k[0]);
    if (f != NO_NODE && ntag(c, f) == N_IDENT && f == k[0] &&
        lookup_ord(c, cnode_ident(c, f)) == SYM_NONE) {
        const char *name = cident(c, cnode_ident(c, f));
        if (!builtin_args_ok(c, i, k[0], name)) {
            set_err(c, i);
            return;
        }
        if (!strncmp(name, "__builtin___", 12)) {  /* the _chk functions */
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an <= 32)
                sizeof_memaccess(c, av, an, name);
        }
        if (!strncmp(name, "__builtin_", 10) && builtin_nonnull(name)) {
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an <= 32)
                check_nonnull(c, av, an, builtin_nonnull(name), NULL, 0, false,
                              true, call_loc(c, k[0]));
        }
        if (!strncmp(name, "__builtin_", 10) &&
            (builtin_format_pos(name) || builtin_scanf_pos(name))) {
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an <= 32)
                check_format_literal(c, av, an, NULL, name, call_loc(c, k[0]));
        }
        if (!strcmp(name, "__builtin_strlen")) {
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an <= 32)
                check_strlen(c, av, an);
        }
        {
            TypeId rt = atomic_result(c, i, name);
            if (!is_err(c, rt)) {
                c->ty[i] = rt;
                c->ef[i] = EF_SIDE;
                return;
            }
        }
        if (!strcmp(name, "__builtin_speculation_safe_value")) {
            /* the result has the type of the first argument */
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an >= 2 && an <= 32 && !node_err(c, av[1]) &&
                !is_err(c, rvt(c, av[1]))) {
                c->ty[i] = unqual(c, rvt(c, av[1]));
                c->ef[i] = EF_SIDE;
                return;
            }
        }
        if (!strcmp(name, "__builtin_choose_expr") ||
            !strcmp(name, "__builtin_call_with_static_chain")) {
            uint32_t av[4], an = nkids(c, i, av, 4), j;
            bool choose = name[10] == 'c' && name[11] == 'h';
            SrcLoc bl = call_loc(c, k[0]);
            if (an > 4 || an - 1 != (choose ? 3u : 2u)) {
                cerror(c, bl, "wrong number of arguments to '%s'", name);
                set_err(c, i);
                return;
            }
            for (j = 1; j < an; j++)
                if (node_err(c, av[j])) {
                    set_err(c, i);
                    return;
                }
            if (choose) {
                if (c->ck[av[1]] != K_ICE || !is_int(c, rvt(c, av[1])))
                    cerror(c, bl, "first argument to '__builtin_choose_expr' "
                           "not a constant");
                copy_node(c, i, c->cv[av[1]] && c->ck[av[1]] == K_ICE
                                    ? av[2] : av[3]);
                return;
            }
            {
                uint32_t a0 = strip_paren(c, av[1]);
                if (a0 == NO_NODE || ntag(c, a0) != N_CALL)
                    cerror(c, bl, "first argument to "
                           "'__builtin_call_with_static_chain' must be a call "
                           "expression");
                else if (!is_ptr(c, rvt(c, av[2])))
                    cerror(c, bl, "second argument to "
                           "'__builtin_call_with_static_chain' must be a "
                           "pointer type");
            }
            copy_node(c, i, av[1]);
            return;
        }
        if (!strcmp(name, "__builtin_shufflevector")) {
            e_shufflevector(c, i, name);
            return;
        }
        if (!strcmp(name, "__builtin_tgmath")) {
            if (!e_tgmath(c, i))
                set_err(c, i);
            return;
        }
        if (!strcmp(name, "__builtin_complex")) {
            e_builtin_complex(c, i, call_loc(c, k[0]));
            return;
        }
        if (!strcmp(name, "__builtin_constant_p") && n >= 2) {
            c->ty[i] = TYPE_B(INT);
            if (c->ck[k[1]] == K_ICE && !(c->ef[k[1]] & EF_OVERFLOW)) {
                c->ck[i] = K_ICE;
                c->cv[i] = 1;
                c->ef[i] = EF_INTOPS;
            } else {
                /* at -O0 gcc folds the call to 1 for a constant, else 0 */
                c->ck[i] = K_ICE;
                c->cv[i] = is_const(c, k[1]);
                c->ef[i] = EF_INTOPS;
            }
            return;
        }
        if (!strcmp(name, "__builtin_LINE") && n == 1) {
            c->ty[i] = TYPE_B(INT);
            c->ck[i] = K_ICE;
            c->cv[i] = presumed_line(c, cnode_loc(c, i));
            c->ef[i] = EF_INTOPS;
            return;
        }
        if (loc_builtin(c, i)) {        /* the address of a string */
            c->ty[i] = type_ptr(TT, type_qual(TYPE_B(CHAR), TQ_CONST));
            c->ck[i] = K_ADDR;
            c->cb[i] = CB_NODE | i;
            return;
        }
        if (!strcmp(name, "__builtin_expect") && n >= 2) {
            c->ty[i] = TYPE_B(LONG);
            if (has_ival(c, k[1])) {
                c->ck[i] = K_FOLD;
                c->cv[i] = cexpr_trunc(c, c->ty[i], c->cv[k[1]]);
                c->ef[i] = EF_CST;
            }
            return;
        }
    }
    if (node_err(c, k[0])) {
        set_err(c, i);
        return;
    }
    t = rvt(c, k[0]);
    if (!is_ptr(c, t) || !is_func(c, pointee(c, t))) {
        uint32_t ref = f != NO_NODE && ntag(c, f) == N_IDENT
                           ? lookup_ord(c, cnode_ident(c, f)) : SYM_NONE;
        Diagnostic *d = ref != SYM_NONE
            ? cerror_d(c, callee_err_loc(c, k[0]), "called object '%s' is not a "
                       "function or function pointer", estr(c, k[0]))
            : cerror_d(c, callee_err_loc(c, k[0]), "called object is not a "
                       "function or function pointer");
        if (ref != SYM_NONE)
            cnote(c, d, csym(c, ref)->loc, "declared here");
        set_err(c, i);
        return;
    }
    warn_for_abs(c, i, k, n);
    DiagOrd o0 = diag_ord(c->diag, c->nodes[i].flags & NF_CUT ? ORD_CUT
                                                              : ORD_NORMAL);
    bool ok = call_args(c, i, k[0], t);
    diag_ord(c->diag, o0);
    if (!ok) {
        set_err(c, i);
        return;
    }
    {
        TypeId ret = type_base(TT, pointee(c, t));
        SrcLoc cl = call_loc(c, k[0]);
        uint32_t cf = strip_paren(c, k[0]);
        bool qv = is_void(c, ret) && tquals(c, ret) != 0;
        /* a function designator cast to an incompatible function type */
        if (cf != NO_NODE && ntag(c, cf) == N_CAST) {
            uint32_t ck2[2], op;
            if (nkids(c, cf, ck2, 2) == 2 &&
                (op = strip_paren(c, ck2[1])) != NO_NODE &&
                ntag(c, op) == N_IDENT && is_func(c, c->ty[op]) &&
                !type_compatible(TT, mvt(c, c->ty[op]),
                                 mvt(c, pointee(c, t)))) {
                cwarn(c, cl, "", "function called through a non-compatible "
                      "type");
                if (qv)
                    cpedwarn(c, cl, "", "function with qualified void return "
                             "type called");
            }
        }
        if (is_void(c, ret)) {
            if (qv)
                cpedwarn(c, cl, "", "function with qualified void return type "
                         "called");
        } else if (!complete(c, ret)) {
            incomplete_error(c, cl, NO_NODE, ret);
            set_err(c, i);
            return;
        }
    }
    c->ty[i] = unqual(c, type_base(TT, pointee(c, t)));
    c->ef[i] = call_pure(c, i, k[0]) ? 0 : EF_SIDE;
}

void alias_deref(Checker *c, uint32_t p, bool use_loc, SrcLoc loc);

void e_index(Checker *c, uint32_t i)
{
    uint32_t k[2], a, x;
    SrcLoc loc = cnode_loc(c, i);
    TypeId pt, et;
    bool swapped = false;
    if (nkids(c, i, k, 2) < 2 || node_err(c, k[0]) || node_err(c, k[1])) {
        set_err(c, i);
        return;
    }
    a = k[0];
    x = k[1];
    if (tkind(c, c->ty[a]) == TY_VECTOR) {
        if (!is_int(c, rvt(c, x))) {
            cerror(c, loc, "array subscript is not an integer");
            set_err(c, i);
            return;
        }
        c->ty[i] = type_base(TT, type_canon(TT, c->ty[a])) |
                   TYPE_QUALS(c->ty[a]);
        c->ef[i] = (c->ef[a] & EF_LVALUE) | ((c->ef[a] | c->ef[x]) & EF_PROP);
        if (has_ival(c, x)) {      /* build_array_ref: a constant index */
            bool ok = false;
            uint64_t es = type_size(TT, c->ty[i], &ok);
            uint64_t vb = type_ent(TT, type_canon(TT, c->ty[a]))->n;
            if (ok && es && (ival_neg(c, rvt(c, x), c->cv[x]) ||
                             c->cv[x] >= vb / es))
                cwarn(c, loc, "array-bounds=", "index value is out of bound");
        }
        return;
    }
    if (!is_array(c, c->ty[a]) && !is_ptr(c, c->ty[a])) {
        if (!is_array(c, c->ty[x]) && !is_ptr(c, c->ty[x])) {
            cerror(c, loc, "subscripted value is neither array nor pointer "
                   "nor vector");
            set_err(c, i);
            return;
        }
        a = k[1];
        x = k[0];
        swapped = true;
    }
    /* require_complete_type at gcc's input_location: the line's first token */
    if (!rvalue_ok_at(c, x, cinput_loc(c, c->nodes[i].tok))) {
        set_err(c, i);
        return;
    }
    if (!is_int(c, rvt(c, x))) {
        cerror(c, loc, "array subscript is not an integer");
        set_err(c, i);
        return;
    }
    pt = rvt(c, a);
    if (is_func(c, pointee(c, pt))) {
        cerror(c, loc, "subscripted value is pointer to function");
        set_err(c, i);
        return;
    }
    if (!swapped && mainv(c, rvt(c, x)) == TYPE_B(CHAR) && !is_intcst(c, x))
        cwarn(c, expr_loc(c, x), "char-subscripts",
              "array subscript has type 'char'");
    if (is_array(c, c->ty[a])) {
        uint32_t v = strip_paren(c, a);
        while (v != NO_NODE && ntag(c, v) == N_MEMBER_EXPR &&
               !(c->nodes[v].flags & NF_ARROW))
            v = strip_paren(c, first_child(c, v));
        if (v != NO_NODE && ntag(c, v) == N_IDENT &&
            (c->ef[v] & EF_REGISTER))
            ped(c, i, loc, "ISO C forbids subscripting 'register' array");
        if (!(c->ef[a] & EF_LVALUE))
            cc90(c, loc, NULL, "ISO C90 forbids subscripting non-lvalue "
                 "array");
        et = elem_of(c, c->ty[a]);
    } else {
        if (!ptr_arith_ok(c, i, loc, pt)) {
            set_err(c, i);
            return;
        }
        et = pointee(c, pt);
        if (is_void(c, et) && !inhibited(c, i, false))
            cwarn(c, loc, "", "dereferencing 'void *' pointer");
        if (has_ival(c, x) && c->cv[x] == 0)    /* p[0] is *p */
            alias_deref(c, a, true, loc);
    }
    c->ty[i] = et;
    c->ef[i] = EF_LVALUE | ((c->ef[a] | c->ef[x]) & EF_PROP);
    if (tquals(c, et) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (c->ck[a] == K_ADDR && has_ival(c, x)) {
        int64_t idx = ival_neg(c, rvt(c, x), c->cv[x]) ? (int64_t)c->cv[x]
                                                       : (int64_t)c->cv[x];
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = c->cb[a];
        c->cv[i] = c->cv[a] + (uint64_t)idx * elem_size(c, pt);
        addr_rvalue(c, i);
    }
}

/* The record a member access applies to: *rec, false after an error. */
static bool member_datum(Checker *c, uint32_t i, uint32_t d, TypeId *rec)
{
    SrcLoc loc = ctok_loc(c, c->nodes[i].tok - 1);
    if (c->nodes[i].flags & NF_ARROW) {
        TypeId t = rvt(c, d);
        if (!is_ptr(c, t)) {
            cerror(c, loc, "invalid type argument of '->' (have %s)",
                   type_q(TT, t));
            return false;
        }
        *rec = pointee(c, t);
        if (is_void(c, *rec) && !inhibited(c, i, false))
            cwarn(c, loc, "", "dereferencing 'void *' pointer");
        alias_deref(c, d, false, loc);
        return true;
    }
    *rec = c->ty[d];
    return true;
}

void e_member(Checker *c, uint32_t i)
{
    uint32_t d = first_child(c, i), name = cnode_ident(c, i);
    SrcLoc loc;
    TypeId rec, ft;
    const Field *f;
    uint64_t off = 0;
    unsigned q = 0;
    bool arrow = (c->nodes[i].flags & NF_ARROW) != 0;
    if (d == NO_NODE || node_err(c, d) || (c->nodes[i].flags & NF_ERROR) ||
        !name) {
        set_err(c, i);
        return;
    }
    loc = ctok_loc(c, c->nodes[i].tok - 1);
    if (!member_datum(c, i, d, &rec)) {
        set_err(c, i);
        return;
    }
    if (!is_record(c, rec)) {
        TypeId r = rec;
        if (is_ptr(c, r) && is_record(c, pointee(c, r))) {
            if (arrow)
                cerror(c, loc, "'%s' is a pointer to pointer; did you mean to "
                       "dereference it before applying '->' to it?",
                       estr(c, d));
            else
                cerror(c, loc, "'%s' is a pointer; did you mean to use '->'?",
                       estr(c, d));
        } else if (!is_err(c, r)) {
            cerror(c, loc, "request for member '%s' in something not a "
                   "structure or union", cident(c, name));
        }
        set_err(c, i);
        return;
    }
    if (!complete(c, rec)) {
        incomplete_error(c, loc, NO_NODE, rec);
        set_err(c, i);
        return;
    }
    f = find_field(c, rec, name, &off, &q);
    if (!f) {
        Best b;
        const char *sug;
        best_init(&b, cident(c, name), &c->fuzzy_work);
        fuzzy_fields(c, &b, rec);
        sug = best_get(&b);
        if (sug)
            cerror(c, cnode_loc(c, i), "%s has no member named '%s'; did you "
                   "mean '%s'?", type_q(TT, rec), cident(c, name), sug);
        else
            cerror(c, loc, "%s has no member named '%s'", type_q(TT, rec),
                   cident(c, name));
        set_err(c, i);
        return;
    }
    cdep_report(c, cinput_loc(c, c->nodes[i].tok), f->name, f->dep, f->dmsg,
                &f->loc);
    ft = type_qual(f->ty, q | tquals(c, rec));
    c->ty[i] = ft;
    c->ef[i] = c->ef[d] & EF_PROP;
    if (tquals(c, ft) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (arrow || (c->ef[d] & EF_LVALUE))
        c->ef[i] |= EF_LVALUE;
    if (f->flags & FF_BITFIELD) {
        c->ef[i] |= EF_BITFIELD;
        if (f->width < type_int_bits(TT, TYPE_B(INT)))
            c->ef[i] |= EF_BFPROMOTE;
        return;
    }
    if (arrow ? c->ck[d] == K_ADDR : (c->ef[d] & EF_ADDRLV) != 0) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = c->cb[d];
        c->cv[i] = c->cv[d] + off / 8;
        addr_rvalue(c, i);
    }
}

