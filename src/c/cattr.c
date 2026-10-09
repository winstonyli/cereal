/* cattr.c - the checker's attributes: collecting, validating and naming
 * them (split from cdecl.c).  Declaration specifiers, declarators and the
 * rest stay in cdecl.c; the shared readers are cdecl_int.h's. */
#include "c/cdecl_int.h"

/* ---- attributes ---------------------------------------------------------- */

static void pos_arg_str(Checker *c, uint32_t arg, char *buf, size_t n);

/* The format types gcc's C front end knows (c-format.cc format_types, minus
 * the ones for other targets), besides printf and scanf. */
/* The format attribute kinds cformat.c checks: 1 printf, 2 scanf, 3 strftime,
 * 4 strfmon (0: any other). */
static int format_kind(const char *ar)
{
    static const char *const nm[] = {"printf", "scanf", "strftime", "strfmon"};
    int k;
    for (k = 0; k < 4; k++)
        if (!strcmp(ar, nm[k]) ||
            (!strncmp(ar, "gnu_", 4) && !strcmp(ar + 4, nm[k])))
            return k + 1;
    return 0;
}

enum { PA_INT, PA_STR, PA_REST };   /* what positional_arg's parameter must be */

/* A pointer to (any qualification of) plain char. */
static bool is_char_ptr(Checker *c, TypeId t)
{
    TypeId b;
    if (type_ckind(TT, t) != TY_PTR)
        return false;
    b = type_canon(TT, type_base(TT, type_canon(TT, t)));
    return type_kind(TT, TYPE_UNQUAL(b)) == TY_CHAR;
}

static bool format_type_known(const char *n)
{
    static const char *const ok[] = {"strftime", "gnu_strftime", "strfmon",
        "gnu_strfmon", "gcc_diag", "gcc_cdiag", "gcc_cxxdiag", "gcc_tdiag",
        "asm_fprintf"};
    size_t k;
    for (k = 0; k < sizeof ok / sizeof *ok; k++)
        if (!strcmp(n, ok[k]))
            return true;
    return false;
}

/* gcc reports patchable_function_entry at the first token of the line of the
 * declarator (the attr_at override when the checker has one). */
static SrcLoc patchable_loc(Checker *c, uint32_t item)
{
    uint32_t at = c->nodes[item].tok, up = item, lv, j;
    Kids sib;
    for (lv = 0; lv < 3 && up != NO_NODE && at == c->nodes[item].tok; lv++) {
        up = c->par[up];
        if (up == NO_NODE)
            break;
        kids_get(c, up, &sib);   /* a leading list is applied at the declarator */
        for (j = 0; j < sib.n; j++)
            if (sib.p[j] > item && (ntag(c, sib.p[j]) == N_DECLARED ||
                                    ntag(c, sib.p[j]) == N_INIT_DECL)) {
                at = c->nodes[cfirst(c, sib.p[j])].tok;
                break;
            }
        kids_free(&sib);
    }
    return cdecl_line_start_loc(c, at);
}

static bool access_check(Checker *c, const uint32_t *arg, uint32_t n, SrcLoc il,
                         TypeId ft);

/* The attribute's name without the leading and trailing "__". */
void attr_norm(const char *s, char *out, size_t n)
{
    size_t len = strlen(s);
    if (!strcmp(s, "__const"))      /* a keyword: gcc names it by its RID, "const" */
        s += 2, len -= 2;
    else if (len > 4 && s[0] == '_' && s[1] == '_' && s[len - 1] == '_' &&
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
/* The aligned(0) attribute last warned about (a struct's are seen twice). */
static uint32_t zero_warn_u, zero_warn_node;

static uint32_t check_user_alignment_(Checker *c, uint32_t e, SrcLoc loc,
                                      bool objfile);

uint32_t check_user_alignment(Checker *c, uint32_t e, SrcLoc loc,
                                     bool objfile)
{
    uint32_t v = check_user_alignment_(c, e, loc, objfile);
    if (!v && c->ck[e] != K_ERR) {
        c->align_err_u = c->u->first_tok + 1;
        c->align_err_node = e;
    }
    return v;
}

static uint32_t check_user_alignment_(Checker *c, uint32_t e, SrcLoc loc,
                                      bool objfile)
{
    TypeId t;
    int64_t v;
    uint64_t uv;
    if (c->ck[e] == K_ERR)
        return 0;
    if (c->align_err_u == c->u->first_tok + 1 && c->align_err_node == e)
        return 0;               /* already diagnosed */
    t = c->ty[e];
    if (!(c->ck[e] == K_ICE || (c->ck[e] == K_FOLD && (c->ef[e] & EF_CST))) ||
        !type_is_integer(TT, t)) {
        cerror(c, loc, "requested alignment is not an integer constant");
        return 0;
    }
    uv = (uint64_t)cexpr_sval(c, e);
    if (uv == 0)
        return 0;
    if ((type_is_signed(TT, t) && (int64_t)uv < 0) || !is_pow2(uv)) {
        if (type_is_signed(TT, t))
            cerror(c, loc, "requested alignment '%lld' is not a positive "
                   "power of 2", (long long)uv);
        else
            cerror(c, loc, "requested alignment '%llu' is not a positive "
                   "power of 2", (unsigned long long)uv);
        return 0;
    }
    if (objfile && uv > ((uint64_t)1 << 28)) {
        cerror(c, loc, "requested alignment '%llu' exceeds object file "
               "maximum %u", (unsigned long long)uv, 1U << 28);
        return 0;
    }
    if (uv >= ((uint64_t)1 << 29)) {
        cerror(c, loc, "requested alignment '%llu' exceeds maximum %u",
               (unsigned long long)uv, 1U << 28);
        return 0;
    }
    v = (int64_t)uv;
    return (uint32_t)v;
}

/* handle_*_attribute argument counts: "wrong number of arguments", at
 * input_location. */
void gnu_attr_argc(Checker *c, uint32_t attr)
{
    static const struct { const char *n; uint32_t lo, hi; } t[] = {
        {"access", 1, 3}, {"alloc_align", 1, 1}, {"assume_aligned", 1, 2}, {"constructor", 0, 1},
        {"copy", 1, 1}, {"destructor", 0, 1},
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
bool attr_scope_of(Checker *c, uint32_t tok)
{
    return c->opt.gnu && tpunct(c, tok + 1) == P_COLONCOLON;
}

/* gcc's c_parser_std_attribute: a name without a namespace that is not one
 * of the standard attributes is pedwarned and dropped, at input_location
 * with the lookahead just past the name and its arguments.  (Before C2X
 * 'ns::name' is not parsed: 'ns' is such a name.) */
static const char *const std_known[] = {"deprecated", "fallthrough",
    "maybe_unused", "nodiscard", "noreturn", "_Noreturn"};

/* An unscoped [[name]] outside the standard set: gcc drops it after the
 * warning std_attr_unknown gives, so nothing else may see it. */
static bool std_attr_dropped(Checker *c, uint32_t attr, uint32_t item)
{
    char name[48];
    uint32_t at = c->nodes[item].tok;
    size_t n;
    if (tokp(c, cnode(c, attr)->tok)->kind != TK_PUNCT ||
        (c->opt.gnu && at >= 1 && tpunct(c, at - 1) == P_COLONCOLON) ||
        attr_scope_of(c, at))
        return false;
    attr_norm(tstr(c, at), name, sizeof name);
    for (n = 0; n < sizeof std_known / sizeof *std_known; n++)
        if (!strcmp(name, std_known[n]))
            return false;
    return true;
}

void std_attr_unknown(Checker *c, uint32_t attr)
{
    const char *const *known = std_known;
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
        if (c->opt.gnu && at >= 1 && tpunct(c, at - 1) == P_COLONCOLON)
            continue;           /* gnu::name, taken as a GNU attribute */
        for (n = 0; n < sizeof std_known / sizeof *std_known; n++)
            if (!strcmp(name, known[n]))
                break;
        if (n < sizeof std_known / sizeof *std_known)
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
bool decl_has_dep_attr(Checker *c, uint32_t d)
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

bool attr_known(const char *name)
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

bool cdecl_attr_known(const char *name)
{
    return attr_known(name);
}

/* malloc (dealloc[, pos]): the deallocator must name a function whose
 * parameter number pos (default 1) is a pointer. */
static void attr_malloc_dealloc(Checker *c, uint32_t arg, uint32_t pos,
                                SrcLoc loc)
{
    TypeId ft;
    uint32_t dr;
    if (ntag(c, arg) != N_IDENT || type_ckind(TT, c->ty[arg]) != TY_FUNC) {
        cerror(c, loc, "'malloc' attribute argument 1 does not name a "
               "function");
        return;
    }
    ft = c->ty[arg];
    if (type_ckind(TT, ft) != TY_FUNC)
        return;
    /* an inline deallocator: with optimization any, else always_inline */
    dr = lookup_ord(c, cnode_ident(c, arg));
    if (dr != SYM_NONE && (csym(c, dr)->flags & CSF_INLINE) &&
        (c->opt.optimize ||
         cdecl_aset_has(c, csym(c, dr)->aset, "always_inline", NULL)))
        cwarn(c, loc, "attributes", "'malloc (%s)' attribute ignored with "
              "deallocation functions declared 'inline'",
              cident(c, cnode_ident(c, arg)));
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


/* 1: big-endian, 2: little-endian, 0: anything else. */
static int sso_value(Checker *c, uint32_t arg)
{
    uint32_t mi = cdep_msg(c, arg);
    const char *sv = c->dep_msgs.data[mi - 1];
    return !strcmp(sv, "big-endian") ? 1 : !strcmp(sv, "little-endian") ? 2 : 0;
}

/* The scalar_storage_order given on a tag node (`struct S __attribute__`). */
uint8_t sso_of_tag(Checker *c, uint32_t n)
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
void sso_check(Checker *c, uint32_t attr)
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
/* gcc words an over-large aligned() as the object file maximum when the
 * attribute lands on a variable with static storage or on a function, and
 * as the plain maximum on a type (typedef, tag, field, parameter, auto). */
static bool attr_on_object(Checker *c, uint32_t attr)
{
    uint32_t p = c->par[attr], d, up, j;
    Kids k;
    bool storage = false, ext = false;
    if (p == NO_NODE)
        return false;
    if (ntag(c, p) == N_INIT_DECL)
        d = c->par[p];
    else if (ntag(c, p) == N_SPECS)
        d = c->par[p];
    else
        return false;
    if (d == NO_NODE || ntag(c, d) != N_DECL)
        return false;
    kids_get(c, d, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_SPECS) {
            Kids sk;
            uint32_t q;
            kids_get(c, k.p[j], &sk);
            for (q = 0; q < sk.n; q++)
                if (ntag(c, sk.p[q]) == N_STORAGE) {
                    int kw = tckw(c, c->nodes[sk.p[q]].tok);
                    if (kw == CK_TYPEDEF) {
                        kids_free(&sk);
                        kids_free(&k);
                        return false;
                    }
                    if (kw == CK_STATIC || kw == CK_EXTERN)
                        storage = true;
                    ext |= kw == CK_EXTERN;
                }
            kids_free(&sk);
        }
    kids_free(&k);
    (void)ext;
    for (up = c->par[d]; up != NO_NODE; up = c->par[up])
        if (ntag(c, up) == N_COMPOUND || ntag(c, up) == N_PARAM ||
            ntag(c, up) == N_MEMBER_DECL || ntag(c, up) == N_STRUCT)
            return storage;
    return true;
}

void attr_collect(Checker *c, uint32_t attr, Attrs *a)
{
    Kids k;
    uint32_t j;
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t item = k.p[j], arg;
        char name[48];
        Kids ak;
        if (ntag(c, item) != N_ATTR_ITEM || std_attr_dropped(c, attr, item))
            continue;
        attr_norm(tstr(c, c->nodes[item].tok), name, sizeof name);
        if (cnode_tok(c, item)->kind == TK_IDENT &&
            ident_by_id(c->in, cnode_tok(c, item)->aux)->ext)
            snprintf(name, sizeof name, "%s", cident(c, cnode_tok(c, item)->aux));
        kids_get(c, item, &ak);
        arg = ak.n ? ak.p[0] : NO_NODE;
        if (arg != NO_NODE && (!strcmp(name, "alias") || !strcmp(name, "ifunc") ||
                               !strcmp(name, "weakref")))
            a->defn = true;
        if (c->attr_defer && !c->attr_quiet && strcmp(name, "gnu") &&
            !attr_known(name) && !attr_scope_of(c, c->nodes[item].tok)) {
            if (a->nunk < 2)      /* `__int128__` is the keyword's spelling */
                snprintf(a->unk[a->nunk++], sizeof a->unk[0], "%s",
                         strcmp(name, "int128") ? name : "__int128");
        } else if (!c->attr_quiet && strcmp(name, "gnu") && !attr_known(name) &&
                   !attr_scope_of(c, c->nodes[item].tok))   /* gnu:: is a [[]] scope */
            cwarn(c, c->attr_at_set ? c->attr_at : iloc(c, c->nodes[item].tok),
                  "attributes", "'%s' attribute directive ignored",
                  strcmp(name, "int128") ? name : "__int128");
        if (!strcmp(name, "aligned")) {
            uint32_t v;
            if (arg == NO_NODE)
                v = c->tgt->default_aligned;
            else
                v = check_user_alignment(c, arg, iloc(c, after_tok(c, attr)),
                                         attr_on_object(c, attr));
            if (!v && arg != NO_NODE && c->ck[arg] != K_ERR &&
                (c->ck[arg] == K_ICE ||
                 (c->ck[arg] == K_FOLD && (c->ef[arg] & EF_CST))) &&
                type_is_integer(TT, c->ty[arg]) && cexpr_sval(c, arg) == 0 &&
                !(zero_warn_u == c->u->first_tok + 1 &&
                  zero_warn_node == arg)) {         /* once per attribute */
                cwarn(c, iloc(c, after_tok(c, attr)), "attributes",
                      "requested alignment '0' is not a positive power of 2");
                zero_warn_u = c->u->first_tok + 1;
                zero_warn_node = arg;
            }
            if (v > a->aligned)
                a->aligned = v;
        } else if (!strcmp(name, "warn_if_not_aligned")) {
            a->wina = true;
            if (arg != NO_NODE) {
                uint32_t v = check_user_alignment(c, arg,
                                                  iloc(c, after_tok(c, attr)),
                                                  attr_on_object(c, attr));
                if (v > a->wina_al)
                    a->wina_al = (uint16_t)v;
            } else
                a->wina_al = 16;     /* BIGGEST_ALIGNMENT */
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
                    !strcmp(name, "destructor")) && arg != NO_NODE &&
                   c->ck[arg] != K_ERR && ak.n <= 1) {  /* else: wrong number */
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
        } else if (!strcmp(name, "optimize") && arg != NO_NODE &&
                   ntag(c, arg) == N_STRING) {
            uint32_t m = cdep_msg(c, arg);  /* pushes: read it, then drop */
            const char *o = c->dep_msgs.data[m - 1];
            char no[104];
            char one[96];
            const char *s = o;
            bool bad = false;
            while (*s && !bad) {
                size_t l = strcspn(s, ", ");
                if (l && l < sizeof one) {
                    memcpy(one, s, l);
                    one[l] = 0;
                    bad = cpragma_optimize_bad(one, no, sizeof no);
                }
                s += l + (s[l] != 0);
            }
            if (bad)
                cwarn(c, cinput_loc(c, c->nodes[item].tok), "attributes",
                      "bad option '%s' to attribute 'optimize'", no);
            free(c->dep_msgs.data[--c->dep_msgs.len]);
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
        } else if (!strcmp(name, "may_alias")) {
            a->may_alias = true;
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
                    char pre[16] = "";
                    if (q)
                        snprintf(pre, sizeof pre, "%u ", (unsigned)q + 1);
                    if (type_ckind(TT, c->ty[x]) == TY_ERROR)
                        cwarn(c, il, "attributes", "'nonnull' attribute "
                              "argument %sis invalid", pre);
                    else if (!(c->ck[x] == K_ICE || c->ck[x] == K_FOLD) ||
                             !type_is_integer(TT, c->ty[x]))
                        cwarn(c, il, "attributes", "'nonnull' attribute "
                              "argument %shas type %s", pre,
                              type_q(TT, c->ty[x]));
                    else
                        cwarn(c, il, "attributes", "'nonnull' attribute "
                              "argument %sis invalid", pre);
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
        } else if (!strcmp(name, "sentinel")) {
            TypeId ft = c->attr_fty;
            SrcLoc il = cinput_loc(c, c->nodes[item].tok);
            if (ft && type_ckind(TT, ft) == TY_FUNC &&
                !(type_ent(TT, type_canon(TT, ft))->flags & TF_VARIADIC))
                cwarn(c, il, "attributes", "'sentinel' attribute only applies "
                      "to variadic functions");
            if (ft && type_ckind(TT, ft) == TY_FUNC && ak.n) {
                uint32_t x = ak.p[0];
                if (type_ckind(TT, c->ty[x]) == TY_ERROR)
                    ;
                else if (!(c->ck[x] == K_ICE || c->ck[x] == K_FOLD) ||
                         !type_is_integer(TT, c->ty[x]))
                    cwarn(c, il, "attributes", "requested position is not an "
                          "integer constant");
                else if (cexpr_sval(c, x) < 0)
                    cwarn(c, il, "attributes", "requested position is less "
                          "than zero");
            }
        } else if (!strcmp(name, "returns_nonnull")) {
            TypeId ft = c->attr_fty;
            if (ft && type_ckind(TT, ft) == TY_FUNC &&
                type_ckind(TT, type_ent(TT, ft)->base) != TY_PTR)
                cerror(c, cinput_loc(c, c->nodes[item].tok),
                       "'returns_nonnull' attribute on a function not "
                       "returning a pointer");
        } else if (!strcmp(name, "format") && ak.n == 3 &&
                   ntag(c, ak.p[0]) == N_IDENT) {
            char ar[128];
            int kind = 0;
            attr_norm(tstr(c, c->nodes[ak.p[0]].tok), ar, sizeof ar);
            kind = format_kind(ar);
            if (!kind && !strcmp(ar, "NSString"))
                cwarn(c, cdecl_line_start_loc(c, c->nodes[item].tok), "format=", "'NSString' is "
                      "only allowed in Objective-C dialects");
            else if (!kind && !format_type_known(ar))
                cwarn(c, cdecl_line_start_loc(c, c->nodes[item].tok), "format=", "'%s' is an "
                      "unrecognized format function type", ar);
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
        } else if (!strcmp(name, "format_arg") && ak.n == 1) {
            uint32_t xs = ak.p[0];
            if (c->ck[xs] == K_ICE || c->ck[xs] == K_FOLD) {
                int64_t sv = cexpr_sval(c, xs);
                if (sv >= 1 && sv < 256)
                    a->fmtarg = (uint8_t)sv;
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
        } else if (!strcmp(name, "nonstring")) {
            a->nonstring = true;
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
                }
            }            else if (type_ckind(TT, c->ty[arg]) != TY_ERROR) {
                char val[96];
                pos_arg_str(c, arg, val, sizeof val);
                cerror(c, cinput_loc(c, c->nodes[item].tok), "'vector_size' "
                       "attribute argument value '%s' is not an integer "
                       "constant", val);
            }
        } else if (!strcmp(name, "patchable_function_entry")) {
            uint32_t q;
            for (q = 0; q < ak.n && q < 2; q++) {
                uint32_t x = ak.p[q];
                char val[96];
                bool bad;
                if (type_ckind(TT, c->ty[x]) == TY_ERROR)
                    break;
                pos_arg_str(c, x, val, sizeof val);
                bad = !(c->ck[x] == K_ICE || c->ck[x] == K_FOLD) ||
                      !type_is_integer(TT, c->ty[x]) ||
                      (type_is_signed(TT, c->ty[x]) && cexpr_sval(c, x) < 0);
                if (bad) {
                    cwarn(c, patchable_loc(c, item), "attributes",
                          "'patchable_function_entry' attribute argument '%s' "
                          "is not an integer constant", val);
                    break;
                }
                if ((uint64_t)cexpr_sval(c, x) > 65535) {
                    cwarn(c, patchable_loc(c, item), "attributes",
                          "'patchable_function_entry' attribute argument '%s' "
                          "exceeds 65535", val);
                    break;
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
            else {
                /* machine modes are upper case apart from a few names */
                const Tok *mt = cnode_tok(c, arg);
                bool lower = false;
                const char *q;
                for (q = m; *q; q++)
                    lower |= (*q >= 'a' && *q <= 'z') || (*q & 0x80);
                if (mt->kind == TK_IDENT &&
                    ident_by_id(c->in, mt->aux)->ext)
                    lower = true;
                if (lower && strncmp(m, "libgcc_", 7))
                    cerror(c, c->attr_at_set ? c->attr_at
                                             : iloc(c, c->nodes[item].tok),
                           "unknown machine mode '%s'",
                           cident(c, mt->aux));
                a->has_mode = false;
            }
        }
        kids_free(&ak);
    }
    kids_free(&k);
}

/* The integer type of the given size and signedness. */
TypeId int_of_size(Checker *c, unsigned bytes, bool uns)
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
TypeId attr_apply_type(Checker *c, TypeId t, const Attrs *a, SrcLoc at)
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
            cerror(c, at, "invalid vector type for attribute "
                   "'vector_size'");
        } else if (a->vector_size == 0) {
            cerror(c, at, "zero vector size");
        } else if ((esz = type_size(TT, el, &ok)) && a->vector_size % esz) {
            cerror(c, at, "vector size not an integral multiple of "
                   "component size");
        } else if (((a->vector_size / esz) & (a->vector_size / esz - 1)) != 0) {
            cerror(c, at, "number of vector components %llu not a "
                   "power of two", (unsigned long long)(a->vector_size / esz));
        } else if (a->vector_size / esz > 2147483646u) {
            cerror(c, at, "number of vector components %llu exceeds "
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

void attrs_merge(Attrs *to, const Attrs *from)
{
    if (from->aligned > to->aligned)
        to->aligned = from->aligned;
    to->packed |= from->packed;
    if (from->sso)
        to->sso = from->sso;
    if (from->ms)
        to->ms = from->ms;
    to->transparent_union |= from->transparent_union;
    to->may_alias |= from->may_alias;
    if (from->has_mode) {
        to->has_mode = true;
        to->mode_bytes = from->mode_bytes;
        to->mode_float = from->mode_float;
        memcpy(to->mode_name, from->mode_name, sizeof to->mode_name);
    }
    if (from->vs_seen) {
        to->vector_size = from->vector_size;
        to->vs_seen = true;
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
    if (from->wina_al > to->wina_al)
        to->wina_al = from->wina_al;
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
    to->nonstring |= from->nonstring;
    to->weak |= from->weak;
    to->noreturn |= from->noreturn;
    to->nonnull |= from->nonnull;
    if (from->fmt)
        to->fmt = from->fmt;
    if (from->fmtarg)
        to->fmtarg = from->fmtarg;
}

/* Unknown specifier attributes are reported once the declarator is known:
 * gcc's input_location is then that declarator's line. */
void attrs_unknown_emit(Checker *c, const Attrs *sa, uint32_t tok)
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
void attrs_misapplied(Checker *c, const Attrs *a, char where, bool local,
                             TypeId fty, uint32_t tok)
{
    bool var = where == 'g' || where == 's' || where == 'a';
    bool nofn = where != 'f';
    SrcLoc loc = iloc(c, tok);
    if (a->noinline && nofn)
        cwarn(c, loc, "attributes", "'noinline' attribute ignored");
    if (a->used && (where == 'a' || where == 'p' || where == 'm'))
        cwarn(c, loc, "attributes", "'used' attribute ignored");
    if (a->nonstring && where == 'f')
        cwarn(c, loc, "attributes", "'nonstring' attribute does not apply to "
              "functions");
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
void attrs_section_check(Checker *c, const Attrs *a, char where,
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
void attrs_zcur_check(Checker *c, const Attrs *a, bool fn, SrcLoc nloc)
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

void attrs_wina_check(Checker *c, const Attrs *a, char where,
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
static bool access_check(Checker *c, const uint32_t *arg, uint32_t n, SrcLoc il,
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
            return false;
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
        return false;
    }
    if (ntag(c, x) != N_IDENT) {
        if (type_ckind(TT, c->ty[x]) != TY_ERROR)
            cerror(c, il, "attribute 'access' mode '%s' is not an identifier; "
                   "expected one of 'read_only', 'read_write', 'write_only', "
                   "or 'none'", cexpr_str(c, x));
        return false;
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
        return false;
    }
    if (n < 2) {
        cerror(c, il, "attribute 'access(%s)' missing an argument", md);
        return false;
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
            return false;
        if (!(c->ck[e] == K_ICE || c->ck[e] == K_FOLD) ||
            !type_is_integer(TT, c->ty[e])) {
            cerror(c, il, "attribute 'access(%s)' invalid positional argument "
                   "%u", list, q);
            return false;
        }
        v = cexpr_sval(c, e);
        pos_arg_str(c, e, val, sizeof val);
        if (v < 1) {
            cerror(c, il, "attribute 'access(%s)' positional argument %u "
                   "invalid value %s", list, q, val);
            return false;
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
                return false;
            }
            pt = type_params(TT, ft)[v - 1];
            if (q == 1) {
                TypeId tgt;
                if (type_ckind(TT, pt) != TY_PTR) {
                    cerror(c, il, "attribute 'access(%s)' positional argument "
                           "1 references non-pointer argument type %s", list,
                           type_q(TT, pt));
                    return false;
                }
                tgt = type_base(TT, pt);
                if (type_ckind(TT, tgt) == TY_FUNC) {
                    cerror(c, il, "attribute 'access(%s)' positional argument "
                           "1 references argument of function type %s", list,
                           type_q(TT, tgt));
                    return false;
                }
                if (m >= 1 && m <= 2 && (TYPE_QUALS(tgt) & TQ_CONST)) {
                    cerror(c, il, "attribute 'access(%s)' positional argument "
                           "1 references 'const'-qualified argument type %s",
                           list, type_q(TT, pt));
                    return false;
                }
            } else if (!type_is_integer(TT, pt)) {
                cerror(c, il, "attribute 'access(%s)' positional argument 2 "
                       "references non-integer argument type %s", list,
                       type_q(TT, pt));
                return false;
            }
        }
    }    return true;
}

/* c-attribs.cc positional_argument for one argument of alloc_align or
 * alloc_size on a function of type fty; false when it warned. */
static bool positional_arg(Checker *c, const char *name, uint32_t arg, int argno,
                           TypeId fty, SrcLoc loc, int mode, int64_t *posout)
{
    char pre[24] = "", val[96];
    TypeId t = c->ty[arg], pt;
    int64_t v;
    uint32_t n;
    uint64_t pos;
    if (argno)
        snprintf(pre, sizeof pre, "%d ", argno);
    if (type_ckind(TT, t) == TY_ERROR) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %sis invalid",
              name, pre);
        return false;
    }
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
    if (posout)
        *posout = v;
    if (!v && mode == PA_REST)      /* format's 0: no arguments to check */
        return true;
    if (!v) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' does "
              "not refer to a function parameter", name, pre, val);
        return false;
    }
    if ((type_ent(TT, fty)->flags & TF_NOPROTO) || mode == PA_REST)
        return true;
    n = type_ent(TT, fty)->n;
    pos = (uint64_t)v;
    if (pos > n) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' "
              "exceeds the number of function parameters %u", name, pre, val, n);
        return false;
    }
    pt = type_params(TT, fty)[pos - 1];
    if (mode == PA_STR) {           /* handle_format_attribute: a char pointer */
        if (!is_char_ptr(c, pt)) {
            cerror(c, loc, "'%s' attribute argument %svalue '%s' refers to "
                   "parameter type %s", name, pre, val, type_q(TT, pt));
            return false;
        }
    } else if (!type_is_integer(TT, pt) ||
        type_kind(TT, TYPE_UNQUAL(type_canon(TT, pt))) == TY_BOOL) {
        cwarn(c, loc, "attributes", "'%s' attribute argument %svalue '%s' "
              "refers to parameter type %s", name, pre, val, type_q(TT, pt));
        return false;
    }
    return true;
}

/* handle_format_attribute / handle_format_arg_attribute on a function of type
 * fty: the format string position names a char pointer, the first argument
 * position (0 or) the `...`, and format_arg returns a string. */
static void format_attr_check(Checker *c, const char *name, const uint32_t *ak,
                              uint32_t n, TypeId fty, SrcLoc loc)
{
    bool fa = !strcmp(name, "format_arg");
    int64_t first = 0;
    int kind = 0;
    if (fa ? n != 1 : n != 3)
        return;
    if (!fa) {
        char ar[128];
        if (ntag(c, ak[0]) != N_IDENT)
            return;
        attr_norm(tstr(c, c->nodes[ak[0]].tok), ar, sizeof ar);
        kind = format_kind(ar);
        if (!kind && !format_type_known(ar))
            return;                 /* warned about when the attribute is read */
    }
    if (!positional_arg(c, name, ak[fa ? 0 : 1], fa ? 0 : 2, fty, loc, PA_STR,
                        NULL))
        return;
    if (fa) {
        TypeId rt = type_base(TT, fty);
        if (!is_char_ptr(c, rt))
            cerror(c, loc, "function does not return string type");
        return;
    }
    if (!positional_arg(c, name, ak[2], 3, fty, loc, PA_REST, &first) || !first ||
        (type_ent(TT, fty)->flags & TF_NOPROTO))
        return;
    {
        unsigned np = type_ent(TT, fty)->n;
        bool var = type_ent(TT, fty)->flags & TF_VARIADIC;
        if (!var || first <= (int64_t)np)
            cerror(c, loc, "'format' attribute argument 3 value '%lld' does "
                   "not refer to a variable argument list", (long long)first);
        else if (first != (int64_t)np + 1)
            cerror(c, loc, "argument to be formatted is not '...'");
        else if (kind == 3)
            cerror(c, loc, "strftime formats cannot format arguments");
    }
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
void strict_flex_check(Checker *c, uint32_t holder, bool field,
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

/* Set while the attributes of a pointer to function are checked: pure and
 * const are not diagnosed there. */
bool alloc_via_ptr;

/* The function being declared (ident id), 0: none; set around the calls
 * for a function so that a redeclaration's alloc_size / alloc_align can be
 * compared with the previous declaration's. */
uint32_t alloc_name;
static SrcLoc alloc_loc;

/* Declaration contexts for attrs_ctx_check. */
const char *ctx_vname;   /* the object attrs_ctx_check is looking at */

/* Attributes whose handler returns "ignored" on a declaration that is not a
 * function, by context (typedef, file-scope variable, automatic local, static
 * local, parameter, field).  The ones with an Attrs flag (noinline, used,
 * weak, packed, alias, weakref, error, warning, cleanup) are in
 * attrs_misapplied. */
static const struct { const char *name; unsigned ctx; } attr_ign_tab[] = {
    {"always_inline", AC_ALL}, {"artificial", AC_ALL}, {"assume", AC_ALL},
    {"cold", AC_ALL}, {"common", AC_T|AC_P|AC_F}, {"const", AC_ALL},
    {"constructor", AC_ALL}, {"destructor", AC_ALL},
    {"externally_visible", AC_T|AC_P|AC_F}, {"fallthrough", AC_ALL},
    {"fentry_name", AC_ALL}, {"fentry_section", AC_ALL}, {"flatten", AC_ALL},
    {"gcc_struct", AC_ALL}, {"gnu_inline", AC_ALL}, {"hot", AC_ALL},
    {"ifunc", AC_ALL},     {"ms_struct", AC_ALL}, {"no_address_safety_analysis", AC_ALL},
    {"no_icf", AC_ALL}, {"no_profile_instrument_function", AC_ALL},
    {"no_sanitize", AC_ALL}, {"no_sanitize_address", AC_ALL},
    {"no_sanitize_coverage", AC_ALL}, {"no_sanitize_thread", AC_ALL},
    {"no_sanitize_undefined", AC_ALL}, {"no_stack_protector", AC_ALL},
    {"noclone", AC_ALL}, {"nocommon", AC_T|AC_P|AC_F},
    {"nodirect_extern_access", AC_T|AC_P|AC_F}, {"noipa", AC_ALL},
    {"noreturn", AC_ALL}, {"nothrow", AC_ALL}, {"optimize", AC_ALL},
    {"pure", AC_ALL}, {"retain", AC_T|AC_L|AC_P|AC_F},
    {"returns_twice", AC_ALL}, {"scalar_storage_order", AC_ALL},
    {"signed_bool_precision", AC_ALL}, {"simd", AC_ALL},
    {"stack_protect", AC_ALL}, {"target", AC_ALL}, {"target_clones", AC_ALL},
    {"transaction_callable", AC_ALL},
    {"transaction_may_cancel_outer", AC_ALL}, {"transaction_pure", AC_ALL},
    {"transaction_safe", AC_ALL}, {"transaction_safe_dynamic", AC_ALL},
    {"transaction_unsafe", AC_ALL}, {"transaction_wrap", AC_ALL},
    {"transparent_union", AC_ALL}, {"vector_mask", AC_ALL},
    {"visibility", AC_T|AC_L|AC_S|AC_P|AC_F}, {"volatile", AC_ALL},
    {"warn_unused", AC_ALL},
};

/* gcc locates an attribute-only declaration at the first token of its line. */
SrcLoc cdecl_line_start_loc(Checker *c, uint32_t tok)
{
    uint32_t line, col, l2, c2;
    SrcFile *f = srcmgr_file_of(c->sm, tloc(c, tok));
    if (!f)
        return tloc(c, tok);
    srcmgr_linecol(f, tloc(c, tok), &line, &col);
    while (tok > 0) {
        SrcLoc pl = tloc(c, tok - 1);
        if (srcmgr_file_of(c->sm, pl) != f)
            break;
        srcmgr_linecol(f, pl, &l2, &c2);
        if (l2 != line)
            break;
        tok--;
    }
    return tloc(c, tok);
}

/* Attributes that need a function type. */
static const char *const attr_fnonly_tab[] = {
    "access", "alloc_align", "alloc_size", "assume_aligned",
    "callee_pop_aggregate_return", "cdecl", "fastcall", "fd_arg",
    "fd_arg_read", "fd_arg_write", "force_align_arg_pointer", "format", "format_arg",
    "indirect_return", "interrupt", "ms_abi", "no_caller_saved_registers",
    "nocf_check", "nonnull", "regparm", "returns_nonnull", "sentinel",
    "sseregparm", "stdcall", "sysv_abi", "thiscall", "warn_unused_result"};

/* The expression a copy attribute names, less &, *, [] and a comma list. */
static uint32_t copy_target(Checker *c, uint32_t e)
{
    for (;;) {
        while (ntag(c, e) == N_PAREN && c->nodes[e].size > 1)
            e--;
        if (ntag(c, e) == N_UNARY && (tpunct(c, c->nodes[e].tok) == P_AMP ||
                                      tpunct(c, c->nodes[e].tok) == P_STAR))
            e = first_child(c, e);
        else if (ntag(c, e) == N_INDEX)
            e = first_child(c, e);
        else if (ntag(c, e) == N_BINARY &&
                 tpunct(c, c->nodes[e].tok) == P_COMMA) {
            uint32_t bk[3], bn = node_children(c->nodes, e, bk, 3);
            if (bn < 2)
                return e;
            e = bk[1];       /* a comma expression: its value */
        } else
            return e;
    }
}

/* decl_attributes on an object, typedef, parameter or field whose type is
 * neither a function nor a pointer to one: attributes that apply only to
 * functions, and the ones ignored in this context. */
static void attrs_ctx_check1(Checker *c, uint32_t holder, TypeId ty, uint32_t tok,
                             unsigned ctx)
{
    Kids k;
    uint32_t j;
    bool fnty;
    TypeId oty = ty;
    if (type_ckind(TT, ty) == TY_PTR)       /* a pointer to function is fine */
        ty = type_base(TT, ty);
    fnty = type_ckind(TT, ty) == TY_FUNC;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char name[48];
            size_t f;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), name, sizeof name);
            if (!strcmp(name, "malloc")) {
                cwarn(c, iloc(c, tok), "attributes", "'malloc' attribute "
                      "ignored; valid only for functions");
                continue;
            }
            if (!strcmp(name, "copy") && (ctx & (AC_G | AC_S | AC_L)) &&
                !(ctx & AC_TLS)) {
                /* a copied tls_model needs thread storage */
                Kids ak;
                kids_get(c, it.p[q], &ak);
                if (ak.n == 1) {
                    uint32_t e = copy_target(c, ak.p[0]);
                    uint32_t ref = ntag(c, e) == N_IDENT ?
                                   lookup_ord(c, cnode_ident(c, e)) : SYM_NONE;
                    if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ &&
                        cdecl_aset_has(c, csym(c, ref)->aset, "tls_model",
                                       NULL))
                        cwarn(c, iloc(c, tok), "attributes", "'tls_model' "
                              "attribute ignored because '%s' does not have "
                              "thread storage duration", ctx_vname);
                }
                kids_free(&ak);
                continue;
            }
            if (!strcmp(name, "leaf")) {
                /* on anything but a function; a decl that is not public
                 * also trips the unit-local check */
                cwarn(c, iloc(c, tok), "attributes", "'leaf' attribute "
                      "ignored");
                if (!(ctx & AC_PUB))
                    cwarn(c, iloc(c, tok), "attributes", "'leaf' attribute "
                          "has no effect on unit local functions");
                continue;
            }
            if (!strcmp(name, "tls_model") && (ctx & (AC_G | AC_S | AC_L))) {
                char v[24];
                if (!(ctx & AC_TLS)) {
                    cwarn(c, iloc(c, tok), "attributes", "'tls_model' "
                          "attribute ignored because '%s' does not have "
                          "thread storage duration", ctx_vname);
                    continue;
                }
                cdecl_attr_args(c, it.p[q], v, sizeof v);
                if (strcmp(v, "\"local-exec\"") && strcmp(v, "\"initial-exec\"") &&
                    strcmp(v, "\"local-dynamic\"") && strcmp(v, "\"global-dynamic\""))
                    cerror(c, iloc(c, tok), "'tls_model' argument must be "
                           "one of 'local-exec', 'initial-exec', "
                           "'local-dynamic', or 'global-dynamic'");
                continue;
            }
            if (!strcmp(name, "nonstring")) {
                /* handle_nonstring_attribute: a character array or pointer */
                TypeId e = type_canon(TT, oty);
                unsigned kd = type_ckind(TT, e);
                bool ok = false;
                if (ctx == AC_T || fnty) {
                    cwarn(c, iloc(c, tok), "attributes", "'nonstring' "
                          "attribute does not apply to %s",
                          ctx == AC_T ? "types" : "functions");
                    continue;
                }
                if (kd == TY_PTR || kd == TY_ARRAY || kd == TY_VLA) {
                    e = type_base(TT, e);
                    kd = type_ckind(TT, e);
                    ok = kd == TY_CHAR || kd == TY_SCHAR || kd == TY_UCHAR;
                }
                if (!ok)
                    cwarn(c, iloc(c, tok), "attributes", "'nonstring' "
                          "attribute ignored on objects of type %s",
                          type_q(TT, oty));
                continue;
            }
            if (type_ckind(TT, ty) == TY_UNION && !strcmp(name, "transparent_union"))
                continue;
            if (fnty)
                continue;
            for (f = 0; f < sizeof attr_fnonly_tab / sizeof *attr_fnonly_tab; f++)
                if (!strcmp(name, attr_fnonly_tab[f])) {
                    cwarn(c, iloc(c, tok), "attributes", "'%s' attribute only "
                          "applies to function types", name);
                    break;
                }
            if (f < sizeof attr_fnonly_tab / sizeof *attr_fnonly_tab)
                continue;
            for (f = 0; f < sizeof attr_ign_tab / sizeof *attr_ign_tab; f++)
                if ((attr_ign_tab[f].ctx & ctx) && !strcmp(name, attr_ign_tab[f].name)) {
                    if (!strcmp(name, "fallthrough") && !cat_file_scope(c)) {
                        /* a leading attribute list that is not a statement */
                        uint32_t d = holder;
                        while (d != NO_NODE && ntag(c, d) != N_DECL &&
                               d != c->par[d])
                            d = c->par[d];
                        if (d != NO_NODE && ntag(c, d) == N_DECL &&
                            cfirst(c, d) == cfirst(c, k.p[j]))
                            cwarn(c, cdecl_line_start_loc(c, c->nodes[k.p[j]].tok),
                                  "attributes", "'fallthrough' attribute "
                                  "not followed by ';'");
                    }
                    cwarn(c, iloc(c, tok), "attributes", "'%s' attribute "
                          "ignored", name);
                    break;
                }
        }
        kids_free(&it);
    }
    kids_free(&k);
}

/* ... on the holder and on the attributes after its '*'s. */
void attrs_ctx_check(Checker *c, uint32_t holder, TypeId ty, uint32_t tok,
                            unsigned ctx)
{
    uint32_t h = holder;
    attrs_ctx_check1(c, holder, ty, tok, ctx);
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
        attrs_ctx_check1(c, nx, ty, tok, ctx);
        h = nx;
    }
}

/* "ignoring attribute 'alloc_size (2)' because it conflicts with previous
 * 'alloc_size (1)'", the argument lists given as "a,b". */
void cdecl_alloc_conflict(Checker *c, SrcLoc loc, const char *name,
                          const char *now, const char *was)
{
    char a[128], b[128];
    size_t i, l = 0;
    for (i = 0; now[i] && l + 2 < sizeof a; i++) {
        a[l++] = now[i];
        if (now[i] == ',')
            a[l++] = ' ';
    }
    a[l] = 0;
    for (i = 0, l = 0; was[i] && l + 2 < sizeof b; i++) {
        b[l++] = was[i];
        if (was[i] == ',')
            b[l++] = ' ';
    }
    b[l] = 0;
    cwarn(c, loc, "attributes", "ignoring attribute '%s (%s)' because it "
          "conflicts with previous '%s (%s)'", name, a, name, b);
}

/* The arguments of the oldest attribute `name` in the set (false: none). */
bool cdecl_aset_first_arg(const Checker *c, uint32_t set, const char *name,
                          char *out, size_t n)
{
    uint32_t k, first = 0;
    if (!set)
        return false;
    for (k = c->ahead.data[set - 1]; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, name))
            first = k;
    if (!first)
        return false;
    snprintf(out, n, "%s", c->anames.data[first - 1].arg);
    return true;
}

/* A redeclaration whose alloc_size / alloc_align differs from the previous
 * declaration's is ignored, with a warning. */
static void alloc_redecl(Checker *c, uint32_t item, const char *name, SrcLoc loc)
{
    uint32_t ref = lookup_ord(c, alloc_name), set, k, first;
    char now[96], was[96];
    if (ref == SYM_NONE ||
        (csym(c, ref)->kind != CS_FUNC && csym(c, ref)->kind != CS_TYPEDEF))
        return;
    set = csym(c, ref)->aset;
    if (!set)
        return;
    cdecl_attr_args(c, item, now, sizeof now);
    for (k = c->ahead.data[set - 1], first = 0; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, name))
            first = k;          /* the oldest: the ignored ones come later */
    if (!first || !strcmp(c->anames.data[first - 1].arg, now))
        return;
    snprintf(was, sizeof was, "%s", c->anames.data[first - 1].arg);
    cdecl_alloc_conflict(c, loc, name, now, was);
    if (c->nign < 8)
        snprintf(c->ign[c->nign++], sizeof c->ign[0], "%.23s", name);
}

/* c-attribs.cc append_access_attrs: the access attributes a function has
 * accepted so far, one per pointer argument; the first one wins and a
 * conflicting later one is dropped with a warning. */
typedef struct {
    char mode[16];
    uint32_t ptr, size;
    bool implied;               /* from a VLA parameter's bound */
    bool star;                  /* an attribute on a [*] parameter */
    bool bnamed;
    SrcLoc bloc;                /* ... and where that bound is declared */
} AccSeen;
static AccSeen acc_l[32];
static unsigned acc_n;
CImplied imp_l[16];      /* this declaration's VLA designations */
unsigned imp_n;
uint32_t imp_name;

static bool acc_parse(const char *arg, AccSeen *o)
{
    char md[32];
    const char *q;
    char *e;
    unsigned long p, z = 0;
    memset(o, 0, sizeof *o);
    if (*arg == '~') {
        o->implied = true;
        arg++;
    }
    q = strchr(arg, ',');
    if (!q || (size_t)(q - arg) >= sizeof md)
        return false;
    memcpy(md, arg, (size_t)(q - arg));
    md[q - arg] = 0;
    attr_norm(md, o->mode, sizeof o->mode);
    p = strtoul(q + 1, &e, 10);
    if (e == q + 1 || !p)
        return false;
    if (*e == ',')
        z = strtoul(e + 1, &e, 10);
    o->ptr = (uint32_t)p;
    o->size = (uint32_t)z;
    return true;
}

/* How the new access designation x disagrees with the accepted e (0: it
 * does not). */
enum { AK_NONE, AK_CONFLICT, AK_MODE, AK_MISSING_OLD, AK_MISSING_NEW, AK_VALUES };

static int acc_kind(const AccSeen *e, const AccSeen *x)
{
    bool vla = e->implied || x->implied || e->star;
    if ((e->implied || e->star) && !e->size)
        return AK_NONE;     /* a [*] bound only says the argument is an array */
    if ((x->implied && !x->size) || (e->implied && x->implied))
        return AK_NONE;
    if (vla && x->size && e->size && x->size != e->size &&
        !strcmp(e->mode, x->mode))
        return AK_CONFLICT;
    if (!e->implied && !x->implied && strcmp(e->mode, x->mode))
        return AK_MODE;     /* a VLA bound implies no mode of its own */
    if (!e->size && x->size)
        return AK_MISSING_OLD;
    if (e->size && !x->size)
        return AK_MISSING_NEW;
    if (e->size != x->size)
        return AK_VALUES;
    return AK_NONE;
}

static void acc_diag(Checker *c, const AccSeen *e, const AccSeen *x, int kind)
{
    char spec[96], sz[16] = "";
    Diagnostic *d = NULL;
    bool vla = e->implied || x->implied;
    /* a VLA designation found after the attribute: gcc names the attribute,
     * in its tree form */
    const AccSeen *sx = x->implied ? e : x;
    if (sx->size)
        snprintf(sz, sizeof sz, ", %u", sx->size);
    snprintf(spec, sizeof spec, x->implied ? "access (%s, %u%s)" :
             "access(%s, %u%s)", sx->mode, sx->ptr, sz);
    switch (kind) {
    case AK_CONFLICT:
        d = cwarn_d(c, DL_WARNING, alloc_loc, "attributes", "attribute '%s' "
                    "positional argument 2 conflicts with previous "
                    "designation by argument %u", spec, e->size);
        break;
    case AK_MODE:
        d = cwarn_d(c, DL_WARNING, alloc_loc, "attributes", "attribute '%s' "
                    "mismatch with mode '%s'", spec, e->mode);
        break;
    case AK_MISSING_OLD:
        d = cwarn_d(c, DL_WARNING, alloc_loc, "attributes", "attribute '%s' "
                    "positional argument 2 missing in previous designation",
                    spec);
        break;
    case AK_MISSING_NEW:
        d = cwarn_d(c, DL_WARNING, alloc_loc, "attributes", "attribute '%s' "
                    "missing positional argument 2 provided in previous "
                    "designation by argument %u", spec, e->size);
        break;
    case AK_VALUES:
        d = cwarn_d(c, DL_WARNING, alloc_loc, "attributes", "attribute '%s' "
                    "mismatched positional argument values %u and %u", spec,
                    x->size, e->size);
        break;
    }
    if (d && vla) {
        const AccSeen *iv = e->implied ? e : x;
        SrcLoc bl = alloc_loc;
        unsigned j;
        if (iv->bnamed)
            bl = iv->bloc;
        else
            for (j = 0; j < imp_n; j++)
                if (imp_l[j].ptr == iv->ptr)
                    bl = imp_l[j].bloc;
        cnote(c, d, bl, "designating the bound of variable length array "
              "argument %u", iv->ptr);
    } else if (d) {
        uint32_t ref = lookup_ord(c, alloc_name);
        if (ref != SYM_NONE && csym(c, ref)->kind == CS_FUNC)
            cnote(c, d, csym(c, ref)->loc, "previous declaration here");
    }
}

/* VLA designations an earlier declaration lost to its attribute: a
 * redeclaration reports them again. */
static struct { AccSeen e, x; } acc_redo[8];
static unsigned acc_nredo;

static void acc_add(Checker *c, const AccSeen *x, bool warn)
{
    unsigned i;
    const AccSeen *e = NULL;
    for (i = 0; i < acc_n; i++)
        if (acc_l[i].ptr == x->ptr) {
            e = &acc_l[i];
            break;
        }
    if (!e) {
        if (acc_n < 32)
            acc_l[acc_n++] = *x;
        return;
    }
    if ((e->implied || e->star) && !e->size)
        return;
    if (x->implied && !x->size)
        acc_l[e - acc_l].star = true;       /* the explicit one is as weak */
    else if (x->implied && !e->implied && !acc_kind(e, x)) {
        /* the same designation twice: the bound's wording wins */
        acc_l[e - acc_l].implied = true;
        acc_l[e - acc_l].bnamed = x->bnamed;
        acc_l[e - acc_l].bloc = x->bloc;
    }
    if (warn) {
        int k = acc_kind(e, x);
        if (k)
            acc_diag(c, e, x, k);
    } else if (x->implied && !e->implied && acc_kind(e, x) && acc_nredo < 8) {
        acc_redo[acc_nredo].e = *e;
        acc_redo[acc_nredo].x = *x;
        acc_nredo++;
    }
}

static bool acc_ready;

/* Start a function declaration; the lookup that replays its earlier
 * declarations waits for the first access attribute (a lookup is recorded
 * in the unit's summary). */
void acc_start(uint32_t name, SrcLoc loc)
{
    alloc_name = name;
    alloc_loc = loc;
    acc_n = 0;
    acc_ready = false;
    imp_n = 0;
    acc_nredo = 0;
}

/* Replay what the earlier declarations accepted (the symbol's attribute
 * chain, oldest first). */
static void acc_replay_ref(Checker *c, uint32_t ref)
{
    uint32_t idx[64], n = 0, k, i;
    acc_ready = true;
    if (ref == SYM_NONE || csym(c, ref)->kind != CS_FUNC || !csym(c, ref)->aset)
        return;
    for (k = c->ahead.data[csym(c, ref)->aset - 1]; k && n < 64;
         k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, "access"))
            idx[n++] = k;
    {
        CImplied oi[16];
        unsigned on = cparm_implied(c, csym(c, ref)->parms, oi, 16), j;
        for (i = n; i-- > 0;) {
            AccSeen x;
            if (!acc_parse(c->anames.data[idx[i] - 1].arg, &x))
                continue;
            for (j = 0; x.implied && j < on; j++)
                if (oi[j].ptr == x.ptr && oi[j].size == x.size) {
                    x.bloc = oi[j].bloc;
                    x.bnamed = oi[j].bnamed;
                }
            acc_add(c, &x, false);
        }
    }
}

static void acc_replay(Checker *c)
{
    acc_replay_ref(c, lookup_ord(c, alloc_name));
}

/* The access attributes the declaration's own VLA parameters imply, checked
 * against what is accepted so far (after the redeclaration's merge, so
 * after the parameter warnings). */
void acc_implied(Checker *c, uint32_t ref, bool redecl, bool def)
{
    CImplied oi[16];
    unsigned on, j, m;
    if (!imp_n && !cdecl_aset_has(c, csym(c, ref)->aset, "access", NULL))
        return;
    on = cparm_implied(c, csym(c, ref)->parms, oi, 16);
    alloc_name = imp_name;
    if (!acc_ready)
        acc_replay_ref(c, ref);
    if (redecl || def)
        for (j = 0; j < acc_nredo; j++)
            acc_diag(c, &acc_redo[j].e, &acc_redo[j].x,
                     acc_kind(&acc_redo[j].e, &acc_redo[j].x));
    if (redecl && !def)
        imp_n = 0;      /* a redeclaration's own bounds add nothing */
    for (j = 0; j < imp_n; j++) {
        AccSeen x;
        memset(&x, 0, sizeof x);
        snprintf(x.mode, sizeof x.mode, "read_write");
        x.ptr = imp_l[j].ptr;
        x.size = imp_l[j].size;
        x.implied = true;
        x.bnamed = imp_l[j].bnamed;
        x.bloc = imp_l[j].bloc;
        for (m = 0; m < on; m++)     /* the earlier declaration's, if it names it */
            if (oi[m].ptr == x.ptr && oi[m].bnamed) {
                x.bloc = oi[m].bloc;
                x.bnamed = true;
            }
        acc_add(c, &x, true);
    }
    alloc_name = 0;
}

/* Keep the declaration's VLA designations with the function's attributes. */
void acc_chain_implied(Checker *c, uint32_t ref)
{
    unsigned j;
    for (j = 0; j < imp_n; j++) {
        char buf[48];
        snprintf(buf, sizeof buf, "~read_write,%u,%u", imp_l[j].ptr,
                 imp_l[j].size);
        aset_add(c, &csym(c, ref)->aset, "access", buf);
    }
    imp_n = 0;
}

/* handle_alloc_align_attribute / handle_alloc_size_attribute for the
 * attributes among holder's children, applied to a function of type fty. */
void attrs_alloc_check(Checker *c, uint32_t holder, TypeId fty,
                              uint32_t tok)
{
    Kids k;
    uint32_t j;
    bool seen_pure = false, seen_const = false;
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
            if ((!strcmp(name, "error") || !strcmp(name, "warning")) &&
                type_ckind(TT, fty) == TY_FUNC && !alloc_via_ptr) {
                /* handle_error_attribute: the message must be a string */
                kids_get(c, it.p[q], &ak);
                if (ak.n && ntag(c, ak.p[0]) != N_STRING)
                    cwarn(c, iloc(c, tok), "attributes",
                          "'%s' attribute ignored", name);
                kids_free(&ak);
                continue;
            }
            if ((!strcmp(name, "pure") || !strcmp(name, "const")) &&
                type_ckind(TT, fty) == TY_FUNC && !alloc_via_ptr) {
                /* handle_pure/const_attribute; the second of the pair is
                 * dropped by the exclusion before its handler runs */
                bool isp = name[0] == 'p';
                if (isp ? seen_const : seen_pure)
                    continue;
                if (isp)
                    seen_pure = true;
                else
                    seen_const = true;
                if (type_ckind(TT, type_base(TT, fty)) == TY_VOID)
                    cwarn(c, iloc(c, tok), "attributes", "'%s' attribute on "
                          "function returning 'void'", name);
                continue;
            }
            if ((!strcmp(name, "format") || !strcmp(name, "format_arg")) &&
                type_ckind(TT, fty) == TY_FUNC) {
                kids_get(c, it.p[q], &ak);
                format_attr_check(c, name, ak.p, ak.n, fty, iloc(c, tok));
                kids_free(&ak);
                continue;
            }
            if (!strncmp(name, "fd_arg", 6) && type_ckind(TT, fty) == TY_FUNC) {
                /* handle_fd_arg_attribute: one integer parameter position */
                kids_get(c, it.p[q], &ak);
                if (ak.n == 1)
                    (void)positional_arg(c, name, ak.p[0], 0, fty, iloc(c, tok),
                                         PA_INT, NULL);
                kids_free(&ak);
                continue;
            }
            if (!strcmp(name, "malloc") && type_ckind(TT, fty) == TY_FUNC &&
                !alloc_via_ptr) {
                /* handle_malloc_attribute */
                TypeId rt = type_base(TT, fty);
                uint32_t pr = alloc_name ? lookup_ord(c, alloc_name) : SYM_NONE;
                bool excl = false;      /* dropped by an earlier declaration's */
                if (pr != SYM_NONE && csym(c, pr)->kind == CS_FUNC)
                    excl = cdecl_aset_has(c, csym(c, pr)->aset, "noreturn", NULL) ||
                            cdecl_aset_has(c, csym(c, pr)->aset, "const", NULL) ||
                            cdecl_aset_has(c, csym(c, pr)->aset, "pure", NULL);
                if (!excl && type_ckind(TT, rt) != TY_PTR &&
                    type_ckind(TT, rt) != TY_ERROR) {
                    cwarn(c, iloc(c, tok), "attributes", "'malloc' attribute "
                          "ignored on functions returning %s; valid only for "
                          "pointer return types", type_q(TT, rt));
                    if (c->nign < 8)
                        snprintf(c->ign[c->nign++], sizeof c->ign[0], "malloc");
                }
                continue;
            }
            if (!strcmp(name, "access")) {
                kids_get(c, it.p[q], &ak);
                if (ak.n && access_check(c, ak.p, ak.n, iloc(c, tok), fty) &&
                    alloc_name && !alloc_via_ptr &&
                    type_ckind(TT, fty) == TY_FUNC) {
                    char buf[64];
                    AccSeen x;
                    cdecl_attr_args(c, it.p[q], buf, sizeof buf);
                    if (!acc_ready)
                        acc_replay(c);
                    if (acc_parse(buf, &x))
                        acc_add(c, &x, true);
                }
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
            if (!strcmp(name, "copy") &&
                type_ckind(TT, type_base(TT, fty)) != TY_PTR &&
                type_ckind(TT, fty) == TY_FUNC) {
                /* the copied alloc_align / alloc_size meet the same check */
                static const char *const an2[] = {"alloc_align", "alloc_size"};
                uint32_t s3[3], ns, m, w;
                kids_get(c, it.p[q], &ak);
                ns = ak.n == 1 ? cexpr_asets(c, ak.p[0], true, s3) : 0;
                if (ns) {   /* a source that ignored them has nothing to copy */
                    TypeId st = c->ty[ak.p[0]];
                    while (type_ckind(TT, st) == TY_PTR)
                        st = type_base(TT, st);
                    if (type_ckind(TT, st) == TY_FUNC &&
                        type_ckind(TT, type_base(TT, st)) != TY_PTR)
                        ns = 0;
                }
                for (w = 0; w < 2; w++)
                    for (m = 0; m < ns; m++)
                        if (cdecl_aset_has(c, s3[m], an2[w], NULL)) {
                            cwarn(c, iloc(c, tok), "attributes", "'%s' "
                                  "attribute ignored on a function returning "
                                  "%s", an2[w],
                                  type_q(TT, type_base(TT, fty)));
                            break;
                        }
                kids_free(&ak);
                continue;
            }
            if (!align && strcmp(name, "alloc_size"))
                continue;
            kids_get(c, it.p[q], &ak);
            if (ak.n && (align ? ak.n == 1 : ak.n <= 2)) {
                SrcLoc loc = iloc(c, tok);
                TypeId rt = type_base(TT, fty);
                if (type_ckind(TT, rt) != TY_PTR) {
                    cwarn(c, loc, "attributes", "'%s' attribute ignored on a "
                          "function returning %s", name, type_q(TT, rt));
                    if (c->nign < 8)
                        snprintf(c->ign[c->nign++], sizeof c->ign[0], "%.23s",
                                 name);
                } else {
                    for (i = 0; i < ak.n && ok; i++)
                        ok = positional_arg(c, name, ak.p[i],
                                            ak.n > 1 ? (int)i + 1 : 0, fty, loc,
                                            PA_INT, NULL);
                    if (ok && alloc_name && !alloc_via_ptr)
                        alloc_redecl(c, it.p[q], name, loc);
                }
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

void aset_add(Checker *c, uint32_t *set, const char *name,
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

void aset_drop(Checker *c, uint32_t set, const char *name)
{
    size_t k;
    for (k = set ? c->ahead.data[set - 1] : 0; k; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, name))
            c->anames.data[k - 1].name[0] = '';
}

/* A typedef keeps only its largest aligned attribute. */
void aset_keep_max_aligned(Checker *c, uint32_t set)
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
bool attrs_item_named(Checker *c, uint32_t holder, const char *want)
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

void attrs_names(Checker *c, uint32_t holder, uint32_t *set)
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
void attrs_names_ptrs(Checker *c, uint32_t h, uint32_t *set)
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
    {"always_inline", {"noinline", "noipa"}, false},
    {"gnu_inline", {"noinline", "noipa"}, false},
    {"noinline", {"always_inline", "gnu_inline"}, false},
    {"noipa", {"always_inline", "gnu_inline"}, false},
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
                /* noipa acts as noinline; one met on this declaration after
                 * an inline attribute makes gcc drop the inline attribute */
                bool noipa = !strcmp(an, "noipa"), swap = noipa && !prev;
                const char *ia = swap ? o : noipa ? "noinline" : an;
                const char *io = swap || !strcmp(o, "noipa") ? "noinline" : o;
                Diagnostic *d = cwarn_d(c, DL_WARNING, iloc(c, tok),
                                        "attributes", "ignoring attribute '%s' "
                                        "because it conflicts with attribute "
                                        "'%s'", ia, io);
                if (d && prev)
                    cnote(c, d, st->prevloc, "previous declaration here");
                else if (d && st->copy_set)
                    cnote(c, d, st->copy_loc, "previous declaration here");
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
                snprintf(c->ign[c->nign++], sizeof c->ign[0], "%.23s",
                         !strcmp(an, "noipa") && !prev ? o
                         : !strcmp(an, "noipa") ? "noinline" : an);
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
void attrs_copy_check(Checker *c, uint32_t holder, uint32_t kind,
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
                e = copy_target(c, e);
                if (ntag(c, e) == N_IDENT) {
                    uint32_t ref = lookup_ord(c, cnode_ident(c, e));
                    if (ref != SYM_NONE)
                        r = csym(c, ref);
                    else if (c->func_sym == SYM_NONE) {
                        cerror(c, cnode_loc(c, e), "'%s' undeclared here (not "
                               "in a function)", cident(c, cnode_ident(c, e)));
                        kids_free(&ak);
                        continue;
                    }
                }
                if (ntag(c, e) == N_STRING || (ntag(c, e) != N_IDENT && !r &&
                    (c->ck[e] == K_ICE || c->ck[e] == K_FOLD))) {
                    /* handle_copy_attribute: reported at the declarator */
                    uint32_t t = tok;
                    while (t > 0 && strcmp(tstr(c, t), cident(c, name)))
                        t--;
                    cerror(c, ctok_loc(c, t), ntag(c, e) == N_STRING ?
                           "'copy' attribute argument cannot be a string" :
                           "'copy' attribute argument cannot be a constant "
                           "arithmetic expression");
                    kids_free(&ak);
                    continue;
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
                } else if (kind == CS_OBJ) {
                    /* the referenced variable's common/nocommon is copied */
                    static const char *const cn[] = {"common", "nocommon"};
                    unsigned m;
                    for (m = 0; m < 2; m++)
                        if (cdecl_aset_has(c, r->aset, cn[m], NULL)) {
                            st->copy_set = true;
                            st->copy_loc = r->loc;
                            attr_excl_generic(c, tok, cn[m], kind, st);
                            st->copy_set = false;
                        }
                }
            }
            kids_free(&ak);
        }
        kids_free(&it);
    }
    kids_free(&k);
}

/* The attributes of node i's ATTRIBUTE children (direct). */
void attrs_of_children(Checker *c, uint32_t i, Attrs *a)
{
    Kids k;
    uint32_t j;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ATTRIBUTE)
            attr_collect(c, k.p[j], a);
    kids_free(&k);
}
