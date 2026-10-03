/* cdecl.c - the checker's declarations: specifiers, declarators, structs,
 * unions, enums, functions (a port of gcc 13's c-decl.cc and the parts of
 * c-parser.cc that decide where its diagnostics point).  Expressions are
 * cexpr.c's; the walk and the scopes are check.c's (check_int.h).
 *
 * The walk visits the nodes of a unit in post-order.  What each visit does:
 *   SPECS        builds the declaration specifiers (gcc's c_declspecs) and
 *                pushes them on Checker.specs; the consumer (DECL, FUNC_DEF,
 *                PARAM, TYPE_NAME, MEMBER_DECL) pops them
 *   STRUCT/ENUM  the tag type (start_struct/finish_struct at OPEN/here)
 *   FUNC         get_parm_info: checks on the parameter list
 *   DECLARED     start_decl: the declarator is analysed (grokdeclarator) and
 *                the declared name enters its scope
 *   INIT_DECL    finish_decl: the initializer completes an array type, ...
 *   MEMBER       grokfield
 *   TYPE_NAME    groktypename (result in Checker.ty[node])
 *
 * Side arrays (Checker.ty/cv/cb/ef) are used per tag:
 *   STRUCT/ENUM  ty: the tag's type; cv: TypeSpecKind;
 *                ef bit 1: the tag was first declared in a prototype scope
 *   TYPE_NAME    ty: the type
 *   PARAM        ty: the type in the function type (adjusted; error type if
 *                erroneous); cb: symbol ref + 1 of the named parameter;
 *                cv: bit 0 named, bit 1 register
 *   FUNC         cv: bit 0 (void) only, bit 1 error in the list
 *   DECLARED     ty: the declared type; cb: symbol ref + 1
 *
 * Symbols: one CSym per entity.  Besides the CSF_* of check_int.h, flags use
 * two private bits: CSF_DECL_EXTERNAL and CSF_TREE_STATIC (gcc's
 * DECL_EXTERNAL/TREE_STATIC); TREE_PUBLIC is linkage == LK_EXTERNAL.
 * Symbols with external linkage (and declarations of externals at block
 * scope) are persistent (gsyms); the rest live for the unit (lsyms).
 *
 * --dump-types prints, per line:
 *   typedef NAME = TYPE
 *   var NAME: TYPE [static|extern|tentative]
 *   func NAME: TYPE [static|extern|inline|defined]
 *   enumconst NAME = VALUE (TYPE)
 *   record layouts, as type_dump_record prints them, when a struct or union
 *   is completed.
 * Names of block-scope entities are printed with the function's name and
 * a colon before them ("f:x"). */
#include "c/check_int.h"

#include <inttypes.h>
#include <string.h>

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)

enum { DC_NORMAL, DC_FIELD, DC_PARM, DC_TYPENAME };

/* ---- small helpers ------------------------------------------------------ */

static unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static const Tok *tokp(const Checker *c, uint32_t tok)
{
    return &c->u->toks[tok].t;
}

static int tpunct(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return P_NONE;
    t = tokp(c, tok);
    return t->kind == TK_PUNCT ? t->punct : P_NONE;
}

static int tckw(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return CK_NONE;
    t = tokp(c, tok);
    if (t->kind != TK_IDENT)
        return CK_NONE;
    return ident_by_id(c->in, t->aux)->ckw & 0xFF;
}

static const char *tstr(const Checker *c, uint32_t tok)
{
    const Tok *t = tokp(c, tok);
    if (t->kind == TK_IDENT)
        return ident_by_id(c->in, t->aux)->str;
    return "";
}

static bool tfrom_macro(const Checker *c, uint32_t tok)
{
    return c->u->toks[tok].exp != 0;
}

static SrcLoc tloc(const Checker *c, uint32_t tok)
{
    if (tok >= c->u->ntoks)
        return c->last_bol;
    return ctok_loc(c, tok);
}

typedef struct Kids {
    uint32_t buf[24];
    uint32_t *p;
    uint32_t n;
} Kids;

static void kids_get(const Checker *c, uint32_t i, Kids *k)
{
    uint32_t n = node_children(c->nodes, i, k->buf, 24);
    k->p = k->buf;
    if (n > 24) {
        k->p = xmalloc(n * sizeof *k->p);
        node_children(c->nodes, i, k->p, n);
    }
    k->n = n;
}

static void kids_free(Kids *k)
{
    if (k->p != k->buf)
        free(k->p);
}

/* The last child of i (NO_NODE if none). */
static uint32_t last_child(const Checker *c, uint32_t i)
{
    return c->nodes[i].size > 1 ? i - 1 : NO_NODE;
}

/* The first child of i. */
static uint32_t first_child(const Checker *c, uint32_t i)
{
    uint32_t k, f = cfirst(c, i);
    if (c->nodes[i].size <= 1)
        return NO_NODE;
    k = i - 1;
    while (cfirst(c, k) > f)
        k = cfirst(c, k) - 1;
    return k;
}

/* The declarator inside a PTR/ARRAY/FUNC node (NO_NODE: abstract). */
static uint32_t inner_decl(const Checker *c, uint32_t i)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (is_declarator_tag(ntag(c, k.p[j]))) {
            r = k.p[j];
            break;
        }
    kids_free(&k);
    return r;
}

/* First / last token of a subtree. */
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
        if (c->nodes[k].tag == N_STRING && c->nodes[k].aux)
            t += c->nodes[k].aux - 1u;
        if (t > m)
            m = t;
    }
    return m;
}

/* The token after subtree i, closing brackets included. */
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

/* ---- gcc's input_location ---------------------------------------------- */

/* The parser set input_location to token tok itself (a tag name, an
 * enumerator, a parse error). */
static void iloc_event(Checker *c, uint32_t tok)
{
    uint64_t a = c->u->first_tok + tok + 1;
    if (a > c->iloc_tok)
        c->iloc_tok = a;
}

/* input_location while the parser's lookahead is token L. */
static SrcLoc iloc(Checker *c, uint32_t L)
{
    uint32_t b;
    if (!c->u->ntoks)
        return c->last_bol;
    if (L >= c->u->ntoks)
        L = c->u->ntoks - 1;
    b = cbol_tok(c, L) - 1;
    if (c->iloc_tok > c->u->first_tok) {
        uint64_t t = c->iloc_tok - 1 - c->u->first_tok;
        if (t <= L && (b == UINT32_MAX || t >= b))
            return ctok_loc(c, (uint32_t)t);
    }
    return cinput_loc(c, L);
}

SrcLoc cdecl_iloc(Checker *c, uint32_t tok)
{
    return iloc(c, tok);
}

/* A pedantic pedwarn about node/token tok, off under __extension__. */
static bool in_extension(Checker *c, uint32_t node)
{
    return node != NO_NODE && cexpr_in_extension(c, node);
}

/* ---- attributes ---------------------------------------------------------- */

static void access_check(Checker *c, const uint32_t *arg, uint32_t n, SrcLoc il,
                         TypeId ft);

/* The attribute's name without the leading and trailing "__". */
static void attr_norm(const char *s, char *out, size_t n)
{
    size_t len = strlen(s);
    if (len > 4 && s[0] == '_' && s[1] == '_' && s[len - 1] == '_' &&
        s[len - 2] == '_') {
        s += 2;
        len -= 4;
    }
    if (len >= n)
        len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}

static bool is_pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

/* check_user_alignment for the constant expression node e: the alignment
 * in bytes (0: none, silently ignored or already diagnosed).  loc: where
 * gcc's error() points; objfile: the attribute form (gcc's caller passes
 * true), which words the upper bound as the object file maximum. */
static uint32_t check_user_alignment(Checker *c, uint32_t e, SrcLoc loc,
                                     bool objfile)
{
    TypeId t;
    int64_t v;
    if (c->ck[e] == K_ERR)
        return 0;
    t = c->ty[e];
    if (!(c->ck[e] == K_ICE || (c->ck[e] == K_FOLD && (c->ef[e] & EF_CST))) ||
        !type_is_integer(TT, t)) {
        cerror(c, loc, "requested alignment is not an integer constant");
        return 0;
    }
    v = cexpr_sval(c, e);
    if (v == 0)
        return 0;
    if (v < 0 || !is_pow2((uint64_t)v)) {
        cerror(c, loc, "requested alignment '%lld' is not a positive power "
               "of 2", (long long)v);
        return 0;
    }
    if (objfile && v > ((int64_t)1 << 28)) {
        cerror(c, loc, "requested alignment '%lld' exceeds object file "
               "maximum %u", (long long)v, 1U << 28);
        return 0;
    }
    if (v >= ((int64_t)1 << 29)) {
        cerror(c, loc, "requested alignment '%lld' exceeds maximum %u",
               (long long)v, 1U << 28);
        return 0;
    }
    return (uint32_t)v;
}

/* handle_*_attribute argument counts: "wrong number of arguments", at
 * input_location. */
static void gnu_attr_argc(Checker *c, uint32_t attr)
{
    static const struct { const char *n; uint32_t lo, hi; } t[] = {
        {"access", 1, 3}, {"alloc_align", 1, 1}, {"assume_aligned", 1, 2}, {"copy", 1, 1},
        {"malloc", 0, 2}, {"section", 1, 1}, {"simd", 0, 1},
        {"strict_flex_array", 1, 1}, {"zero_call_used_regs", 1, 1}};
    Kids k;
    uint32_t j;
    if (tokp(c, cnode(c, attr)->tok)->kind == TK_PUNCT)
        return;
    uint32_t at = c->nodes[attr].tok;
    Kids sib;
    uint32_t up = attr, lv;
    for (lv = 0; lv < 3 && up != NO_NODE && at == c->nodes[attr].tok; lv++) {
        up = c->par[up];
        if (up == NO_NODE)
            break;
        kids_get(c, up, &sib);     /* a leading attribute: applied at the declarator */
        for (j = 0; j < sib.n; j++)
            if (sib.p[j] > attr && (ntag(c, sib.p[j]) == N_DECLARED ||
                                    ntag(c, sib.p[j]) == N_INIT_DECL)) {
                at = c->nodes[cfirst(c, sib.p[j])].tok;
                break;
            }
        kids_free(&sib);
    }
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        char name[48];
        Kids ak;
        size_t n;
        if (ntag(c, k.p[j]) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[k.p[j]].tok), name, sizeof name);
        kids_get(c, k.p[j], &ak);
        for (n = 0; n < sizeof t / sizeof *t; n++)
            if (!strcmp(name, t[n].n) && (ak.n < t[n].lo || ak.n > t[n].hi)) {
                Diagnostic *d = cerror_d(c, cinput_loc(c, at),
                       "wrong number of arguments specified for '%s' "
                       "attribute", name);
                if (d && t[n].lo == t[n].hi)
                    cnote(c, d, cinput_loc(c, at), "expected %u, found %u",
                          t[n].lo, (unsigned)ak.n);
                else if (d)
                    cnote(c, d, cinput_loc(c, at),
                          "expected between %u and %u, found %u", t[n].lo,
                          t[n].hi, (unsigned)ak.n);
            }
        if (!strcmp(name, "simd") && ak.n == 1) {
            char v[24];
            cdecl_attr_args(c, k.p[j], v, sizeof v);
            if (ntag(c, ak.p[0]) != N_STRING)
                cerror(c, cinput_loc(c, at),
                       "attribute 'simd' argument not a string");
            else if (strcmp(v, "\"inbranch\"") && strcmp(v, "\"notinbranch\""))
                cerror(c, cinput_loc(c, at), "only 'inbranch' and "
                       "'notinbranch' flags are allowed for '__simd__' "
                       "attribute");
        }
        kids_free(&ak);
    }
    kids_free(&k);
}

/* [[ns::name]]: the token of ns is followed by :: (two colons). */
static bool attr_scope_of(Checker *c, uint32_t tok)
{
    return c->opt.gnu && tpunct(c, tok + 1) == P_COLON &&
           tpunct(c, tok + 2) == P_COLON;
}

/* gcc's c_parser_std_attribute: a name without a namespace that is not one
 * of the standard attributes is pedwarned and dropped, at input_location
 * with the lookahead just past the name and its arguments.  (Before C2X
 * 'ns::name' is not parsed: 'ns' is such a name.) */
static void std_attr_unknown(Checker *c, uint32_t attr)
{
    static const char *const known[] = {"deprecated", "fallthrough",
        "maybe_unused", "nodiscard", "noreturn", "_Noreturn"};
    Kids k;
    uint32_t j;
    if (tokp(c, cnode(c, attr)->tok)->kind != TK_PUNCT)
        return;
    if (c->par[attr] != NO_NODE && ntag(c, c->par[attr]) == N_ENUMERATOR)
        iloc_event(c, cnode(c, c->par[attr])->tok);
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        char name[48];
        size_t n;
        if (ntag(c, k.p[j]) != N_ATTR_ITEM)
            continue;
        uint32_t at = c->nodes[k.p[j]].tok;
        attr_norm(tstr(c, at), name, sizeof name);
        if (c->opt.gnu && at >= 2 && tpunct(c, at - 1) == P_COLON &&
            tpunct(c, at - 2) == P_COLON)
            continue;           /* gnu::name, taken as a GNU attribute */
        for (n = 0; n < sizeof known / sizeof *known; n++)
            if (!strcmp(name, known[n]))
                break;
        if (n < sizeof known / sizeof *known)
            continue;
        if (attr_scope_of(c, at))
            cwarn(c, iloc(c, last_tok(c, k.p[j]) + 1), "attributes",
                  "'%s::%s' scoped attribute directive ignored", name,
                  tstr(c, at + 3));
        else
            cwarn(c, iloc(c, last_tok(c, k.p[j]) + 1), "attributes",
                  "'%s' attribute ignored", name);
    }
}

/* Whether declaration node d itself carries a deprecated/unavailable
 * attribute (not one on a nested parameter): its uses of such typedefs
 * are not reported. */
static bool decl_has_dep_attr(Checker *c, uint32_t d)
{
    uint32_t k;
    if (d == NO_NODE)
        return false;
    for (k = cfirst(c, d); k < d; k++) {
        char name[48];
        uint32_t at, owner;
        if (ntag(c, k) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[k].tok), name, sizeof name);
        if (strcmp(name, "deprecated") && strcmp(name, "unavailable"))
            continue;
        at = c->par[k];
        owner = at == NO_NODE ? NO_NODE : c->par[at];
        if (owner == d || (owner != NO_NODE && c->par[owner] == d &&
                           (ntag(c, owner) == N_INIT_DECL ||
                            ntag(c, owner) == N_MEMBER)))
            return true;
    }
    return false;
}

/* The attribute names gcc 13 registers for C on x86-64 (probed with
 * __has_attribute), sorted, without the __ affixes. */
static const char *const known_attrs[] = {
    "access", "alias", "aligned", "alloc_align", "alloc_size",
    "always_inline", "artificial", "assume", "assume_aligned",
    "callee_pop_aggregate_return", "cdecl", "cf_check", "cleanup", "cold",
    "common", "const", "constructor", "copy", "deprecated", "designated_init",
    "destructor", "error", "externally_visible", "fallthrough", "fastcall",
    "fd_arg", "fd_arg_read", "fd_arg_write", "fentry_name", "fentry_section",
    "flatten", "force_align_arg_pointer", "format", "format_arg",
    "function_return", "gcc_struct", "gnu_inline", "hot", "ifunc",
    "indirect_branch", "indirect_return", "interrupt", "leaf", "malloc",
    "may_alias", "maybe_unused", "mode", "ms_abi", "ms_hook_prologue",
    "ms_struct", "naked", "no_address_safety_analysis",
    "no_caller_saved_registers", "no_icf", "no_instrument_function",
    "no_profile_instrument_function", "no_reorder", "no_sanitize",
    "no_sanitize_address", "no_sanitize_coverage", "no_sanitize_thread",
    "no_sanitize_undefined", "no_split_stack", "no_stack_limit",
    "no_stack_protector", "nocf_check", "noclone", "nocommon",
    "nodirect_extern_access", "nodiscard", "noinit", "noinline", "noipa",
    "nonnull", "nonstring", "noplt", "noreturn", "nothrow",
    "objc_nullability", "objc_root_class", "optimize", "packed",
    "patchable_function_entry", "pure", "regparm", "retain",
    "returns_nonnull", "returns_twice", "scalar_storage_order", "section",
    "sentinel", "signed_bool_precision", "simd", "sseregparm",
    "stack_protect", "stdcall", "strict_flex_array", "symver", "sysv_abi",
    "tainted_args", "target", "target_clones", "thiscall", "tls_model",
    "transaction_callable", "transaction_may_cancel_outer",
    "transaction_pure", "transaction_safe", "transaction_safe_dynamic",
    "transaction_unsafe", "transaction_wrap", "transparent_union",
    "unavailable", "uninitialized", "unused", "used", "vector_mask",
    "vector_size", "visibility", "volatile", "warn_if_not_aligned",
    "warn_unused", "warn_unused_result", "warning", "weak", "weakref",
    "zero_call_used_regs",
};

static bool attr_known(const char *name)
{
    size_t lo = 0, hi = sizeof known_attrs / sizeof *known_attrs;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, known_attrs[mid]);
        if (!r)
            return true;
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return false;
}

/* malloc (dealloc[, pos]): the deallocator must name a function whose
 * parameter number pos (default 1) is a pointer. */
static void attr_malloc_dealloc(Checker *c, uint32_t arg, uint32_t pos,
                                SrcLoc loc)
{
    TypeId ft;
    if (ntag(c, arg) != N_IDENT || type_ckind(TT, c->ty[arg]) != TY_FUNC) {
        cerror(c, loc, "'malloc' attribute argument 1 does not name a "
               "function");
        return;
    }
    ft = c->ty[arg];
    if (type_ckind(TT, ft) != TY_FUNC)
        return;
    if (pos != NO_NODE) {
        uint32_t n = type_ent(TT, ft)->n;
        if (!(c->ck[pos] == K_ICE || c->ck[pos] == K_FOLD)) {
            cwarn(c, loc, "attributes", "'malloc' attribute argument has type "
                  "%s", type_q(TT, c->ty[pos]));
        } else if (type_ent(TT, ft)->flags & TF_NOPROTO) {
            /* nothing is known of the parameters */
        } else if (c->cv[pos] < 1) {
            cwarn(c, loc, "attributes", "'malloc' attribute argument value "
                  "'%lld' does not refer to a function parameter",
                  (long long)c->cv[pos]);
        } else if (c->cv[pos] > n) {
            cwarn(c, loc, "attributes", "'malloc' attribute argument value "
                  "'%lld' exceeds the number of function parameters %u",
                  (long long)c->cv[pos], n);
        } else if (type_ckind(TT, type_params(TT, ft)[c->cv[pos] - 1]) !=
                   TY_PTR) {
            cwarn(c, loc, "attributes", "'malloc' attribute argument value "
                  "'%lld' refers to parameter type %s", (long long)c->cv[pos],
                  type_q(TT, type_params(TT, ft)[c->cv[pos] - 1]));
        }
        return;
    }
    if (!type_ent(TT, ft)->n) {
        if (type_ent(TT, ft)->flags & TF_NOPROTO)
            cerror(c, loc, "'malloc' attribute argument 1 must take a "
                   "pointer type as its first argument");
        else
            cerror(c, loc, "'malloc' attribute argument 1 must take a "
                   "pointer type as its first argument; have 'void'");
    } else if (type_ckind(TT, type_params(TT, ft)[0]) != TY_PTR) {
        Diagnostic *d = cerror_d(c, loc, "'malloc' attribute argument 1 must "
                                 "take a pointer type as its first argument; "
                                 "have %s", type_q(TT, type_params(TT, ft)[0]));
        if (d && c->cb[arg] && !(c->cb[arg] & CB_NODE))
            cnote(c, d, csym(c, c->cb[arg] - 1)->loc,
                  "referenced symbol declared here");
    }
}

static bool is_rec(Checker *c, TypeId t);

/* 1: big-endian, 2: little-endian, 0: anything else. */
static int sso_value(Checker *c, uint32_t arg)
{
    uint32_t mi = cdep_msg(c, arg);
    const char *sv = c->dep_msgs.data[mi - 1];
    return !strcmp(sv, "big-endian") ? 1 : !strcmp(sv, "little-endian") ? 2 : 0;
}

/* The scalar_storage_order given on a tag node (`struct S __attribute__`). */
static uint8_t sso_of_tag(Checker *c, uint32_t n)
{
    Kids k;
    uint32_t j;
    uint8_t v = 0;
    kids_get(c, n, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char name[48];
            Kids ak;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), name, sizeof name);
            if (strcmp(name, "scalar_storage_order"))
                continue;
            kids_get(c, it.p[q], &ak);
            if (ak.n && ntag(c, ak.p[0]) == N_STRING)
                v = (uint8_t)sso_value(c, ak.p[0]);
            kids_free(&ak);
        }
        kids_free(&it);
    }
    kids_free(&k);
    return v;
}

/* handle_scalar_storage_order_attribute's errors.  gcc reports them at the
 * tag name for a tag attribute (or one following a tag), else at the token
 * after the attribute. */
static void sso_check(Checker *c, uint32_t attr)
{
    Kids k;
    uint32_t j, up = c->par[attr];
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        Kids ak;
        char name[48];
        SrcLoc loc;
        if (ntag(c, k.p[j]) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[k.p[j]].tok), name, sizeof name);
        if (strcmp(name, "scalar_storage_order"))
            continue;
        kids_get(c, k.p[j], &ak);
        if (!ak.n)
            continue;
        {
            uint32_t at = c->nodes[attr].tok, before;
            int depth = 0;
            bool tdef = false, tagp = up != NO_NODE &&
                        (ntag(c, up) == N_STRUCT || ntag(c, up) == N_ENUM);
            for (;; at++) {
                int pu = tpunct(c, at);
                if (pu == P_LPAREN)
                    depth++;
                else if (pu == P_RPAREN && --depth == 0) {
                    at++;
                    break;
                }
                if (at + 1 >= c->u->ntoks)
                    break;
            }
            loc = tloc(c, at);          /* the token after the attribute */
            before = c->nodes[attr].tok ? c->nodes[attr].tok - 1 : 0;
            if (tagp && tpunct(c, before) == P_RBRACE) {
                /* after the body: the tag name */
                uint32_t tt = c->nodes[up].tok + 1;
                while (!strncmp(tstr(c, tt), "__attribute", 11)) {
                    int dp = 0;
                    for (tt++; tt < c->u->ntoks; tt++) {
                        int pu = tpunct(c, tt);
                        if (pu == P_LPAREN)
                            dp++;
                        else if (pu == P_RPAREN && --dp == 0) {
                            tt++;
                            break;
                        }
                    }
                }
                if (tpunct(c, tt) == P_NONE)
                    loc = tloc(c, tt);
            } else if (!tagp || tpunct(c, before) != P_NONE ||
                       !strcmp(tstr(c, before), "struct") ||
                       !strcmp(tstr(c, before), "union") ||
                       !strcmp(tstr(c, before), "enum")) {
                /* before the tag name: reported at the name (loc) */
                if (!tagp)
                    goto decl_attr;
            } else {
                /* `struct S __attribute__`: a typedef's type attribute, else
                 * a declaration attribute (-Wattributes) */
                uint32_t tk;
            decl_attr:
                for (tk = c->nodes[attr].tok; tk-- > 0; ) {
                    int pu = tpunct(c, tk);
                    if (pu == P_SEMI || pu == P_LBRACE || pu == P_RBRACE)
                        break;
                    if (!strcmp(tstr(c, tk), "typedef")) {
                        tdef = true;
                        break;
                    }
                }
                if (!tdef) {
                    kids_free(&ak);
                    continue;
                }
                if (tagp)
                    loc = tloc(c, before);   /* the tag name */
            }
        }
        if (ntag(c, ak.p[0]) != N_STRING)
            cerror(c, loc, "attribute 'scalar_storage_order' argument not a "
                   "string");
        else if (!sso_value(c, ak.p[0]))
            cerror(c, loc, "attribute 'scalar_storage_order' argument must "
                   "be one of 'big-endian' or 'little-endian'");
        kids_free(&ak);
    }
    kids_free(&k);
}

/* Collects the type-affecting attributes of one ATTRIBUTE node. */
static void attr_collect(Checker *c, uint32_t attr, Attrs *a)
{
    Kids k;
    uint32_t j;
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t item = k.p[j], arg;
        char name[48];
        Kids ak;
        if (ntag(c, item) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[item].tok), name, sizeof name);
        kids_get(c, item, &ak);
        arg = ak.n ? ak.p[0] : NO_NODE;
        if (arg != NO_NODE && (!strcmp(name, "alias") || !strcmp(name, "ifunc") ||
                               !strcmp(name, "weakref")))
            a->defn = true;
        if (c->attr_defer && !c->attr_quiet && strcmp(name, "gnu") &&
            !attr_known(name) && !attr_scope_of(c, c->nodes[item].tok)) {
            if (a->nunk < 2)
                snprintf(a->unk[a->nunk++], sizeof a->unk[0], "%s", name);
        } else if (!c->attr_quiet && strcmp(name, "gnu") && !attr_known(name) &&
                   !attr_scope_of(c, c->nodes[item].tok))   /* gnu:: is a [[]] scope */
            cwarn(c, c->attr_at_set ? c->attr_at : iloc(c, c->nodes[item].tok),
                  "attributes", "'%s' attribute directive ignored", name);
        if (!strcmp(name, "aligned")) {
            uint32_t v;
            if (arg == NO_NODE)
                v = c->tgt->default_aligned;
            else
                v = check_user_alignment(c, arg, iloc(c, after_tok(c, attr)),
                                         true);
            if (v > a->aligned)
                a->aligned = v;
        } else if (!strcmp(name, "warn_if_not_aligned")) {
            a->wina = true;
            if (arg != NO_NODE)
                (void)check_user_alignment(c, arg,
                                           iloc(c, after_tok(c, attr)), true);
        } else if (!strcmp(name, "malloc") && arg != NO_NODE &&
                   c->ck[arg] != K_ERR) {
            attr_malloc_dealloc(c, arg, ak.n > 1 ? ak.p[1] : NO_NODE,
                                iloc(c, after_tok(c, attr)));
        } else if (!strcmp(name, "packed")) {
            a->packed = true;
        } else if (!strcmp(name, "copy") && arg != NO_NODE &&
                   type_ckind(TT, c->ty[arg]) == TY_PTR) {
            /* copy((T *)0) of a packed struct/union type copies packed */
            TypeId bt = type_canon(TT, type_base(TT, type_canon(TT, c->ty[arg])));
            if (type_ckind(TT, bt) == TY_STRUCT ||
                type_ckind(TT, bt) == TY_UNION) {
                const Record *rc = type_record(TT, bt);
                if (rc->flags & RF_PACKED)
                    a->packed = true;
                if ((rc->flags & RF_USER_ALIGN) && rc->align > a->aligned)
                    a->aligned = rc->align;
            }
        } else if ((!strcmp(name, "constructor") ||
                    !strcmp(name, "destructor")) && arg != NO_NODE) {
            int64_t pv = 0;
            SrcLoc il = cinput_loc(c, c->nodes[item].tok);
            bool isc = name[0] == 'c';
            if (!(c->ck[arg] == K_ICE || c->ck[arg] == K_FOLD) ||
                !type_is_integer(TT, c->ty[arg]) ||
                (pv = cexpr_sval(c, arg)) < 0 || pv > 65535)
                cerror(c, il, "%s priorities must be integers from 0 to "
                       "65535 inclusive", isc ? "constructor" : "destructor");
            else if (pv <= 100)
                cwarn(c, il, "prio-ctor-dtor", "%s priorities from 0 to 100 "
                      "are reserved for the implementation",
                      isc ? "constructor" : "destructor");
        } else if (!strcmp(name, "section") && arg != NO_NODE) {
            a->sec_any = true;
            if (ntag(c, arg) != N_STRING) {
                if (!a->sec_bad)
                    a->sec_bad = cinput_loc(c, c->nodes[item].tok);
            } else if (!a->sec) {
                a->sec = cdep_msg(c, arg);
            } else if (!a->sec2) {
                a->sec2 = cdep_msg(c, arg);
            }
        } else if (!strcmp(name, "scalar_storage_order") && arg != NO_NODE) {
            if (ntag(c, arg) == N_STRING)
                a->sso = (uint8_t)sso_value(c, arg);
        } else if (!strcmp(name, "ms_struct")) {
            a->ms = 1;
        } else if (!strcmp(name, "gcc_struct")) {
            a->ms = -1;
        } else if (!strcmp(name, "transparent_union")) {
            a->transparent_union = true;
        } else if (!strcmp(name, "noreturn") || !strcmp(name, "__noreturn__")) {
            a->noreturn = true;
        } else if (!strcmp(name, "deprecated") ||
                   !strcmp(name, "unavailable")) {
            if (name[0] == 'u')
                a->unavailable = true;
            else
                a->deprecated = true;
            if (arg != NO_NODE && ntag(c, arg) == N_STRING)
                a->dep_msg = cdep_msg(c, arg);
        } else if (!strcmp(name, "nonnull")) {
            uint64_t m = 0;
            uint32_t q;
            TypeId ft = c->attr_fty;
            bool fn = ft && type_ckind(TT, ft) == TY_FUNC;
            bool proto = fn && !(type_ent(TT, ft)->flags & TF_NOPROTO), ok = true;
            SrcLoc il = cinput_loc(c, c->nodes[item].tok);
            if (!ak.n) {
                if (fn && !proto) {
                    cerror(c, il, "'nonnull' attribute without arguments on a "
                           "non-prototype");
                    ok = false;
                } else
                    m = NN_ALL;
            }
            for (q = 0; q < ak.n && ok; q++) {
                uint32_t x = ak.p[q];
                int64_t v = 0;
                if (!(c->ck[x] == K_ICE || c->ck[x] == K_FOLD) ||
                    !type_is_integer(TT, c->ty[x]) ||
                    (v = cexpr_sval(c, x)) < 1) {
                    cwarn(c, il, "attributes",
                          "'nonnull' attribute argument is invalid");
                    ok = false;
                } else if (proto) {
                    const TypeEnt *fe = type_ent(TT, ft);
                    if ((uint64_t)v > fe->n && !(fe->flags & TF_VARIADIC)) {
                        cwarn(c, il, "attributes", "'nonnull' attribute "
                              "argument value '%lld' exceeds the number of "
                              "function parameters %u", (long long)v,
                              (unsigned)fe->n);
                        ok = false;
                    } else if ((uint64_t)v <= fe->n &&
                               type_ckind(TT, type_params(TT, ft)[v - 1]) !=
                                   TY_PTR) {
                        cwarn(c, il, "attributes", "'nonnull' attribute "
                              "argument value '%lld' refers to parameter type "
                              "%s", (long long)v,
                              type_q(TT, type_params(TT, ft)[v - 1]));
                        ok = false;
                    }
                }
                if (ok && v <= 63)
                    m |= (uint64_t)1 << (v - 1);
            }
            if (ok)
                a->nonnull |= m;
        } else if (!strcmp(name, "returns_nonnull")) {
            TypeId ft = c->attr_fty;
            if (ft && type_ckind(TT, ft) == TY_FUNC &&
                type_ckind(TT, type_ent(TT, ft)->base) != TY_PTR)
                cerror(c, cinput_loc(c, c->nodes[item].tok),
                       "'returns_nonnull' attribute on a function not "
                       "returning a pointer");
        } else if (!strcmp(name, "format") && ak.n == 3 &&
                   ntag(c, ak.p[0]) == N_IDENT) {
            char ar[24];
            int kind = 0;
            attr_norm(tstr(c, c->nodes[ak.p[0]].tok), ar, sizeof ar);
            if (!strcmp(ar, "printf") || !strcmp(ar, "gnu_printf"))
                kind = 1;
            else if (!strcmp(ar, "scanf") || !strcmp(ar, "gnu_scanf"))
                kind = 2;
            if (kind) {
                uint32_t xs = ak.p[1], xf = ak.p[2];
                if ((c->ck[xs] == K_ICE || c->ck[xs] == K_FOLD) &&
                    (c->ck[xf] == K_ICE || c->ck[xf] == K_FOLD)) {
                    int64_t sv = cexpr_sval(c, xs), fv = cexpr_sval(c, xf);
                    if (sv >= 1 && sv < 4096 && fv >= 0 && fv < 4096)
                        a->fmt = (uint32_t)kind << 24 | (uint32_t)sv << 12 |
                                 (uint32_t)fv;
                }
            }
        } else if (!strcmp(name, "zero_call_used_regs") && ak.n == 1) {
            static const char *const ok[] = {"skip", "used-gpr-arg", "used-arg",
                "used-gpr", "used", "all-gpr-arg", "all-arg", "all-gpr", "all"};
            char v[24];
            unsigned q;
            cdecl_attr_args(c, item, v, sizeof v);
            a->zcur = 3;
            snprintf(a->zcur_arg, sizeof a->zcur_arg, "%s", v + 1);
            a->zcur_arg[strcspn(a->zcur_arg, "\"")] = 0;
            if (ntag(c, ak.p[0]) != N_STRING)
                a->zcur = 2;
            else
                for (q = 0; q < sizeof ok / sizeof *ok; q++)
                    if (!strcmp(a->zcur_arg, ok[q]))
                        a->zcur = 1;
        } else if (!strcmp(name, "gnu_inline")) {
            a->gnu_inline = true;
        } else if (!strcmp(name, "noinline")) {
            a->noinline = true;
        } else if (!strcmp(name, "used")) {
            a->used = true;
        } else if (!strcmp(name, "designated_init")) {
            a->desig = true;
        } else if (!strcmp(name, "alias")) {
            a->alias = true;
        } else if (!strcmp(name, "ifunc")) {
            if (a->weak || a->weakref)
                a->e_wi = true;
            else
                a->ifunc = true;
        } else if (!strcmp(name, "weakref")) {
            a->weakref = true;
        } else if (!strcmp(name, "error")) {
            a->errattr = true;
        } else if (!strcmp(name, "warning")) {
            a->warnattr = true;
        } else if (!strcmp(name, "cleanup")) {
            a->cleanup = true;
            a->cleanup_arg = arg == NO_NODE ? 0 : arg;
        } else if (!strcmp(name, "unused")) {
            a->unused = true;
        } else if (!strcmp(name, "weak") || !strcmp(name, "__weak__")) {
            if (a->ifunc)
                a->e_iw = true;
            else
                a->weak = true;
        } else if (!strcmp(name, "vector_size") && arg != NO_NODE) {
            if ((c->ck[arg] == K_ICE || c->ck[arg] == K_FOLD) &&
                type_is_integer(TT, c->ty[arg])) {
                int64_t v = cexpr_sval(c, arg);
                SrcLoc il = cinput_loc(c, c->nodes[item].tok);
                if (v < 0 && type_is_signed(TT, c->ty[arg])) {
                    cerror(c, il, "'vector_size' attribute argument value "
                           "'%lld' is negative", (long long)v);
                } else if (v < 0) {
                    cerror(c, il, "'vector_size' attribute argument value "
                           "'%llu' exceeds 9223372036854775807",
                           (unsigned long long)v);
                } else if (a->vs_seen) {
                    if (!a->vs_dup)
                        a->vs_dup = il;
                } else {
                    a->vector_size = (uint64_t)v;
                    a->vs_seen = true;
                    a->vs_loc = il;
                }
            }
        } else if (!strcmp(name, "mode") && arg != NO_NODE &&
                   ntag(c, arg) == N_IDENT) {
            char m[16];
            attr_norm(tstr(c, c->nodes[arg].tok), m, sizeof m);
            a->has_mode = true;
            snprintf(a->mode_name, sizeof a->mode_name, "%s", m);
            a->mode_bytes = 0;
            a->mode_float = 0;
            if (!strcmp(m, "QI") || !strcmp(m, "byte"))
                a->mode_bytes = 1;
            else if (!strcmp(m, "HI"))
                a->mode_bytes = 2;
            else if (!strcmp(m, "SI"))
                a->mode_bytes = 4;
            else if (!strcmp(m, "DI"))
                a->mode_bytes = 8;
            else if (!strcmp(m, "TI"))
                a->mode_bytes = 16;
            else if (!strcmp(m, "word") || !strcmp(m, "pointer") ||
                     !strcmp(m, "unwind_word"))
                a->mode_bytes = c->tgt->ptr_size;
            else if (!strcmp(m, "SF"))
                a->mode_float = 1;
            else if (!strcmp(m, "DF"))
                a->mode_float = 2;
            else if (!strcmp(m, "XF"))
                a->mode_float = 3;
            else if (!strcmp(m, "TF"))
                a->mode_float = 4;
            else
                a->has_mode = false;
        }
        kids_free(&ak);
    }
    kids_free(&k);
}

/* The integer type of the given size and signedness. */
static TypeId int_of_size(Checker *c, unsigned bytes, bool uns)
{
    static const int sk[] = {TY_SCHAR, TY_SHORT, TY_INT, TY_LONG, TY_LLONG,
                             TY_INT128};
    static const int uk[] = {TY_UCHAR, TY_USHORT, TY_UINT, TY_ULONG,
                             TY_ULLONG, TY_UINT128};
    size_t k;
    for (k = 0; k < sizeof sk / sizeof *sk; k++)
        if (c->tgt->size[sk[k]] == bytes)
            return TYPE_MK(uns ? uk[k] : sk[k], 0);
    return ERRT;
}

/* The mode and vector_size attributes applied to a base type. */
static TypeId attr_apply_type(Checker *c, TypeId t, const Attrs *a)
{
    if (a->has_mode && !(type_ckind(TT, t) == TY_ERROR)) {
        unsigned q = TYPE_QUALS(t);
        TypeId n = ERRT;
        if (a->mode_bytes && type_is_integer(TT, t)) {
            n = int_of_size(c, a->mode_bytes, !type_is_signed(TT, t));
            if (a->mode_bytes == 1 && type_kind(TT, TYPE_UNQUAL(type_canon(TT, t))) == TY_BOOL)
                n = TYPE_B(BOOL);
        } else if (a->mode_bytes && type_is_float(TT, t))
            n = ERRT;
        else if (a->mode_float && (type_is_float(TT, t) || type_is_integer(TT, t))) {
            static const int fk[] = {0, TY_FLOAT, TY_DOUBLE, TY_LDOUBLE,
                                     TY_FLOAT128};
            n = TYPE_MK(fk[a->mode_float], 0);
        }
        if (type_ckind(TT, n) != TY_ERROR)
            t = n | q;
    }
    if (a->vs_seen) {
        TypeId el = type_canon(TT, t);
        unsigned q = TYPE_QUALS(t);
        bool ok = true;
        uint64_t esz;
        if (type_ckind(TT, el) == TY_ERROR)
            return t;
        if (!(type_is_integer(TT, el) || type_is_float(TT, el)) ||
            type_kind(TT, el) == TY_BOOL) {
            cerror(c, a->vs_loc, "invalid vector type for attribute "
                   "'vector_size'");
        } else if (a->vector_size == 0) {
            cerror(c, a->vs_loc, "zero vector size");
        } else if ((esz = type_size(TT, el, &ok)) && a->vector_size % esz) {
            cerror(c, a->vs_loc, "vector size not an integral multiple of "
                   "component size");
        } else if (a->vector_size / esz > 2147483646u) {
            cerror(c, a->vs_loc, "number of vector components %llu exceeds "
                   "2147483646", (unsigned long long)(a->vector_size / esz));
        } else {
            t = type_vector(TT, TYPE_UNQUAL(el), a->vector_size) | q;
            if (a->vs_dup)
                cerror(c, a->vs_dup, "invalid vector type for attribute "
                       "'vector_size'");
        }
    }
    return t;
}

static void attrs_merge(Attrs *to, const Attrs *from)
{
    if (from->aligned > to->aligned)
        to->aligned = from->aligned;
    to->packed |= from->packed;
    if (from->sso)
        to->sso = from->sso;
    if (from->ms)
        to->ms = from->ms;
    to->transparent_union |= from->transparent_union;
    if (from->has_mode) {
        to->has_mode = true;
        to->mode_bytes = from->mode_bytes;
        to->mode_float = from->mode_float;
        memcpy(to->mode_name, from->mode_name, sizeof to->mode_name);
    }
    if (from->vs_seen) {
        to->vector_size = from->vector_size;
        to->vs_seen = true;
        to->vs_loc = from->vs_loc;
        to->vs_dup = from->vs_dup;
    }
    to->deprecated |= from->deprecated;
    to->unavailable |= from->unavailable;
    to->gnu_inline |= from->gnu_inline;
    if (from->zcur) {
        to->zcur = from->zcur;
        memcpy(to->zcur_arg, from->zcur_arg, sizeof to->zcur_arg);
    }
    if (from->dep_msg)
        to->dep_msg = from->dep_msg;
    if (from->wina)
        to->wina = true;
    if (from->sec_any) {
        to->sec_any = true;
        if (!to->sec)
            to->sec = from->sec;
        else if (from->sec && !to->sec2)
            to->sec2 = from->sec;
        if (!to->sec2)
            to->sec2 = from->sec2;
        if (!to->sec_bad)
            to->sec_bad = from->sec_bad;
    }
    to->unused |= from->unused;
    to->noinline |= from->noinline;
    to->alias |= from->alias;
    to->defn |= from->defn;
    /* from (the specifiers) was written first */
    if (from->ifunc && to->weak) {
        to->weak = false;
        to->e_iw = true;
    }
    if ((from->weak || from->weakref) && to->ifunc) {
        to->ifunc = false;
        to->e_wi = true;
    }
    to->e_wi |= from->e_wi;
    to->e_iw |= from->e_iw;
    to->ifunc |= from->ifunc;
    to->desig |= from->desig;
    to->weakref |= from->weakref;
    to->errattr |= from->errattr;
    to->warnattr |= from->warnattr;
    to->cleanup |= from->cleanup;
    if (from->cleanup_arg)
        to->cleanup_arg = from->cleanup_arg;
    to->used |= from->used;
    to->weak |= from->weak;
    to->noreturn |= from->noreturn;
    to->nonnull |= from->nonnull;
    if (from->fmt)
        to->fmt = from->fmt;
}

/* Unknown specifier attributes are reported once the declarator is known:
 * gcc's input_location is then that declarator's line. */
static void attrs_unknown_emit(Checker *c, const Attrs *sa, uint32_t tok)
{
    uint8_t k;
    for (k = 0; k < sa->nunk; k++)
        cwarn(c, iloc(c, tok), "attributes",
              "'%s' attribute directive ignored", sa->unk[k]);
}

/* Attributes that gcc's handlers drop on the wrong kind of declaration:
 * 'where' is 't' typedef, 'f' function, 'g' file-scope variable, 's' static
 * local, 'a' automatic local, 'p' parameter, 'm' field (of type fty, 0 for a
 * bit-field); local: declared in a block. */
static void attrs_misapplied(Checker *c, const Attrs *a, char where, bool local,
                             TypeId fty, uint32_t tok)
{
    bool var = where == 'g' || where == 's' || where == 'a';
    bool nofn = where != 'f';
    SrcLoc loc = iloc(c, tok);
    if (a->noinline && nofn)
        cwarn(c, loc, "attributes", "'noinline' attribute ignored");
    if (a->used && (where == 'a' || where == 'p' || where == 'm'))
        cwarn(c, loc, "attributes", "'used' attribute ignored");
    if (a->weak && (where == 't' || where == 'p' || where == 'm'))
        cwarn(c, loc, "attributes", "'weak' attribute ignored");
    if (a->packed && where != 'm')
        cwarn(c, loc, "attributes", "'packed' attribute ignored");
    if (a->packed && where == 'm' && fty && type_align(TT, fty) == 1)
        cwarn(c, loc, "attributes", "'packed' attribute ignored for field of "
              "type %s", type_q(TT, fty));
    if (a->alias && (where == 't' || where == 'p' || where == 'm' ||
                     (local && (where == 'f' || var))))
        cwarn(c, loc, "attributes", "'alias' attribute ignored");
    if (a->weakref && (where == 't' || where == 'p' || where == 'm'))
        cwarn(c, loc, "attributes", "'weakref' attribute ignored");
    if (a->errattr && nofn)
        cwarn(c, loc, "attributes", "'error' attribute ignored");
    if (a->warnattr && nofn)
        cwarn(c, loc, "attributes", "'warning' attribute ignored");
    if (a->cleanup && (where != 'a' && !(local && where == 'g')))
        cwarn(c, loc, "attributes", "'cleanup' attribute ignored");
}

/* handle_section_attribute for a declaration of `name` at nloc.  where is
 * attrs_misapplied's code; localvar: an automatic or block-scope extern
 * variable (no TREE_STATIC). */
static void attrs_section_check(Checker *c, const Attrs *a, char where,
                                bool localvar, uint32_t name, SrcLoc nloc)
{
    if (!a->sec_any)
        return;
    if (where == 't' || where == 'p' || where == 'm')
        cerror(c, nloc, "section attribute not allowed for '%s'",
               cident(c, name));
    else if (a->sec_bad)
        cerror(c, a->sec_bad, "section attribute argument not a string "
               "constant");
    else if (localvar)
        cerror(c, nloc, "section attribute cannot be specified for local "
               "variables");
    else if (a->sec2 && strcmp(c->dep_msgs.data[a->sec - 1],
                               c->dep_msgs.data[a->sec2 - 1]))
        cerror(c, nloc, "section of '%s' conflicts with previous "
               "declaration", cident(c, name));
}

/* common_handle_aligned_attribute for warn_if_not_aligned: only a type or a
 * non-bit-field member may carry it. */
/* zero_call_used_regs: functions only, with a known string argument; the
 * errors are at the declared name. */
static void attrs_zcur_check(Checker *c, const Attrs *a, bool fn, SrcLoc nloc)
{
    if (!a->zcur)
        return;
    if (!fn)
        cerror(c, nloc, "'zero_call_used_regs' attribute applies only to "
               "functions");
    else if (a->zcur == 2)
        cerror(c, nloc, "'zero_call_used_regs' argument not a string");
    else if (a->zcur == 3)
        cerror(c, nloc, "unrecognized 'zero_call_used_regs' attribute "
               "argument '%s'", a->zcur_arg);
}

static void attrs_wina_check(Checker *c, const Attrs *a, char where,
                             bool bitfield, uint32_t name, SrcLoc nloc)
{
    if (!a->wina || where == 't' || (where == 'm' && !bitfield))
        return;
    if (where == 'p')
        cerror(c, nloc, "alignment may not be specified for '%s'",
               cident(c, name));
    else
        cerror(c, nloc, "'warn_if_not_aligned' may not be specified for '%s'",
               cident(c, name));
}

/* An alloc_align/alloc_size argument as gcc prints it (%qE): an integer
 * constant by value, anything else as written. */
static void pos_arg_str(Checker *c, uint32_t arg, char *buf, size_t n)
{
    if (c->ck[arg] == K_ICE || c->ck[arg] == K_FOLD) {
        int64_t v = cexpr_sval(c, arg);
        if (type_is_signed(TT, c->ty[arg]))
            snprintf(buf, n, "%lld", (long long)v);
        else
            snprintf(buf, n, "%llu", (unsigned long long)v);
    } else
        snprintf(buf, n, "%s", cexpr_str(c, arg));
}

/* handle_access_attribute: the mode, then up to two positional arguments
 * checked against the function type of the declaration (when known). */
static void access_check(Checker *c, const uint32_t *arg, uint32_t n, SrcLoc il,
                         TypeId ft)
{
    static const char *const mo[] = {"read_only", "read_write", "write_only",
                                     "none"};
    char md[24], list[256], val[96];
    const char *mode;
    uint32_t x = arg[0], q, m;
    bool ok = false;
    if (ntag(c, x) == N_CALL) {         /* read_only () */
        uint32_t cal = cfirst(c, x);
        if (ntag(c, cal) != N_IDENT)
            cal = NO_NODE;
        if (cal == NO_NODE)
            return;
        mode = tstr(c, c->nodes[cal].tok);
        attr_norm(mode, md, sizeof md);
        for (q = 0; q < 4; q++)
            ok |= !strcmp(md, mo[q]);
        if (ok)
            cerror(c, il, "attribute 'access' unexpected '(' after mode '%s'; "
                   "expected a positional argument or ')'", mode);
        else
            cerror(c, il, "attribute 'access' invalid mode '%s'; expected one "
                   "of 'read_only', 'read_write', 'write_only', or 'none'",
                   mode);
        return;
    }
    if (ntag(c, x) != N_IDENT) {
        if (type_ckind(TT, c->ty[x]) != TY_ERROR)
            cerror(c, il, "attribute 'access' mode '%s' is not an identifier; "
                   "expected one of 'read_only', 'read_write', 'write_only', "
                   "or 'none'", cexpr_str(c, x));
        return;
    }
    mode = tstr(c, c->nodes[x].tok);
    attr_norm(mode, md, sizeof md);
    for (q = 0, m = 0; q < 4; q++)
        if (!strcmp(md, mo[q])) {
            ok = true;
            m = q;
        }
    if (!ok) {
        cerror(c, il, "attribute 'access' invalid mode '%s'; expected one of "
               "'read_only', 'read_write', 'write_only', or 'none'", mode);
        return;
    }
    if (n < 2) {
        cerror(c, il, "attribute 'access(%s)' missing an argument", md);
        return;
    }
    snprintf(list, sizeof list, "%s", md);
    for (q = 1; q < n && q < 3; q++) {
        size_t l = strlen(list);
        pos_arg_str(c, arg[q], val, sizeof val);
        snprintf(list + l, sizeof list - l, ", %s", val);
    }
    for (q = 1; q < n && q < 3; q++) {
        uint32_t e = arg[q];
        int64_t v;
        if (type_ckind(TT, c->ty[e]) == TY_ERROR)
            return;
        if (!(c->ck[e] == K_ICE || c->ck[e] == K_FOLD) ||
            !type_is_integer(TT, c->ty[e])) {
            cerror(c, il, "attribute 'access(%s)' invalid positional argument "
                   "%u", list, q);
            return;
        }
        v = cexpr_sval(c, e);
        pos_arg_str(c, e, val, sizeof val);
        if (v < 1) {
            cerror(c, il, "attribute 'access(%s)' positional argument %u "
                   "invalid value %s", list, q, val);
            return;
        }
        if (ft && type_ckind(TT, ft) == TY_FUNC) {
            const TypeEnt *fe = type_ent(TT, ft);
            TypeId pt;
            if ((uint64_t)v > fe->n) {
                if (fe->flags & TF_VARIADIC)
                    continue;
                cerror(c, il, "attribute 'access(%s)' positional argument %u "
                       "value %s exceeds number of function arguments %u",
                       list, q, val, (unsigned)fe->n);
                return;
            }
            pt = type_params(TT, ft)[v - 1];
            if (q == 1) {
                TypeId tgt;
                if (type_ckind(TT, pt) != TY_PTR) {
                    cerror(c, il, "attribute 'access(%s)' positional argument "
                           "1 references non-pointer argument type %s", list,
                           type_q(TT, pt));
                    return;
                }
                tgt = type_base(TT, pt);
                if (type_ckind(TT, tgt) == TY_FUNC) {
                    cerror(c, il, "attribute 'access(%s)' positional argument "
                           "1 references argument of function type %s", list,
                           type_q(TT, tgt));
                    return;
                }
                if (m >= 1 && m <= 2 && (TYPE_QUALS(tgt) & TQ_CONST)) {
                    cerror(c, il, "attribute 'access(%s)' positional argument "
                           "1 references 'const'-qualified argument type %s",
                           list, type_q(TT, pt));
                    return;
                }
            } else if (!type_is_integer(TT, pt)) {
                cerror(c, il, "attribute 'access(%s)' positional argument 2 "
                       "references non-integer argument type %s", list,
                       type_q(TT, pt));
                return;
            }
        }
    }
}

/* c-attribs.cc positional_argument for one argument of alloc_align or
 * alloc_size on a function of type fty; false when it warned. */
static bool positional_arg(Checker *c, const char *name, uint32_t arg, int argno,
                           TypeId fty, SrcLoc loc)
{
    char pre[24] = "", val[96];
    TypeId t = c->ty[arg], pt;
    int64_t v;
    uint32_t n;
    uint64_t pos;
    if (argno)
        snprintf(pre, sizeof pre, "%d ", argno);
    if (type_ckind(TT, t) == TY_ERROR)
        return false;
    if (!type_is_integer(TT, t)) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %shas type %s",
              name, pre, type_q(TT, t));
        return false;
    }
    pos_arg_str(c, arg, val, sizeof val);
    if (c->ck[arg] != K_ICE && c->ck[arg] != K_FOLD) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' is "
              "not an integer constant", name, pre, val);
        return false;
    }
    v = cexpr_sval(c, arg);
    if (!v) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' does "
              "not refer to a function parameter", name, pre, val);
        return false;
    }
    if (type_ent(TT, fty)->flags & TF_NOPROTO)
        return true;
    n = type_ent(TT, fty)->n;
    pos = (uint64_t)v;
    if (pos > n) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' "
              "exceeds the number of function parameters %u", name, pre, val, n);
        return false;
    }
    pt = type_params(TT, fty)[pos - 1];
    if (!type_is_integer(TT, pt) ||
        type_kind(TT, TYPE_UNQUAL(type_canon(TT, pt))) == TY_BOOL) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' "
              "refers to parameter type %s", name, pre, val, type_q(TT, pt));
        return false;
    }
    return true;
}

/* handle_assume_aligned_attribute, for a function of type fty. */
static void assume_aligned_check(Checker *c, const uint32_t *arg, uint32_t n,
                                 TypeId fty, SrcLoc loc)
{
    TypeId rt = type_base(TT, fty);
    uint32_t k;
    int64_t first = 0;
    if (type_ckind(TT, rt) != TY_PTR) {
        cwarn(c, loc, "attributes", "'assume_aligned' attribute ignored on a "
              "function returning %s", type_q(TT, rt));
        return;
    }
    for (k = 0; k < n && k < 2; k++) {
        char val[96];
        int64_t v;
        if (type_ckind(TT, c->ty[arg[k]]) == TY_ERROR)
            return;
        pos_arg_str(c, arg[k], val, sizeof val);
        if (!type_is_integer(TT, c->ty[arg[k]]) ||
            (c->ck[arg[k]] != K_ICE && c->ck[arg[k]] != K_FOLD)) {
            cwarn(c, loc, "attributes", "'assume_aligned' attribute argument "
                  "%s is not an integer constant", val);
            return;
        }
        v = cexpr_sval(c, arg[k]);
        if (v < 0) {
            cwarn(c, loc, "attributes", "'assume_aligned' attribute argument "
                  "%s is not positive", val);
            return;
        }
        if (k == 0) {
            if (v == 0 || (v & (v - 1))) {
                cwarn(c, loc, "attributes", "'assume_aligned' attribute "
                      "argument %s is not a power of 2", val);
                return;
            }
            first = v;
        } else if (v >= first) {
            cwarn(c, loc, "attributes", "'assume_aligned' attribute argument "
                  "%s is not in the range [0, %lld]", val,
                  (long long)first - 1);
            return;
        }
    }
}

/* handle_strict_flex_array_attribute: only an array field may carry it, with
 * an integer constant argument in 0..3. */
static void strict_flex_check(Checker *c, uint32_t holder, bool field,
                              TypeId ty, uint32_t name, SrcLoc loc,
                              uint32_t tok0)
{
    Kids k;
    uint32_t j;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[32];
            Kids ak;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (strcmp(an, "strict_flex_array"))
                continue;
            if (tok0 != NO_NODE && c->nodes[k.p[j]].tok > tok0 &&
                tokp(c, c->nodes[k.p[j]].tok)->kind == TK_PUNCT) {
                /* [[...]] after a type specifier appertains to the type */
                cwarn(c, tloc(c, tok0), "attributes", "'strict_flex_array' "
                      "attribute does not apply to types");
                continue;
            }
            if (!field) {
                cerror(c, loc, "'strict_flex_array' attribute may not be "
                       "specified for '%s'", cident(c, name));
                continue;
            }
            kids_get(c, it.p[q], &ak);
            if (ak.n == 1 && type_ckind(TT, c->ty[ak.p[0]]) != TY_ERROR) {
                uint32_t a0 = ak.p[0];
                bool ice = type_is_integer(TT, c->ty[a0]) &&
                           (c->ck[a0] == K_ICE || c->ck[a0] == K_FOLD);
                int64_t v = ice ? cexpr_sval(c, a0) : 0;
                char val[96];
                pos_arg_str(c, a0, val, sizeof val);
                if (!ice)
                    cerror(c, loc, "'strict_flex_array' attribute argument "
                           "not an integer");
                else if (v < 0 || v > 3)
                    cerror(c, loc, "'strict_flex_array' attribute argument "
                           "'%s' is not an integer constant between 0 and 3",
                           val);
                else if (type_ckind(TT, ty) != TY_ARRAY)
                    cerror(c, loc, "'strict_flex_array' attribute may not be "
                           "specified for a non-array field");
            }
            kids_free(&ak);
        }
        kids_free(&it);
    }
    kids_free(&k);
}

/* handle_alloc_align_attribute / handle_alloc_size_attribute for the
 * attributes among holder's children, applied to a function of type fty. */
static void attrs_alloc_check(Checker *c, uint32_t holder, TypeId fty,
                              uint32_t tok)
{
    Kids k;
    uint32_t j;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char name[48];
            Kids ak;
            size_t i;
            bool align, ok = true;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), name, sizeof name);
            align = !strcmp(name, "alloc_align");
            if (!strcmp(name, "access")) {
                kids_get(c, it.p[q], &ak);
                if (ak.n)
                    access_check(c, ak.p, ak.n, iloc(c, tok), fty);
                kids_free(&ak);
                continue;
            }
            if (!strcmp(name, "warn_unused_result") &&
                type_ckind(TT, type_base(TT, fty)) == TY_VOID &&
                type_ckind(TT, fty) == TY_FUNC) {
                /* handle_warn_unused_result_attribute */
                cwarn(c, iloc(c, tok), "attributes",
                      "'warn_unused_result' attribute ignored");
                if (c->nign < 8)
                    snprintf(c->ign[c->nign++], sizeof c->ign[0], "%.23s", name);
                continue;
            }
            if (!strcmp(name, "assume_aligned")) {
                kids_get(c, it.p[q], &ak);
                if (ak.n && ak.n <= 2)
                    assume_aligned_check(c, ak.p, ak.n, fty, iloc(c, tok));
                kids_free(&ak);
                continue;
            }
            if (!align && strcmp(name, "alloc_size"))
                continue;
            kids_get(c, it.p[q], &ak);
            if (ak.n && (align ? ak.n == 1 : ak.n <= 2)) {
                SrcLoc loc = iloc(c, tok);
                TypeId rt = type_base(TT, fty);
                if (type_ckind(TT, rt) != TY_PTR)
                    cwarn(c, loc, "attributes", "'%s' attribute ignored on a "
                          "function returning %s", name, type_q(TT, rt));
                else
                    for (i = 0; i < ak.n && ok; i++)
                        ok = positional_arg(c, name, ak.p[i],
                                            ak.n > 1 ? (int)i + 1 : 0, fty, loc);
            }
            kids_free(&ak);
        }
        kids_free(&it);
    }
    kids_free(&k);
}

/* ---- attribute names ---------------------------------------------------
 * Every attribute written on (or copied to) a symbol, field or record is kept
 * by name, for __builtin_has_attribute and for 'copy'. */

/* Whether the attribute `name` written with arguments have equals the query
 * want.  nonnull without arguments covers every parameter; nonnull(N,...)
 * and alloc_size(N,M) are lists any element of which is found. */
static bool attr_arg_eq(const char *name, const char *have, const char *want)
{
    if ((!strcmp(name, "nonnull") || !strcmp(name, "alloc_size")) && *want) {
        size_t n = strlen(want);
        const char *p;
        if (!*have)
            return true;
        for (p = have; (p = strstr(p, want)) != NULL; p++)
            if ((p == have || p[-1] == ',') && (!p[n] || p[n] == ','))
                return true;
        return false;
    }
    return !strcmp(have, want);
}

/* Whether the set has the attribute name; with arg (not NULL) one whose
 * arguments are those. */
bool cdecl_aset_has(const Checker *c, uint32_t set, const char *name,
                    const char *arg)
{
    size_t k;
    if (!set)
        return false;
    for (k = c->ahead.data[set - 1]; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, name) &&
            (!arg || attr_arg_eq(name, c->anames.data[k - 1].arg, arg)))
            return true;
    return false;
}

/* The attribute set written on the typedef declaration t names (0: none). */
uint32_t cdecl_typedef_aset(const Checker *c, TypeId t)
{
    size_t k;
    for (k = 0; k + 1 < c->tdas.len; k += 2)
        if (c->tdas.data[k] == TYPE_IDX(t))
            return c->tdas.data[k + 1];
    return 0;
}

/* The arguments of an attribute item as one comparable string: integer
 * values, string contents, identifiers, comma separated. */
void cdecl_attr_args(Checker *c, uint32_t item, char *out, size_t n)
{
    Kids ak;
    uint32_t j;
    size_t l = 0;
    out[0] = 0;
    kids_get(c, item, &ak);
    for (j = 0; j < ak.n && l + 1 < n; j++) {
        uint32_t a = ak.p[j];
        if (ntag(c, a) == N_STRING) {
            uint32_t m = cdep_msg(c, a);   /* pushes: read it back, then drop */
            l += snprintf(out + l, n - l, "%s\"%s\"", j ? "," : "",
                          c->dep_msgs.data[m - 1]);
            free(c->dep_msgs.data[--c->dep_msgs.len]);
        }
        else if (c->ck[a] == K_ICE ||
                 (c->ck[a] == K_FOLD && (c->ef[a] & EF_CST)))
            l += snprintf(out + l, n - l, "%s%lld", j ? "," : "",
                          (long long)c->cv[a]);
        else if (ntag(c, a) == N_IDENT)
            l += snprintf(out + l, n - l, "%s%s", j ? "," : "",
                          cident(c, cnode_ident(c, a)));
        if (l >= n)
            l = n - 1;
    }
    kids_free(&ak);
}

void cdecl_attr_name(const char *s, char *out, size_t n)
{
    attr_norm(s, out, n);
}

static void aset_add(Checker *c, uint32_t *set, const char *name,
                     const char *arg)
{
    AName n;
    for (unsigned k = 0; k < c->nign; k++)
        if (!strcmp(c->ign[k], name))
            return;             /* dropped by an exclusion */
    if (cdecl_aset_has(c, *set, name, arg))
        return;
    if (!*set) {
        uint32_t z = 0;
        *set = ++c->nasets;
        vec_push(&c->ahead, z);
    }
    n.set = *set;
    n.prev = c->ahead.data[*set - 1];
    snprintf(n.name, sizeof n.name, "%s", name);
    snprintf(n.arg, sizeof n.arg, "%s", arg);
    vec_push(&c->anames, n);
    c->ahead.data[*set - 1] = (uint32_t)c->anames.len;
}

/* handle_copy_attribute: what is not copied. */
static bool copy_excluded(const char *n)
{
    static const char *const x[] = {
        "alias", "ifunc", "always_inline", "gnu_inline", "noinline",
        "visibility", "weakref", "target_clones", "deprecated", "unavailable",
        "weak", "malloc", "warn_unused_result", "artificial", "fallthrough",
        "copy"};
    size_t k;
    for (k = 0; k < sizeof x / sizeof *x; k++)
        if (!strcmp(n, x[k]))
            return true;
    return false;
}

static void aset_drop(Checker *c, uint32_t set, const char *name)
{
    size_t k;
    for (k = set ? c->ahead.data[set - 1] : 0; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, name))
            c->anames.data[k - 1].name[0] = '';
}

/* A typedef keeps only its largest aligned attribute. */
static void aset_keep_max_aligned(Checker *c, uint32_t set)
{
    size_t k;
    long long best = 0;
    for (k = c->ahead.data[set - 1]; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, "aligned") &&
            atoll(c->anames.data[k - 1].arg) > best)
            best = atoll(c->anames.data[k - 1].arg);
    for (k = c->ahead.data[set - 1]; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, "aligned") &&
            c->anames.data[k - 1].arg[0] &&
            atoll(c->anames.data[k - 1].arg) < best)
            c->anames.data[k - 1].name[0] = 0;
}

static void aset_copy(Checker *c, uint32_t *dst, uint32_t src)
{
    size_t k;
    if (!src || src == *dst)
        return;
    for (k = 0; k < c->anames.len; k++) {
        AName a = c->anames.data[k];
        if (a.set == src && !copy_excluded(a.name))
            aset_add(c, dst, a.name, a.arg);
    }
}

/* The attribute names of one ATTRIBUTE node; copy(X) brings X's. */
void cdecl_attrs_names(Checker *c, uint32_t attr, uint32_t *set)
{
    Kids it;
    uint32_t q;
    kids_get(c, attr, &it);
    for (q = 0; q < it.n; q++) {
        char an[32];
        if (ntag(c, it.p[q]) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
        if (!strcmp(an, "copy")) {
            Kids ak;
            kids_get(c, it.p[q], &ak);
            if (ak.n == 1) {
                uint32_t s3[3], n = cexpr_asets(c, ak.p[0], true, s3), m;
                for (m = 0; m < n; m++)
                    aset_copy(c, set, s3[m]);
            }
            kids_free(&ak);
        } else {
            char args[24];
            cdecl_attr_args(c, it.p[q], args, sizeof args);
            aset_add(c, set, an, args);
        }
    }
    kids_free(&it);
}

/* Whether holder writes the attribute `want` itself (not through copy). */
static bool attrs_item_named(Checker *c, uint32_t holder, const char *want)
{
    Kids k;
    uint32_t j;
    bool found = false;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n && !found; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n && !found; q++) {
            char an[32];
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            found = !strcmp(an, want);
        }
        kids_free(&it);
    }
    kids_free(&k);
    return found;
}

static void attrs_names(Checker *c, uint32_t holder, uint32_t *set)
{
    Kids k;
    uint32_t j;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ATTRIBUTE)
            cdecl_attrs_names(c, k.p[j], set);
    kids_free(&k);
}

/* Attributes after a '*' of the declarator belong to the declaration too. */
static void attrs_names_ptrs(Checker *c, uint32_t h, uint32_t *set)
{
    for (;;) {
        Kids hk;
        uint32_t m, nx = NO_NODE;
        kids_get(c, h, &hk);
        for (m = 0; m < hk.n && nx == NO_NODE; m++)
            if (ntag(c, hk.p[m]) == N_PTR)
                nx = hk.p[m];
        kids_free(&hk);
        if (nx == NO_NODE)
            return;
        attrs_names(c, nx, set);
        h = nx;
    }
}

/* The function attributes a declaration ends up with, applied in source order
 * (gcc's decl_attributes): 'pure' and 'const' exclude each other, and 'copy'
 * brings the referenced symbol's along.  *_loc: where the attribute came from
 * (for the note), have_*: whether it came from another declaration. */
typedef struct AttrState {
    bool pure, cnst;
    bool pure_from, cnst_from;
    SrcLoc pure_loc, cnst_loc;
    uint64_t nonnull;
    bool inited;             /* the declared name looked up (a summary read) */
    uint32_t pset;           /* the previous declaration's attribute names */
    bool hasprev;
    SrcLoc prevloc;
    char cur[24][24];        /* the attributes this declaration kept so far */
    unsigned ncur;
    bool ign_packed, ign_aligned;
    uint32_t calign, palign;
} AttrState;

/* attribs.cc diag_attr_exclusions over c-attribs.cc's attr_*_exclusions:
 * each attribute with the ones it may not be combined with.  fn_only: the
 * exclusion does not apply to variables and types. */
static const struct AttrExcl {
    const char *n, *ex[8];
    bool fn_only;
} attr_excl_tab[] = {
    {"aligned", {"packed"}, true}, {"packed", {"aligned"}, true},
    {"cold", {"cold", "hot"}, false}, {"hot", {"cold", "hot"}, false},
    {"common", {"common", "nocommon"}, false},
    {"nocommon", {"common", "nocommon"}, false},
    {"always_inline", {"noinline"}, false}, {"gnu_inline", {"noinline"}, false},
    {"noinline", {"always_inline", "gnu_inline"}, false},
    {"noreturn", {"alloc_align", "alloc_size", "const", "malloc", "pure",
                  "returns_twice", "warn_unused_result"}, false},
    {"warn_unused_result", {"noreturn", "warn_unused_result"}, false},
    {"returns_twice", {"noreturn"}, false},
    {"alloc_align", {"const", "noreturn", "pure"}, false},
    {"alloc_size", {"const", "noreturn", "pure"}, false},
    {"malloc", {"const", "noreturn", "pure"}, false},
    {"const", {"const", "alloc_align", "alloc_size", "malloc", "noreturn",
               "pure"}, false},
    {"pure", {"const", "alloc_align", "alloc_size", "malloc", "noreturn",
              "pure"}, false},
    {"stack_protect", {"stack_protect", "no_stack_protector"}, true},
    {"no_stack_protector", {"stack_protect", "no_stack_protector"}, true},
};

/* True when attribute an conflicts with one already on this declaration or
 * on the previous one (it is then ignored, with a warning). */
static bool attr_excl_generic(Checker *c, uint32_t tok, const char *an,
                              uint32_t kind, AttrState *st)
{
    size_t i, e;
    unsigned k;
    for (i = 0; i < sizeof attr_excl_tab / sizeof *attr_excl_tab; i++) {
        const struct AttrExcl *x = &attr_excl_tab[i];
        if (strcmp(x->n, an) || (x->fn_only && kind != CS_FUNC))
            continue;
        for (e = 0; e < 8 && x->ex[e]; e++) {
            const char *o = x->ex[e];
            bool prev = false, hit = false;
            if (!strcmp(o, an))
                continue;
            if ((!strcmp(an, "pure") || !strcmp(an, "const")) &&
                (!strcmp(o, "pure") || !strcmp(o, "const")))
                continue;               /* attr_excl */
            for (k = 0; k < st->ncur && !hit; k++)
                hit = !strcmp(st->cur[k], o);
            if (!hit && st->hasprev && cdecl_aset_has(c, st->pset, o, NULL))
                hit = prev = true;
            if (!hit)
                continue;
            {
                Diagnostic *d = cwarn_d(c, DL_WARNING, iloc(c, tok),
                                        "attributes", "ignoring attribute '%s' "
                                        "because it conflicts with attribute "
                                        "'%s'", an, o);
                if (d && prev)
                    cnote(c, d, st->prevloc, "previous declaration here");
                /* gcc 13 says it twice when noreturn meets an earlier
                 * alloc_align or alloc_size (Wattributes-6.c) */
                if (prev && !strcmp(an, "noreturn") &&
                    (!strcmp(o, "alloc_align") || !strcmp(o, "alloc_size"))) {
                    d = cwarn_d(c, DL_WARNING, iloc(c, tok), "attributes",
                                "ignoring attribute '%s' because it conflicts "
                                "with attribute '%s'", an, o);
                    if (d)
                        cnote(c, d, st->prevloc, "previous declaration here");
                }
            }
            if (c->nign < 8)
                snprintf(c->ign[c->nign++], sizeof c->ign[0], "%.23s", an);
            if (!strcmp(an, "packed"))
                st->ign_packed = true;
            if (!strcmp(an, "aligned"))
                st->ign_aligned = true;
            return true;
        }
        break;
    }
    if (st->ncur < 24)
        snprintf(st->cur[st->ncur++], sizeof st->cur[0], "%.23s", an);
    return false;
}

static void attr_excl(Checker *c, uint32_t tok, bool fn_pure, AttrState *st,
                      bool from, SrcLoc loc)
{
    /* fn_pure: the incoming attribute is 'pure', else 'const' */
    bool conflict = fn_pure ? st->cnst : st->pure;
    if (conflict) {
        Diagnostic *d = cwarn_d(c, DL_WARNING, iloc(c, tok), "attributes",
                                "ignoring attribute '%s' because it conflicts "
                                "with attribute '%s'", fn_pure ? "pure" : "const",
                                fn_pure ? "const" : "pure");
        if (d && (fn_pure ? st->cnst_from : st->pure_from))
            cnote(c, d, fn_pure ? st->cnst_loc : st->pure_loc,
                  "previous declaration here");
    } else if (fn_pure) {
        st->pure = true;
        st->pure_from = from;
        st->pure_loc = loc;
    } else {
        st->cnst = true;
        st->cnst_from = from;
        st->cnst_loc = loc;
    }
}

static void attrs_state_init(Checker *c, AttrState *st, uint32_t kind,
                             uint32_t name)
{
    uint32_t ref = name ? lookup_ord(c, name) : SYM_NONE;
    st->inited = true;
    if (ref != SYM_NONE && (kind == CS_FUNC || kind == CS_OBJ)) {
        const CSym *r = csym(c, ref);
        if (r->kind == kind && r->name == name) {
            st->pset = r->aset;
            st->hasprev = true;
            st->prevloc = r->loc;
            st->palign = r->ualign;
            st->pure = (r->flags & CSF_PURE) != 0;
            st->cnst = (r->flags & CSF_CONSTFN) != 0;
            st->pure_from = st->cnst_from = true;
            st->pure_loc = st->cnst_loc = r->loc;
        }
    }
}

/* c-attribs.cc handle_copy_attribute: the symbol referenced must be a
 * different declaration of the same kind as the one being declared. */
static void attrs_copy_check(Checker *c, uint32_t holder, uint32_t kind,
                             uint32_t name, uint32_t tok, AttrState *st)
{
    Kids k;
    uint32_t j;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[48];
            Kids ak;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (!st->inited) {
                attrs_state_init(c, st, kind, name);
                c->nign = 0;
            }
            if (attr_excl_generic(c, tok, an, kind, st))
                continue;
            if (kind == CS_FUNC && !strcmp(an, "aligned")) {
                Kids aa;
                uint32_t na = 0;
                kids_get(c, it.p[q], &aa);
                if (!aa.n)
                    na = c->tgt->default_aligned;
                else if ((c->ck[aa.p[0]] == K_ICE || c->ck[aa.p[0]] == K_FOLD) &&
                         type_is_integer(TT, c->ty[aa.p[0]]) &&
                         cexpr_sval(c, aa.p[0]) > 0)
                    na = (uint32_t)cexpr_sval(c, aa.p[0]);
                kids_free(&aa);
                if (na && (st->calign > na || st->palign > na)) {
                    bool note = st->palign > st->calign;
                    uint32_t shown = note ? st->palign : st->calign;
                    Diagnostic *d = cwarn_d(c, DL_WARNING, iloc(c, tok),
                                            "attributes", "ignoring attribute "
                                            "'aligned (%u)' because it "
                                            "conflicts with attribute 'aligned "
                                            "(%u)'", na, shown);
                    if (d && note)
                        cnote(c, d, st->prevloc, "previous declaration here");
                    continue;
                }
                if (na && na > st->calign)
                    st->calign = na;
            }
            if (kind == CS_FUNC && (!strcmp(an, "pure") || !strcmp(an, "const"))) {
                attr_excl(c, tok, an[0] == 'p', st, false, 0);
                continue;
            }
            if (strcmp(an, "copy"))
                continue;
            kids_get(c, it.p[q], &ak);
            if (ak.n == 1) {
                uint32_t e = ak.p[0];
                if (!st->inited)
                    attrs_state_init(c, st, kind, name);
                CSym *r = NULL;
                for (;;) {
                    while (ntag(c, e) == N_PAREN && c->nodes[e].size > 1)
                        e--;
                    if (ntag(c, e) == N_UNARY && (tpunct(c, c->nodes[e].tok) == P_AMP ||
                                                  tpunct(c, c->nodes[e].tok) == P_STAR))
                        e = first_child(c, e);
                    else
                        break;
                }
                if (ntag(c, e) == N_IDENT) {
                    uint32_t ref = lookup_ord(c, cnode_ident(c, e));
                    if (ref != SYM_NONE)
                        r = csym(c, ref);
                }
                if (r && r->name == name) {
                    Diagnostic *d = cwarn_d(c, DL_WARNING, iloc(c, tok),
                                            "attributes", "'copy' attribute "
                                            "ignored on a redeclaration of the "
                                            "referenced symbol");
                    if (d)
                        cnote(c, d, r->loc, "previous declaration here");
                } else if (!r && ntag(c, e) == N_CAST) {
                    /* copy of a type's attributes: nothing to diagnose */
                } else if (!r || r->kind != kind) {
                    Diagnostic *d = cwarn_d(c, DL_WARNING, iloc(c, tok),
                                            "attributes", "'copy' attribute "
                                            "ignored on a declaration of a "
                                            "different kind than referenced "
                                            "symbol");
                    if (d && r)
                        cnote(c, d, r->loc, "symbol '%s' referenced by '%s' "
                              "declared here", cident(c, r->name),
                              cident(c, name));
                } else if (kind == CS_FUNC) {
                    if (r->flags & CSF_PURE)
                        attr_excl(c, tok, true, st, true, r->loc);
                    if (r->flags & CSF_CONSTFN)
                        attr_excl(c, tok, false, st, true, r->loc);
                    st->nonnull |= r->nonnull;
                }
            }
            kids_free(&ak);
        }
        kids_free(&it);
    }
    kids_free(&k);
}

/* The attributes of node i's ATTRIBUTE children (direct). */
static void attrs_of_children(Checker *c, uint32_t i, Attrs *a)
{
    Kids k;
    uint32_t j;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ATTRIBUTE)
            attr_collect(c, k.p[j], a);
    kids_free(&k);
}

/* A storage class or function specifier (declspecs_add_scspec). */
static void add_scspec(Checker *c, Spec *s, uint32_t tok)
{
    int kw = tckw(c, tok);
    const char *sp = tstr(c, tok);
    SrcLoc loc = tloc(c, tok);
    SrcLoc il = iloc(c, tok);
    unsigned n = SC_NONE;
    bool dupe = false;

    if (s->non_sc_seen)
        cwarn(c, il, "old-style-declaration",
              "'%s' is not at beginning of declaration", sp);
    switch (kw) {
    case CK_INLINE:
        s->is_inline = true;
        s->inline_loc = loc;
        break;
    case CK_NORETURN:
        s->is_noreturn = true;
        s->noreturn_loc = loc;
        break;
    case CK_THREAD_LOCAL:
        dupe = s->thread;
        if (s->sc == SC_AUTO)
            cerror(c, il, "'%s' used with 'auto'", sp);
        else if (s->sc == SC_REGISTER)
            cerror(c, il, "'%s' used with 'register'", sp);
        else if (s->sc == SC_TYPEDEF)
            cerror(c, il, "'%s' used with 'typedef'", sp);
        else {
            s->thread = true;
            s->thread_gnu = !strcmp(sp, "__thread");
            if (!s->thread_gnu)
                cped11(c, loc, "ISO C99 does not support '%s'", sp);
            s->thread_loc = loc;
        }
        break;
    case CK_AUTO:
        n = SC_AUTO;
        break;
    case CK_EXTERN:
        n = SC_EXTERN;
        if (s->thread && s->thread_gnu)
            cerror(c, il, "'__thread' before 'extern'");
        break;
    case CK_REGISTER:
        n = SC_REGISTER;
        break;
    case CK_STATIC:
        n = SC_STATIC;
        if (s->thread && s->thread_gnu)
            cerror(c, il, "'__thread' before 'static'");
        break;
    case CK_TYPEDEF:
        n = SC_TYPEDEF;
        break;
    default:
        return;
    }
    if (n != SC_NONE && n == s->sc)
        dupe = true;
    if (dupe) {
        if (kw == CK_THREAD_LOCAL)
            cerror(c, il, "duplicate '_Thread_local' or '__thread'");
        else
            cerror(c, il, "duplicate '%s'", sp);
    }
    if (n != SC_NONE) {
        if (s->sc != SC_NONE && n != s->sc)
            cerror(c, il, "multiple storage classes in declaration "
                   "specifiers");
        else {
            s->sc = (uint8_t)n;
            s->sc_loc = loc;
            if (n != SC_EXTERN && n != SC_STATIC && s->thread) {
                cerror(c, il, "'%s' used with '%s'",
                       s->thread_gnu ? "__thread" : "_Thread_local", sp);
                s->thread = false;
            }
        }
    }
}

static void add_qual(Checker *c, Spec *s, uint32_t tok)
{
    int kw = tckw(c, tok);
    const char *sp = tstr(c, tok);
    SrcLoc loc = tloc(c, tok);
    unsigned q = 0;
    unsigned idx = 0;
    bool dupe;
    uint32_t prev;
    s->non_sc_seen = true;
    switch (kw) {
    case CK_CONST: q = TQ_CONST; idx = 0; break;
    case CK_VOLATILE: q = TQ_VOLATILE; idx = 1; break;
    case CK_RESTRICT: q = TQ_RESTRICT; idx = 2; break;
    case CK_ATOMIC: q = TQ_ATOMIC; idx = 3; break;
    default: return;
    }
    if (q == TQ_ATOMIC)
        cped11(c, loc, "ISO C99 does not support the '_Atomic' qualifier");
    dupe = (s->quals & q) != 0;
    prev = s->qual_tok[idx];
    s->quals |= q;
    s->qual_tok[idx] = tok + 1;
    if (!s->qual_loc)
        s->qual_loc = loc;
    if (q == TQ_RESTRICT)
        s->restrict_q = true;
    if (dupe && prev && !tfrom_macro(c, prev - 1) && !tfrom_macro(c, tok))
        cwarn(c, loc, "duplicate-decl-specifier",
              "duplicate '%s' declaration specifier", sp);
}

/* The name of the word currently in s, for "both 'X' and 'Y'". */
static const char *word_name(const Spec *s)
{
    switch (s->word) {
    case TW_VOID: return "void";
    case TW_BOOL: return "_Bool";
    case TW_CHAR: return "char";
    case TW_INT: return "int";
    case TW_FLOAT: return "float";
    case TW_DOUBLE: return "double";
    case TW_INT128: return "__int128";
    case TW_FLOATN:
    case TW_DECIMAL: return s->wname;
    case TW_AUTO_TYPE: return "__auto_type";
    default: return "";
    }
}

static unsigned wbit(unsigned w)
{
    return 1u << w;
}

/* A modifier (long, short, signed, unsigned, complex) against the word. */
static bool mod_conflict(Checker *c, const Spec *s, const char *mod,
                         unsigned words, SrcLoc loc)
{
    if (s->word != TW_NONE && (words & wbit(s->word))) {
        cerror(c, loc, "both '%s' and '%s' in declaration specifiers", mod,
               word_name(s));
        return true;
    }
    return false;
}

#define W_ALLBUT_INTDBL (wbit(TW_AUTO_TYPE) | wbit(TW_VOID) | wbit(TW_BOOL) | \
                         wbit(TW_CHAR) | wbit(TW_FLOAT) | wbit(TW_INT128) |    \
                         wbit(TW_FLOATN) | wbit(TW_DECIMAL))

/* A type specifier keyword (declspecs_add_type, keyword part). */
static void add_type_kw(Checker *c, Spec *s, uint32_t tok)
{
    int kw = tckw(c, tok);
    const char *sp = tstr(c, tok);
    SrcLoc loc = tloc(c, tok);
    bool dupe = false;

    s->non_sc_seen = true;
    if (!s->type_loc)
        s->type_loc = loc;
    if (s->has_type) {
        cerror(c, loc, "two or more data types in declaration specifiers");
        return;
    }
    switch (kw) {
    case CK_LONG:
        if (s->long_long) {
            cerror(c, loc, "'long long long' is too long for GCC");
            return;
        }
        if (s->is_long) {
            if (s->word == TW_DOUBLE) {
                cerror(c, loc, "both 'long long' and 'double' in declaration "
                       "specifiers");
                return;
            }
            s->long_long = true;
            return;
        }
        if (s->is_short)
            cerror(c, loc, "both 'long' and 'short' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "long", W_ALLBUT_INTDBL, loc))
            s->is_long = true;
        return;
    case CK_SHORT:
        dupe = s->is_short;
        if (s->is_long)
            cerror(c, loc, "both 'long' and 'short' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "short",
                               W_ALLBUT_INTDBL | wbit(TW_DOUBLE), loc))
            s->is_short = true;
        break;
    case CK_SIGNED:
        dupe = s->is_signed;
        if (s->is_unsigned)
            cerror(c, loc, "both 'signed' and 'unsigned' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "signed",
                               wbit(TW_AUTO_TYPE) | wbit(TW_VOID) |
                               wbit(TW_BOOL) | wbit(TW_FLOAT) |
                               wbit(TW_DOUBLE) | wbit(TW_FLOATN) |
                               wbit(TW_DECIMAL), loc))
            s->is_signed = true;
        break;
    case CK_UNSIGNED:
        dupe = s->is_unsigned;
        if (s->is_signed)
            cerror(c, loc, "both 'signed' and 'unsigned' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "unsigned",
                               wbit(TW_AUTO_TYPE) | wbit(TW_VOID) |
                               wbit(TW_BOOL) | wbit(TW_FLOAT) |
                               wbit(TW_DOUBLE) | wbit(TW_FLOATN) |
                               wbit(TW_DECIMAL), loc))
            s->is_unsigned = true;
        break;
    case CK_COMPLEX:
        dupe = s->is_complex;
        if (!mod_conflict(c, s, "complex",
                          wbit(TW_AUTO_TYPE) | wbit(TW_VOID) | wbit(TW_BOOL) |
                          wbit(TW_DECIMAL), loc)) {
            s->is_complex = true;
            s->complex_loc = loc;
        }
        break;
    case CK_IMAGINARY:
        /* an extension gcc rejects; reported as an unknown type name */
        return;
    default: {
        /* the words */
        unsigned nw = TW_NONE;
        if (s->word != TW_NONE) {
            cerror(c, loc, "two or more data types in declaration "
                   "specifiers");
            return;
        }
        switch (kw) {
        case CK_AUTO_TYPE:
            if (!mod_conflict(c, s, s->is_long ? "long" : s->is_short ? "short"
                              : s->is_signed ? "signed" : s->is_unsigned
                              ? "unsigned" : "complex", ~0u, loc) &&
                (s->is_long || s->is_short || s->is_signed || s->is_unsigned ||
                 s->is_complex)) {
                cerror(c, loc, "both '%s' and '__auto_type' in declaration "
                       "specifiers",
                       s->is_long ? "long" : s->is_short ? "short"
                       : s->is_signed ? "signed" : s->is_unsigned
                       ? "unsigned" : "complex");
                return;
            }
            if (s->is_long || s->is_short || s->is_signed ||
                s->is_unsigned || s->is_complex)
                return;
            nw = TW_AUTO_TYPE;
            break;
        case CK_VOID:
        case CK_BOOL: {
            const char *w = kw == CK_VOID ? "void" : "_Bool";
            const char *m = s->is_long ? "long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : s->is_complex ? "complex" : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and '%s' in declaration specifiers",
                       m, w);
                return;
            }
            nw = kw == CK_VOID ? TW_VOID : TW_BOOL;
            break;
        }
        case CK_CHAR: {
            const char *m = s->is_long ? "long" : s->is_short ? "short" : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and 'char' in declaration "
                       "specifiers", m);
                return;
            }
            nw = TW_CHAR;
            break;
        }
        case CK_INT:
            nw = TW_INT;
            break;
        case CK_FLOAT: {
            const char *m = s->is_long ? "long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and 'float' in declaration "
                       "specifiers", m);
                return;
            }
            nw = TW_FLOAT;
            break;
        }
        case CK_DOUBLE: {
            const char *m = s->long_long ? "long long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and 'double' in declaration "
                       "specifiers", m);
                return;
            }
            nw = TW_DOUBLE;
            break;
        }
        case CK_INT128: {
            size_t l = strlen(sp);
            if (!(l >= 2 && sp[l - 1] == '_' && sp[l - 2] == '_'))
                cpedantic(c, loc, "ISO C does not support '__int128' types");
            if (s->is_long)
                cerror(c, loc, "both '__int128' and 'long' in declaration "
                       "specifiers");
            else if (s->is_short)
                cerror(c, loc, "both '__int128' and 'short' in declaration "
                       "specifiers");
            else {
                s->word = TW_INT128;
                s->type_loc = loc;
            }
            return;
        }
        case CK_FLOATN: {
            const char *m;
            static const struct { const char *sp; int ty; const char *nm; }
                fl[] = {{"_Float16", TY_FLOAT16, "_Float16"},
                        {"__bf16", TY_BF16, "__bf16"},
                        {"_Float32", TY_FLOAT32, "_Float32"},
                        {"_Float64", TY_FLOAT64, "_Float64"},
                        {"_Float128", TY_FLOAT128, "_Float128"},
                        {"__float128", TY_FLOAT128, "_Float128"},
                        {"_Float32x", TY_FLOAT32X, "_Float32x"},
                        {"_Float64x", TY_FLOAT64X, "_Float64x"},
                        {"_Float128x", 0, "_Float128x"},
                        {"__float80", TY_LDOUBLE, "__float80"},
                        {"__ibm128", TY_IBM128, "__ibm128"}};
            size_t k;
            s->nty = 0;
            s->wname = sp;
            for (k = 0; k < sizeof fl / sizeof *fl; k++)
                if (!strcmp(fl[k].sp, sp)) {
                    s->nty = (uint32_t)fl[k].ty;
                    s->wname = fl[k].nm;
                }
            if (strcmp(sp, "__bf16") && strcmp(sp, "__float80") &&
                strcmp(sp, "__ibm128") && strcmp(sp, "__float128"))
                cpedantic(c, loc, "ISO C does not support the '%s' type",
                          s->wname);
            m = s->is_long ? "long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and '%s' in declaration specifiers",
                       m, s->wname);
                return;
            }
            if (!s->nty || !c->tgt->size[s->nty]) {
                cerror(c, loc, "'%s' is not supported on this target",
                       s->wname);
                s->nty = TY_INT;
            }
            nw = TW_FLOATN;
            break;
        }
        case CK_SAT:
            cpedantic(c, loc, "ISO C does not support saturating types");
            return;
        case CK_FIXED:
            /* no fixed-point support on this target; typed as int */
            cerror(c, loc, "fixed-point types not supported for this target");
            cpedantic(c, loc, "ISO C does not support fixed-point types");
            nw = TW_INT;
            break;
        case CK_DECIMAL: {
            const char *m = s->long_long ? "long long" : s->is_long ? "long"
                : s->is_short ? "short" : s->is_signed ? "signed"
                : s->is_unsigned ? "unsigned" : s->is_complex ? "complex"
                : NULL;
            s->wname = sp;
            s->nty = !strcmp(sp, "_Decimal32") ? TY_DEC32
                     : !strcmp(sp, "_Decimal64") ? TY_DEC64 : TY_DEC128;
            if (m)
                cerror(c, loc, "both '%s' and '%s' in declaration specifiers",
                       m, sp);
            else
                s->word = TW_DECIMAL;
            cpedantic(c, loc, "ISO C does not support decimal floating-point "
                      "before C2X");
            if (!m)
                s->type_loc = loc;
            return;
        }
        default:
            return;
        }
        s->word = (uint8_t)nw;
        s->type_loc = loc;
        return;
    }
    }
    if (dupe)
        cerror(c, loc, "duplicate '%s'", sp);
}

/* A whole type (typedef name, tag, typeof, _Atomic(type)). */
static void add_type_whole(Checker *c, Spec *s, TypeId t, unsigned kind,
                           uint32_t tok)
{
    SrcLoc loc = tloc(c, tok);
    s->non_sc_seen = true;
    s->kind = (uint8_t)kind;
    if (s->has_type || s->word != TW_NONE || s->is_long || s->is_short ||
        s->is_signed || s->is_unsigned || s->is_complex) {
        cerror(c, loc, "two or more data types in declaration specifiers");
        return;
    }
    s->ty = t;
    s->has_type = true;
    s->type_loc = loc;
    if (type_ckind(TT, t) == TY_ERROR)
        s->error = true;
}

/* finish_declspecs: the type the words and modifiers name. */
static void finish_declspecs(Checker *c, Spec *s)
{
    TypeId t = ERRT;
    if (s->has_type) {
        if (s->error)
            s->ty = TYPE_B(INT);
        return;
    }
    if (s->word == TW_NONE) {
        if (s->is_long || s->is_short || s->is_signed || s->is_unsigned)
            s->word = TW_INT;
        else if (s->is_complex) {
            s->word = TW_DOUBLE;
            cpedantic(c, s->complex_loc, "ISO C does not support plain "
                      "'complex' meaning 'double complex'");
        } else {
            s->word = TW_INT;
            s->default_int = true;
        }
    }
    switch (s->word) {
    case TW_AUTO_TYPE:
        t = TYPE_B(INT);
        break;
    case TW_VOID:
        t = TYPE_B(VOID);
        break;
    case TW_BOOL:
        t = TYPE_B(BOOL);
        break;
    case TW_CHAR:
        t = s->is_signed ? TYPE_B(SCHAR) : s->is_unsigned ? TYPE_B(UCHAR)
                                                         : TYPE_B(CHAR);
        goto cplx;
    case TW_INT128:
        t = s->is_unsigned ? TYPE_B(UINT128) : TYPE_B(INT128);
        if (!c->tgt->size[TY_INT128])
            t = TYPE_B(INT);
        goto cplx;
    case TW_INT:
        if (s->long_long)
            t = s->is_unsigned ? TYPE_B(ULLONG) : TYPE_B(LLONG);
        else if (s->is_long)
            t = s->is_unsigned ? TYPE_B(ULONG) : TYPE_B(LONG);
        else if (s->is_short)
            t = s->is_unsigned ? TYPE_B(USHORT) : TYPE_B(SHORT);
        else
            t = s->is_unsigned ? TYPE_B(UINT) : TYPE_B(INT);
    cplx:
        if (s->is_complex) {
            cpedantic(c, s->complex_loc, "ISO C does not support complex "
                      "integer types");
            t = type_complex(TT, t);
        }
        break;
    case TW_FLOAT:
        t = s->is_complex ? type_complex(TT, TYPE_B(FLOAT)) : TYPE_B(FLOAT);
        break;
    case TW_DOUBLE:
        if (s->is_long)
            t = s->is_complex ? type_complex(TT, TYPE_B(LDOUBLE))
                              : TYPE_B(LDOUBLE);
        else
            t = s->is_complex ? type_complex(TT, TYPE_B(DOUBLE))
                              : TYPE_B(DOUBLE);
        break;
    case TW_FLOATN:
        t = TYPE_MK(s->nty, 0);
        if (s->is_complex)
            t = type_complex(TT, t);
        break;
    case TW_DECIMAL:
        t = TYPE_MK(s->nty, 0);
        break;
    default:
        break;
    }
    s->ty = t;
}

/* The location gcc's shadow_tag uses for "here": the SPECS node's tokens. */

static void specs_visit(Checker *c, uint32_t i)
{
    Spec s;
    Kids k;
    uint32_t j;
    memset(&s, 0, sizeof s);
    s.node = i;
    s.tag_node = NO_NODE;
    s.kind = TSK_NONE;
    s.tok0 = c->nodes[i].tok;
    s.loc = tloc(c, s.tok0);
    {
        /* a declaration as the body of a label (C2X) */
        uint32_t d = c->par[i], l = d == NO_NODE ? NO_NODE : c->par[d];
        if (l != NO_NODE && ntag(c, d) == N_DECL && d == l - 1 &&
            cfirst(c, d) == cfirst(c, i) &&
            (ntag(c, l) == N_LABEL || ntag(c, l) == N_CASE ||
             ntag(c, l) == N_DEFAULT))
        {
            DiagOrd o0 = diag_ord(c->diag, ORD_EARLY);
            cpedantic(c, s.loc, "a label can only be part of a statement "
                      "and a declaration is not a statement");
            diag_ord(c->diag, o0);
        }
    }
    s.tok1 = s.tok0;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t n = k.p[j];
        const Node *nd = cnode(c, n);
        switch (nd->tag) {
        case N_STORAGE:
        case N_FUNCSPEC:
            add_scspec(c, &s, nd->tok);
            break;
        case N_QUAL:
            add_qual(c, &s, nd->tok);
            break;
        case N_TYPESPEC:
            add_type_kw(c, &s, nd->tok);
            break;
        case N_TYPEDEF_NAME: {
            uint32_t ref = lookup_ord(c, cnode_ident(c, n));
            TypeId t = ERRT;
            if (nd->flags & NF_ERROR) /* an unknown type name, diagnosed */
                ref = SYM_NONE;
            else if (ref != SYM_NONE && csym(c, ref)->kind == CS_TYPEDEF)
                t = csym(c, ref)->ty;
            else if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ)
                t = csym(c, ref)->ty;   /* the parser took it for a typedef: gcc types it as the variable */
            else if (ref == SYM_NONE || csym(c, ref)->kind != CS_TYPEDEF)
                cerror(c, iloc(c, nd->tok), "'%s' fails to be a typedef or "
                       "built in type", tstr(c, nd->tok));
            add_type_whole(c, &s, t, TSK_TYPEDEF, nd->tok);
            if (ref != SYM_NONE) {
                csym(c, ref)->flags |= CSF_USED;
                if ((csym(c, ref)->flags & (CSF_DEPRECATED | CSF_UNAVAILABLE)) &&
                    !decl_has_dep_attr(c, c->par[c->par[n]]))
                {
                    /* gcc names the tagged type, not a plain typedef */
                    const CSym *ts = csym(c, ref);
                    TypeId ct = type_canon(TT, ts->ty);
                    SrcLoc nl = 0;
                    bool have = false;
                    if (type_ckind(TT, ct) == TY_STRUCT || type_ckind(TT, ct) == TY_UNION) {
                        const Record *r = type_record(TT, ct);
                        if (r) {
                            nl = r->loc;
                            have = true;
                        }
                    } else if (type_ckind(TT, ct) == TY_ENUM) {
                        const Enum *en = type_enum(TT, ct);
                        if (en) {
                            nl = en->loc;
                            have = true;
                        }
                    }
                    cdep_use(c, cinput_loc(c, nd->tok), ts, have ? &nl : NULL);
                }
            }
            break;
        }
        case N_STRUCT:
        case N_ENUM:
            add_type_whole(c, &s, c->ty[n], (unsigned)(c->cv[n] & 0xff),
                           nd->tok);
            if (c->cb[n]) {
                s.xref_name = c->cb[n];
                s.xref_loc = (SrcLoc)(c->cv[n] >> 8);
            }
            if (!s.error || s.has_type)
                s.tag_node = n;
            if (sso_of_tag(c, n))
                s.attrs.sso = sso_of_tag(c, n);
            break;
        case N_TYPEOF: {
            uint32_t a = first_child(c, n);
            TypeId t = ERRT;
            if (a != NO_NODE) {
                if (ntag(c, a) == N_TYPE_NAME)
                    t = c->ty[a];
                else
                    t = c->ty[a];
            }
            add_type_whole(c, &s, t, TSK_TYPEOF, nd->tok);
            break;
        }
        case N_ATOMIC_TYPE: {
            uint32_t a = first_child(c, n);
            TypeId t = a != NO_NODE ? c->ty[a] : ERRT;
            SrcLoc loc = tloc(c, nd->tok);
            cped11(c, loc, "ISO C99 does not support the '_Atomic' "
                      "qualifier");
            if (type_ckind(TT, t) != TY_ERROR) {
                if (type_ckind(TT, t) == TY_ARRAY ||
                    type_ckind(TT, t) == TY_VLA) {
                    cerror(c, loc, "'_Atomic'-qualified array type");
                    t = ERRT;
                } else if (type_ckind(TT, t) == TY_FUNC) {
                    cerror(c, loc, "'_Atomic'-qualified function type");
                    t = ERRT;
                } else if (TYPE_QUALS(t) || TYPE_QUALS(type_canon(TT, t))) {
                    cerror(c, loc, "'_Atomic' applied to a qualified type");
                    t = ERRT;
                } else
                    t |= TQ_ATOMIC;
            }
            add_type_whole(c, &s, t, TSK_TYPEOF, nd->tok);
            break;
        }
        case N_ALIGNAS: {
            uint32_t a = first_child(c, n);
            SrcLoc loc = tloc(c, nd->tok);
            uint32_t v = 0;
            s.alignas_seen = true;
            s.alignas_loc = loc;
            cped11(c, loc, "ISO C99 does not support '%s'",
                      tstr(c, nd->tok));
            if (a != NO_NODE) {
                if (ntag(c, a) == N_TYPE_NAME) {
                    if (type_ckind(TT, c->ty[a]) != TY_ERROR)
                        v = type_align(TT, c->ty[a]);
                } else
                    v = check_user_alignment(c, a, iloc(c, after_tok(c, n)),
                                         false);
            }
            if (v > s.align)
                s.align = v;
            break;
        }
        case N_ATTRIBUTE:
            s.has_attrs = true;
            c->attr_defer = true;
            attr_collect(c, n, &s.attrs);
            c->attr_defer = false;
            break;
        default:
            break;
        }
    }
    kids_free(&k);
    s.tok1 = k.n ? after_tok(c, i) : s.tok0;
    finish_declspecs(c, &s);
    if (s.attrs.aligned > s.align)
        s.align = s.attrs.aligned;
    if (s.attrs.has_mode || s.attrs.vs_seen)
        s.ty = attr_apply_type(c, s.ty, &s.attrs);
    if (type_ckind(TT, s.ty) == TY_ERROR)
        s.error = true;
    c->ty[i] = s.ty;
    vec_push(&c->specs, s);
}

/* ---- type helpers --------------------------------------------------------- */

static TypeKind tkind(Checker *c, TypeId t)
{
    return type_ckind(TT, t);
}

static bool is_err(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ERROR;
}

/* Does function type t have a parameter of erroneous type? */
static bool func_err_param(Checker *c, TypeId t)
{
    const TypeEnt *e;
    uint32_t i;
    t = type_canon(TT, t);
    if (type_ckind(TT, t) != TY_FUNC)
        return false;
    e = type_ent(TT, t);
    for (i = 0; i < e->n; i++)
        if (is_err(c, type_params(TT, t)[i]))
            return true;
    return false;
}

static bool is_arr(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k == TY_ARRAY || k == TY_VLA;
}

static bool is_func(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_FUNC;
}

static bool is_void(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_VOID;
}

static bool is_prototype(Checker *c, TypeId t)
{
    return is_func(c, t) &&
           !(type_ent(TT, type_canon(TT, t))->flags & TF_NOPROTO);
}

static TypeId strip_arrays(Checker *c, TypeId t)
{
    while (is_arr(c, t))
        t = type_base(TT, t);
    return t;
}

/* The qualifiers a type has, whether written or through a typedef. */
static unsigned all_quals(Checker *c, TypeId t)
{
    t = strip_arrays(c, t);
    return TYPE_QUALS(t) | TYPE_QUALS(type_canon(TT, t));
}

static bool object_or_incomplete(Checker *c, TypeId t)
{
    return !is_func(c, t);
}

/* c_build_qualified_type: quals on t (an array's go to its elements);
 * restrict on a non-pointer is an error (at input_location, ltok). */
static TypeId qualify(Checker *c, TypeId t, unsigned q, uint32_t ltok)
{
    if (!q || is_err(c, t))
        return t;
    if (q & TQ_RESTRICT) {
        TypeId e = strip_arrays(c, t);
        if (tkind(c, e) != TY_PTR ||
            !object_or_incomplete(c, type_base(TT, e))) {
            cerror(c, iloc(c, ltok), "invalid use of 'restrict'");
            q &= ~(unsigned)TQ_RESTRICT;
        }
    }
    return t | q;
}

/* Is the constant expression node e an INTEGER_CST (K_ICE, or a folded
 * constant)?  If not, may it still be folded to one? */
static bool node_int_cst(Checker *c, uint32_t e)
{
    return c->ck[e] == K_ICE || (c->ck[e] == K_FOLD && (c->ef[e] & EF_CST));
}

/* ---- declarator analysis ------------------------------------------------- */

enum { GD_NONE, GD_VAR, GD_PARM, GD_FIELD, GD_FUNC, GD_TYPEDEF, GD_TYPENAME };

typedef struct GDecl {
    int what;
    uint32_t name;           /* ident, 0: none */
    uint32_t name_node;
    SrcLoc loc;              /* the declaration's own location */
    TypeId ty;
    CSym s;
    bool array_param;
    TypeId pre;              /* DC_PARM: the array type before it decayed */
    int width;               /* fields: bits, -1: not a bit-field */
    bool default_int;
    bool funcdef_ok;
} GDecl;

/* The location of an abstract declarator's (missing) name. */
static SrcLoc abstract_loc(Checker *c, uint32_t top, uint32_t after)
{
    uint32_t d = top, inner;
    if (d != NO_NODE) {
        while ((inner = inner_decl(c, d)) != NO_NODE)
            d = inner;
        if (ntag(c, d) == N_ARRAY)
            return tloc(c, cnode(c, d)->tok);
    }
    return iloc(c, after);
}

static unsigned quals_of(Checker *c, uint32_t node)
{
    Kids k;
    uint32_t j;
    unsigned q = 0;
    kids_get(c, node, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_QUAL)
            switch (tckw(c, cnode(c, k.p[j])->tok)) {
            case CK_CONST: q |= TQ_CONST; break;
            case CK_VOLATILE: q |= TQ_VOLATILE; break;
            case CK_RESTRICT: q |= TQ_RESTRICT; break;
            case CK_ATOMIC: q |= TQ_ATOMIC; break;
            default: break;
            }
    kids_free(&k);
    return q;
}

static unsigned quals_of_warn(Checker *c, uint32_t node)
{
    unsigned q = quals_of(c, node);
    if (q & TQ_ATOMIC) {
        Kids k;
        uint32_t j;
        kids_get(c, node, &k);
        for (j = 0; j < k.n; j++)
            if (ntag(c, k.p[j]) == N_QUAL &&
                tckw(c, cnode(c, k.p[j])->tok) == CK_ATOMIC)
                cped11(c, tloc(c, cnode(c, k.p[j])->tok),
                          "ISO C99 does not support the '_Atomic' qualifier");
        kids_free(&k);
    }
    return q;
}

static bool has_child_attr(Checker *c, uint32_t node)
{
    Kids k;
    uint32_t j;
    bool r = false;
    kids_get(c, node, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ATTRIBUTE &&
            c->nodes[k.p[j]].tok > c->nodes[node].tok)
            r = true;   /* inside '[' ']', not before it */
    kids_free(&k);
    return r;
}

/* The size expression child of an array declarator, or NO_NODE. */
static uint32_t array_size_node(Checker *c, uint32_t a)
{
    uint32_t l = last_child(c, a);
    if (l == NO_NODE)
        return NO_NODE;
    if (cexpr_is_expr(ntag(c, l)))
        return l;
    return NO_NODE;
}

/* The parameter type list of a FUNC declarator: what grokparms and
 * get_parm_info make of it.  Returns the function's parameter types in
 * ps (malloc'd, *n of them), *flags for TF_*; ret false if the list is
 * "erroneous" (no list at all). */
typedef struct PInfo {
    TypeId *ps;
    uint32_t n;
    unsigned flags;
    bool krlist;             /* an identifier list */
    bool empty;              /* () */
} PInfo;

static void func_params(Checker *c, uint32_t f, PInfo *pi)
{
    Kids k;
    uint32_t j, np = 0;
    memset(pi, 0, sizeof *pi);
    kids_get(c, f, &k);
    pi->ps = xmalloc((k.n + 1) * sizeof *pi->ps);
    if (cnode(c, f)->flags & NF_KR)
        pi->krlist = true;
    for (j = 0; j < k.n; j++) {
        uint32_t p = k.p[j];
        if (is_real_param(c, p)) {
            bool named = c->cv[p] & 1;
            TypeId t = c->ty[p];
            /* a void parameter without a name is (void) or an error */
            if (!named && is_void(c, t) && !is_err(c, t))
                continue;
            pi->ps[np++] = t;
        } else if (ntag(c, p) == N_KR_IDENT) {
            pi->krlist = true;
        }
    }
    kids_free(&k);
    pi->n = np;
    if (cnode(c, f)->flags & NF_VARIADIC)
        pi->flags |= TF_VARIADIC;
}

/* The parameter list has exactly one parameter, unnamed and of type void. */
static bool lone_void(Checker *c, uint32_t f)
{
    Kids k;
    uint32_t j, np = 0, only = NO_NODE;
    bool r;
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++)
        if (is_real_param(c, k.p[j]) || ntag(c, k.p[j]) == N_KR_IDENT) {
            np++;
            only = k.p[j];
        }
    kids_free(&k);
    r = np == 1 && is_real_param(c, only) && !(c->cv[only] & 1) &&
        is_void(c, c->ty[only]);
    return r;
}

static bool func_has_params(Checker *c, uint32_t f)
{
    Kids k;
    uint32_t j;
    bool r = false;
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++)
        if (is_real_param(c, k.p[j]) || ntag(c, k.p[j]) == N_KR_IDENT)
            r = true;
    kids_free(&k);
    return r;
}

static bool func_has_vla_unspec(Checker *c, uint32_t f)
{
    uint32_t k = f, lo = cfirst(c, f);
    /* a function declarator inside a parameter has a prototype scope of its own */
    while (k > lo) {
        k--;
        if (ntag(c, k) == N_FUNC) {
            k = cfirst(c, k);
            continue;
        }
        if (ntag(c, k) == N_ARRAY && (cnode(c, k)->flags & NF_STAR))
            return true;
    }
    return false;
}

static void warn_if_shadowing(Checker *c, const CSym *x);

/* grokparms: checks on the parameter list, at input_location. */
static void grokparms(Checker *c, uint32_t f, bool funcdef, uint32_t ltok,
                      const PInfo *pi, bool *incomplete_def)
{
    Kids k;
    uint32_t j, parmno = 0;
    SrcLoc il = iloc(c, ltok);
    bool empty = !func_has_params(c, f);
    *incomplete_def = false;
    if (funcdef && func_has_vla_unspec(c, f))
        cerror(c, il, "'[*]' not allowed in other than function prototype "
               "scope");
    if (empty && !funcdef)
        cwarn(c, il, "strict-prototypes", "function declaration isn't a "
              "prototype");
    if (pi->krlist) {
        if (!funcdef)
            cpedwarn(c, il, "", "parameter names (without types) in function "
                     "declaration");
        return;
    }
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t p = k.p[j];
        TypeId t;
        bool named;
        if (!is_real_param(c, p))
            continue;
        t = c->ty[p];
        named = c->cv[p] & 1;
        if (!named && !is_err(c, t) && is_void(c, t))
            continue;
        parmno++;
        if (named && !c->cb[p])
            named = false;
        if (named && (csym(c, c->cb[p] - 1)->flags & CSF_USED))
            warn_if_shadowing(c, csym(c, c->cb[p] - 1));
        if (is_err(c, t))
            continue;
        if (!type_is_complete(TT, t)) {
            /* the lone (void) is not a parameter */
            if (parmno == 1 && lone_void(c, f))
                continue;
            if (funcdef) {
                if (named)
                    cerror(c, csym(c, c->cb[p] - 1)->loc,
                           "parameter %u ('%s') has incomplete type",
                           parmno, cident(c, csym(c, c->cb[p] - 1)->name));
                else
                    cerror(c, c->cb[p] ? csym(c, c->cb[p] - 1)->loc : il,
                           "parameter %u has incomplete type", parmno);
                *incomplete_def = true;
            } else if (is_void(c, t)) {
                if (named)
                    cwarn(c, csym(c, c->cb[p] - 1)->loc, "",
                          "parameter %u ('%s') has void type",
                          parmno, cident(c, csym(c, c->cb[p] - 1)->name));
                else
                    cwarn(c, c->cb[p] ? csym(c, c->cb[p] - 1)->loc : il, "",
                          "parameter %u has void type", parmno);
            }
        }
    }
    kids_free(&k);
}

/* ---- bit-fields -------------------------------------------------------- */

/* The minimum precision of v in a type of signedness uns (gcc's
 * tree_int_cst_min_precision). */
static unsigned min_prec(uint64_t v, bool neg, bool uns)
{
    unsigned n = 0;
    if (neg) {
        uint64_t m = ~v;
        while (m) {
            n++;
            m >>= 1;
        }
        return n + 1;
    }
    if (v == 0)
        return 1;
    while (v) {
        n++;
        v >>= 1;
    }
    return n + (uns ? 0 : 1);
}

/* Is a value of the enumeration type e too wide for w bits? */
static bool enum_narrower(Checker *c, TypeId e, unsigned w)
{
    uint32_t k;
    bool uns = !type_is_signed(TT, e), narrow = false, any = false;
    TypeId ce = type_canon(TT, e);
    for (k = 0; k < c->gsyms.len + c->lsyms.len; k++) {
        const CSym *s = k < c->gsyms.len ? &c->gsyms.data[k]
                                         : &c->lsyms.data[k - c->gsyms.len];
        bool neg;
        if (s->kind != CS_ENUMCONST || type_canon(TT, s->ty) != ce)
            continue;
        any = true;
        neg = !uns && (int64_t)s->val < 0;
        if (w < min_prec(s->val, neg, uns))
            narrow = true;
    }
    return any && narrow;
}

/* check_bitfield_type_and_width: the width node w of the field name (0:
 * unnamed); *ty may change (invalid types become unsigned); returns the
 * width in bits. */
static int check_bitfield(Checker *c, SrcLoc loc, TypeId *ty, uint32_t w,
                          uint32_t name, uint32_t ltok)
{
    const char *nm = name ? cident(c, name) : "<anonymous>";
    int64_t width = 1;
    unsigned max;
    TypeId t = *ty;
    bool ok = true;
    if (w != NO_NODE) {
        if (c->ck[w] == K_ERR || c->ck[w] == K_NONE || c->ck[w] == K_FLOAT ||
            c->ck[w] == K_ADDR || !type_is_integer(TT, c->ty[w])) {
            cerror(c, loc, "bit-field '%s' width not an integer constant", nm);
            ok = false;
        } else {
            if (!node_int_cst(c, w))
                cpedantic(c, loc, "bit-field '%s' width not an integer "
                          "constant expression", nm);
            if (c->ef[w] & EF_OVERFLOW)
                cconst_overflow(c, loc);
            width = cexpr_sval(c, w);
            if (width < 0 && !type_is_signed(TT, c->ty[w]))
                width = INT64_MAX;
            if (width < 0) {
                cerror(c, loc, "negative width in bit-field '%s'", nm);
                width = 1;
            } else if (width == 0 && name) {
                cerror(c, loc, "zero width for bit-field '%s'", nm);
                width = 1;
            }
        }
    }
    (void)ok;
    (void)ltok;
    if (is_err(c, t))
        return (int)width;
    if (!type_is_integer(TT, t)) {
        cerror(c, loc, "bit-field '%s' has invalid type", nm);
        t = TYPE_B(UINT) | TYPE_QUALS(t);
        *ty = t;
    }
    max = tkind(c, t) == TY_BOOL ? 1 : type_int_bits(TT, t);
    if (width > (int64_t)max) {
        cerror(c, loc, "width of '%s' exceeds its type", nm);
        width = max;
    }
    if (tkind(c, t) == TY_ENUM && enum_narrower(c, t, (unsigned)width))
        cwarn(c, loc, "", "'%s' is narrower than values of its type", nm);
    return (int)width;
}

/* ---- grokdeclarator ------------------------------------------------------ */

static void fill_func_type(Checker *c, uint32_t f, bool funcdef, uint32_t ltok,
                           TypeId ret, TypeId *out)
{
    PInfo pi;
    bool incomplete;
    unsigned fl;
    uint32_t j;
    func_params(c, f, &pi);
    grokparms(c, f, funcdef, ltok, &pi, &incomplete);
    for (j = 0; j < pi.n; j++)
        pi.ps[j] = TYPE_UNQUAL(pi.ps[j]);
    fl = pi.flags;
    if (!func_has_params(c, f) || pi.krlist || incomplete)
        fl |= TF_NOPROTO;
    if (fl & TF_NOPROTO) {
        if (pi.krlist || incomplete)
            pi.n = 0;
        else
            fl &= ~(unsigned)TF_VARIADIC;
    }
    *out = type_func(TT, ret, pi.ps, pi.n, fl);
    free(pi.ps);
}

/* The declarator chain from the root to (not including) the name. */
typedef struct Chain {
    uint32_t buf[32];
    uint32_t *p;
    uint32_t n;
} Chain;

static void chain_get(Checker *c, uint32_t top, Chain *ch)
{
    uint32_t d = top, cap = 32;
    ch->p = ch->buf;
    ch->n = 0;
    while (d != NO_NODE && ntag(c, d) != N_NAME) {
        if (ch->n == cap) {
            uint32_t *np = xmalloc(cap * 2 * sizeof *np);
            memcpy(np, ch->p, ch->n * sizeof *np);
            if (ch->p != ch->buf)
                free(ch->p);
            ch->p = np;
            cap *= 2;
        }
        ch->p[ch->n++] = d;
        d = inner_decl(c, d);
    }
}

static void chain_free(Chain *ch)
{
    if (ch->p != ch->buf)
        free(ch->p);
}

/* -Wlarger-than=N: an object declaration (layout_decl) of more than N bytes. */
static void larger_than(Checker *c, SrcLoc loc, uint32_t name, TypeId ty)
{
    uint64_t lim, sz;
    bool ok;
    if (!name || !diag_enabled(c->diag, "larger-than=") ||
        !(lim = diag_option_size(c->diag, "larger-than=")))
        return;
    sz = type_size(TT, ty, &ok);
    if (ok && sz > lim)
        cwarn(c, loc, "larger-than=", "size of '%s' %llu bytes exceeds "
              "maximum object size %llu", cident(c, name),
              (unsigned long long)sz, (unsigned long long)lim);
}

static bool valid_array_size(Checker *c, SrcLoc loc, TypeId elem, uint64_t n,
                             uint32_t name)
{
    bool ok;
    uint64_t es = type_size(TT, elem, &ok);
    if (!ok || !es)
        return true;
    if (n > (uint64_t)INT64_MAX / es) {
        if (name)
            cerror(c, loc, "size of array '%s' exceeds maximum object size "
                   "'9223372036854775807'", cident(c, name));
        else
            cerror(c, loc, "size of array exceeds maximum object size "
                   "'9223372036854775807'");
        return false;
    }
    return true;
}

static bool flex_struct(Checker *c, TypeId t);

/* grokdeclarator.  ltok: gcc's lookahead token at the diagnostics without a
 * location of their own (input_location); after: the token after the
 * declarator (for the location of an abstract one). */
static void grok(Checker *c, const Spec *sp, uint32_t top, int ctx,
                 bool funcdef, bool initialized, uint32_t width_node,
                 uint32_t ltok, uint32_t after, GDecl *g)
{
    TypeId type = sp->ty;
    bool threadp = sp->thread;
    unsigned sc = sp->sc;
    unsigned type_quals, array_ptr_quals = 0;
    bool array_ptr_attrs = false, array_parm_static = false;
    bool array_parm_vla_unspec = false;
    bool size_varies = false, funcdef_syntax = false;
    bool bitfield = width_node != NO_NODE;
    bool filescope = cat_file_scope(c);
    uint32_t name = 0, name_node = NO_NODE, d;
    SrcLoc loc = 0;
    unsigned first_kind = 0;
    Chain ch;
    uint32_t ci;
    int wbits = -1;
    bool default_int = sp->default_int;
    (void)size_varies;

    memset(g, 0, sizeof *g);
    g->width = -1;
    g->ty = ERRT;
    g->what = GD_NONE;
    if (is_err(c, type))
        return;
    /* the name, the location */
    d = top;
    while (d != NO_NODE) {
        unsigned tg = ntag(c, d);
        if (tg == N_NAME) {
            name_node = d;
            name = cnode_ident(c, d);
            loc = tloc(c, cnode(c, d)->tok);
            if (!first_kind)
                first_kind = N_NAME;
            break;
        }
        if (!first_kind)
            first_kind = tg;
        funcdef_syntax = tg == N_FUNC;
        d = inner_decl(c, d);
    }
    if (name_node == NO_NODE)
        loc = abstract_loc(c, top, after);
    if (funcdef && !funcdef_syntax)
        return;
    g->name = name;
    g->name_node = name_node;
    g->loc = loc;
    g->default_int = default_int;
    if ((ctx == DC_NORMAL || ctx == DC_FIELD) && filescope &&
        type_is_vm(TT, type)) {
        if (name)
            cerror(c, loc, "variably modified '%s' at file scope",
                   cident(c, name));
        else
            cerror(c, loc, "variably modified field at file scope");
        type = TYPE_B(INT);
    }
    if (default_int && !funcdef) {
        if (name)
            cpedwarn(c, loc, "implicit-int", "type defaults to 'int' in "
                     "declaration of '%s'", cident(c, name));
        else
            cpedwarn(c, loc, "implicit-int", "type defaults to 'int' in type "
                     "name");
    }
    type_quals = sp->quals | all_quals(c, type);
    type_quals &= TQ_MASK;
    if ((sp->quals & TQ_ATOMIC) && is_arr(c, type))
        cerror(c, loc, "'_Atomic'-qualified array type");
    if (funcdef && (threadp || sc == SC_AUTO || sc == SC_REGISTER ||
                    sc == SC_TYPEDEF)) {
        if (sc == SC_AUTO)
            cpedwarn(c, loc, filescope ? "" : "pedantic",
                     "function definition declared 'auto'");
        if (sc == SC_REGISTER)
            cerror(c, loc, "function definition declared 'register'");
        if (sc == SC_TYPEDEF)
            cerror(c, loc, "function definition declared 'typedef'");
        if (threadp)
            cerror(c, loc, "function definition declared '%s'",
                   sp->thread_gnu ? "__thread" : "_Thread_local");
        threadp = false;
        if (sc == SC_AUTO || sc == SC_REGISTER || sc == SC_TYPEDEF)
            sc = SC_NONE;
    } else if (ctx != DC_NORMAL && (sc != SC_NONE || threadp)) {
        if (ctx == DC_PARM && sc == SC_REGISTER) {
            /* allowed */
        } else {
            if (ctx == DC_FIELD) {
                if (name)
                    cerror(c, loc, "storage class specified for structure "
                           "field '%s'", cident(c, name));
                else
                    cerror(c, loc, "storage class specified for structure "
                           "field");
            } else if (ctx == DC_PARM) {
                if (name)
                    cerror(c, loc, "storage class specified for parameter "
                           "'%s'", cident(c, name));
                else
                    cerror(c, loc, "storage class specified for unnamed "
                           "parameter");
            } else if (c->cd_clit)
                cpedantic(c, tloc(c, c->cd_clit), "ISO C forbids storage "
                          "class specifiers in compound literals before C2X");
            else
                cerror(c, loc, "storage class specified for typename");
            sc = SC_NONE;
            threadp = false;
        }
    } else if (sc == SC_EXTERN && initialized && !funcdef) {
        if (filescope)
            cwarn(c, loc, "", "'%s' initialized and declared 'extern'",
                  cident(c, name));
        else
            cerror(c, loc, "'%s' has both 'extern' and initializer",
                   cident(c, name));
    } else if (filescope) {
        if (sc == SC_AUTO)
            cerror(c, loc, "file-scope declaration of '%s' specifies 'auto'",
                   cident(c, name));
        if (sc == SC_REGISTER)
            cpedantic(c, iloc(c, ltok), "file-scope declaration of '%s' "
                      "specifies 'register'", cident(c, name));
    } else {
        if (sc == SC_EXTERN && funcdef)
            cerror(c, loc, "nested function '%s' declared 'extern'",
                   cident(c, name));
        else if (threadp && sc == SC_NONE) {
            cerror(c, loc, "function-scope '%s' implicitly auto and declared "
                   "'%s'", cident(c, name),
                   sp->thread_gnu ? "__thread" : "_Thread_local");
            threadp = false;
        }
    }

    chain_get(c, top, &ch);
    for (ci = 0; ci < ch.n; ci++) {
        uint32_t dn = ch.p[ci];
        unsigned tg = ntag(c, dn);
        if (is_err(c, type))
            continue;
        if (array_ptr_quals || array_ptr_attrs || array_parm_static) {
            cerror(c, loc, "static or type qualifiers in non-parameter array "
                   "declarator");
            array_ptr_quals = 0;
            array_ptr_attrs = false;
            array_parm_static = false;
        }
        if (tg == N_ARRAY) {
            uint32_t sz = array_size_node(c, dn);
            bool itype_none = true, unspec;
            bool vla = false;
            uint64_t n = 0;
            const Node *an = cnode(c, dn);
            array_ptr_quals = quals_of_warn(c, dn);
            array_ptr_attrs = has_child_attr(c, dn);
            array_parm_static = (an->flags & NF_STATIC) != 0;
            unspec = (an->flags & NF_STAR) && !(c->cv[dn] & 1);
            array_parm_vla_unspec = unspec;
            if (is_void(c, type)) {
                if (name)
                    cerror(c, loc, "declaration of '%s' as array of voids",
                           cident(c, name));
                else
                    cerror(c, loc, "declaration of type name as array of "
                           "voids");
                type = ERRT;
            }
            if (is_func(c, type)) {
                if (name)
                    cerror(c, loc, "declaration of '%s' as array of "
                           "functions", cident(c, name));
                else
                    cerror(c, loc, "declaration of type name as array of "
                           "functions");
                type = ERRT;
            }
            if (c->opt.pedantic && flex_struct(c, type))
                cpedantic(c, loc, "invalid use of structure with flexible "
                          "array member");
            if (sz != NO_NODE && c->ck[sz] == K_ERR &&
                is_err(c, c->ty[sz]))
                type = ERRT;
            if (is_err(c, type))
                continue;
            if (sz != NO_NODE) {
                bool size_int_const = node_int_cst(c, sz);
                bool folded = c->ck[sz] == K_ICE || c->ck[sz] == K_FOLD;
                bool this_varies = false;
                int64_t sv = 0;
                TypeId st = c->ty[sz];
                bool bad_size = false;
                if (c->ck[sz] == K_ERR) {
                    type = ERRT;
                    continue;
                }
                if (!type_is_integer(TT, st)) {
                    if (name)
                        cerror(c, loc, "size of array '%s' has non-integer "
                               "type", cident(c, name));
                    else
                        cerror(c, loc, "size of unnamed array has non-integer "
                               "type");
                    bad_size = true;
                } else if (!cexpr_rvalue_ok_at(c, sz,
                                               cnode_loc(c, dn))) {
                    /* require_complete_type, at the '[' */
                    bad_size = true;
                }
                if (bad_size) {
                    n = 1;
                    size_int_const = true;
                    folded = true;
                } else if (folded) {
                    sv = cexpr_sval(c, sz);
                    if (sv < 0 && !type_is_signed(TT, st)) {
                        /* too large for an unsigned constant */
                        sv = INT64_MAX;
                        n = (uint64_t)cexpr_sval(c, sz);
                    } else
                        n = (uint64_t)sv;
                }
                if (folded && !bad_size && sv == 0 && n == 0) {
                    if (name)
                        cpedantic(c, loc, "ISO C forbids zero-size array '%s'",
                                  cident(c, name));
                    else
                        cpedantic(c, loc, "ISO C forbids zero-size array");
                }
                if (folded) {
                    if (sv < 0) {
                        if (name)
                            cerror(c, loc, "size of array '%s' is negative",
                                   cident(c, name));
                        else
                            cerror(c, loc, "size of unnamed array is "
                                   "negative");
                        n = 1;
                        sv = 1;
                        size_int_const = true;
                    }
                    if (!size_int_const) {
                        if ((ctx == DC_NORMAL || ctx == DC_FIELD) && filescope)
                            cpedwarn(c, iloc(c, ltok), "", "variably modified "
                                     "'%s' at file scope",
                                     name ? cident(c, name) : "");
                        else
                            this_varies = size_varies = true;
                        if (this_varies) {
                            if (name)
                                cwarn(c, iloc(c, ltok), "vla", "ISO C90 "
                                      "forbids variable length array '%s'",
                                      cident(c, name));
                            else
                                cwarn(c, iloc(c, ltok), "vla", "ISO C90 "
                                      "forbids variable length array");
                        }
                    }
                } else if ((ctx == DC_NORMAL || ctx == DC_FIELD) && filescope) {
                    cerror(c, loc, "variably modified '%s' at file scope",
                           name ? cident(c, name) : "");
                    n = 1;
                    folded = true;
                } else {
                    this_varies = size_varies = true;
                    if (name)
                        cwarn(c, iloc(c, ltok), "vla", "ISO C90 forbids "
                              "variable length array '%s'", cident(c, name));
                    else
                        cwarn(c, iloc(c, ltok), "vla", "ISO C90 forbids "
                              "variable length array");
                }
                if (this_varies)
                    vla = true;
                if (!vla && folded && n > (uint64_t)INT64_MAX) {
                    if (name)
                        cerror(c, loc, "size of array '%s' is too large",
                               cident(c, name));
                    else
                        cerror(c, loc, "size of unnamed array is too large");
                    type = ERRT;
                    continue;
                }
                itype_none = false;
                if (!vla && !folded)
                    itype_none = true;
            } else if (ctx == DC_FIELD) {
                /* a flexible array member (or [*]) */
                if (array_parm_vla_unspec) {
                    size_varies = true;
                    vla = true;
                    itype_none = false;
                }
            } else if (ctx == DC_PARM) {
                if (array_parm_vla_unspec) {
                    vla = true;
                    size_varies = true;
                    itype_none = false;
                }
            } else if (ctx == DC_TYPENAME) {
                if (array_parm_vla_unspec) {
                    cwarn(c, iloc(c, ltok), "", "'[*]' not in a declaration");
                    vla = true;
                    size_varies = true;
                    itype_none = false;
                }
            }
            if (!type_is_complete(TT, type)) {
                Diagnostic *dd = cerror_d(c, loc, "array type has incomplete "
                                          "element type %s", type_q(TT, type));
                if (is_arr(c, type)) {
                    if (name)
                        cnote(c, dd, loc, "declaration of '%s' as "
                              "multidimensional array must have bounds for "
                              "all dimensions except the first",
                              cident(c, name));
                    else
                        cnote(c, dd, loc, "declaration of multidimensional "
                              "array must have bounds for all dimensions "
                              "except the first");
                }
                type = ERRT;
            } else {
                if (sp->node != NO_NODE) {
                    /* layout_type: a user alignment the element size does not
                     * respect */
                    bool ok;
                    uint64_t esz = type_size(TT, type, &ok);
                    unsigned al = type_align(TT, type);
                    if (ok && al && esz % al)
                        cerror(c, tloc(c, first_tok(c, sp->node)),
                               esz < al ? "alignment of array elements is "
                               "greater than element size" :
                               "size of array element is not a multiple of "
                               "its alignment");
                }
                if (!vla) {      /* an array of variable-size type */
                    TypeId et = type_canon(TT, type);
                    while (tkind(c, et) == TY_ARRAY)
                        et = type_canon(TT, type_base(TT, et));
                    if (tkind(c, et) == TY_VLA ||
                        ((tkind(c, et) == TY_STRUCT || tkind(c, et) == TY_UNION) &&
                         (type_record(TT, et)->flags & RF_VLA)))
                        vla = true;
                }
                if (vla)
                    type = type_vla(TT, type);
                else if (sz != NO_NODE || (unspec && !vla))
                    type = type_array(TT, type, n);
                else
                    type = type_array_incomplete(TT, type);
                if (!vla && sz != NO_NODE &&
                    !valid_array_size(c, loc, type_base(TT, type), n, name))
                    type = ERRT;
            }
            (void)itype_none;
            if (ctx != DC_PARM && (array_ptr_quals || array_ptr_attrs ||
                                   array_parm_static)) {
                cerror(c, loc, "static or type qualifiers in non-parameter "
                       "array declarator");
                array_ptr_quals = 0;
                array_ptr_attrs = false;
                array_parm_static = false;
            }
        } else if (tg == N_FUNC) {
            bool really = funcdef && inner_decl(c, dn) != NO_NODE &&
                          ntag(c, inner_decl(c, dn)) == N_NAME;
            if (is_func(c, type)) {
                if (name)
                    cerror(c, loc, "'%s' declared as function returning a "
                           "function", cident(c, name));
                else
                    cerror(c, loc, "type name declared as function returning a "
                           "function");
                type = TYPE_B(INT);
            }
            if (is_arr(c, type)) {
                if (name)
                    cerror(c, loc, "'%s' declared as function returning an "
                           "array", cident(c, name));
                else
                    cerror(c, loc, "type name declared as function returning "
                           "an array");
                type = TYPE_B(INT);
            }
            if (type_quals) {
                SrcLoc sl = 0;
                unsigned k;
                for (k = 0; k < 4; k++)
                    if (sp->qual_tok[k]) {
                        SrcLoc l = tloc(c, sp->qual_tok[k] - 1);
                        if (!sl || l < sl)
                            sl = l;
                    }
                if (!sl)
                    sl = sp->kind == TSK_TYPEDEF ? sp->type_loc : 0;
                if (!sl)
                    sl = loc;
                if (is_void(c, type) && really)
                    cpedwarn(c, sl, "", "function definition has qualified "
                             "void return type");
                else
                    cwarn(c, sl, "ignored-qualifiers", "type qualifiers "
                          "ignored on function return type");
                type = qualify(c, type, type_quals, ltok);
            }
            type_quals = 0;
            {
                TypeId ft;
                fill_func_type(c, dn, really, ltok, type, &ft);
                type = ft;
            }
            size_varies = false;
        } else { /* N_PTR */
            uint32_t pn = dn;
            if ((type_quals & TQ_ATOMIC) && is_func(c, type)) {
                cerror(c, loc, "'_Atomic'-qualified function type");
                type_quals &= ~(unsigned)TQ_ATOMIC;
            } else if (is_func(c, type) && type_quals)
                cpedantic(c, loc, "ISO C forbids qualified function types");
            if (type_quals)
                type = qualify(c, type, type_quals, ltok);
            size_varies = false;
            type = type_ptr(TT, type);
            type_quals = quals_of_warn(c, pn);
        }
    }
    chain_free(&ch);

    if (bitfield) {
        wbits = check_bitfield(c, loc, &type, width_node, name, ltok);
        if (type_quals & TQ_ATOMIC) {
            if (name)
                cerror(c, loc, "bit-field '%s' has atomic type",
                       cident(c, name));
            else
                cerror(c, loc, "bit-field has atomic type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        }
    }
    g->width = wbits;
    if (sp->alignas_seen) {
        if (sc == SC_TYPEDEF)
            cerror(c, loc, "alignment specified for typedef '%s'",
                   cident(c, name));
        else if (sc == SC_REGISTER)
            cerror(c, loc, "alignment specified for 'register' object '%s'",
                   cident(c, name));
        else if (ctx == DC_PARM) {
            if (name)
                cerror(c, loc, "alignment specified for parameter '%s'",
                       cident(c, name));
            else
                cerror(c, loc, "alignment specified for unnamed parameter");
        } else if (bitfield) {
            if (name)
                cerror(c, loc, "alignment specified for bit-field '%s'",
                       cident(c, name));
            else
                cerror(c, loc, "alignment specified for unnamed bit-field");
        } else if (is_func(c, type))
            cerror(c, loc, "alignment specified for function '%s'",
                   cident(c, name));
        else if (sp->align && !is_err(c, type)) {
            if (sp->alignas_seen && sp->align < type_align(TT, type) &&
                sp->attrs.aligned < sp->align) {
                if (name)
                    cerror(c, loc, "'_Alignas' specifiers cannot reduce "
                           "alignment of '%s'", cident(c, name));
                else
                    cerror(c, loc, "'_Alignas' specifiers cannot reduce "
                           "alignment of unnamed field");
                g->s.align = 0;
            } else
                g->s.align = (uint16_t)sp->align;
        }
    }
    if (!g->s.align && sp->align && !sp->alignas_seen)
        g->s.align = (uint16_t)sp->align;

    if (sc == SC_TYPEDEF) {
        if ((type_quals & TQ_ATOMIC) && is_func(c, type)) {
            cerror(c, loc, "'_Atomic'-qualified function type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        } else if (is_func(c, type) && type_quals)
            cpedantic(c, loc, "ISO C forbids qualified function types");
        if (type_quals)
            type = qualify(c, type, type_quals, ltok);
        if (sp->is_inline)
            cpedwarn(c, loc, "", "typedef '%s' declared 'inline'",
                     cident(c, name));
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "typedef '%s' declared '_Noreturn'",
                     cident(c, name));
        g->s.name = name;
        g->s.loc = loc;
        g->what = GD_TYPEDEF;
        g->ty = type;
        g->s.kind = CS_TYPEDEF;
        g->s.sc = SC_TYPEDEF;
        g->s.ty = type;
        return;
    }
    if (ctx == DC_TYPENAME) {
        if ((type_quals & TQ_ATOMIC) && is_func(c, type)) {
            cerror(c, loc, "'_Atomic'-qualified function type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        } else if (is_func(c, type) && type_quals)
            cpedantic(c, loc, "ISO C forbids const or volatile function "
                      "types");
        if (type_quals)
            type = qualify(c, type, type_quals, ltok);
        g->what = GD_TYPENAME;
        g->ty = type;
        return;
    }
    if (ctx == DC_FIELD && !is_err(c, type) && type_is_vm(TT, type))
        cpedantic(c, loc, "a member of a structure or union cannot have a "
                  "variably modified type");
    if (is_void(c, type) && ctx != DC_PARM &&
        !((ctx != DC_FIELD && !is_func(c, type)) &&
          (sc == SC_EXTERN ||
           (filescope && !(sc == SC_STATIC || sc == SC_REGISTER))))) {
        cerror(c, loc, "variable or field '%s' declared void",
               name ? cident(c, name) : "");
        type = TYPE_B(INT);
    }
    g->s.name = name;
    g->s.loc = loc;
    g->s.sc = (uint8_t)sc;
    if (ctx == DC_PARM) {
        bool arrp = false;
        if (is_arr(c, type)) {
            TypeId e = type_base(TT, type);
            g->pre = type;
            if (type_quals)
                e = qualify(c, e, type_quals, ltok);
            type = type_ptr(TT, e);
            type_quals = array_ptr_quals;
            if (type_quals)
                type = qualify(c, type, type_quals, ltok);
            if (array_ptr_attrs)
                cwarn(c, loc, "attributes", "attributes in parameter array "
                      "declarator ignored");
            arrp = true;
        } else if (is_func(c, type)) {
            if (type_quals & TQ_ATOMIC) {
                cerror(c, loc, "'_Atomic'-qualified function type");
                type_quals &= ~(unsigned)TQ_ATOMIC;
            } else if (type_quals)
                cpedantic(c, loc, "ISO C forbids qualified function types");
            type = type_ptr(TT, type | type_quals);
            type_quals = 0;
        } else if (type_quals)
            type = qualify(c, type, type_quals, ltok);
        if (sp->is_inline)
            cpedwarn(c, loc, "", "parameter '%s' declared 'inline'",
                     name && *cident(c, name) ? cident(c, name)
                                              : "({anonymous})");
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "parameter '%s' declared '_Noreturn'",
                     name && *cident(c, name) ? cident(c, name)
                                              : "({anonymous})");
        g->what = GD_PARM;
        g->array_param = arrp;
        g->ty = type;
        g->s.kind = CS_OBJ;
        g->s.flags = CSF_PARAM | CSF_DEFINED | (arrp ? CSF_ARRAY_PARM : 0);
        g->s.ty = type;
        return;
    }
    if (ctx == DC_FIELD) {
        if (is_func(c, type)) {
            cerror(c, loc, "field '%s' declared as a function",
                   name ? cident(c, name) : "");
            type = type_ptr(TT, type);
        } else if (!is_err(c, type) &&
                   !(type_is_complete(TT, type) ||
                     (tkind(c, type) == TY_ARRAY &&
                      (type_ent(TT, type_canon(TT, type))->flags &
                       TF_INCOMPLETE)))) {
            if (name)
                cerror(c, loc, "field '%s' has incomplete type",
                       cident(c, name));
            else
                cerror(c, loc, "unnamed field has incomplete type");
            type = ERRT;
        }
        if (!is_err(c, type))
            type = qualify(c, type, type_quals, ltok);
        g->what = GD_FIELD;
        g->ty = type;
        g->s.kind = CS_OBJ;
        g->s.ty = type;
        return;
    }
    if (is_func(c, type)) {
        bool fs = !filescope;
        if (sc == SC_REGISTER || threadp) {
            cerror(c, loc, "invalid storage class for function '%s'",
                   cident(c, name));
        } else if (fs) {
            if (sc == SC_AUTO)
                cpedantic(c, loc, "invalid storage class for function '%s'",
                          cident(c, name));
            else if (sc == SC_STATIC) {
                cerror(c, loc, "invalid storage class for function '%s'",
                       cident(c, name));
                if (funcdef)
                    sc = SC_NONE;
                else {
                    g->what = GD_NONE;
                    return;
                }
            }
        }
        g->s.sc = (uint8_t)sc;
        if (type_quals & TQ_ATOMIC) {
            cerror(c, loc, "'_Atomic'-qualified function type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        } else if (type_quals)
            cpedantic(c, loc, "ISO C forbids qualified function types");
        if (sc == SC_AUTO && fs)
            g->s.flags &= ~(unsigned)CSF_DECL_EXTERNAL;
        else if (sp->is_inline && sc != SC_STATIC) {
            /* DECL_EXTERNAL = (extern) == gnu89 semantics */
            if ((sc == SC_EXTERN) == sp->attrs.gnu_inline)
                g->s.flags |= CSF_DECL_EXTERNAL;
        } else if (!initialized)
            g->s.flags |= CSF_DECL_EXTERNAL;
        g->s.linkage = (sc == SC_STATIC || sc == SC_AUTO) ? LK_INTERNAL
                                                          : LK_EXTERNAL;
        if (name && !strcmp(cident(c, name), "main")) {
            if (sp->is_inline)
                cpedwarn(c, loc, "", "cannot inline function 'main'");
            if (sp->is_noreturn)
                cpedwarn(c, loc, "", "'main' declared '_Noreturn'");
        } else {
            if (sp->is_inline)
                g->s.flags |= CSF_INLINE;
            if (sp->attrs.gnu_inline)
                g->s.flags |= CSF_GNU_INLINE;
            if (sp->is_noreturn) {
                cped11(c, loc, "ISO C99 does not support '_Noreturn'");
                g->s.flags |= CSF_NORETURN;
            }
        }
        if (sp->attrs.noreturn)
            g->s.flags |= CSF_NORETURN;
        g->what = GD_FUNC;
        g->s.kind = CS_FUNC;
    } else {
        bool extern_ref = !initialized && sc == SC_EXTERN;
        if (!is_err(c, type))
            type = qualify(c, type, type_quals, ltok);
        if (extern_ref && !filescope) {
            uint32_t ge = name && name < c->nidents ? c->ext[name] : 0;
            uint32_t vis = lookup_ord(c, name);
            csum_read_ext(c, name);
            if (ge && ge - 1 != vis && csym(c, ge - 1)->kind == CS_OBJ &&
                csym(c, ge - 1)->linkage != LK_EXTERNAL)
                cerror(c, loc, "variable previously declared 'static' "
                       "redeclared 'extern'");
        }
        if (sp->is_inline)
            cpedwarn(c, loc, "", "variable '%s' declared 'inline'",
                     cident(c, name));
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "variable '%s' declared '_Noreturn'",
                     cident(c, name));
        if (sc == SC_EXTERN)
            g->s.flags |= CSF_DECL_EXTERNAL;
        if (filescope) {
            g->s.linkage = sc != SC_STATIC ? LK_EXTERNAL : LK_INTERNAL;
            if (!extern_ref)
                g->s.flags |= CSF_TREE_STATIC;
        } else {
            if (sc == SC_STATIC)
                g->s.flags |= CSF_TREE_STATIC;
            g->s.linkage = extern_ref ? LK_EXTERNAL : LK_NONE;
        }
        if (threadp)
            g->s.flags |= CSF_THREAD;
        g->what = GD_VAR;
        g->s.kind = CS_OBJ;
    }
    if ((sc == SC_EXTERN || (sc == SC_NONE && is_func(c, type) && !funcdef)) &&
        type_is_vm(TT, type)) {
        if (is_func(c, type))
            cerror(c, loc, "non-nested function with variably modified type");
        else
            cerror(c, loc, "object with variably modified type must have no "
                   "linkage");
    }
    g->ty = type;
    g->s.ty = type;
    (void)first_kind;
}

/* ---- symbols: duplicate declarations, pushdecl ---------------------------- */

static bool sym_public(const CSym *s)
{
    return s->linkage == LK_EXTERNAL;
}

static bool sym_external(const CSym *s)
{
    return (s->flags & (CSF_DECL_EXTERNAL | CSF_IMPLICIT)) != 0;
}

static bool sym_defined(const CSym *s)
{
    return (s->flags & CSF_DEFINED) != 0;
}

/* DECL_FILE_SCOPE_P: the symbol is a persistent one. */
static bool ref_file_scope(uint32_t ref)
{
    return !(ref & SYM_LOCAL);
}

/* gcc's tree code of a symbol. */
static int dcode(const CSym *s)
{
    return s->kind * 2 + (s->kind == CS_OBJ && (s->flags & CSF_PARAM) ? 1 : 0);
}

static bool extern_inline(const CSym *s)
{
    return (s->flags & CSF_INLINE) && sym_external(s);
}

static const char *sname(Checker *c, const CSym *s)
{
    return cident(c, s->name);
}

/* The type a typedef names (a typedef symbol's ty is its own entry). */
static TypeId typedef_under(Checker *c, TypeId t)
{
    if (type_kind(TT, t) == TY_TYPEDEF)
        return type_ent(TT, t)->base | TYPE_QUALS(t);
    return t;
}

/* comptypes: a K&R definition's parameter types only ask for a warning
 * when they disagree with a prototype (gcc's value 2), not for an error. */
static bool compat_gcc(Checker *c, TypeId a, TypeId b)
{
    TypeId ca = type_canon(TT, a), cb = type_canon(TT, b);
    if (type_kind(TT, ca) == TY_FUNC && type_kind(TT, cb) == TY_FUNC) {
        const TypeEnt *ea = type_ent(TT, ca), *eb = type_ent(TT, cb);
        bool npa = ea->flags & TF_NOPROTO, npb = eb->flags & TF_NOPROTO;
        if (npa != npb) {
            const TypeEnt *np = npa ? ea : eb;
            TypeId stripped = type_func(TT, np->base, NULL, 0, TF_NOPROTO);
            return type_compatible(TT, npa ? stripped : a,
                                   npa ? b : stripped);
        }
    }
    return type_compatible(TT, a, b);
}

/* Two function types whose return types differ only by volatile (gcc
 * accepts them, with a pedwarn: the old way to write noreturn). */
static bool volatile_ret_only(Checker *c, TypeId a, TypeId b)
{
    TypeId ca = type_canon(TT, a), cb = type_canon(TT, b), ra, rb;
    const TypeEnt *ea, *eb;
    if (type_kind(TT, ca) != TY_FUNC || type_kind(TT, cb) != TY_FUNC)
        return false;
    ea = type_ent(TT, ca);
    eb = type_ent(TT, cb);
    ra = type_base(TT, ca);
    rb = type_base(TT, cb);
    if ((TYPE_QUALS(ra) ^ TYPE_QUALS(rb)) != TQ_VOLATILE)
        return false;
    ra = TYPE_UNQUAL(ra) | (TYPE_QUALS(ra) & ~TQ_VOLATILE);
    rb = TYPE_UNQUAL(rb) | (TYPE_QUALS(rb) & ~TQ_VOLATILE);
    return compat_gcc(c, type_func(TT, ra, type_params(TT, ca), ea->n,
                                   ea->flags),
                      type_func(TT, rb, type_params(TT, cb), eb->n,
                                eb->flags));
}

/* The note of locate_old_decl. */
static void locate_old_decl(Checker *c, Diagnostic *d, const CSym *o)
{
    if (!d)
        return;
    if (sym_defined(o) || o->kind == CS_ENUMCONST)
        cnote(c, d, o->loc, "previous definition of '%s' with type %s",
              sname(c, o), type_q(TT, o->ty));
    else if (o->flags & CSF_IMPLICIT)
        cnote(c, d, o->loc, "previous implicit declaration of '%s' with type "
              "%s", sname(c, o), type_q(TT, o->ty));
    else
        cnote(c, d, o->loc, "previous declaration of '%s' with type %s",
              sname(c, o), type_q(TT, o->ty));
}

/* Would conversion of the two types differ only by an enum against an
 * integer type somewhere (gcc's enum_and_int_p)? */
static bool enum_int_pair(Checker *c, TypeId a, TypeId b)
{
    TypeKind ka, kb;
    a = type_canon(TT, a);
    b = type_canon(TT, b);
    ka = tkind(c, a);
    kb = tkind(c, b);
    if (ka == TY_ENUM && kb != TY_ENUM && type_is_integer(TT, b))
        return true;
    if (kb == TY_ENUM && ka != TY_ENUM && type_is_integer(TT, a))
        return true;
    if (ka != kb)
        return false;
    if (ka == TY_PTR || ka == TY_ARRAY)
        return enum_int_pair(c, type_base(TT, a), type_base(TT, b));
    if (ka == TY_FUNC) {
        uint32_t n = (uint32_t)type_ent(TT, a)->n, m = (uint32_t)type_ent(TT, b)->n, k;
        if (enum_int_pair(c, type_base(TT, a), type_base(TT, b)))
            return true;
        for (k = 0; k < n && k < m; k++)
            if (enum_int_pair(c, type_params(TT, a)[k], type_params(TT, b)[k]))
                return true;
    }
    return false;
}

static void arglist_conflict(Checker *c, Diagnostic *d, const CSym *nw,
                             const CSym *o, TypeId newtype, TypeId oldtype)
{
    TypeId t;
    uint32_t n, k;
    const TypeId *ps;
    if (!d || o->kind != CS_FUNC ||
        !type_compatible(TT, type_base(TT, oldtype), type_base(TT, newtype)) ||
        !((!is_prototype(c, oldtype) && !sym_defined(o)) ||
          (!is_prototype(c, newtype) && !sym_defined(nw))))
        return;
    t = is_prototype(c, oldtype) ? oldtype : newtype;
    if (!is_prototype(c, t))
        return;
    t = type_canon(TT, t);
    if (type_ent(TT, t)->flags & TF_VARIADIC) {
        cnote(c, d, iloc(c, c->cd_ltok), "a parameter list with an ellipsis "
              "cannot match an empty parameter name list declaration");
        return;
    }
    n = (uint32_t)type_ent(TT, t)->n;
    ps = type_params(TT, t);
    for (k = 0; k < n; k++)
        if (type_default_promote(TT, ps[k]) != ps[k]) {
            cnote(c, d, iloc(c, c->cd_ltok), "an argument type that has a "
                  "default promotion cannot match an empty parameter name "
                  "list declaration");
            return;
        }
}

static TypeId main_variant(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    return (TYPE_QUALS(k) & TQ_ATOMIC) ? TYPE_UNQUAL(k) | TQ_ATOMIC
                                       : TYPE_UNQUAL(k);
}

/* validate_proto_after_old_defn: *d gets the error's diagnostic. */
static bool validate_proto_after_old_defn(Checker *c, const CSym *nw,
                                          const CSym *o, Diagnostic **d)
{
    TypeId ot = type_canon(TT, o->ty), nt = type_canon(TT, nw->ty);
    uint32_t on = (uint32_t)type_ent(TT, ot)->n;
    uint32_t nn = (uint32_t)type_ent(TT, nt)->n, k;
    const TypeId *op = type_params(TT, ot), *np = type_params(TT, nt);
    *d = NULL;
    for (k = 0; k < on || k < nn; k++) {
        TypeId a, b;
        if (k >= on) {
            *d = cerror_d(c, nw->loc, "prototype for '%s' declares more "
                          "arguments than previous old-style definition",
                          sname(c, nw));
            return false;
        }
        if (k >= nn) {
            *d = cerror_d(c, nw->loc, "prototype for '%s' declares fewer "
                          "arguments than previous old-style definition",
                          sname(c, nw));
            return false;
        }
        if (is_err(c, op[k]) || is_err(c, np[k]))
            return false;
        a = main_variant(c, op[k]);
        b = main_variant(c, np[k]);
        if (!type_compatible(TT, a, b)) {
            *d = cerror_d(c, nw->loc, "prototype for '%s' declares argument "
                          "%u with incompatible type", sname(c, nw), k + 1);
            return false;
        }
    }
    cwarn(c, nw->loc, "", "prototype for '%s' follows non-prototype "
          "definition", sname(c, nw));
    return true;
}

/* diagnose_mismatched_decls: are the two consistent?  nfile/ofile:
 * DECL_FILE_SCOPE_P of the new and the old declaration. */
static bool diagnose_mismatched(Checker *c, CSym *nw, bool nfile,
                                const CSym *o, bool ofile,
                                bool new_implicit_int, TypeId *newtypep,
                                TypeId *oldtypep)
{
    TypeId newtype = nw->ty, oldtype = o->ty;
    Diagnostic *d, *wd = NULL;
    bool pedwarned = false, enum_and_int = false, note_new = false;
    *newtypep = newtype;
    *oldtypep = oldtype;
    if (is_err(c, oldtype) || is_err(c, newtype))
        return false;
    if (dcode(o) != dcode(nw)) {
        d = cerror_d(c, nw->loc, "'%s' redeclared as different kind of "
                     "symbol", sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    if (o->kind == CS_ENUMCONST) {
        d = cerror_d(c, nw->loc, "redeclaration of enumerator '%s'",
                     sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    {
        TypeId a = o->kind == CS_TYPEDEF ? typedef_under(c, oldtype) : oldtype;
        TypeId b = o->kind == CS_TYPEDEF ? typedef_under(c, newtype) : newtype;
        if (!compat_gcc(c, a, b)) {
            if (nw->kind == CS_FUNC && sym_defined(nw) &&
                is_void(c, type_base(TT, oldtype)) &&
                type_canon(TT, type_base(TT, newtype)) == TYPE_B(INT) &&
                new_implicit_int && !sym_defined(o)) {
                d = cpedwarn(c, nw->loc, "", "conflicting types "
                             "for '%s'", sname(c, nw));
                pedwarned = d != NULL;
                wd = d;
                nw->ty = newtype = oldtype;
                *newtypep = newtype;
            } else if (nw->kind == CS_FUNC &&
                       is_void(c, type_base(TT, newtype)) &&
                       type_canon(TT, type_base(TT, oldtype)) == TYPE_B(INT) &&
                       (o->flags & CSF_IMPLICIT) && !sym_defined(o)) {
                d = cpedwarn(c, sym_defined(nw) ? nw->loc : iloc(c, c->cd_ltok),
                             "", "conflicting types for '%s'; have %s",
                             sname(c, nw), type_q(TT, newtype));
                pedwarned = d != NULL;
                wd = d;
                note_new = true; /* gcc retypes the implicit decl first */
                oldtype = newtype;
                *oldtypep = oldtype;
            } else if (nw->kind == CS_FUNC && func_err_param(c, newtype)) {
                /* a parameter that was already diagnosed matches anything */
                nw->ty = newtype = oldtype;
                *newtypep = newtype;
            } else if (nw->kind == CS_FUNC && volatile_ret_only(c, a, b)) {
                int k;
                /* comptypes runs three times (twice for a block-scope one) */
                for (k = 0; k < (cat_file_scope(c) ? 3 : 2); k++)
                    cpedwarn(c, cinput_loc(c, c->cd_ltok), "", "function "
                             "return types not compatible due to 'volatile'");
            } else {
                if (TYPE_QUALS(b) != TYPE_QUALS(a))
                    d = cerror_d(c, nw->loc, "conflicting type qualifiers for "
                                 "'%s'", sname(c, nw));
                else
                    d = cerror_d(c, nw->loc, "conflicting types for '%s'; have "
                                 "%s", sname(c, nw), type_q(TT, newtype));
                arglist_conflict(c, d, nw, o, newtype, oldtype);
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (nw->kind == CS_FUNC && type_tu_mixed(TT, a, b)) {
            int k;
            for (k = 0; k < 2; k++)
                cpedwarn(c, cinput_loc(c, c->cd_ltok), "pedantic", "function types "
                         "not truly compatible in ISO C");
        } else if (nw->kind != CS_TYPEDEF && enum_int_pair(c, a, b) &&
                   type_canon(TT, a) != type_canon(TT, b)) {
            enum_and_int = true;
            wd = cwarn_d(c, DL_WARNING, nw->loc, "enum-int-mismatch",
                         "conflicting types for '%s' due to enum/integer "
                         "mismatch; have %s", sname(c, nw),
                         type_q(TT, newtype));
        }
    }
    (void)enum_and_int;
    if (nw->kind == CS_TYPEDEF) {
        TypeId a = typedef_under(c, oldtype), b = typedef_under(c, newtype);
        if (type_canon(TT, a) != type_canon(TT, b)) {
            d = cerror_d(c, nw->loc, "redefinition of typedef '%s' with "
                         "different type", sname(c, nw));
            locate_old_decl(c, d, o);
            return false;
        }
        if (type_is_vm(TT, b)) {
            d = cerror_d(c, nw->loc, "redefinition of typedef '%s' with "
                         "variably modified type", sname(c, nw));
            locate_old_decl(c, d, o);
        } else {
            d = cped11(c, nw->loc, "redefinition of typedef '%s'",
                          sname(c, nw));
            locate_old_decl(c, d, o);
        }
        return true;
    } else if (nw->kind == CS_FUNC) {
        if (sym_defined(nw)) {
            /* an extern inline (gnu_inline) definition may be overridden */
            if (sym_defined(o) &&
                (!extern_inline(o) || extern_inline(nw) ||
                 !(o->flags & CSF_GNU_INLINE))) {
                d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (sym_defined(o) && !is_prototype(c, oldtype) &&
                   is_prototype(c, newtype) &&
                   type_ent(TT, type_canon(TT, oldtype))->n) {
            if (!validate_proto_after_old_defn(c, nw, o, &d)) {
                locate_old_decl(c, d, o);
                return false;
            }
        }
        if (sym_public(o) && !sym_public(nw)) {
            if (!extern_inline(o)) {
                d = cerror_d(c, nw->loc, "static declaration of '%s' follows "
                             "non-static declaration", sname(c, nw));
                locate_old_decl(c, d, o);
            }
            return false;
        } else if (sym_public(nw) && !sym_public(o)) {
            if (!ofile) {
                d = cerror_d(c, nw->loc, "non-static declaration of '%s' "
                             "follows static declaration", sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        }
    } else if (nw->kind == CS_OBJ && !(nw->flags & CSF_PARAM)) {
        if ((nw->flags & CSF_THREAD) != (o->flags & CSF_THREAD)) {
            if (nw->flags & CSF_THREAD)
                d = cerror_d(c, nw->loc, "thread-local declaration of '%s' "
                             "follows non-thread-local declaration",
                             sname(c, nw));
            else
                d = cerror_d(c, nw->loc, "non-thread-local declaration of "
                             "'%s' follows thread-local declaration",
                             sname(c, nw));
            locate_old_decl(c, d, o);
            return false;
        }
        if (sym_defined(nw) && sym_defined(o)) {
            d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
            locate_old_decl(c, d, o);
            return false;
        }
        if (nfile && sym_public(nw) != sym_public(o)) {
            if (sym_external(nw)) {
                if (!ofile) {
                    d = cerror_d(c, nw->loc, "extern declaration of '%s' "
                                 "follows declaration with no linkage",
                                 sname(c, nw));
                    locate_old_decl(c, d, o);
                    return false;
                }
            } else {
                if (sym_public(nw))
                    d = cerror_d(c, nw->loc, "non-static declaration of '%s' "
                                 "follows static declaration", sname(c, nw));
                else
                    d = cerror_d(c, nw->loc, "static declaration of '%s' "
                                 "follows non-static declaration",
                                 sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (!nfile) {
            if (sym_external(nw)) {
                /* extern with initializer at block scope: an error already */
            } else if (sym_external(o)) {
                d = cerror_d(c, nw->loc, "declaration of '%s' with no linkage "
                             "follows extern declaration", sname(c, nw));
                locate_old_decl(c, d, o);
            } else {
                d = cerror_d(c, nw->loc, "redeclaration of '%s' with no "
                             "linkage", sname(c, nw));
                locate_old_decl(c, d, o);
            }
            return false;
        }
    }
    if (nw->kind == CS_OBJ && (nw->flags & CSF_PARAM) &&
        !(o->flags & CSF_FWD)) {
        d = cerror_d(c, nw->loc, "redefinition of parameter '%s'",
                     sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    if (nw->kind == CS_FUNC && nw->parms && o->parms)
        cparm_compare(c, nw->parms, o->parms);
    if (!wd && !((nw->flags & CSF_PARAM) && (o->flags & CSF_FWD)) &&
        !(nw->kind == CS_FUNC && sym_defined(nw) && !sym_defined(o)) &&
        !(sym_external(o) && !sym_external(nw)) &&
        !(nw->kind == CS_OBJ && sym_defined(nw) && !sym_defined(o)))
        wd = cwarn_d(c, DL_WARNING, nw->loc, "redundant-decls", "redundant "
                     "redeclaration of '%s'", sname(c, nw));
    if (wd || pedwarned)
    {
        CSym tmp = *o;
        tmp.ty = newtype;
        locate_old_decl(c, wd, note_new ? &tmp : o);
    }
    return true;
}

/* handle_weak_attribute + declare_weak for a declaration of an object or
 * function: an inline function ignores it, a symbol without external linkage
 * cannot be weak. */
static void weak_apply(Checker *c, CSym *s, bool is_inline)
{
    if (s->kind == CS_FUNC && is_inline) {
        cwarn(c, s->loc, "attributes", "inline function '%s' declared weak",
              sname(c, s));
        return;
    }
    if (!sym_public(s)) {
        cerror(c, s->loc, "weak declaration of '%s' must be public",
               sname(c, s));
        return;
    }
    s->flags |= CSF_WEAK;
}

/* diagnose_mismatched_decls: an inline declaration after one with
 * noinline, or noinline after an inline one. */
static void inline_follows(Checker *c, const CSym *nw, uint32_t ltok,
                           uint32_t sn, uint32_t idecl)
{
    uint32_t ref = lookup_ord(c, nw->name);
    const CSym *o;
    Diagnostic *d = NULL;
    bool nw_noinline;
    unsigned k;
    if (ref == SYM_NONE || csym(c, ref)->kind != CS_FUNC)
        return;
    o = csym(c, ref);
    nw_noinline = attrs_item_named(c, sn, "noinline") ||
                  attrs_item_named(c, idecl, "noinline");
    for (k = 0; k < c->nign; k++)
        if (!strcmp(c->ign[k], "noinline"))
            nw_noinline = false;
    if ((nw->flags & CSF_INLINE) && !(o->flags & CSF_INLINE) &&
        cdecl_aset_has(c, o->aset, "noinline", NULL))
        d = cwarn_d(c, DL_WARNING, iloc(c, ltok), "attributes", "inline declaration "
                    "of '%s' follows declaration with attribute 'noinline'",
                    sname(c, nw));
    else if ((o->flags & CSF_INLINE) && nw_noinline)
        d = cwarn_d(c, DL_WARNING, nw->loc, "attributes", "declaration of "
                    "'%s' with attribute 'noinline' follows inline "
                    "declaration", sname(c, nw));
    locate_old_decl(c, d, o);
}

/* merge_decls: nw is consistent with o; o becomes the merged declaration. */
static void merge_decls(Checker *c, CSym *nw, CSym *o, TypeId newtype,
                        TypeId oldtype)
{
    bool is_fn = nw->kind == CS_FUNC;
    bool new_def = is_fn && sym_defined(nw);
    bool new_proto = is_fn && is_prototype(c, nw->ty);
    bool old_proto = o->kind == CS_FUNC && is_prototype(c, o->ty);
    bool ext_new = sym_external(nw), pub, ext_final, static_final;
    bool infunc = c->func_sym != SYM_NONE;
    CSym m = *nw;
    if (nw->kind == CS_TYPEDEF)
        m.ty = o->ty;
    else
        m.ty = type_composite(TT, newtype, oldtype);
    if ((!sym_defined(nw) && sym_defined(o)) || (old_proto && !new_proto))
        m.loc = o->loc;
    m.flags |= o->flags & (CSF_DEFINED | CSF_USED | CSF_NORETURN | CSF_THREAD |
                           CSF_INLINE | CSF_BLOCK_EXTERN | CSF_TENTATIVE |
                           CSF_WEAK | CSF_ADDR_WARNED | CSF_DEPRECATED |
                           CSF_UNAVAILABLE | CSF_INNER_COMP | CSF_GNU_INLINE | CSF_PURE | CSF_CONSTFN);
    /* merge_weak: PR 49899, a static function cannot become weak and public */
    if ((nw->flags & CSF_WEAK) && !(o->flags & CSF_WEAK) && !sym_public(o) &&
        sym_public(nw))
        cerror(c, m.loc, "weak declaration of '%s' being applied to a already "
               "existing, static definition", sname(c, nw));
    if (!m.dep_msg)
        m.dep_msg = o->dep_msg;
    if (o->sect && nw->sect &&
        strcmp(c->dep_msgs.data[o->sect - 1], c->dep_msgs.data[nw->sect - 1]))
        cwarn(c, iloc(c, c->cd_ltok), "attributes", "ignoring attribute "
              "'section (\"%s\")' because it conflicts with previous "
              "'section (\"%s\")'", c->dep_msgs.data[nw->sect - 1],
              c->dep_msgs.data[o->sect - 1]);
    if (o->sect)
        m.sect = o->sect;
    m.nonnull |= o->nonnull;
    if (o->fmt)
        m.fmt = o->fmt;
    if (o->aset)
        m.aset = o->aset;
    if (o->parms) {
        m.parms = o->parms;
        if (nw->parms != o->parms)
            cparm_release(c, nw->parms);
    }
    if (!new_def)
        m.flags |= o->flags & (CSF_PROTO_DEF | CSF_KR_DEF);
    m.def_loc = sym_defined(nw) ? nw->loc : o->def_loc;
    if (sym_defined(nw) && !nw->def_loc)
        m.def_loc = nw->loc;
    m.flags &= ~(unsigned)CSF_IMPLICIT;
    if (o->align > m.align)
        m.align = o->align;
    if (o->ualign > m.ualign)
        m.ualign = o->ualign;
    if (is_fn && (nw->flags & CSF_INLINE || o->flags & CSF_INLINE) &&
        !((nw->flags | o->flags) & CSF_GNU_INLINE) &&
        (!(nw->flags & CSF_INLINE) || !(o->flags & CSF_INLINE) ||
         !sym_external(o)) &&
        ext_new && !infunc)
        ext_new = false;
    if (new_def && ((nw->flags | o->flags) & CSF_INLINE) && !sym_public(o))
        ext_new = false;
    if (is_fn)
        pub = sym_public(nw) && sym_public(o);
    else
        pub = ext_new ? sym_public(o) : sym_public(nw);
    if (ext_new) {
        ext_final = sym_external(o);
        static_final = (o->flags & CSF_TREE_STATIC) != 0;
    } else {
        ext_final = false;
        static_final = (nw->flags & CSF_TREE_STATIC) != 0;
    }
    m.flags &= ~(unsigned)(CSF_DECL_EXTERNAL | CSF_TREE_STATIC);
    if (ext_final)
        m.flags |= CSF_DECL_EXTERNAL;
    if (static_final)
        m.flags |= CSF_TREE_STATIC;
    if (pub)
        m.linkage = LK_EXTERNAL;
    else {
        uint8_t l = ext_new ? o->linkage : nw->linkage;
        if (o->linkage == LK_INTERNAL || nw->linkage == LK_INTERNAL)
            l = LK_INTERNAL;
        m.linkage = l == LK_EXTERNAL ? LK_INTERNAL : l;
    }
    if (nw->kind == CS_TYPEDEF)
        m.linkage = LK_NONE;
    *o = m;
}

static bool duplicate_decls(Checker *c, CSym *nw, bool nfile, uint32_t oldref,
                            bool implicit_int)
{
    TypeId nt, ot;
    CSym *o = csym(c, oldref);
    if (!diagnose_mismatched(c, nw, nfile, o, ref_file_scope(oldref),
                             implicit_int, &nt, &ot))
        return false;
    merge_decls(c, nw, o, nt, ot);
    return true;
}

static bool fn_pointer_type(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_PTR && is_func(c, type_base(TT, t));
}

/* The option of a parameter/local shadow warning: -Wshadow itself, else
 * -Wshadow=compatible-local when the types are compatible, else
 * -Wshadow=local. */
static const char *shadow_id(Checker *c, const CSym *old, const CSym *x)
{
    if (diag_enabled(c->diag, "shadow"))
        return "shadow";
    return type_compatible(TT, old->ty, x->ty) ? "shadow=compatible-local"
                                               : "shadow=local";
}

static void warn_if_shadowing(Checker *c, const CSym *x)
{
    uint32_t bi;
    if (x->name >= c->nidents)
        return;
    for (bi = c->top[NS_ORD][x->name]; bi; bi = c->log.data[bi - 1].prev) {
        uint32_t ref = c->log.data[bi - 1].ref;
        const CSym *old = csym(c, ref);
        Diagnostic *d = NULL;
        if (old == x)
            continue;
        if (old->flags & CSF_ERROR)
            continue;
        if (old->flags & CSF_PARAM)
            d = cwarn_d(c, DL_WARNING, x->loc, shadow_id(c, old, x),
                        "declaration of '%s' shadows a parameter",
                        sname(c, x));
        else if (ref_file_scope(ref)) {
            if (old->kind == CS_FUNC && x->kind != CS_FUNC &&
                !fn_pointer_type(c, x->ty))
                continue;
            d = cwarn_d(c, DL_WARNING, x->loc, "shadow", "declaration of '%s' "
                        "shadows a global declaration", sname(c, x));
        } else
            d = cwarn_d(c, DL_WARNING, x->loc, shadow_id(c, old, x),
                        "declaration of '%s' shadows a previous local",
                        sname(c, x));
        if (d)
            cnote(c, d, old->loc, "shadowed declaration is here");
        break;
    }
}

/* The enclosing bindings of name when a block-scope external declaration
 * of type newty arrives: the first one of a file-scope entity keeps the type
 * it has now, and an incomplete array at file scope completed here (other
 * than to one element) is diagnosed at the end of the file. */
static void outer_bindings(Checker *c, uint32_t name, TypeId newty)
{
    uint32_t bi = c->top[NS_ORD][name];
    bool saved = false;
    TypeId vt = 0;
    bool have_vt = false;
    const TypeEnt *ne = type_ent(TT, type_canon(TT, newty));
    for (; bi; bi = c->log.data[bi - 1].prev) {
        Bind *bd = &c->log.data[bi - 1];
        CSym *s = csym(c, bd->ref);
        bool var_fn = (s->kind == CS_OBJ && !(s->flags & CSF_PARAM)) ||
                      s->kind == CS_FUNC;
        if (!have_vt) {
            vt = bd->ty ? bd->ty - 1 : s->ty;
            have_vt = true;
        }
        if (!var_fn || !ref_file_scope(bd->ref))
            continue;
        if (!saved && !bd->ty) {
            bd->ty = s->ty + 1;
            saved = true;
        } else if (!saved) {
            saved = true;
        }
        if (cbind_scope(c, bi) == SCK_FILE && s->kind == CS_OBJ &&
            (s->flags & CSF_TREE_STATIC) && type_ckind(TT, vt) == TY_ARRAY &&
            !type_is_complete(TT, vt) && ne->kind == TY_ARRAY &&
            !(ne->flags & TF_INCOMPLETE) && ne->n != 1)
            s->flags |= CSF_INNER_COMP;
    }
}

/* The type the innermost binding of name gives symbol s (gcc keeps it per
 * binding in c_binding.u.type; the symbol's own type is the composite of
 * all its declarations). */
static TypeId bound_type(Checker *c, uint32_t name, const CSym *s)
{
    uint32_t t = cbind_type(c, name);
    return t ? t - 1 : s->ty;
}

/* A block-scope external declaration of type newty was merged into symbol
 * ref: the binding it made (bi, an index + 1 into the log) sees only the
 * composite of the type visible before (vt, or none) and newty. */
static void bind_this_type(Checker *c, uint32_t bi, uint32_t ref, TypeId vt,
                           bool have_vt, TypeId newty)
{
    TypeId all = csym(c, ref)->ty, t = all;
    if (!have_vt)
        t = newty;
    else if (!is_err(c, vt) && type_compatible(TT, vt, newty))
        t = type_composite(TT, vt, newty);
    if (t != all || c->log.data[bi - 1].ty)
        c->log.data[bi - 1].ty = t + 1;
}

/* pushdecl: enters x in the current scope, merging it with an earlier
 * declaration of the same entity.  Returns the symbol the name now denotes. */
static uint32_t pushdecl(Checker *c, const CSym *xin, bool implicit_int)
{
    CSym x = *xin;
    uint32_t name = x.name, b, ref;
    bool filescope = cat_file_scope(c);
    bool varfn = (x.kind == CS_OBJ && !(x.flags & CSF_PARAM)) ||
                 x.kind == CS_FUNC;
    bool nfile = c->func_sym == SYM_NONE ||
                 (varfn && sym_public(&x) && !sym_defined(&x));
    bool global = nfile && !(x.flags & CSF_PARAM);
    bool skip = false, pub = sym_public(&x);
    if (!name) {
        if (x.kind == CS_TYPEDEF)
            x.ty = type_typedef(TT, 0, x.ty);
        return csym_new(c, global, &x);
    }
    if (!filescope && varfn && pub)
        x.flags |= CSF_BLOCK_EXTERN;
    csum_decl(c, name, filescope, varfn && pub);
    b = cbound_here(c, NS_ORD, name);
    if (b) {
        uint32_t vis = c->log.data[b - 1].ref, use = vis;
        bool split = varfn && pub && sym_public(csym(c, vis)) && c->ext[name];
        TypeId vt = bound_type(c, name, csym(c, vis)), newty = x.ty;
        if (split)
            use = c->ext[name] - 1;
        if (x.kind == CS_OBJ && type_ckind(TT, newty) == TY_ARRAY &&
            type_is_complete(TT, newty))
            csym(c, vis)->flags &= ~(unsigned)CSF_INNER_COMP;
        if (duplicate_decls(c, &x, nfile, use, implicit_int)) {
            if (split && !is_err(c, newty))
                bind_this_type(c, b, use, vt, true, newty);
            return use;
        }
        if (x.kind == CS_TYPEDEF && csym(c, vis)->kind == CS_OBJ)
            return vis;     /* gcc keeps the variable: the name stays bound to it */
        skip = true;
    }
    ref = SYM_NONE;
    if (!skip) {
        if (((x.flags & CSF_DECL_EXTERNAL) || filescope) && varfn) {
            uint32_t tb = c->top[NS_ORD][name], visref = SYM_NONE;
            uint32_t e = c->ext[name];
            if (tb) {
                uint32_t r = c->log.data[tb - 1].ref;
                const CSym *vs = csym(c, r);
                if ((vs->kind == CS_FUNC ||
                     (vs->kind == CS_OBJ && !(vs->flags & CSF_PARAM))) &&
                    ref_file_scope(r))
                    visref = r;
            }
            TypeId newty = x.ty;
            TypeId vt = visref != SYM_NONE ? bound_type(c, name, csym(c, visref))
                                           : 0;
            if (!filescope)
                cwarn(c, x.loc, "nested-externs", "nested extern declaration "
                      "of '%s'", sname(c, &x));
            if (e && !filescope && !is_err(c, newty))
                outer_bindings(c, name, newty);
            if (e && duplicate_decls(c, &x, nfile, e - 1, implicit_int)) {
                cbind(c, NS_ORD, name, e - 1);
                if (!filescope && !is_err(c, newty))
                    bind_this_type(c, (uint32_t)c->log.len, e - 1, vt,
                                   visref != SYM_NONE, newty);
                return e - 1;
            } else if (pub) {
                if (visref != SYM_NONE && !e &&
                    duplicate_decls(c, &x, nfile, visref, implicit_int)) {
                    cbind(c, NS_ORD, name, visref);
                    return visref;
                }
                if (x.kind == CS_FUNC && !e)
                    cexpr_builtin_decl(c, &x);
                ref = csym_new(c, global, &x);
                c->ext[name] = (ref & ~SYM_LOCAL) + 1;
                if (ref & SYM_LOCAL)
                    c->ext[name] = 0;
            }
        }
        if (!(x.flags & CSF_PARAM))
            warn_if_shadowing(c, &x);
    }
    if (ref == SYM_NONE) {
        if (x.kind == CS_TYPEDEF) {
            SrcFile *sf = srcmgr_file_of(c->sm, x.loc);
            x.ty = type_typedef(TT, name, x.ty);
            if (sf && sf->system_header)
                TT->ents.data[TYPE_IDX(x.ty)].flags |= TF_SYSHDR;
        }
        ref = csym_new(c, global, &x);
    }
    cbind(c, NS_ORD, name, ref);
    return ref;
}

/* ---- the specifier stack, small helpers ------------------------------------ */

static int find_spec(Checker *c, uint32_t specs_node)
{
    size_t k = c->specs.len;
    while (k-- > 0)
        if (c->specs.data[k].node == specs_node)
            return (int)k;
    return -1;
}

/* Drops the specifiers of the consumer node and of what it contains. */
static void pop_specs(Checker *c, uint32_t consumer)
{
    uint32_t f = cfirst(c, consumer);
    while (c->specs.len && vec_last(&c->specs).node >= f)
        c->specs.len--;
}

/* pending_xref_error */
static void pending_xref(Checker *c, Spec *sp)
{
    if (sp->xref_name) {
        cerror(c, sp->xref_loc, "'%s' defined as wrong kind of tag",
               cident(c, sp->xref_name));
        sp->xref_name = 0;
    }
}

/* The token that ends a declarator's text for the parser's lookahead: the
 * first ',' ';' (or '=' with eq, '}' or a closing bracket at depth 0) from
 * the token tok. */
static uint32_t scan_end(Checker *c, uint32_t tok, bool eq)
{
    int depth = 0;
    uint32_t k;
    for (k = tok; k < c->u->ntoks; k++)
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE:
            depth++;
            break;
        case P_RPAREN: case P_RBRACKET:
            if (depth > 0)
                depth--;
            else if (eq)
                break;
            else
                return k;
            break;
        case P_RBRACE:
            if (depth > 0)
                depth--;
            else
                return k;
            break;
        case P_COMMA: case P_SEMI:
            if (depth == 0)
                return k;
            break;
        case P_ASSIGN:
            if (depth == 0 && eq)
                return k;
            break;
        default:
            break;
        }
    return c->u->ntoks ? c->u->ntoks - 1 : 0;
}

static bool is_incomplete_array(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ARRAY &&
           (type_ent(TT, type_canon(TT, t))->flags & TF_INCOMPLETE);
}

/* --dump-types: one line per declaration (see the header). */
static void dump_decl(Checker *c, const CSym *s)
{
    StrBuf sb;
    FILE *f = c->opt.dump;
    const char *pre = "";
    if (!f || c->quiet || !s->name || (s->flags & CSF_PARAM))
        return;
    memset(&sb, 0, sizeof sb);
    if (!cat_file_scope(c) && c->func_sym != SYM_NONE) {
        sb_printf(&sb, "%s:", cident(c, csym(c, c->func_sym)->name));
        pre = sb_cstr(&sb);
    }
    switch (s->kind) {
    case CS_TYPEDEF: {
        StrBuf t;
        memset(&t, 0, sizeof t);
        type_print(TT, &t, typedef_under(c, s->ty));
        fprintf(f, "typedef %s%s = %s\n", pre, sname(c, s), sb_cstr(&t));
        sb_free(&t);
        break;
    }
    case CS_OBJ: {
        StrBuf t;
        memset(&t, 0, sizeof t);
        type_print(TT, &t, s->ty);
        fprintf(f, "var %s%s: %s%s%s%s\n", pre, sname(c, s), sb_cstr(&t),
                s->sc == SC_STATIC ? " static" : "",
                (s->flags & CSF_DECL_EXTERNAL) ? " extern" : "",
                (s->flags & CSF_TENTATIVE) ? " tentative" : "");
        sb_free(&t);
        break;
    }
    case CS_FUNC: {
        StrBuf t;
        memset(&t, 0, sizeof t);
        type_print(TT, &t, s->ty);
        fprintf(f, "func %s%s: %s%s%s%s%s\n", pre, sname(c, s), sb_cstr(&t),
                s->sc == SC_STATIC ? " static" : "",
                s->sc == SC_EXTERN ? " extern" : "",
                (s->flags & CSF_INLINE) ? " inline" : "",
                (s->flags & CSF_DEFINED) ? " defined" : "");
        sb_free(&t);
        break;
    }
    default:
        break;
    }
    sb_free(&sb);
}

/* Which declarations must reach cdecl_finish_object: the first call does the
 * end-of-translation-unit checks, so any declaration will do as its cue. */
static void queue_object(Checker *c, uint32_t ref)
{
    size_t k;
    for (k = 0; k < c->tentative.len; k++)
        if (c->tentative.data[k] == ref)
            return;
    vec_push(&c->tentative, ref);
}

static void ensure_finish_cue(Checker *c, uint32_t ref)
{
    if (!c->tentative.len)
        vec_push(&c->tentative, ref);
}

/* ---- array sizes from initializers (complete_array_type) --------------------- */

static uint32_t strip_parens(Checker *c, uint32_t e)
{
    while (e != NO_NODE && ntag(c, e) == N_PAREN)
        e = first_child(c, e);
    return e;
}

/* complete_array_type for the initializer init (NO_NODE: none); returns
 * gcc's failure code, the completed array type in *out. */
static int complete_array(Checker *c, TypeId type, uint32_t init,
                          bool do_default, TypeId *out)
{
    TypeId cn = type_canon(TT, type), elem = type_base(TT, cn);
    int failure = 0;
    int64_t n = 0;
    *out = type;
    if (init != NO_NODE) {
        uint32_t e = strip_parens(c, init);
        while (e != NO_NODE && cexpr_is_extension(c, e))
            e = strip_parens(c, first_child(c, e));
        if (ntag(c, init) == N_INIT_LIST) {
            /* cinit.c left the element count in cv */
            if (c->ck[init] == K_ICE) {
                n = (int64_t)c->cv[init];
                if (n == 0 && c->opt.pedantic)
                    failure = 3;
            } else
                n = 1;
        } else if (ntag(c, e) == N_STRING) {
            bool o1, o2;
            uint64_t bytes = type_size(TT, c->ty[e], &o1);
            uint64_t es = type_size(TT, elem, &o2);
            if (o1 && o2 && es)
                n = (int64_t)(bytes / es);
            else
                n = 1;
        } else if (c->ck[init] != K_ERR && !is_err(c, c->ty[init]) &&
                   ntag(c, e) == N_COMPOUND_LIT &&
                   tkind(c, c->ty[e]) == TY_ARRAY &&
                   !is_incomplete_array(c, c->ty[e])) {
            /* the literal is replaced by its constructor */
            n = (int64_t)type_ent(TT, type_canon(TT, c->ty[e]))->n;
        } else {
            if (!(c->ck[init] == K_ERR || is_err(c, c->ty[init])))
                failure = 1;
            n = 1;
        }
    } else {
        failure = 2;
        if (!do_default)
            return failure;
        n = 1;
    }
    *out = type_array(TT, elem, (uint64_t)n);
    return failure;
}

/* ---- shadow_tag: declarations without declarators ---------------------------- */

static void shadow_tag(Checker *c, Spec *sp, int warned, uint32_t ltok)
{
    bool file = cat_file_scope(c);
    bool tagged = sp->kind == TSK_TAGDEF || sp->kind == TSK_TAGFIRSTREF;
    bool anyq = sp->quals != 0;
    SrcLoc il = iloc(c, ltok);
    TypeKind tk = tkind(c, sp->ty);
    if (!sp->default_int && sp->kind != TSK_TYPEDEF) {
        if (!sp->error && (tk == TY_STRUCT || tk == TY_UNION || tk == TY_ENUM)) {
            uint32_t name;
            if (tk == TY_ENUM)
                name = type_enum(TT, sp->ty)->tag;
            else
                name = type_record(TT, sp->ty)->tag;
            if (sp->restrict_q) {
                cerror(c, il, "invalid use of 'restrict'");
                warned = 1;
            }
            if (!name) {
                if (warned != 1 && tk != TY_ENUM) {
                    cpedwarn(c, il, "", "unnamed struct/union that defines no "
                             "instances");
                    warned = 1;
                }
            } else if (!tagged && sp->sc != SC_NONE) {
                if (warned != 1)
                    cpedwarn(c, il, "", "empty declaration with storage class "
                             "specifier does not redeclare tag");
                warned = 1;
                pending_xref(c, sp);
            } else if (!tagged && anyq) {
                if (warned != 1)
                    cpedwarn(c, il, "", "empty declaration with type qualifier "
                             "does not redeclare tag");
                warned = 1;
                pending_xref(c, sp);
            } else if (!tagged && sp->alignas_seen) {
                if (warned != 1)
                    cpedwarn(c, il, "", "empty declaration with '_Alignas' "
                             "does not redeclare tag");
                warned = 1;
                pending_xref(c, sp);
            } else if (!tagged && tk == TY_ENUM) {
                if (warned != 1 &&
                    cpedantic(c, il, "empty declaration of 'enum' type does "
                              "not redeclare tag"))
                    warned = 1;
                pending_xref(c, sp);
            } else {
                sp->xref_name = 0;
                if (!cbound_here(c, NS_TAG, name)) {
                    TypeId t = tk == TY_ENUM
                        ? type_new_enum(TT, name, il)
                        : type_new_record(TT, name, tk == TY_UNION, il);
                    cbind(c, NS_TAG, name, t);
                }
            }
        } else if (warned != 1) {
            cpedwarn(c, il, "", "useless type name in empty declaration");
            warned = 1;
        }
    } else if (warned != 1 && sp->kind == TSK_TYPEDEF) {
        cpedwarn(c, il, "", "useless type name in empty declaration");
        warned = 1;
    }
    sp->xref_name = 0;
    if (sp->is_inline) {
        cerror(c, il, "'inline' in empty declaration");
        warned = 1;
    }
    if (sp->is_noreturn) {
        cerror(c, il, "'_Noreturn' in empty declaration");
        warned = 1;
    }
    if (file && sp->sc == SC_AUTO) {
        cerror(c, il, "'auto' in file-scope empty declaration");
        warned = 1;
    }
    if (file && sp->sc == SC_REGISTER) {
        cerror(c, il, "'register' in file-scope empty declaration");
        warned = 1;
    }
    if (!warned && sp->sc != SC_NONE) {
        cwarn(c, il, "", "useless storage class specifier in empty "
              "declaration");
        warned = 2;
    }
    if (!warned && sp->thread) {
        cwarn(c, il, "", "useless '%s' in empty declaration",
              sp->thread_gnu ? "__thread" : "_Thread_local");
        warned = 2;
    }
    if (!warned && anyq) {
        cwarn(c, il, "", "useless type qualifier in empty declaration");
        warned = 2;
    }
    if (!warned && sp->alignas_seen) {
        cwarn(c, il, "", "useless '_Alignas' in empty declaration");
        warned = 2;
    }

}

/* ---- DECL, DECLARED, INIT_DECL ----------------------------------------------- */

static uint32_t find_declared(Checker *c, uint32_t idecl)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, idecl, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_DECLARED)
            r = k.p[j];
    kids_free(&k);
    return r;
}

static void funcdef_declared(Checker *c, uint32_t declared);
static uint32_t funcdef_fnode(Checker *c, uint32_t top);
static uint32_t find_child(Checker *c, uint32_t i, unsigned tag);

static void decl_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i);
    if ((cnode(c, i)->flags & (NF_NESTED | NF_ERROR)) == (NF_NESTED | NF_ERROR) &&
        !in_extension(c, i))
        cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C forbids nested "
                  "functions");
    if (sn != NO_NODE && ntag(c, sn) == N_SPECS &&
        c->nodes[i].size == c->nodes[sn].size + 1) {
        int si = find_spec(c, sn);
        if (si >= 0) {
            Spec sp = c->specs.data[si];
            shadow_tag(c, &sp, 0, sp.tok1);
        }
    }
    pop_specs(c, i);
}

/* Attributes given after the declarator. */
static void decl_attrs(Checker *c, uint32_t idecl, Attrs *a)
{
    memset(a, 0, sizeof *a);
    attrs_of_children(c, idecl, a);
}

static uint32_t last_enumerator(Checker *c, uint32_t n)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, n, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ENUMERATOR)
            r = k.p[j];
    kids_free(&k);
    return r;
}

/* A declarator whose specifiers define a deprecated/unavailable tag uses the
 * type: gcc warns at the tag (anonymous struct: its '{', enum: its first
 * enumerator) once per declarator. */
static void dep_spec_use(Checker *c, const Spec *sp)
{
    uint32_t n = sp->tag_node, tag, tok;
    TypeId t;
    SrcLoc loc, note;
    uint32_t dep, dmsg;
    bool isenum;
    if (n == NO_NODE || (ntag(c, n) != N_STRUCT && ntag(c, n) != N_ENUM) ||
        !(cnode(c, n)->flags & NF_BODY))
        return;
    t = type_canon(TT, c->ty[n]);
    isenum = type_ckind(TT, t) == TY_ENUM;
    if (isenum) {
        const Enum *e = type_enum(TT, t);
        if (!e)
            return;
        dep = e->dep;
        dmsg = e->dmsg;
    } else {
        const Record *r = type_record(TT, t);
        if (!r)
            return;
        dep = r->dep;
        dmsg = r->dmsg;
    }
    if (!(dep & (CSF_DEPRECATED | CSF_UNAVAILABLE)))
        return;
    tag = find_child(c, n, N_TAG);
    for (tok = cnode(c, n)->tok; tpunct(c, tok) != P_LBRACE; tok++)
        ;
    if (tag != NO_NODE) {
        loc = note = tloc(c, cnode(c, tag)->tok);
        if (isenum) {
            uint32_t en = last_enumerator(c, n);
            if (en != NO_NODE)
                loc = tloc(c, cnode(c, en)->tok);
        }
    } else if (isenum) {
        uint32_t en = last_enumerator(c, n);
        note = tloc(c, tok);
        loc = en != NO_NODE ? tloc(c, cnode(c, en)->tok) : note;
    } else {
        loc = tloc(c, tok);
        note = tloc(c, cnode(c, n)->tok);
    }
    cdep_named(c, loc, tag != NO_NODE ? cident(c, cnode_ident(c, tag)) : NULL,
               dep, dmsg, &note);
}

static void declared_visit(Checker *c, uint32_t i)
{
    uint32_t idecl = c->par[i], decl, sn, top, name_tok, end, ltok, ref;
    int si;
    Spec sp;
    bool initialized, kr, file, incomp_init = false;
    GDecl g;
    CSym s;
    Attrs a;
    SrcLoc il;
    if (ntag(c, idecl) == N_FUNC_DEF) {
        funcdef_declared(c, i);
        return;
    }
    decl = c->par[idecl];
    sn = first_child(c, decl);
    top = first_child(c, idecl);
    kr = c->par[decl] != NO_NODE && ntag(c, c->par[decl]) == N_FUNC_DEF;
    si = find_spec(c, sn);
    if (si < 0 || ntag(c, sn) != N_SPECS)
        return;
    if (c->nodes[sn].size == 1 && cfirst(c, idecl) == sn + 1)
        cpedwarn(c, tloc(c, c->specs.data[si].tok0), "", "data definition has "
                 "no type or storage class");
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    dep_spec_use(c, &sp);
    name_tok = cnode(c, idecl)->tok;
    end = scan_end(c, name_tok, true);
    initialized = tpunct(c, end) == P_ASSIGN;
    ltok = initialized ? end + 1 : end;
    il = iloc(c, ltok);
    file = cat_file_scope(c);
    c->cd_ltok = ltok;
    grok(c, &sp, top, kr ? DC_PARM : DC_NORMAL, false, initialized, NO_NODE,
         ltok, ltok, &g);
    if (g.what == GD_NONE)
        return;
    s = g.s;
    if (s.kind == CS_OBJ)
        larger_than(c, s.loc, s.name, s.ty);
    c->attr_fty = s.kind == CS_FUNC ? type_canon(TT, s.ty) : 0;
    decl_attrs(c, idecl, &a);
    c->attr_fty = 0;
    if (a.has_mode || a.vs_seen) {
        s.ty = attr_apply_type(c, s.ty, &a);
        g.ty = s.ty;
    }
    attrs_unknown_emit(c, &sp.attrs, ltok);
    if (g.what == GD_FUNC && s.kind == CS_FUNC) {
        /* the declared type keeps the typedef names of the parameters */
        TypeId aft = type_kind(TT, s.ty) == TY_FUNC ? s.ty : type_canon(TT, s.ty);
        attrs_alloc_check(c, sn, aft, ltok);
        attrs_alloc_check(c, idecl, aft, ltok);
    }
    if (s.kind == CS_OBJ && type_ckind(TT, s.ty) == TY_PTR &&
        type_ckind(TT, type_base(TT, s.ty)) == TY_FUNC) {
        /* a pointer to function: the attributes describe the function */
        attrs_alloc_check(c, sn, type_base(TT, s.ty), ltok);
        attrs_alloc_check(c, idecl, type_base(TT, s.ty), ltok);
    }
    if (s.kind == CS_OBJ) {
        strict_flex_check(c, sn, false, s.ty, s.name, s.loc, sp.tok0);
        strict_flex_check(c, idecl, false, s.ty, s.name, s.loc, NO_NODE);
    }
    if (s.kind == CS_OBJ && !(type_ckind(TT, s.ty) == TY_PTR &&
        type_ckind(TT, type_base(TT, s.ty)) == TY_FUNC) &&
        (attrs_item_named(c, sn, "assume_aligned") ||
         attrs_item_named(c, idecl, "assume_aligned"))) {
        cwarn(c, iloc(c, ltok), "attributes", "'assume_aligned' attribute only "
              "applies to function types");
    }
    bool ign_packed = false, ign_aligned = false;
    if (s.kind == CS_FUNC || s.kind == CS_OBJ) {
        AttrState st = {0};
        uint32_t h = idecl;
        attrs_copy_check(c, sn, s.kind, s.name, ltok, &st);
        for (;;) {       /* attributes after a '*' belong to the declaration */
            Kids hk;
            uint32_t m, nx = NO_NODE;
            kids_get(c, h, &hk);
            for (m = 0; m < hk.n && nx == NO_NODE; m++)
                if (ntag(c, hk.p[m]) == N_PTR)
                    nx = hk.p[m];
            kids_free(&hk);
            if (nx == NO_NODE)
                break;
            attrs_copy_check(c, nx, s.kind, s.name, ltok, &st);
            h = nx;
        }
        attrs_copy_check(c, idecl, s.kind, s.name, ltok, &st);
        c->last_ualign = st.calign;
        ign_packed = st.ign_packed;
        ign_aligned = st.ign_aligned;
        if (s.kind == CS_FUNC) {
            s.flags |= (st.pure ? CSF_PURE : 0) | (st.cnst ? CSF_CONSTFN : 0);
            a.nonnull |= st.nonnull;
        }
    }
    attrs_merge(&a, &sp.attrs);
    if (ign_packed)
        a.packed = false;
    if (ign_aligned)
        a.aligned = 0;
    if (a.desig && !(g.what == GD_TYPEDEF && type_ckind(TT, s.ty) == TY_STRUCT))
        cerror(c, iloc(c, ltok), "'designated_init' attribute is only valid on "
               "'struct' type");
    attrs_misapplied(c, &a, kr ? 'p' : g.what == GD_TYPEDEF ? 't' :
                     g.what == GD_FUNC ? 'f' :
                     file ? 'g' : s.sc == SC_STATIC ? 's' :
                     s.sc == SC_EXTERN ? 'g' : 'a', !file, 0, ltok);
    attrs_section_check(c, &a, kr ? 'p' : g.what == GD_TYPEDEF ? 't' :
                        g.what == GD_FUNC ? 'f' : 'g',
                        g.what == GD_VAR && !file && s.sc != SC_STATIC,
                        s.name, s.loc);
    attrs_zcur_check(c, &a, g.what == GD_FUNC && !kr, s.loc);
    attrs_wina_check(c, &a, kr ? 'p' : g.what == GD_TYPEDEF ? 't' : 'g', false,
                     s.name, s.loc);
    s.sect = a.sec;
    if (s.kind == CS_OBJ && !file && (s.flags & CSF_TREE_STATIC)) {
        TypeId et = s.ty;
        while (is_arr(c, et))
            et = type_base(TT, type_canon(TT, et));
        if (!(TYPE_QUALS(et) & TQ_CONST))
            cdecl_record_inline_static(c, s.loc, s.name, true);
    }
    if (a.cleanup && a.cleanup_arg && s.kind == CS_OBJ && !file &&
        s.sc != SC_STATIC && s.sc != SC_EXTERN && !kr) {
        /* handle_cleanup_attribute: a function taking the variable's address;
         * it counts as a use of the variable */
        uint32_t ca = a.cleanup_arg, fr = SYM_NONE;
        if (ntag(c, ca) == N_IDENT)
            fr = lookup_ord(c, cnode_ident(c, ca));
        if (ntag(c, ca) != N_IDENT)
            cerror(c, il, "cleanup argument not an identifier");
        else if (fr == SYM_NONE || csym(c, fr)->kind != CS_FUNC)
            cerror(c, il, "cleanup argument not a function");
        else {
            csym(c, fr)->flags |= CSF_USED;
            cexpr_cleanup_call(c, fr, s.ty, s.loc, il);
        }
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;   /* never warned about */
    }
    if (a.transparent_union && g.what == GD_TYPEDEF &&
        type_ckind(TT, s.ty) == TY_UNION) {
        /* handle_transparent_union_attribute on a typedef: the union type
         * itself becomes transparent when its first member fits */
        Record *r = type_record(TT, s.ty);
        if (r && (r->flags & RF_COMPLETE) && r->nfields) {
            const Field *fl = &TT->fields.data[r->fields];
            bool sok;
            uint64_t sz = type_size(TT, fl->ty, &sok);
            if (sok && sz == r->size && !(fl->flags & FF_BITFIELD))
                r->flags |= RF_TRANSPARENT;
            else
                cwarn(c, s.loc, "attributes", "'transparent_union' attribute "
                      "ignored");
        } else if (r && !(r->flags & RF_COMPLETE))
            r->flags |= RF_TRANSPARENT;
    }
    if (a.aligned > s.align)
        s.align = (uint16_t)a.aligned;
    if (a.unused)
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;
    if (a.deprecated || a.unavailable) {
        s.flags |= a.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
        s.dep_msg = a.dep_msg;
    }
    if (a.e_wi)
        cerror(c, s.loc, "weak '%s' cannot be defined 'ifunc'", sname(c, &s));
    if (a.e_iw)
        cerror(c, s.loc, "indirect function '%s' cannot be declared weak",
               sname(c, &s));
    if (a.weakref && (s.kind == CS_OBJ || s.kind == CS_FUNC) && sym_public(&s))
        cerror(c, s.loc, "'weakref' symbol '%s' must have static linkage",
               sname(c, &s));
    if (a.weak && !kr && (s.kind == CS_OBJ || s.kind == CS_FUNC))
        weak_apply(c, &s, sp.is_inline);
    if (a.weakref && (s.kind == CS_OBJ || s.kind == CS_FUNC))
        s.flags |= CSF_WEAK;   /* a weakref is weak too */
    /* handle_alias_ifunc_attributes: a function alias has an initial value, so
     * a later definition (or an earlier one) is a redefinition */
    if (a.defn && s.kind == CS_FUNC && g.what == GD_FUNC)
        s.flags |= CSF_DEFINED;
    if (a.noreturn && s.kind == CS_FUNC)
        s.flags |= CSF_NORETURN;
    if (a.nonnull && s.kind == CS_FUNC)
        s.nonnull = a.nonnull;
    if (a.fmt && s.kind == CS_FUNC)
        s.fmt = a.fmt;
    if (g.what == GD_FUNC && s.kind == CS_FUNC && !kr)
        s.parms = cparm_make(c, funcdef_fnode(c, top));
    if (file && g.what == GD_VAR && s.sc == SC_REGISTER &&
        find_child(c, idecl, N_ASM_LABEL) != NO_NODE)
        s.flags |= CSF_REGISTER_NAMED;
    if (g.what == GD_VAR && g.name &&
        !strcmp(cident(c, g.name), "main") && sym_public(&s))
        cwarn(c, s.loc, "main", "'main' is usually a function");
    if (initialized) {
        switch (g.what) {
        case GD_TYPEDEF:
            cerror(c, il, "typedef '%s' is initialized (use '__typeof__' "
                   "instead)", cident(c, g.name));
            initialized = false;
            incomp_init = true;   /* the initializer is still parsed */
            break;
        case GD_FUNC:
            cerror(c, il, "function '%s' is initialized like a variable",
                   cident(c, g.name));
            initialized = false;
            incomp_init = true;   /* the initializer is still parsed */
            break;
        case GD_PARM:
            cerror(c, il, "parameter '%s' is initialized", cident(c, g.name));
            initialized = false;
            incomp_init = true;   /* the initializer is still parsed */
            break;
        default:
            if (is_err(c, s.ty))
                initialized = false;
            else if (type_is_complete(TT, s.ty)) {
                /* fine */
            } else if (!is_arr(c, s.ty)) {
                cerror(c, il, "variable '%s' has initializer but incomplete "
                       "type", cident(c, g.name));
                initialized = false;
                incomp_init = true;   /* gcc still digests the initializer */
            }
            break;
        }
    }
    if (initialized && g.what == GD_VAR && sp.word == TW_AUTO_TYPE)
        s.flags |= CSF_AUTO_TYPE;
    if (initialized) {
        s.flags |= CSF_DEFINED;
        s.def_loc = s.loc;
        if (file)
            s.flags |= CSF_TREE_STATIC;
    }
    if (g.what == GD_VAR && file && !initialized &&
        !(s.flags & CSF_DECL_EXTERNAL))
        s.flags |= CSF_TENTATIVE;
    if (g.what == GD_TYPEDEF) {
        uint32_t nents = (uint32_t)TT->ents.len;
        if (a.sso == 1 && is_rec(c, type_canon(TT, s.ty)))
            s.ty = type_clone_record(TT, s.ty);   /* a distinct variant */
        ref = pushdecl(c, &s, false);
        /* a redeclaration can only raise the alignment */
        if (s.align && type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF) {
            TypeEnt *te = &TT->ents.data[TYPE_IDX(csym(c, ref)->ty)];
            if (TT->ents.len > nents || s.align > te->align) {
                te->align = s.align;
                te->flags |= TF_ALIGNED;
            }
        }
    } else {
        if (g.what == GD_FUNC)
            inline_follows(c, &s, ltok, sn, idecl);
        ref = pushdecl(c, &s, false);
    }
    {
        CSym *t = csym(c, ref);
        if (initialized && (t->flags & CSF_DECL_EXTERNAL)) {
            t->flags &= ~(unsigned)CSF_DECL_EXTERNAL;
            t->flags |= CSF_TREE_STATIC;
        }
        c->ty[i] = t->ty;
    }
    attrs_names(c, sn, &csym(c, ref)->aset);
    attrs_names_ptrs(c, idecl, &csym(c, ref)->aset);
    attrs_names(c, idecl, &csym(c, ref)->aset);
    if (a.packed)               /* ignored (attrs_misapplied), so not kept */
        aset_drop(c, csym(c, ref)->aset, "packed");
    if (csym(c, ref)->kind == CS_TYPEDEF && csym(c, ref)->aset &&
        type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF) {
        uint32_t p[2] = {TYPE_IDX(csym(c, ref)->ty), csym(c, ref)->aset};
        aset_keep_max_aligned(c, p[1]);
        vec_push(&c->tdas, p[0]);
        vec_push(&c->tdas, p[1]);
    }
    if (c->last_ualign > csym(c, ref)->ualign)
        csym(c, ref)->ualign = c->last_ualign;
    c->last_ualign = 0;
    c->nign = 0;
    if (sp.is_noreturn)
        aset_add(c, &csym(c, ref)->aset, "noreturn", "");
    if (csym(c, ref)->kind == CS_FUNC && csym(c, ref)->linkage == LK_INTERNAL) {
        /* leaf only means something for external functions */
        if (attrs_item_named(c, sn, "leaf") || attrs_item_named(c, idecl, "leaf"))
            cwarn(c, iloc(c, ltok), "attributes", "'leaf' attribute has no "
                  "effect on unit local functions");
        aset_drop(c, csym(c, ref)->aset, "leaf");
    }
    c->cb[i] = ref + 1;
    c->cv[i] = initialized ? 1 : incomp_init ? 2 : 0;
    if (g.what == GD_FUNC)
        cexpr_record_params(c, i, ref, false);
    if (g.what == GD_VAR || g.what == GD_FUNC)
        ensure_finish_cue(c, ref);
}

static void init_decl_visit(Checker *c, uint32_t idecl)
{
    uint32_t declared = find_declared(c, idecl), ref;
    bool init_ok, file = cat_file_scope(c);
    uint32_t init = NO_NODE;
    CSym *s;
    TypeId type;
    if (declared == NO_NODE || !c->cb[declared])
        return;
    ref = c->cb[declared] - 1;
    s = csym(c, ref);
    init_ok = c->cv[declared] & 1;
    if (declared != idecl - 1 && init_ok)
        init = idecl - 1;
    cinit_decl_done(c, idecl);
    type = s->ty;
    if ((s->flags & CSF_AUTO_TYPE) && init != NO_NODE) {
        /* __auto_type: the initializer's type after lvalue conversion */
        TypeId it = cexpr_rvalue_type(c, init);
        if (!is_err(c, it) && type_ckind(TT, it) != TY_ERROR) {
            s->ty = type = it | TYPE_QUALS(s->ty);
        }
    }
    if (s->kind == CS_OBJ && !(s->flags & CSF_PARAM)) {
        if (is_incomplete_array(c, type) &&
            !(sym_public(s) && !file)) {
            bool tstatic = (s->flags & CSF_TREE_STATIC) != 0;
            bool do_default = tstatic ? (c->opt.pedantic && !sym_public(s))
                                      : !(s->flags & CSF_DECL_EXTERNAL);
            TypeId nt;
            int failure = complete_array(c, type, init, do_default, &nt);
            switch (failure) {
            case 1:
                cerror(c, s->loc, "initializer fails to determine size of "
                       "'%s'", sname(c, s));
                break;
            case 2:
                if (do_default)
                    cerror(c, s->loc, "array size missing in '%s'",
                           sname(c, s));
                break;
            case 3:
                cerror(c, s->loc, "zero or negative size array '%s'",
                       sname(c, s));
                break;
            default:
                break;
            }
            if (failure != 2 || do_default) {
                s->ty = nt;
                type = nt;
            }
        }
        if (!is_err(c, type) && !type_is_complete(TT, type) &&
            ((s->flags & CSF_TREE_STATIC)
                 ? (sym_defined(s) || !ref_file_scope(ref))
                 : !(s->flags & CSF_DECL_EXTERNAL))) {
            cerror(c, s->loc, "storage size of '%s' isn't known", sname(c, s));
            s->ty = type = ERRT;
        }
        {
            TypeKind k = tkind(c, type);
            if ((k == TY_STRUCT || k == TY_UNION || k == TY_ENUM) &&
                !type_is_complete(TT, type) && (s->flags & CSF_TREE_STATIC))
                queue_object(c, ref);
        }
        if ((s->flags & (CSF_TREE_STATIC | CSF_DECL_EXTERNAL)) &&
            !is_err(c, type) && tkind(c, type) == TY_VLA) {
            cerror(c, s->loc, "storage size of '%s' isn't constant",
                   sname(c, s));
            s->ty = ERRT;
        }
    }
    dump_decl(c, csym(c, ref));
}

/* ---- structs, unions, enums ------------------------------------------------- */

static uint32_t find_child(Checker *c, uint32_t i, unsigned tag)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == tag) {
            r = k.p[j];
            break;
        }
    kids_free(&k);
    return r;
}

/* The tag bound in the innermost scope (a TypeId, 0: none). */
static TypeId tag_here(Checker *c, uint32_t name)
{
    uint32_t b = cbound_here(c, NS_TAG, name);
    return b ? c->log.data[b - 1].ref : 0;
}

static const char *tag_kw(int want)
{
    return want == TY_UNION ? "union" : want == TY_ENUM ? "enum" : "struct";
}

/* pushtag of a new forward reference. */
static TypeId new_tag(Checker *c, int want, uint32_t name, SrcLoc loc)
{
    TypeId t = want == TY_ENUM
        ? type_new_enum(TT, name, loc)
        : type_new_record(TT, name, want == TY_UNION, loc);
    cbind(c, NS_TAG, name, t);
    return t;
}

/* The unit's stray ';' in the struct body being defined, up to token upto
 * (gcc's parser diagnoses them as it goes). */
static void struct_semis(Checker *c, uint32_t upto)
{
    RecDef *rd;
    uint32_t k;
    int depth = 0;
    if (!c->recs.len)
        return;
    rd = &vec_last(&c->recs);
    if (rd->is_enum)
        return;
    for (k = (uint32_t)rd->first_ec; k < upto && k < c->u->ntoks; k++) {
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE:
            depth++;
            break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE:
            depth--;
            break;
        case P_SEMI:
            if (depth == 0 && k > 0 &&
                (tpunct(c, k - 1) == P_SEMI || tpunct(c, k - 1) == P_LBRACE))
                cpedantic(c, tloc(c, k), "extra semicolon in struct or union "
                          "specified");
            break;
        default:
            break;
        }
    }
    if (upto > rd->first_ec)
        rd->first_ec = upto;
}

static bool being_defined(Checker *c, TypeId t)
{
    size_t k;
    for (k = 0; k < c->recs.len; k++)
        if (type_canon(TT, c->recs.data[k].ty) == type_canon(TT, t))
            return true;
    return false;
}

static void tag_visit(Checker *c, uint32_t i)
{
    iloc_event(c, cnode(c, i)->tok);
}

/* start_struct / start_enum, at the '{'. */
static void open_visit(Checker *c, uint32_t i)
{
    uint32_t st = c->par[i], tagn = find_child(c, st, N_TAG), open_tok;
    int want = cnode(c, i)->aux == 2 ? TY_ENUM
             : cnode(c, i)->aux == 1 ? TY_UNION : TY_STRUCT;
    uint32_t name = tagn != NO_NODE ? cnode_ident(c, tagn) : 0;
    SrcLoc loc;
    TypeId t, ref = 0;
    RecDef rd;
    bool created = false;
    open_tok = cnode(c, i)->tok;
    if (tagn == NO_NODE)
        iloc_event(c, open_tok);
    if (name)
        loc = tloc(c, cnode(c, tagn)->tok);
    else if (want == TY_ENUM)
        loc = tloc(c, open_tok);
    else
        loc = tloc(c, cnode(c, st)->tok);
    if (name && cat_file_scope(c))
        csum_touch(c, SUM_TAG, name);     /* defines or completes the tag */
    if (name)
        ref = tag_here(c, name);
    if (ref && (int)tkind(c, ref) != want) {
        cerror(c, iloc(c, open_tok), "'%s' defined as wrong kind of tag",
               cident(c, name));
        ref = 0;
    }
    memset(&rd, 0, sizeof rd);
    if (want == TY_ENUM) {
        Enum *e;
        SrcLoc oldloc = 0;
        if (!ref) {
            t = new_tag(c, want, name, loc);
            created = true;
        } else
            t = ref;
        e = type_enum(TT, t);
        oldloc = e->loc;
        e->loc = loc;
        if (being_defined(c, t))
            cerror(c, loc, "nested redefinition of 'enum %s'",
                   name ? cident(c, name) : "");
        if (e->complete) {
            Diagnostic *d = cerror_d(c, loc, "redeclaration of 'enum %s'",
                                     name ? cident(c, name) : "");
            if (d && oldloc)
                cnote(c, d, oldloc, "originally defined here");
            e->complete = false;
        }
        rd.is_enum = true;
        rd.first_ec = (uint32_t)c->ecs.len;
        rd.next_ty = TYPE_B(INT);
    } else {
        Record *r = ref ? type_record(TT, ref) : NULL;
        if (r) {
            if (r->flags & RF_COMPLETE) {
                Diagnostic *d = cerror_d(c, loc, "redefinition of '%s %s'",
                                         tag_kw(want), cident(c, name));
                if (d && r->loc)
                    cnote(c, d, r->loc, "originally defined here");
                ref = 0;
            } else if (r->flags & RF_DEFINING) {
                cerror(c, loc, "nested redefinition of '%s %s'", tag_kw(want),
                       cident(c, name));
                ref = 0;
            }
        }
        if (!ref) {
            t = new_tag(c, want, name, loc);
            created = true;
        } else
            t = ref;
        r = type_record(TT, t);
        r->loc = loc;
        r->flags |= RF_DEFINING;
        rd.first_ec = open_tok + 1;
    }
    if (cexpr_cxx_compat(c, st)) {
        /* in_sizeof / in_typeof / in_alignof: the parser is inside one */
        bool in_sz = false, in_ty = false, in_al = false;
        uint32_t a;
        for (a = c->par[st]; a != NO_NODE; a = c->par[a])
            switch (ntag(c, a)) {
            case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: in_sz = true; break;
            case N_TYPEOF: in_ty = true; break;
            case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE: in_al = true; break;
            default: break;
            }
        if (in_sz || in_ty || in_al)
            cwarn(c, loc, "c++-compat", "defining type in '%s' expression is "
                  "invalid in C++", in_sz ? "sizeof" : in_ty ? "typeof"
                                                              : "alignof");
    }
    if (cexpr_cxx_compat(c, st)) {
        /* c_cast_expr / the compound literal: the type name itself defines
         * the tag (ctsk_tagdef) */
        uint32_t a = c->par[st], prev = st;
        for (; a != NO_NODE; prev = a, a = c->par[a]) {
            unsigned tg = ntag(c, a);
            if (tg == N_CAST || tg == N_COMPOUND_LIT) {
                if (first_child(c, a) == prev)
                    cwarn(c, tloc(c, cnode(c, a)->tok), "c++-compat",
                          "defining a type in a %s is invalid in C++",
                          tg == N_CAST ? "cast" : "compound literal");
                break;
            }
            if (tg == N_STRUCT || tg == N_ENUM || tg == N_COMPOUND ||
                tg == N_DECL || tg == N_FUNC_DEF)
                break;
        }
    }
    rd.ty = t;
    rd.first = (uint32_t)c->fields.len;
    vec_push(&c->recs, rd);
    c->ty[i] = t;
    if (created && cscope_kind(c) == SCK_PROTO)
        c->ef[i] |= 2;
}

/* parser_xref_tag: 'struct S' without a body. */
static void xref_visit(Checker *c, uint32_t i, int want)
{
    uint32_t tagn = find_child(c, i, N_TAG), name;
    SrcLoc loc, xloc = 0;
    TypeId t, ref;
    unsigned kind = TSK_TAGREF;
    uint32_t tt_tok;
    if (tagn == NO_NODE) {
        c->ty[i] = ERRT;
        c->cv[i] = TSK_TAGREF;
        return;
    }
    tt_tok = cnode(c, tagn)->tok;
    name = cnode_ident(c, tagn);
    loc = tloc(c, tt_tok);
    ref = clookup(c, NS_TAG, name);
    if (ref && (int)tkind(c, ref) != want) {
        SrcLoc il = iloc(c, tt_tok + 1);
        if (cbound_here(c, NS_TAG, name))
            cerror(c, il, "'%s' defined as wrong kind of tag",
                   cident(c, name));
        else {
            c->cb[i] = name;
            xloc = il;
        }
        ref = 0;
    }
    if (ref) {
        t = ref;
        /* straight from the table: a name-only use is not a layout read */
        uint32_t x = type_ent(TT, type_canon(TT, t))->extra;
        if (want == TY_ENUM) {
            const Enum *e = &TT->enums.data[x];
            cdep_report(c, loc, name, e->dep, e->dmsg, &e->loc);
        } else {
            const Record *r = &TT->recs.data[x];
            cdep_report(c, loc, name, r->dep, r->dmsg, &r->loc);
        }
    } else {
        t = new_tag(c, want, name, loc);
        kind = TSK_TAGFIRSTREF;
        if (cscope_kind(c) == SCK_PROTO)
            c->ef[i] |= 2;
    }
    if (want == TY_ENUM && find_child(c, i, N_TYPE_NAME) != NO_NODE) {
        /* enum e : T;  (C2X) fixes the underlying type: complete */
        Enum *e = type_enum(TT, t);
        TypeId u = c->ty[find_child(c, i, N_TYPE_NAME)];
        if (!is_err(c, u) && type_is_integer(TT, u)) {
            e->underlying = u;
            e->complete = true;
        }
    } else if (want == TY_ENUM && c->opt.pedantic &&
               !type_enum(TT, t)->complete)
        cpedantic(c, loc, "ISO C forbids forward references to 'enum' types");
    c->ty[i] = t;
    c->cv[i] = kind | ((uint64_t)xloc << 8);
}

/* ---- finish_struct ----------------------------------------------------------- */

static bool flex_struct(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    Record *r;
    if (k != TY_STRUCT && k != TY_UNION)
        return false;
    r = type_record(TT, type_canon(TT, t));
    return r && (r->flags & RF_FLEXIBLE);
}

static void dup_add(Checker *c, uint32_t **seen, size_t *ns, size_t *cap,
                    uint32_t name, SrcLoc loc, bool *dup)
{
    size_t k;
    *dup = false;
    for (k = 0; k < *ns; k++)
        if ((*seen)[k] == name) {
            cerror(c, loc, "duplicate member '%s'", cident(c, name));
            *dup = true;
            return;
        }
    if (*ns == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *seen = xrealloc(*seen, *cap * sizeof **seen);
    }
    (*seen)[(*ns)++] = name;
}

static void dup_nested(Checker *c, TypeId t, uint32_t **seen, size_t *ns,
                       size_t *cap, int depth)
{
    Record *r = type_record(TT, type_canon(TT, t));
    uint32_t k;
    if (!r || !(r->flags & RF_COMPLETE) || depth > 16)
        return;
    for (k = 0; k < r->nfields; k++) {
        const Field *fl = &TT->fields.data[r->fields + k];
        bool dup;
        if (fl->name)
            dup_add(c, seen, ns, cap, fl->name, fl->loc, &dup);
        else if (tkind(c, fl->ty) == TY_STRUCT ||
                 tkind(c, fl->ty) == TY_UNION)
            dup_nested(c, fl->ty, seen, ns, cap, depth + 1);
    }
}

static bool is_rec(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_STRUCT || tkind(c, t) == TY_UNION;
}

static void struct_finish(Checker *c, uint32_t i, uint32_t open, int want)
{
    TypeId t = c->ty[open];
    RecDef rd;
    FieldIn *f;
    uint32_t n, k, m = 0, close_tok;
    Attrs a;
    SrcLoc loc;
    bool named = false, saw_named = false;
    uint32_t *seen = NULL;
    size_t ns = 0, cap = 0;
    Record *r;
    int depth = 0;
    if (!c->recs.len)
        return;
    rd = vec_last(&c->recs);
    r = type_record(TT, t);
    loc = r ? r->loc : tloc(c, cnode(c, i)->tok);
    /* the closing brace, the stray semicolons, a missing one */
    close_tok = cnode(c, open)->tok;
    for (k = close_tok; k < c->u->ntoks; k++) {
        int p = tpunct(c, k);
        if (p == P_LBRACE)
            depth++;
        else if (p == P_RBRACE && --depth == 0) {
            close_tok = k;
            break;
        }
    }
    struct_semis(c, close_tok);
    memset(&a, 0, sizeof a);
    {
        Attrs ma;
        memset(&ma, 0, sizeof ma);
        c->attr_quiet = true;
        attrs_of_children(c, i, &ma);
        c->attr_quiet = false;
        if (ma.has_mode)
            cerror(c, tloc(c, close_tok), "mode '%s' applied to inappropriate "
                   "type", ma.mode_name);
    }
    c->attr_at = loc;               /* gcc reports a tag attribute at the tag name */
    c->attr_at_set = true;
    attrs_of_children(c, i, &a);
    c->attr_at_set = false;
    if (a.noinline)
        cwarn(c, iloc(c, close_tok), "attributes",
              "'noinline' attribute does not apply to types");
    if (a.used)
        cwarn(c, iloc(c, close_tok), "attributes",
              "'used' attribute does not apply to types");
    if (a.desig && want == TY_UNION)
        cerror(c, iloc(c, close_tok), "'designated_init' attribute is only "
               "valid on 'struct' type");
    n = (uint32_t)c->fields.len - rd.first;
    f = c->fields.data + rd.first;
    if (c->opt.pedantic) {
        for (k = 0; k < n; k++)
            if (f[k].name)
                named = true;
        if (!named) {
            DiagOrd o0 = diag_ord(c->diag, n > 0 ? ORD_LATE : ORD_NORMAL);
            /* finish_struct: after a missing semicolon */
            if (want == TY_UNION)
                cpedantic(c, loc, n ? "union has no named members"
                                    : "union has no members");
            else
                cpedantic(c, loc, n ? "struct has no named members"
                                    : "struct has no members");
            diag_ord(c->diag, o0);
        }
    }
    for (k = 0; k < n; k++) {
        bool is_last = k == n - 1 || want == TY_UNION;
        if (is_err(c, f[k].ty))
            continue;
        if (is_incomplete_array(c, f[k].ty)) {
            if (want == TY_UNION) {
                cerror(c, f[k].loc, "flexible array member in union");
                f[k].ty = ERRT;
            } else if (!is_last) {
                cerror(c, f[k].loc, "flexible array member not at end of "
                       "struct");
                f[k].ty = ERRT;
            } else if (!saw_named) {
                cerror(c, f[k].loc, "flexible array member in a struct with "
                       "no named members");
                f[k].ty = ERRT;
            }
        }
        if (c->opt.pedantic && want == TY_STRUCT && flex_struct(c, f[k].ty))
            cpedantic(c, f[k].loc, "invalid use of structure with flexible "
                      "array member");
        if (f[k].name || is_rec(c, f[k].ty))
            saw_named = true;
    }
    for (k = 0; k < n; k++) {
        bool dup;
        if (f[k].name) {
            dup_add(c, &seen, &ns, &cap, f[k].name, f[k].loc, &dup);
            if (dup)
                f[k].name = 0;
        } else if (is_rec(c, f[k].ty) && f[k].width < 0)
            dup_nested(c, f[k].ty, &seen, &ns, &cap, 0);
    }
    free(seen);
    for (k = 0; k < n; k++)
        if (!is_err(c, f[k].ty))
            f[m++] = f[k];
    csum_read_pack(c);
    type_complete_record(TT, t, f, m, c->pack, a.aligned, a.packed,
                         a.ms);
    if (a.desig && want != TY_UNION)
        type_record(TT, t)->flags |= RF_DESIGNATED;
    if (a.sso == 1)     /* the target is little-endian */
        type_record(TT, t)->flags |= RF_SSO;
    r = type_record(TT, t);
    {
        uint32_t as = r->aset;
        attrs_names(c, i, &as);
        r->aset = as;
    }
    r->dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    r->dmsg = a.dep_msg;
    if (a.transparent_union && want == TY_UNION) {
        bool ok = r->nfields > 0;
        if (ok) {
            const Field *fl = &TT->fields.data[r->fields];
            bool sok;
            uint64_t sz = type_size(TT, fl->ty, &sok);
            ok = sok && sz == r->size && !(fl->flags & FF_BITFIELD);
        }
        if (ok)
            r->flags |= RF_TRANSPARENT;
        else
            cwarn(c, loc, "", "union cannot be made transparent");
    }
    if (c->opt.dump && !c->quiet) {
        StrBuf sb;
        memset(&sb, 0, sizeof sb);
        type_dump_record(TT, &sb, t);
        fputs(sb_cstr(&sb), c->opt.dump);
        sb_free(&sb);
    }
    c->fields.len = rd.first;
    c->recs.len--;
    c->ty[i] = t;
    c->cv[i] = TSK_TAGDEF;
    if (c->ef[open] & 2)
        c->ef[i] |= 2;
}

/* ---- enumerators ------------------------------------------------------------- */

static bool val_neg(Checker *c, uint64_t v, TypeId ty)
{
    return type_is_signed(TT, ty) && (int64_t)v < 0;
}

/* a < b, as mathematical values of types ta and tb. */
static bool val_lt(Checker *c, uint64_t a, TypeId ta, uint64_t b, TypeId tb)
{
    bool na = val_neg(c, a, ta), nb = val_neg(c, b, tb);
    if (na != nb)
        return na;
    if (na)
        return (int64_t)a < (int64_t)b;
    return a < b;
}

static void enumerator_visit(Checker *c, uint32_t i)
{
    uint32_t name = cnode_ident(c, i), vn = NO_NODE, k;
    RecDef *rd;
    SrcLoc nloc = tloc(c, cnode(c, i)->tok), vloc = nloc;
    TypeId vt = TYPE_B(INT);
    uint64_t v = 0;
    bool have = false, wide = false;
    Kids kk;
    CSym s;
    uint32_t ref;
    if (!c->recs.len || !vec_last(&c->recs).is_enum)
        return;
    rd = &vec_last(&c->recs);
    iloc_event(c, cnode(c, i)->tok);
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++)
        if (cexpr_is_expr(ntag(c, kk.p[k])))
            vn = kk.p[k];
    kids_free(&kk);
    if (vn != NO_NODE) {
        TypeId ty = c->ty[vn];
        vloc = tloc(c, first_tok(c, vn));
        if (c->ck[vn] == K_ERR && is_err(c, ty)) {
            /* ignored: the default */
        } else if (!type_is_integer(TT, ty)) {
            cerror(c, vloc, "enumerator value for '%s' is not an integer "
                   "constant", cident(c, name));
        } else {
            bool ok = c->ck[vn] == K_ICE;
            if (!ok && c->ck[vn] == K_FOLD) {
                cpedantic(c, vloc, "enumerator value for '%s' is not an "
                          "integer constant expression", cident(c, name));
                ok = true;
            }
            if (!ok)
                cerror(c, iloc(c, after_tok(c, vn)), "enumerator value for "
                       "'%s' is not an integer constant", cident(c, name));
            else {
                if (c->ef[vn] & EF_OVERFLOW)
                    cconst_overflow(c, nloc);
                vt = type_int_promote(TT, ty);
                v = cexpr_trunc(c, vt, (uint64_t)cexpr_sval(c, vn));
                have = true;
            }
        }
    }
    if (!have) {
        wide = rd->next_ty == TYPE_B(UINT128) || rd->next_ty == TYPE_B(INT128);
        v = rd->next;
        vt = rd->next_ty;
        if (rd->next_overflow)
            cerror(c, vloc, "overflow in enumeration values");
    }
    if (wide)
        cpedantic(c, vloc, "enumerator value outside the range of '%s'",
                  type_is_signed(TT, vt) ? "intmax_t" : "uintmax_t");
    if (wide) {
        /* wider than intmax_t: the value itself is not representable */
    } else if (!cexpr_fits(c, v, vt, TYPE_B(INT))) {
        cpedantic(c, vloc, "ISO C restricts enumerator values to range of "
                  "'int' before C2X");
    } else {
        vt = TYPE_B(INT);
        v = cexpr_trunc(c, vt, v);
    }
    /* the next value */
    {
        uint64_t nv = cexpr_trunc(c, vt, v + 1);
        bool ovf = val_lt(c, nv, vt, v, vt);
        TypeId nt = vt;
        if (ovf) {
            unsigned prec = type_int_bits(TT, vt) + 1;
            bool uns = !type_is_signed(TT, vt);
            TypeId nw = uns ? TYPE_B(ULONG) : TYPE_B(LONG);
            if (prec > type_int_bits(TT, nw))
                nw = uns ? TYPE_B(ULLONG) : TYPE_B(LLONG);
            if (prec > type_int_bits(TT, nw) && c->tgt->size[TY_INT128])
                nw = uns ? TYPE_B(UINT128) : TYPE_B(INT128);
            if (prec <= type_int_bits(TT, nw)) {
                ovf = false;
                nt = nw;
                nv = nw == TYPE_B(UINT128) || nw == TYPE_B(INT128)
                         ? 0 : cexpr_trunc(c, nw, v + 1);
            }
        }
        rd->next = nv;
        rd->next_ty = nt;
        rd->next_overflow = ovf;
    }
    memset(&s, 0, sizeof s);
    s.name = name;
    s.kind = CS_ENUMCONST;
    s.ty = vt;      /* the value's type until the enum is finished */
    s.loc = nloc;
    s.val = v;
    s.vty = vt;
    ref = pushdecl(c, &s, false);
    vec_push(&c->ecs, ref);
}

/* c_common_type_for_size */
static TypeId type_for_bits(Checker *c, unsigned bits, bool uns)
{
    if (bits <= 8)
        return uns ? TYPE_B(UCHAR) : TYPE_B(SCHAR);
    if (bits <= 16)
        return uns ? TYPE_B(USHORT) : TYPE_B(SHORT);
    if (bits <= 32)
        return uns ? TYPE_B(UINT) : TYPE_B(INT);
    if (bits <= 64) {
        if (type_int_bits(TT, TYPE_B(LONG)) == 64)
            return uns ? TYPE_B(ULONG) : TYPE_B(LONG);
        return uns ? TYPE_B(ULLONG) : TYPE_B(LLONG);
    }
    if (bits <= 128 && c->tgt->size[TY_INT128])
        return uns ? TYPE_B(UINT128) : TYPE_B(INT128);
    return ERRT;
}

static void enum_finish(Checker *c, uint32_t i, uint32_t open)
{
    TypeId t = c->ty[open];
    RecDef rd;
    Enum *e;
    Attrs a;
    uint32_t k, ne;
    uint64_t mn = 0, mx = 0;
    TypeId mnt = TYPE_B(INT), mxt = TYPE_B(INT);
    bool uns, wider;
    unsigned prec, p2;
    TypeId tem;
    if (!c->recs.len)
        return;
    rd = vec_last(&c->recs);
    ne = (uint32_t)c->ecs.len - rd.first_ec;
    memset(&a, 0, sizeof a);
    attrs_of_children(c, i, &a);
    e = type_enum(TT, t);
    if (a.packed)
        e->packed = true;
    e->dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    e->dmsg = a.dep_msg;
    if (a.desig)
        cerror(c, ne ? csym(c, c->ecs.data[rd.first_ec + ne - 1])->loc :
               tloc(c, cnode(c, i)->tok), "'designated_init' attribute is "
               "only valid on 'struct' type");
    for (k = 0; k < ne; k++) {
        const CSym *s = csym(c, c->ecs.data[rd.first_ec + k]);
        if (k == 0) {
            mn = mx = s->val;
            mnt = mxt = s->vty;
            continue;
        }
        if (val_lt(c, mx, mxt, s->val, s->vty)) {
            mx = s->val;
            mxt = s->vty;
        }
        if (val_lt(c, s->val, s->vty, mn, mnt)) {
            mn = s->val;
            mnt = s->vty;
        }
    }
    uns = !val_neg(c, mn, mnt);
    p2 = min_prec(mn, !uns, uns);
    prec = min_prec(mx, val_neg(c, mx, mxt), uns);
    if (p2 > prec)
        prec = p2;
    wider = !cexpr_fits(c, mn, mnt, TYPE_B(INT)) ||
            !cexpr_fits(c, mx, mxt, TYPE_B(INT));
    if (e->packed || c->opt.short_enums || prec > 32) {
        tem = type_for_bits(c, prec, uns);
        if (is_err(c, tem)) {
            cpedwarn(c, iloc(c, after_tok(c, i)), "", "enumeration values "
                     "exceed range of largest integer");
            tem = TYPE_B(LLONG);
        }
    } else
        tem = uns ? TYPE_B(UINT) : TYPE_B(INT);
    {
        uint32_t utn = find_child(c, i, N_TYPE_NAME);
        if (utn != NO_NODE && !is_err(c, c->ty[utn]) &&
            type_is_integer(TT, c->ty[utn])) {
            tem = c->ty[utn];       /* C2X fixed underlying type */
            wider = false;
        }
    }
    if (a.has_mode && a.mode_bytes) {
        TypeId mt = int_of_size(c, a.mode_bytes, uns);
        if (!is_err(c, mt)) {
            if (prec > a.mode_bytes * 8u && ne)
                cerror(c, csym(c, c->ecs.data[rd.first_ec])->loc,
                       "specified mode too small for enumerated values");
            else
                tem = mt;
            wider = false;
        }
    }
    e->underlying = tem;
    e->complete = true;
    for (k = 0; k < ne; k++) {
        CSym *s = csym(c, c->ecs.data[rd.first_ec + k]);
        s->ty = t;
        if (wider)
            s->vty = t;
    }
    if (c->opt.dump && !c->quiet) {
        for (k = 0; k < ne; k++) {
            const CSym *s = csym(c, c->ecs.data[rd.first_ec + k]);
            StrBuf sb;
            memset(&sb, 0, sizeof sb);
            type_print(TT, &sb, s->vty);
            if (val_neg(c, s->val, s->vty) || type_is_signed(TT, s->vty))
                fprintf(c->opt.dump, "enumconst %s = %" PRId64 " (%s)\n",
                        sname(c, s), (int64_t)s->val, sb_cstr(&sb));
            else
                fprintf(c->opt.dump, "enumconst %s = %" PRIu64 " (%s)\n",
                        sname(c, s), s->val, sb_cstr(&sb));
            sb_free(&sb);
        }
    }
    c->ecs.len = rd.first_ec;
    c->fields.len = rd.first;
    c->recs.len--;
    c->ty[i] = t;
    c->cv[i] = TSK_TAGDEF;
    if (c->ef[open] & 2)
        c->ef[i] |= 2;
}

static void struct_visit(Checker *c, uint32_t i)
{
    uint32_t open = find_child(c, i, N_OPEN);
    int want;
    if (cnode(c, i)->tag == N_ENUM)
        want = TY_ENUM;
    else
        want = tckw(c, cnode(c, i)->tok) == CK_UNION ? TY_UNION : TY_STRUCT;
    if (want == TY_ENUM && find_child(c, i, N_TYPE_NAME) != NO_NODE) {
        uint32_t tn = find_child(c, i, N_TYPE_NAME);
        uint32_t tg_ = find_child(c, i, N_TAG);
        cpedantic(c, tloc(c, tg_ != NO_NODE ? cnode(c, tg_)->tok
                                              : first_tok(c, tn) - 1),
                  "ISO C does not support specifying 'enum' underlying types "
                  "before C2X");
    }
    if (open == NO_NODE)
        xref_visit(c, i, want);
    else if (want == TY_ENUM)
        enum_finish(c, i, open);
    else
        struct_finish(c, i, open, want);
}

/* ---- members ------------------------------------------------------------------ */

/* The delimiter (',' ';' '}') ending the j-th declarator of a member
 * declaration whose specifiers end before token start. */
static uint32_t member_delim(Checker *c, uint32_t start, uint32_t j)
{
    uint32_t k = start;
    for (;;) {
        k = scan_end(c, k, false);
        if (j == 0 || tpunct(c, k) != P_COMMA)
            return k;
        j--;
        k++;
    }
}

/* Attributes written after a '*' in a declarator belong to the pointer
 * type: aligned sets its alignment, packed is dropped with a warning (and
 * conflicts with an aligned before it in the same list). */
static void ptr_type_attrs(Checker *c, uint32_t d, TypeId ty, uint32_t tok,
                           Attrs *out)
{
    Kids k;
    uint32_t j;
    kids_get(c, d, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t x = k.p[j];
        if (is_declarator_tag(ntag(c, x))) {
            ptr_type_attrs(c, x, ty, tok, out);
        } else if (ntag(c, d) == N_PTR && ntag(c, x) == N_ATTRIBUTE) {
            Attrs t;
            Kids it;
            uint32_t q;
            bool saw_aligned = false;
            memset(&t, 0, sizeof t);
            attr_collect(c, x, &t);
            if (t.aligned > out->aligned)
                out->aligned = t.aligned;
            kids_get(c, x, &it);
            for (q = 0; q < it.n; q++) {
                char an[48];
                if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                    continue;
                attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
                if (!strcmp(an, "aligned"))
                    saw_aligned = true;
                else if (!strcmp(an, "packed")) {
                    if (saw_aligned)
                        cwarn(c, iloc(c, tok), "attributes", "ignoring "
                              "attribute 'packed' because it conflicts with "
                              "attribute 'aligned'");
                    else
                        cwarn(c, iloc(c, tok), "attributes", "'packed' "
                              "attribute ignored for type %s",
                              type_q(TT, ty));
                }
            }
            kids_free(&it);
        }
    }
    kids_free(&k);
}

static void member_visit(Checker *c, uint32_t i)
{
    uint32_t md = c->par[i], sn, top = NO_NODE, w = NO_NODE, k, idx = 0, ltok;
    Kids kk;
    Spec sp;
    int si;
    GDecl g;
    Attrs a;
    FieldIn fi;
    if (md == NO_NODE || ntag(c, md) != N_MEMBER_DECL)
        return;
    sn = first_child(c, md);
    si = find_spec(c, sn);
    if (si < 0)
        return;
    struct_semis(c, first_tok(c, md));
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    dep_spec_use(c, &sp);
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++) {
        if (is_declarator_tag(ntag(c, kk.p[k])))
            top = kk.p[k];
        else if (cexpr_is_expr(ntag(c, kk.p[k])) &&
                 (cnode(c, i)->flags & NF_BITFIELD))
            w = kk.p[k];
    }
    kids_free(&kk);
    kids_get(c, md, &kk);
    for (k = 0; k < kk.n && kk.p[k] != i; k++)
        if (ntag(c, kk.p[k]) == N_MEMBER)
            idx++;
    kids_free(&kk);
    ltok = member_delim(c, sp.tok1, idx);
    c->cd_ltok = ltok;
    grok(c, &sp, top, DC_FIELD, false, false, w, ltok, ltok, &g);
    if (g.what == GD_NONE)
        return;
    memset(&a, 0, sizeof a);
    attrs_of_children(c, i, &a);
    if (a.has_mode || a.vs_seen)
        g.ty = attr_apply_type(c, g.ty, &a);
    attrs_unknown_emit(c, &sp.attrs, ltok);
    if (top != NO_NODE)
        ptr_type_attrs(c, top, g.ty, ltok, &a);
    attrs_merge(&a, &sp.attrs);
    attrs_misapplied(c, &a, 'm', false, w == NO_NODE ? g.ty : 0, ltok);
    attrs_section_check(c, &a, 'm', false, g.name, g.loc);
    strict_flex_check(c, sp.node, true, g.ty, g.name, g.loc, sp.tok0);
    strict_flex_check(c, i, true, g.ty, g.name, g.loc, NO_NODE);
    attrs_zcur_check(c, &a, false, g.loc);
    attrs_wina_check(c, &a, 'm', g.width >= 0, g.name, g.loc);
    memset(&fi, 0, sizeof fi);
    fi.name = g.name;
    fi.ty = g.ty;
    fi.width = g.width;
    fi.align = g.s.align > a.aligned ? g.s.align : (uint16_t)a.aligned;
    fi.packed = a.packed;
    fi.loc = g.loc;
    fi.dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    fi.dmsg = a.dep_msg;
    attrs_names(c, sp.node, &fi.aset);
    attrs_names(c, i, &fi.aset);
    if (!fi.packed && cdecl_aset_has(c, fi.aset, "packed", NULL))
        fi.packed = true;       /* copied from another declaration */
    vec_push(&c->fields, fi);
}

static void member_decl_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i);
    int si = sn != NO_NODE ? find_spec(c, sn) : -1;
    if (si >= 0 && c->nodes[i].size == c->nodes[sn].size + 1) {
        Spec sp = c->specs.data[si];
        uint32_t ltok = sp.tok1;
        struct_semis(c, first_tok(c, i));
        if (sp.kind == TSK_NONE && !sp.has_type && sp.default_int) {
            cpedantic(c, tloc(c, sp.tok0), "ISO C forbids member declarations "
                      "with no members");
            shadow_tag(c, &sp, c->opt.pedantic ? 1 : 0, ltok);
        } else if (!sp.error) {
            TypeKind tk = tkind(c, sp.ty);
            bool ok = false;
            if ((tk == TY_STRUCT || tk == TY_UNION) && sp.kind != TSK_TYPEDEF)
                ok = type_record(TT, type_canon(TT, sp.ty))->tag == 0;
            if (!ok)
                cpedwarn(c, tloc(c, ltok), "", "declaration does not declare "
                         "anything");
            else {
                GDecl g;
                cped11(c, tloc(c, ltok), "ISO C99 doesn't support unnamed "
                          "structs/unions");
                grok(c, &sp, NO_NODE, DC_FIELD, false, false, NO_NODE, ltok,
                     ltok, &g);
                if (g.what != GD_NONE) {
                    FieldIn fi;
                    memset(&fi, 0, sizeof fi);
                    fi.ty = g.ty;
                    fi.width = -1;
                    fi.align = g.s.align > sp.attrs.aligned
                        ? g.s.align : (uint16_t)sp.attrs.aligned;
                    fi.packed = sp.attrs.packed;
                    fi.loc = tloc(c, ltok);
                    {
                        /* gcc locates an anonymous member at its '{' */
                        uint32_t q;
                        for (q = cfirst(c, sn); q <= sn; q++)
                            if (ntag(c, q) == N_OPEN) {
                                fi.loc = tloc(c, cnode(c, q)->tok);
                                break;
                            }
                    }
                    vec_push(&c->fields, fi);
                }
            }
        }
    }
    pop_specs(c, i);
}

/* ---- _Static_assert, #pragma pack ---------------------------------------------- */

static void static_assert_visit(Checker *c, uint32_t i)
{
    uint32_t e = first_child(c, i), s = NO_NODE, k;
    SrcLoc aloc = tloc(c, cnode(c, i)->tok), vloc;
    Kids kk;
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        struct_semis(c, cnode(c, i)->tok);
    if (!in_extension(c, i)) {
        size_t n0 = c->diag->all.len;
        cped11(c, aloc, "ISO C99 does not support '_Static_assert'");
        choist(c, i, n0);
    }
    if (e == NO_NODE)
        return;
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++)
        if (ntag(c, kk.p[k]) == N_STRING)
            s = kk.p[k];
    kids_free(&kk);
    if (s == e)
        return;
    vloc = cnode_loc(c, e);
    if (!type_is_integer(TT, c->ty[e])) {
        /* an erroneous value has no location of its own: the first token */
        cerror(c, is_err(c, c->ty[e]) ? tloc(c, first_tok(c, e)) : vloc,
               "expression in static assertion is not an integer");
        return;
    }
    if (c->ck[e] != K_ICE) {
        if (c->ck[e] == K_FOLD)
            cpedantic(c, vloc, "expression in static assertion is not an "
                      "integer constant expression");
        else {
            if (c->ck[e] != K_ERR)
                cerror(c, vloc, "expression in static assertion is not "
                       "constant");
            return;
        }
    }
    if (cexpr_sval(c, e) == 0) {
        if (s != NO_NODE) {
            StrBuf sb;
            uint32_t p, np = cnode(c, s)->aux ? cnode(c, s)->aux : 1;
            memset(&sb, 0, sizeof sb);
            sb_putc(&sb, '"');
            for (p = 0; p < np; p++) {
                const Tok *t = tokp(c, cnode(c, s)->tok + p);
                const char *tx = tok_text_raw(c->sm, c->in, t);
                size_t n = t->len, off = 0;
                while (off < n && tx[off] != '"')
                    off++;
                if (off < n)
                    off++;
                if (n > off && tx[n - 1] == '"')
                    n--;
                if (n > off)
                    sb_putn(&sb, tx + off, n - off);
            }
            sb_putc(&sb, '"');
            cerror(c, aloc, "static assertion failed: %s", sb_cstr(&sb));
            sb_free(&sb);
        } else
            cerror(c, aloc, "static assertion failed");
    }
}

/* #pragma GCC diagnostic push | pop | ignored|warning|error|fatal "-Wname";
 * s is just past "GCC".  The checker's diagnostics follow the new state. */
static void diag_pragma(Checker *c, const char *s, const char *end)
{
    char kind[16], opt[96], flag[112];
    size_t n = 0;
    uint64_t h;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (end - s < 10 || strncmp(s, "diagnostic", 10))
        return;
    s += 10;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    while (s < end && *s >= 'a' && *s <= 'z' && n < sizeof kind - 1)
        kind[n++] = *s++;
    kind[n] = 0;
    if (!strcmp(kind, "push")) {
        DiagState st;
        st.cfg = c->diag_cur ? diag_config_clone(c->diag_cur) : NULL;
        st.dig = c->diag_dig;
        vec_push(&c->diag_stack, st);
        return;
    }
    if (!strcmp(kind, "pop")) {
        DiagState st;
        if (!c->diag_stack.len)
            return;
        st = vec_last(&c->diag_stack);
        c->diag_stack.len--;
        diag_config_free(c->diag_cur);
        c->diag_cur = st.cfg;
        c->diag_dig = st.dig;
        c->diag->cfg = c->diag_cur ? c->diag_cur : c->diag_cfg0;
        return;
    }
    if (strcmp(kind, "ignored") && strcmp(kind, "warning") &&
        strcmp(kind, "error") && strcmp(kind, "fatal"))
        return;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (end - s < 5 || s[0] != '"' || s[1] != '-' || s[2] != 'W')
        return;
    s += 3;
    for (n = 0; s < end && *s != '"' && n < sizeof opt - 1; s++)
        opt[n++] = *s;
    opt[n] = 0;
    if (!c->diag_cur) {
        c->diag_cfg0 = c->diag->cfg;
        c->diag_cur = diag_config_clone(c->diag->cfg);
    }
    if (!strcmp(kind, "ignored")) {
        snprintf(flag, sizeof flag, "no-%s", opt);
        diag_config_apply(c->diag_cur, flag);
    } else if (!strcmp(kind, "warning")) {
        diag_config_apply(c->diag_cur, opt);
        snprintf(flag, sizeof flag, "no-error=%s", opt);
        diag_config_apply(c->diag_cur, flag);
    } else {
        snprintf(flag, sizeof flag, "error=%s", opt);
        diag_config_apply(c->diag_cur, flag);
    }
    c->diag->cfg = c->diag_cur;
    for (h = 14695981039346656037ull, s = kind; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    for (s = opt; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    c->diag_dig = (c->diag_dig + h) * 0x9E3779B97F4A7C15ull | 1;
}

static void pragma_visit(Checker *c, uint32_t i)
{
    const Tok *t = tokp(c, cnode(c, i)->tok);
    const char *s = tok_text_raw(c->sm, c->in, t), *end;
    size_t n = t->len;
    int action = 0;   /* 0 set, 1 push, 2 pop */
    long val = -1;
    bool have = false;
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        struct_semis(c, cnode(c, i)->tok);
    end = s + n;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (end - s >= 6 && !strncmp(s, "pragma", 6))
        s += 6;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (end - s > 3 && !strncmp(s, "GCC", 3) && (s[3] == ' ' || s[3] == '	')) {
        diag_pragma(c, s + 3, end);
        return;
    }
    if (end - s < 4 || strncmp(s, "pack", 4))
        return;
    s += 4;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (s >= end || *s != '(')
        return;
    s++;
    for (;;) {
        while (s < end && (*s == ' ' || *s == '\t' || *s == ','))
            s++;
        if (s >= end || *s == ')')
            break;
        if (*s >= '0' && *s <= '9') {
            val = 0;
            have = true;
            while (s < end && *s >= '0' && *s <= '9')
                val = val * 10 + (*s++ - '0');
        } else if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                   *s == '_') {
            const char *b = s;
            while (s < end && ((*s >= 'a' && *s <= 'z') ||
                               (*s >= 'A' && *s <= 'Z') || *s == '_' ||
                               (*s >= '0' && *s <= '9')))
                s++;
            if (s - b == 4 && !strncmp(b, "push", 4))
                action = 1;
            else if (s - b == 3 && !strncmp(b, "pop", 3))
                action = 2;
        } else
            return;
    }
    if (have && !(val == 0 || val == 1 || val == 2 || val == 4 || val == 8 ||
                  val == 16))
        return;
    if (action == 1) {
        vec_push(&c->pack_stack, c->pack);
        if (have)
            c->pack = (unsigned)val;
    } else if (action == 2) {
        if (c->pack_stack.len) {
            c->pack = vec_last(&c->pack_stack);
            c->pack_stack.len--;
        }
    } else
        c->pack = have ? (unsigned)val : 0;
}

/* ---- parameters, array declarators, function declarators --------------------- */

static void param_visit(Checker *c, uint32_t p)
{
    uint32_t sn = first_child(c, p), top = NO_NODE, after = after_tok(c, p);
    uint32_t l = last_child(c, p), ref;
    int si;
    Spec sp;
    GDecl g;
    CSym s;
    Attrs a;
    while (l != NO_NODE && ntag(c, l) == N_ATTRIBUTE) {
        /* trailing attributes: the declarator is the node before them */
        Kids k;
        uint32_t j, prev = NO_NODE;
        kids_get(c, p, &k);
        for (j = 0; j < k.n && k.p[j] != l; j++)
            prev = k.p[j];
        kids_free(&k);
        l = prev;
    }
    if (l != NO_NODE && is_declarator_tag(ntag(c, l)))
        top = l;
    si = sn != NO_NODE && ntag(c, sn) == N_SPECS ? find_spec(c, sn) : -1;
    if (si < 0) {
        c->ty[p] = ERRT;
        pop_specs(c, p);
        return;
    }
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    c->cd_ltok = after;
    grok(c, &sp, top, DC_PARM, false, false, NO_NODE, after, after, &g);
    if (g.what == GD_NONE) {
        c->ty[p] = ERRT;
        pop_specs(c, p);
        return;
    }
    s = g.s;
    if (!g.name) {
        uint32_t d = top, inner;
        bool arr = false;
        if (d != NO_NODE) {
            while ((inner = inner_decl(c, d)) != NO_NODE)
                d = inner;
            arr = ntag(c, d) == N_ARRAY;
        }
        if (!arr)
            s.loc = tloc(c, first_tok(c, p));
    }
    memset(&a, 0, sizeof a);
    attrs_of_children(c, p, &a);
    larger_than(c, s.loc, s.name, s.ty);
    attrs_unknown_emit(c, &sp.attrs, first_tok(c, p));
    attrs_merge(&a, &sp.attrs);
    attrs_misapplied(c, &a, 'p', false, 0, first_tok(c, p));
    attrs_zcur_check(c, &a, false, s.loc);
    attrs_section_check(c, &a, 'p', false, s.name, s.loc);
    if (a.unused)
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;
    if (a.deprecated || a.unavailable) {
        s.flags |= a.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
        s.dep_msg = a.dep_msg;
    }
    if ((cnode(c, p)->flags & NF_SEMI) && c->fwd_warned != c->par[p] + 1) {
        /* mark_forward_parm_decls: once per parameter scope */
        c->fwd_warned = c->par[p] + 1;
        cpedantic(c, iloc(c, after + 1),
                  "ISO C forbids forward parameter declarations");
    }
    ref = pushdecl(c, &s, false);
    if (cnode(c, p)->flags & NF_FWD)
        csym(c, ref)->flags |= CSF_FWD;
    else
        csym(c, ref)->flags &= ~(unsigned)CSF_FWD;
    c->ty[p] = g.ty;
    c->cb[p] = ref + 1;
    c->cv[p] = (g.name ? 1 : 0) | (sp.sc == SC_REGISTER ? 2 : 0);
    if (g.array_param)
        c->cv[p] |= (uint64_t)g.pre << 32;
    pop_specs(c, p);
}

static void array_visit(Checker *c, uint32_t i)
{
    if (!(cnode(c, i)->flags & NF_STAR))
        return;
    switch (cscope_kind(c)) {
    case SCK_PROTO:
        break;
    case SCK_FUNC:
        if (c->cur_func_node != NO_NODE)
            c->ef[c->cur_func_node] |= 1;
        break;
    default:
        cerror(c, tloc(c, cnode(c, i)->tok), "'[*]' not allowed in other than "
               "function prototype scope");
        c->cv[i] |= 1;
        break;
    }
}

/* The location of a parameter's symbol. */
static SrcLoc param_loc(Checker *c, uint32_t p)
{
    if (c->cb[p])
        return csym(c, c->cb[p] - 1)->loc;
    return tloc(c, cnode(c, p)->tok);
}

/* GNU forward parameter declarations (mark_forward_parm_decls, and
 * get_parm_info's check that each was declared again). */
static void fwd_params(Checker *c, uint32_t f)
{
    Kids k;
    uint32_t j, m;
    c->fwd_warned = 0;
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t p = k.p[j];
        bool redone = false;
        if (ntag(c, p) != N_PARAM || !(cnode(c, p)->flags & NF_FWD))
            continue;
        for (m = 0; m < k.n && !redone; m++)
            redone = c->cb[p] && is_real_param(c, k.p[m]) &&
                     c->cb[k.p[m]] == c->cb[p];
        if (!redone)
            cerror(c, param_loc(c, p), "parameter '%s' has just a forward "
                   "declaration", c->cb[p] && csym(c, c->cb[p] - 1)->name
                       ? cident(c, csym(c, c->cb[p] - 1)->name)
                       : "({anonymous})");
    }
    kids_free(&k);
}

/* get_parm_info, at the FUNC node. */
static void func_visit(Checker *c, uint32_t f)
{
    unsigned fl = cnode(c, f)->flags;
    bool var = (fl & NF_VARIADIC) != 0, has = func_has_params(c, f);
    uint32_t k, first = cfirst(c, f), nparm = 0, ntags = 0, j;
    bool gave = false;
    if (var && !has)
        cpedantic(c, tloc(c, cnode(c, f)->tok + 1), "ISO C requires a named "
                  "argument before '...' before C2X");
    if (!has || (fl & NF_KR))
        return;
    fwd_params(c, f);
    c->stack.len = 0;
    for (k = f; k-- > first;) {
        unsigned t = ntag(c, k);
        if (t == N_SCOPE && c->par[k] == f)
            break;
        if (t == N_FUNC) {
            k = cfirst(c, k);
            continue;
        }
        if (t == N_PARAM && !(cnode(c, k)->flags & NF_FWD)) {
            nparm++;
            vec_push(&c->stack, k);
        } else if ((t == N_STRUCT || t == N_ENUM) && (c->ef[k] & 2)) {
            ntags++;
            vec_push(&c->stack, k);
        }
    }
    if (nparm == 1 && ntags == 0 && lone_void(c, f)) {
        uint32_t p = c->stack.data[0];
        SrcLoc loc = param_loc(c, p);
        if (TYPE_QUALS(c->ty[p]) || (c->cv[p] & 2))
            cerror(c, loc, "'void' as only parameter may not be qualified");
        if (var)
            cerror(c, loc, "'void' must be the only parameter");
        return;
    }
    for (j = 0; j < c->stack.len; j++) {
        uint32_t e = c->stack.data[j];
        if (is_real_param(c, e)) {
            if (!(c->cv[e] & 1) && is_void(c, c->ty[e]) && !is_err(c, c->ty[e])
                && !gave) {
                cerror(c, param_loc(c, e), "'void' must be the only "
                       "parameter");
                gave = true;
            }
        } else {
            TypeId t = c->ty[e];
            TypeKind tk = tkind(c, t);
            uint32_t name = 0;
            SrcLoc loc = 0;
            const char *kw = tk == TY_ENUM ? "enum"
                                           : tk == TY_UNION ? "union" : "struct";
            if (tk == TY_ENUM) {
                Enum *en = type_enum(TT, t);
                if (en) {
                    name = en->tag;
                    loc = en->loc;
                }
            } else if (tk == TY_STRUCT || tk == TY_UNION) {
                Record *r = type_record(TT, t);
                if (r) {
                    name = r->tag;
                    loc = r->loc;
                }
            } else
                continue;
            if (tk == TY_UNION && !name)
                continue;
            if (name)
                cwarn(c, loc, "", "'%s %s' declared inside parameter list will "
                      "not be visible outside of this definition or "
                      "declaration", kw, cident(c, name));
            else
                cwarn(c, loc, "", "anonymous %s declared inside parameter list "
                      "will not be visible outside of this definition or "
                      "declaration", kw);
        }
    }
}

/* ---- type names ---------------------------------------------------------------- */

static void typename_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i), top = NO_NODE, l = last_child(c, i);
    uint32_t after = after_tok(c, i);
    int si;
    Spec sp;
    GDecl g;
    if (l != NO_NODE && is_declarator_tag(ntag(c, l)))
        top = l;
    si = sn != NO_NODE && ntag(c, sn) == N_SPECS ? find_spec(c, sn) : -1;
    if (si < 0) {
        c->ty[i] = ERRT;
        pop_specs(c, i);
        return;
    }
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    c->cd_ltok = after;
    c->cd_clit = c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_COMPOUND_LIT
                     ? after + 1 : 0;
    grok(c, &sp, top, DC_TYPENAME, false, false, NO_NODE, after, after, &g);
    c->cd_clit = 0;
    c->ty[i] = g.what == GD_NONE || sp.error ? ERRT : g.ty;
    pop_specs(c, i);
}

TypeId type_of_typename(Checker *c, uint32_t i)
{
    return c->ty[i];
}

/* ---- function definitions ------------------------------------------------------- */

typedef struct FdParts {
    uint32_t specs, declared, scope, compound, scope_end;
} FdParts;

static void fd_parts(Checker *c, uint32_t fd, FdParts *p)
{
    Kids k;
    uint32_t j;
    p->specs = p->declared = p->scope = p->compound = p->scope_end = NO_NODE;
    kids_get(c, fd, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t n = k.p[j];
        switch (ntag(c, n)) {
        case N_SPECS:
            if (p->specs == NO_NODE)
                p->specs = n;
            break;
        case N_DECLARED:
            if (p->declared == NO_NODE)
                p->declared = n;
            break;
        case N_SCOPE:
            if (p->scope == NO_NODE)
                p->scope = n;
            break;
        case N_COMPOUND:
            p->compound = n;
            break;
        case N_SCOPE_END:
            p->scope_end = n;
            break;
        default:
            break;
        }
    }
    kids_free(&k);
}

uint32_t cdecl_inner_decl(const Checker *c, uint32_t i)
{
    return inner_decl(c, i);
}

uint32_t cdecl_array_size_node(Checker *c, uint32_t a)
{
    return array_size_node(c, a);
}

unsigned cdecl_quals_of(Checker *c, uint32_t node)
{
    return quals_of(c, node);
}

/* The FUNC declarator directly around the name. */
static uint32_t funcdef_fnode(Checker *c, uint32_t top)
{
    uint32_t d = top;
    while (d != NO_NODE && ntag(c, d) != N_NAME) {
        uint32_t in = inner_decl(c, d);
        if (in == NO_NODE)
            return NO_NODE;
        if (ntag(c, d) == N_FUNC && ntag(c, in) == N_NAME)
            return d;
        d = in;
    }
    return NO_NODE;
}

static TypeId plain_type(Checker *c, TypeId t)
{
    return TYPE_UNQUAL(type_canon(TT, t));
}

/* check_main_parameter_types */
static void check_main_params(Checker *c, SrcLoc loc, TypeId ft)
{
    const TypeEnt *e = type_ent(TT, type_canon(TT, ft));
    uint32_t n = e->n, j, argct = 0;
    const char *nm = "main";
    for (j = 0; j < n; j++) {
        TypeId t = type_params(TT, type_canon(TT, ft))[j];
        TypeId b;
        if (is_err(c, t))
            break;
        argct++;
        b = tkind(c, t) == TY_PTR ? type_base(TT, t) : ERRT;
        switch (argct) {
        case 1:
            if (plain_type(c, t) != TYPE_B(INT))
                cpedwarn(c, loc, "main", "first argument of '%s' should be "
                         "'int'", nm);
            break;
        case 2:
        case 3:
            if (tkind(c, t) != TY_PTR || tkind(c, b) != TY_PTR ||
                plain_type(c, type_base(TT, b)) != TYPE_B(CHAR)) {
                if (argct == 2)
                    cpedwarn(c, loc, "main", "second argument of '%s' should "
                             "be 'char **'", nm);
                else
                    cpedwarn(c, loc, "main", "third argument of '%s' should "
                             "probably be 'char **'", nm);
            }
            break;
        default:
            break;
        }
    }
    if (argct > 0 && (argct < 2 || argct > 3))
        cpedwarn(c, loc, "main", "'%s' takes only zero or two arguments", nm);
    if (type_ent(TT, type_canon(TT, ft))->flags & TF_VARIADIC)
        cpedwarn(c, loc, "main", "'%s' declared as variadic function", nm);
}

static bool isnt_proto(Checker *c, const CSym *old)
{
    return !old || !is_prototype(c, old->ty);
}

/* start_function, at the DECLARED node of a FUNC_DEF. */
static void funcdef_declared(Checker *c, uint32_t declared)
{
    uint32_t fd = c->par[declared], top = declared - 1, ltok, fnode, ref, b;
    FdParts fp;
    int si;
    Spec sp;
    GDecl g;
    CSym s, oldc;
    bool have_old = false, nested = c->func_sym != SYM_NONE, pub, is_main, inl;
    SrcLoc loc;
    const char *name;
    TypeId rt;
    fd_parts(c, fd, &fp);
    if (fp.specs == NO_NODE)
        return;
    si = find_spec(c, fp.specs);
    if (si < 0)
        return;
    if (nested && !in_extension(c, fd))
        cpedantic(c, tloc(c, cnode(c, fd)->tok), "ISO C forbids nested "
                  "functions");
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    ltok = after_tok(c, top);
    c->cd_ltok = ltok;
    grok(c, &sp, top, DC_NORMAL, true, true, NO_NODE, ltok, ltok, &g);
    if (g.what != GD_FUNC || !is_func(c, g.s.ty))
        return;
    attrs_unknown_emit(c, &sp.attrs, ltok);
    attrs_alloc_check(c, fp.specs, type_kind(TT, g.s.ty) == TY_FUNC ? g.s.ty :
                      type_canon(TT, g.s.ty), ltok);
    {
        AttrState st = {0};
        attrs_copy_check(c, fp.specs, CS_FUNC, g.s.name, ltok, &st);
        g.s.flags |= (st.pure ? CSF_PURE : 0) | (st.cnst ? CSF_CONSTFN : 0);
        g.s.nonnull |= st.nonnull | sp.attrs.nonnull;
        if (sp.attrs.fmt)
            g.s.fmt = sp.attrs.fmt;
    }
    attrs_section_check(c, &sp.attrs, 'f', false, g.s.name, g.s.loc);
    attrs_zcur_check(c, &sp.attrs, true, g.s.loc);
    attrs_wina_check(c, &sp.attrs, 'f', false, g.s.name, g.s.loc);
    g.s.sect = sp.attrs.sec;
    s = g.s;
    loc = s.loc;
    name = cident(c, s.name);
    if (nested)
        s.linkage = LK_INTERNAL;
    if (sp.attrs.weak)
        weak_apply(c, &s, sp.is_inline);
    /* the return type */
    rt = type_base(TT, s.ty);
    if (!is_err(c, rt) && !is_void(c, rt) && !type_is_complete(TT, rt)) {
        const TypeEnt *e = type_ent(TT, type_canon(TT, s.ty));
        uint32_t n = e->n, flags = e->flags & (TF_VARIADIC | TF_NOPROTO);
        TypeId *ps = xmalloc((n + 1) * sizeof *ps);
        if (n)
            memcpy(ps, type_params(TT, type_canon(TT, s.ty)), n * sizeof *ps);
        cerror(c, loc, "return type is an incomplete type");
        s.ty = type_func(TT, TYPE_B(VOID), ps, n, flags);
        free(ps);
    }
    if (g.default_int)
        cpedwarn(c, loc, "implicit-int", "return type defaults to 'int'");
    b = cbound_here(c, NS_ORD, s.name);
    if (b && csym(c, c->log.data[b - 1].ref)->kind == CS_FUNC) {
        oldc = *csym(c, c->log.data[b - 1].ref);
        have_old = true;
    }
    c->cd_have_proto = false;
    c->cd_proto = 0;
    c->cd_proto_loc = 0;
    if (!is_prototype(c, s.ty)) {
        TypeId oldt = have_old ? oldc.ty : s.ty;
        if (have_old && is_func(c, oldt) &&
            type_compatible(TT, type_base(TT, oldt), type_base(TT, s.ty))) {
            if (type_ent(TT, type_canon(TT, oldt))->flags & TF_VARIADIC) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "", "'%s' defined "
                                        "as variadic function without "
                                        "prototype", name);
                locate_old_decl(c, d, &oldc);
            }
            s.ty = type_composite(TT, oldt, s.ty);
        }
        if (sym_public(&s)) {
            uint32_t e = s.name < c->nidents ? c->ext[s.name] : 0;
            csum_read_ext(c, s.name);
            if (e) {
                const CSym *es = csym(c, e - 1);
                if (is_func(c, es->ty) &&
                    type_compatible(TT, type_base(TT, s.ty),
                                    type_base(TT, es->ty))) {
                    c->cd_proto_loc = es->loc;
                    if (is_prototype(c, es->ty)) {
                        c->cd_have_proto = true;
                        c->cd_proto = es->ty;
                    }
                }
            }
        }
    }
    pub = sym_public(&s);
    is_main = !strcmp(name, "main");
    inl = (s.flags & CSF_INLINE) != 0;
    if (diag_enabled(c->diag, "strict-prototypes") && !is_prototype(c, s.ty) &&
        isnt_proto(c, have_old ? &oldc : NULL))
        cwarn(c, loc, "strict-prototypes", "function declaration isn't a "
              "prototype");
    else if (diag_enabled(c->diag, "missing-prototypes") && pub && !is_main &&
             isnt_proto(c, have_old ? &oldc : NULL) && !inl)
        cwarn(c, loc, "missing-prototypes", "no previous prototype for '%s'",
              name);
    else if (diag_enabled(c->diag, "missing-prototypes") && have_old &&
             (oldc.flags & CSF_USED) && !is_prototype(c, oldc.ty))
        cwarn(c, loc, "missing-prototypes", "'%s' was used with no prototype "
              "before its definition", name);
    else if (diag_enabled(c->diag, "missing-declarations") && pub &&
             !have_old && !is_main && !inl)
        cwarn(c, loc, "missing-declarations", "no previous declaration for "
              "'%s'", name);
    else if (diag_enabled(c->diag, "missing-declarations") && have_old &&
             (oldc.flags & CSF_USED) && (oldc.flags & CSF_IMPLICIT))
        cwarn(c, loc, "missing-declarations", "'%s' was used with no "
              "declaration before its definition", name);
    s.flags |= CSF_DEFINED | CSF_TREE_STATIC;
    s.def_loc = loc;
    fnode = funcdef_fnode(c, top);
    if (fnode != NO_NODE) {
        unsigned ff = cnode(c, fnode)->flags;
        if (!(ff & NF_KR) && (func_has_params(c, fnode) || (ff & NF_VARIADIC)))
            s.flags |= CSF_PROTO_DEF;
        else
            s.flags |= CSF_KR_DEF;
    }
    if (is_main) {
        TypeId r = type_base(TT, s.ty);
        if (plain_type(c, r) != TYPE_B(INT))
            cpedwarn(c, loc, "main", "return type of 'main' is not 'int'");
        else if (TYPE_QUALS(type_canon(TT, r)) & TQ_ATOMIC)
            cpedwarn(c, loc, "main", "'_Atomic'-qualified return type of "
                     "'main'");
        check_main_params(c, loc, s.ty);
        if (!pub)
            cpedwarn(c, loc, "main", "'main' is normally a non-static "
                     "function");
    }
    s.parms = cparm_make(c, funcdef_fnode(c, top));
    ref = pushdecl(c, &s, g.default_int);
    {
        CSym *t = csym(c, ref);
        t->flags |= CSF_DEFINED | CSF_TREE_STATIC |
                    (s.flags & (CSF_PROTO_DEF | CSF_KR_DEF));
        t->def_loc = loc;
    }
    attrs_names(c, fp.specs, &csym(c, ref)->aset);
    if (sp.is_noreturn)
        aset_add(c, &csym(c, ref)->aset, "noreturn", "");
    dump_decl(c, csym(c, ref));
    c->func_sym = ref;
    c->cur_func_node = fd;
    c->ef[fd] = 0;
    c->func_node = fnode;
    c->ty[declared] = csym(c, ref)->ty;
    c->cb[declared] = ref + 1;
    cexpr_record_params(c, declared, ref, true);
    c->cv[declared] = 1;
    ensure_finish_cue(c, ref);
}

static bool in_seen(const uint32_t *seen, uint32_t n, uint32_t ref)
{
    uint32_t j;
    for (j = 0; j < n; j++)
        if (seen[j] == ref)
            return true;
    return false;
}

static void unbind_to(Checker *c, uint32_t mark)
{
    while (c->log.len > mark) {
        Bind *b = &c->log.data[--c->log.len];
        c->top[b->ns][b->ident] = b->prev;
    }
}

/* store_parm_decls, at the BODY leaf. */
static void body_visit(Checker *c, uint32_t i)
{
    uint32_t comp = c->par[i], fd, f, se, j, mark, inner, declared;
    FdParts fp;
    bool krf, proto;
    SrcLoc il, fnloc;
    Kids k;
    if (comp == NO_NODE)
        return;
    if (ntag(c, comp) == N_FUNC_DEF)    /* no '{' followed the declarations */
        fd = comp;
    else if (ntag(c, comp) == N_COMPOUND)
        fd = c->par[comp];
    else
        return;
    if (fd == NO_NODE || ntag(c, fd) != N_FUNC_DEF || c->cur_func_node != fd ||
        c->func_node == NO_NODE)
        return;
    fd_parts(c, fd, &fp);
    declared = fp.declared;
    f = c->func_node;
    il = iloc(c, cnode(c, i)->tok);
    inner = inner_decl(c, f);
    fnloc = inner != NO_NODE ? tloc(c, cnode(c, inner)->tok)
                             : csym(c, c->func_sym)->loc;
    krf = (cnode(c, f)->flags & NF_KR) != 0;
    proto = !krf && (func_has_params(c, f) ||
                     (cnode(c, f)->flags & NF_VARIADIC));
    se = last_child(c, f);
    mark = vec_last(&c->scopes).log;
    if (proto) {
        if (c->log.len > mark) {
            cerror(c, fnloc, "old-style parameter declarations in prototyped "
                   "function definition");
            unbind_to(c, mark);
        }
        kids_get(c, f, &k);
        for (j = 0; j < k.n; j++) {
            uint32_t p = k.p[j];
            CSym *s;
            if (!is_real_param(c, p) || !c->cb[p])
                continue;
            if (!(c->cv[p] & 1) && is_void(c, c->ty[p]) &&
                !is_err(c, c->ty[p]))
                continue;
            s = csym(c, c->cb[p] - 1);
            larger_than(c, s->loc, s->name, s->ty);   /* declared again in the body */
            if (s->name) {
                cbind(c, NS_ORD, s->name, c->cb[p] - 1);
                if (!(s->flags & CSF_USED))
                    warn_if_shadowing(c, s);
            } else
                cpedantic(c, s->loc, "ISO C does not support omitting "
                          "parameter names in function definitions before "
                          "C2X");
        }
        kids_free(&k);
        if (se != NO_NODE && ntag(c, se) == N_SCOPE_END) {
            uint32_t q;
            for (q = 0; q < c->cb[se]; q++) {
                const Bind *bd = &c->saved.data[c->cv[se] + q];
                if (!bd->ident)
                    continue;
                if (bd->ns == NS_ORD) {
                    const CSym *s = csym(c, bd->ref);
                    if (s->kind == CS_OBJ && (s->flags & CSF_PARAM))
                        continue;
                }
                cbind(c, bd->ns, bd->ident, bd->ref);
            }
        }
        return;
    }
    /* old style */
    {
        uint32_t *pl, *seen, np = 0, ns = 0, nk;
        uint32_t cap;
        cwarn(c, fnloc, "old-style-definition", "old-style function "
              "definition");
        if (c->ef[fd] & 1)
            cerror(c, il, "'[*]' not allowed in other than function "
                   "prototype scope");
        kids_get(c, f, &k);
        nk = k.n;
        cap = nk + (uint32_t)(c->log.len - mark) + 2;
        pl = xmalloc(cap * sizeof *pl);
        seen = xmalloc(cap * sizeof *seen);
        for (j = 0; j < nk; j++) {
            uint32_t kn = k.p[j], name, bi, ref;
            CSym *s;
            if (ntag(c, kn) != N_KR_IDENT)
                continue;
            name = cnode_ident(c, kn);
            bi = cbound_here(c, NS_ORD, name);
            if (bi) {
                ref = c->log.data[bi - 1].ref;
                s = csym(c, ref);
                if (s->flags & CSF_ERROR)
                    continue;
                if (!(s->kind == CS_OBJ && (s->flags & CSF_PARAM))) {
                    cerror(c, s->loc, "'%s' declared as a non-parameter",
                           cident(c, name));
                    continue;
                }
                if (in_seen(seen, ns, ref)) {
                    cerror(c, s->loc, "multiple parameters named '%s'",
                           cident(c, name));
                    continue;
                }
                if (is_void(c, s->ty)) {
                    cerror(c, s->loc, "parameter '%s' declared with void type",
                           cident(c, name));
                    s->ty = TYPE_B(INT);
                }
                warn_if_shadowing(c, s);
            } else {
                CSym n;
                memset(&n, 0, sizeof n);
                n.name = name;
                n.kind = CS_OBJ;
                n.flags = CSF_PARAM | CSF_DEFINED;
                n.ty = TYPE_B(INT);
                n.loc = fnloc;
                ref = pushdecl(c, &n, false);
                warn_if_shadowing(c, csym(c, ref));
                DiagOrd o0 = diag_ord(c->diag, ORD_LATE);
                cpedwarn(c, fnloc, "implicit-int", "type of '%s' defaults to "
                         "'int'", cident(c, name));
                diag_ord(c->diag, o0);
            }
            seen[ns++] = ref;
            pl[np++] = ref;
        }
        kids_free(&k);
        for (j = (uint32_t)c->log.len; j-- > mark;) {
            const Bind *bd = &c->log.data[j];
            CSym *s;
            if (bd->ns != NS_ORD)
                continue;
            s = csym(c, bd->ref);
            if (!(s->kind == CS_OBJ && (s->flags & CSF_PARAM)))
                continue;
            if (!is_err(c, s->ty) && !type_is_complete(TT, s->ty)) {
                cerror(c, s->loc, "parameter '%s' has incomplete type",
                       sname(c, s));
                s->ty = ERRT;
            }
            if (!in_seen(seen, ns, bd->ref)) {
                DiagOrd o0 = diag_ord(c->diag, ORD_LATE);
                cerror(c, s->loc, "declaration for parameter '%s' but no "
                       "such parameter", sname(c, s));
                diag_ord(c->diag, o0);
                if (np < cap)
                    pl[np++] = bd->ref;
            }
        }
        if (c->cd_have_proto) {
            TypeId pt = type_canon(TT, c->cd_proto);
            uint32_t n = type_ent(TT, pt)->n, li, pi;
            bool variadic = (type_ent(TT, pt)->flags & TF_VARIADIC) != 0;
            TypeId *pp = xmalloc((n + 1) * sizeof *pp);
            if (n)
                memcpy(pp, type_params(TT, pt), n * sizeof *pp);
            for (li = 0, pi = 0;; li++, pi++) {
                bool parm = li < np, tyvalid = pi < n;
                CSym *s;
                TypeId at, vt;
                if (!(parm || (tyvalid && !is_err(c, pp[pi]))))
                    break;
                (void)variadic;
                if (!parm || !tyvalid) {
                    cerror(c, il, "number of arguments doesn't match "
                           "prototype");
                    cerror(c, c->cd_proto_loc, "prototype declaration");
                    break;
                }
                s = csym(c, pl[li]);
                vt = pp[pi];
                if (is_err(c, s->ty) || is_err(c, vt))
                    continue;
                at = type_default_promote(TT, s->ty);
                if (!type_compatible(TT, plain_type(c, at),
                                     plain_type(c, vt)) ||
                    ((TYPE_QUALS(type_canon(TT, at)) & TQ_ATOMIC) !=
                     (TYPE_QUALS(type_canon(TT, vt)) & TQ_ATOMIC))) {
                    if ((TYPE_QUALS(type_canon(TT, at)) & TQ_ATOMIC) ==
                            (TYPE_QUALS(type_canon(TT, vt)) & TQ_ATOMIC) &&
                        plain_type(c, s->ty) == plain_type(c, vt)) {
                        cpedantic(c, s->loc, "promoted argument '%s' doesn't "
                                  "match prototype", sname(c, s));
                        cpedantic(c, c->cd_proto_loc, "prototype "
                                  "declaration");
                    } else {
                        cerror(c, s->loc, "argument '%s' doesn't match "
                               "prototype", sname(c, s));
                        cerror(c, c->cd_proto_loc, "prototype declaration");
                    }
                }
            }
            free(pp);
        } else if (np > 0) {
            CSym *fs = csym(c, c->func_sym);
            if (is_func(c, fs->ty) && !is_prototype(c, fs->ty)) {
                TypeId *ps = xmalloc(np * sizeof *ps), nt;
                for (j = 0; j < np; j++)
                    ps[j] = TYPE_UNQUAL(type_default_promote(
                        TT, csym(c, pl[j])->ty));
                nt = type_func(TT, type_base(TT, fs->ty), ps, np, TF_NOPROTO);
                free(ps);
                fs = csym(c, c->func_sym);
                fs->ty = nt;
                if (declared != NO_NODE)
                    c->ty[declared] = nt;
            }
        }
        free(pl);
        free(seen);
    }
}

/* ---- unused things --------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* The identifier e names, as a target of a plain assignment: not a read. */
static void wr_mark(Checker *c, uint32_t e)
{
    for (;;) {
        switch (ntag(c, e)) {
        case N_IDENT:
            vec_push(&c->stack, e);
            return;
        case N_PAREN:
            e = first_child(c, e);
            continue;
        case N_MEMBER_EXPR:
            if (!(cnode(c, e)->flags & NF_ARROW)) {
                e = first_child(c, e);
                continue;
            }
            return;
        case N_INDEX: {
            uint32_t base = first_child(c, e);
            if (base != NO_NODE && is_arr(c, c->ty[base])) {
                e = base;
                continue;
            }
            return;
        }
        default:
            return;
        }
    }
}

/* An expression whose value is not used. */
static void st_mark(Checker *c, uint32_t e)
{
    while (ntag(c, e) == N_PAREN)
        e = first_child(c, e);
    if (ntag(c, e) == N_ASSIGN && tpunct(c, cnode(c, e)->tok) == P_ASSIGN) {
        wr_mark(c, first_child(c, e));
    } else if (ntag(c, e) == N_BINARY &&
               tpunct(c, cnode(c, e)->tok) == P_COMMA) {
        Kids k;
        kids_get(c, e, &k);
        if (k.n == 2) {
            st_mark(c, k.p[0]);
            st_mark(c, k.p[1]);
        }
        kids_free(&k);
    }
}

/* Which of the names are read (any use but assigning with '=') in nodes
 * [first, last]. */
static void read_scan(Checker *c, uint32_t first, uint32_t last,
                      const uint32_t *names, uint8_t *read, uint32_t n)
{
    uint32_t k, j;
    c->stack.len = 0;
    for (k = first; k <= last; k++) {
        if (ntag(c, k) == N_EXPR_STMT) {
            uint32_t e = first_child(c, k);
            if (e != NO_NODE)
                st_mark(c, e);
        } else if (ntag(c, k) == N_FOR) {
            Kids kk;
            kids_get(c, k, &kk);
            if (kk.n >= 4) {
                if (cexpr_is_expr(ntag(c, kk.p[1])))
                    st_mark(c, kk.p[1]);
                if (cexpr_is_expr(ntag(c, kk.p[3])))
                    st_mark(c, kk.p[3]);
            }
            kids_free(&kk);
        }
    }
    if (c->stack.len)
        qsort(c->stack.data, c->stack.len, sizeof(uint32_t), cmp_u32);
    for (k = first; k <= last; k++) {
        uint32_t id;
        if (ntag(c, k) != N_IDENT)
            continue;
        if (c->stack.len &&
            bsearch(&k, c->stack.data, c->stack.len, sizeof(uint32_t),
                    cmp_u32))
            continue;
        id = cnode_ident(c, k);
        for (j = 0; j < n; j++)
            if (names[j] == id)
                read[j] = 1;
    }
    c->stack.len = 0;
}

/* The warnings of pop_scope for the block of nodes [first, last] (a SCOPE
 * to its SCOPE_END, or to the end of a function's body). */
static void unused_scan(Checker *c, uint32_t first, uint32_t last)
{
    uint32_t k, nc = 0, cap = 0, j;
    uint32_t *cand = NULL, *names = NULL;
    uint8_t *read = NULL;
    int depth = 0;
    bool need_read = false;
    for (k = first; k <= last; k++) {
        unsigned t = ntag(c, k);
        if (t == N_SCOPE)
            depth++;
        else if (t == N_SCOPE_END)
            depth--;
        else if (t == N_DECLARED && depth == 1 && c->cb[k]) {
            if (nc == cap) {
                cap = cap ? cap * 2 : 8;
                cand = xrealloc(cand, cap * sizeof *cand);
            }
            cand[nc++] = k;
        }
    }
    if (!nc) {
        cstmt_emit_labels(c, first, -1);
        return;
    }
    names = xmalloc(nc * sizeof *names);
    read = xcalloc(nc, 1);
    for (j = 0; j < nc; j++) {
        const CSym *s = csym(c, c->cb[cand[j]] - 1);
        names[j] = s->name;
        if (s->kind == CS_OBJ && !(s->flags & CSF_PARAM) && s->name &&
            (s->flags & CSF_USED) && !sym_public(s))
            need_read = true;
    }
    if (need_read && diag_enabled(c->diag, "unused-but-set-variable"))
        read_scan(c, first, last, names, read, nc);
    else
        memset(read, 1, nc);
    for (j = nc; j-- > 0;) {
        CSym *s = csym(c, c->cb[cand[j]] - 1);
        cstmt_emit_labels(c, first, (int64_t)cand[j]);
        if (s->kind == CS_OBJ && !(s->flags & CSF_PARAM) && s->name) {
            if (!(s->flags & CSF_USED)) {
                DiagOrd o0 = diag_ord(c->diag, ORD_LATE); /* reported when the scope closes */
                cwarn(c, s->loc, "unused-variable", "unused variable '%s'",
                      sname(c, s));
                diag_ord(c->diag, o0);
                if (sym_public(s))
                    s->flags |= CSF_USED;
            } else if (!read[j] && !sym_public(s) &&
                       !(s->flags & CSF_ATTR_UNUSED))
                cwarn(c, s->loc, "unused-but-set-variable", "variable '%s' set "
                      "but not used", sname(c, s));
        } else if (s->kind == CS_FUNC && !sym_public(s) && !sym_defined(s) &&
                   s->name && !ref_file_scope(c->cb[cand[j]] - 1))
            cerror(c, s->loc, "nested function '%s' declared but never "
                   "defined", sname(c, s));
    }
    cstmt_emit_labels(c, first, -1);
    free(cand);
    free(names);
    free(read);
}

/* A compound statement or a for statement closed its scope. */
static void block_scope_end(Checker *c, uint32_t i)
{
    uint32_t f = cfirst(c, i);
    if (ntag(c, f) != N_SCOPE)
        return;
    unused_scan(c, f, i - 1);
}

/* finish_function, at the SCOPE_END of a function's scope. */
void cdecl_func_end(Checker *c, uint32_t se)
{
    uint32_t fd = c->par[se], f, j, np = 0, nk, p;
    FdParts fp;
    uint32_t *pl, *names;
    uint8_t *read;
    Kids k;
    if (fd == NO_NODE || ntag(c, fd) != N_FUNC_DEF || c->cur_func_node != fd ||
        c->func_node == NO_NODE)
        return;
    fd_parts(c, fd, &fp);
    f = c->func_node;
    if (fp.scope != NO_NODE)
        unused_scan(c, fp.scope, se - 1);
    /* the parameters, in order */
    kids_get(c, f, &k);
    nk = k.n;
    pl = xmalloc((nk + 1) * sizeof *pl);
    for (j = 0; j < nk; j++) {
        uint32_t kn = k.p[j];
        if (is_real_param(c, kn)) {
            if (c->cb[kn] && (c->cv[kn] & 1) &&
                !in_seen(pl, np, c->cb[kn] - 1))
                pl[np++] = c->cb[kn] - 1;
        } else if (ntag(c, kn) == N_KR_IDENT) {
            uint32_t bi = cbound_here(c, NS_ORD, cnode_ident(c, kn));
            if (bi) {
                uint32_t ref = c->log.data[bi - 1].ref;
                const CSym *s = csym(c, ref);
                if (s->kind == CS_OBJ && (s->flags & CSF_PARAM) &&
                    !in_seen(pl, np, ref))
                    pl[np++] = ref;
            }
        }
    }
    kids_free(&k);
    names = xmalloc((np + 1) * sizeof *names);
    read = xcalloc(np + 1, 1);
    for (j = 0; j < np; j++)
        names[j] = csym(c, pl[j])->name;
    if (np && fp.scope != NO_NODE &&
        diag_enabled(c->diag, "unused-but-set-parameter"))
        read_scan(c, fp.scope, se - 1, names, read, np);
    else
        memset(read, 1, np);
    for (j = 0; j < np; j++) {
        const CSym *s = csym(c, pl[j]);
        if ((s->flags & CSF_USED) && !read[j] &&
            !(s->flags & CSF_ATTR_UNUSED))
            cwarn(c, s->loc, "unused-but-set-parameter", "parameter '%s' set "
                  "but not used", sname(c, s));
    }
    /* typedefs made in the function and never used */
    if (fp.scope != NO_NODE && c->diag->nerrors == 0 &&
        diag_enabled(c->diag, "unused-local-typedefs")) {
        uint32_t q;
        for (q = fp.scope; q < se; q++)
            if (ntag(c, q) == N_DECLARED && c->cb[q]) {
                CSym *s = csym(c, c->cb[q] - 1);
                if (s->kind == CS_TYPEDEF && s->name && !(s->flags & CSF_USED)) {
                    cwarn(c, s->loc, "unused-local-typedefs", "typedef '%s' "
                          "locally defined but not used", sname(c, s));
                    s->flags |= CSF_USED;
                }
            }
    }
    for (j = 0; j < np; j++) {
        const CSym *s = csym(c, pl[j]);
        if (s->name && !(s->flags & CSF_USED))
            cwarn(c, s->loc, "unused-parameter", "unused parameter '%s'",
                  sname(c, s));
    }
    free(pl);
    free(names);
    free(read);
    /* back to the enclosing function, if any */
    p = c->par[fd];
    while (p != NO_NODE && ntag(c, p) != N_FUNC_DEF)
        p = c->par[p];
    c->func_sym = SYM_NONE;
    c->cur_func_node = NO_NODE;
    c->func_node = NO_NODE;
    if (p != NO_NODE) {
        FdParts op;
        fd_parts(c, p, &op);
        if (op.declared != NO_NODE && c->cb[op.declared]) {
            c->func_sym = c->cb[op.declared] - 1;
            c->cur_func_node = p;
            c->func_node = funcdef_fnode(c, op.declared - 1);
        }
    }
}

void cdecl_body_scope(Checker *c, uint32_t scope)
{
    (void)c;
    (void)scope;
}

/* ---- the end of the translation unit ----------------------------------------- */

void cdecl_record_inline_static(Checker *c, SrcLoc loc, uint32_t name,
                                bool modifiable)
{
    InlStatic is;
    if (c->func_sym == SYM_NONE || !extern_inline(csym(c, c->func_sym)))
        return;
    is.loc = loc;
    is.fref = c->func_sym;
    is.name = name;
    is.modifiable = modifiable;
    vec_push(&c->inl_statics, is);
}

/* check_inline_statics: newest first, and only the functions that are still
 * inline definitions. */
void cdecl_check_inline_statics(Checker *c)
{
    size_t k;
    for (k = c->inl_statics.len; k-- > 0;) {
        const InlStatic *is = &c->inl_statics.data[k];
        const CSym *f = csym(c, is->fref);
        if (!extern_inline(f))
            continue;
        cpedwarn(c, is->loc, "", is->modifiable
                 ? "'%s' is static but declared in inline function '%s' which "
                   "is not static"
                 : "'%s' is static but used in inline function '%s' which is "
                   "not static", cident(c, is->name), sname(c, f));
    }
}

void cdecl_finish_object(Checker *c, uint32_t ref)
{
    size_t k;
    if (!c->tentative.len || ref != c->tentative.data[0])
        return;
    /* incomplete types of objects that are never completed */
    for (k = 0; k < c->tentative.len; k++) {
        CSym *s = csym(c, c->tentative.data[k]);
        if (s->kind == CS_OBJ && (s->flags & CSF_TREE_STATIC) &&
            !is_err(c, s->ty) && !is_void(c, s->ty) && !type_is_complete(TT, s->ty) &&
            !is_incomplete_array(c, s->ty)) {
            cerror(c, s->loc, "storage size of '%s' isn't known", sname(c, s));
            s->ty = ERRT;
        }
    }
    /* the external scope: inline functions never defined */
    for (k = c->gsyms.len; k-- > 0;) {
        const CSym *s = &c->gsyms.data[k];
        if (s->kind == CS_FUNC && (s->flags & CSF_INLINE) && sym_public(s) &&
            !sym_defined(s) && s->name)
            cpedwarn(c, s->loc, "", "inline function '%s' declared but never "
                     "defined", sname(c, s));
    }
    /* the file scope */
    for (k = 0; k < c->gsyms.len; k++) {
        const CSym *s = &c->gsyms.data[k];
        if (s->kind == CS_OBJ && (s->flags & CSF_INNER_COMP) && s->name)
            cerror(c, s->loc, "type of array '%s' completed incompatibly with "
                   "implicit initialization", sname(c, s));
        if (s->kind == CS_FUNC && !sym_public(s) && !sym_defined(s) &&
            (s->flags & CSF_DECL_EXTERNAL) && s->name &&
            !cdecl_aset_has(c, s->aset, "weakref", NULL)) {
            if (s->flags & CSF_USED)
                cpedwarn(c, s->loc, "", "'%s' used but never defined",
                         sname(c, s));
            else
                cwarn(c, s->loc, "unused-function", "'%s' declared 'static' "
                      "but never defined", sname(c, s));
        }
    }
    {
        int pass;
        for (pass = 0; pass < 2; pass++)
            for (k = 0; k < c->gsyms.len; k++) {
                CSym *s = &c->gsyms.data[k];
                if (s->kind == CS_OBJ && (s->flags & CSF_TENTATIVE) &&
                    !(s->flags & CSF_DECL_EXTERNAL) && s->name &&
                    sym_public(s) == (pass == 1) &&
                    is_incomplete_array(c, s->ty)) {
                    cwarn(c, s->loc, "", "array '%s' assumed to have one "
                          "element", sname(c, s));
                    s->ty = type_array(TT, type_base(TT, s->ty), 1);
                }
            }
    }
    for (k = 0; k < c->gsyms.len; k++) {
        const CSym *s = &c->gsyms.data[k];
        if (s->kind == CS_OBJ && s->sc == SC_REGISTER &&
            !(s->flags & (CSF_REGISTER_NAMED | CSF_PARAM)) && s->name &&
            !is_err(c, s->ty))
            cerror(c, s->loc, "register name not specified for '%s'",
                   sname(c, s));
    }
}

/* ---- the dispatch ------------------------------------------------------------- */

void cdecl_node(Checker *c, uint32_t i)
{
    switch (ntag(c, i)) {
    case N_SPECS:
        specs_visit(c, i);
        break;
    case N_EMPTY:
        if (cat_file_scope(c))
            cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C does not allow "
                      "extra ';' outside of a function");
        break;
    case N_FUNC_DEF:
        pop_specs(c, i);
        break;
    case N_DECL:
        decl_visit(c, i);
        break;
    case N_GOTO_EXPR:
        cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C forbids 'goto *expr;'");
        break;
    case N_DECLARED: {
        /* gcc declares the name before it parses the initializer: a
         * conflict is reported even if the initializer has a syntax error */
        bool quiet = c->quiet;
        c->quiet = quiet && i + 1 < c->nn && ntag(c, i + 1) == N_INIT_DECL;
        DiagOrd o0 = diag_ord(c->diag, ORD_EARLY);
        declared_visit(c, i);
        diag_ord(c->diag, o0);
        c->quiet = quiet;
        cinit_declared(c, i);
        break;
    }
    case N_INIT_DECL:
        init_decl_visit(c, i);
        break;
    case N_ATTRIBUTE: {
        /* parser diagnostics: gcc gives them in units with errors too */
        bool quiet = c->quiet;
        c->quiet = false;
        if (tokp(c, cnode(c, i)->tok)->kind == TK_PUNCT)
            cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C does not support "
                      "'[[]]' attributes before C2X");
        std_attr_unknown(c, i);
        gnu_attr_argc(c, i);
        sso_check(c, i);
        c->quiet = quiet;
        break;
    }
    case N_STRUCT:
    case N_ENUM:
        struct_visit(c, i);
        break;
    case N_TAG:
        tag_visit(c, i);
        break;
    case N_OPEN:
        open_visit(c, i);
        break;
    case N_ENUMERATOR:
        enumerator_visit(c, i);
        break;
    case N_MEMBER:
        member_visit(c, i);
        break;
    case N_MEMBER_DECL:
        member_decl_visit(c, i);
        break;
    case N_STATIC_ASSERT:
        static_assert_visit(c, i);
        break;
    case N_PRAGMA:
        pragma_visit(c, i);
        break;
    case N_PARAM:
        param_visit(c, i);
        break;
    case N_FUNC:
        func_visit(c, i);
        break;
    case N_ARRAY:
        array_visit(c, i);
        break;
    case N_TYPE_NAME:
        typename_visit(c, i);
        break;
    case N_BODY:
        body_visit(c, i);
        break;
    case N_COMPOUND:
    case N_FOR:
        block_scope_end(c, i);
        break;
    default:
        break;
    }
}
