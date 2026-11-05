/* cexpr_bi.c - compound literals, statement expressions, __builtin_va_arg, offsetof, has_attribute, _Generic
 * (split from cexpr.c; see its header comment). */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- compound literals, statement expressions --------------------------------------- */

void e_complit(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[0]);
    if (is_err(c, t)) {
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ARRAY &&
        (type_ent(TT, type_canon(TT, t))->flags & TF_INCOMPLETE)) {
        /* cinit.c left the element count in the list's cv */
        uint64_t n = c->ck[k[1]] == K_ICE ? c->cv[k[1]] : 1;
        t = type_array(TT, elem_of(c, t), n);
    }
    cc90(c, ctok_loc(c, c->nodes[k[1]].tok), DW_NONE, "ISO C90 forbids compound "
         "literals");
    if (c->ck[k[1]] == K_ERR) {
        set_err(c, i);
        return;
    }
    if (!type_is_complete(TT, t)) {
        incomplete_error(c, cnode_loc(c, k[1]), NO_NODE, t);
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = EF_LVALUE;
    /* P2c: the initializer's contents */
    if (!in_function(c) && tkind(c, t) != TY_VLA) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = CB_NODE | i;
        addr_rvalue(c, i);
    }
}

/* The node of the value of the statement expression i (NO_NODE: none). */
uint32_t stmt_expr_value(Checker *c, uint32_t i)
{
    uint32_t k = i - 1, it;
    if (c->nodes[i].size < 4 || k < 3 || ntag(c, k - 1) != N_SCOPE_END)
        return NO_NODE;
    it = k - 2;
    while (it > 0 && (ntag(c, it) == N_LABEL || ntag(c, it) == N_CASE ||
                      ntag(c, it) == N_DEFAULT))
        it--;  /* a labeled last statement yields its own value */
    return ntag(c, it) == N_EXPR_STMT && c->nodes[it].size > 1 ? it - 1
                                                                : NO_NODE;
}

void e_stmt_expr(Checker *c, uint32_t i)
{
    uint32_t k = i - 1, e;
    SrcLoc loc = cnode_loc(c, i);
    if (cnode(c, i)->flags & NF_ERROR) {    /* the parser reported it */
        set_err(c, i);
        return;
    }
    if (!in_function(c)) {
        cerror(c, loc, "braced-group within expression allowed only inside a "
                       "function");
        set_err(c, i);
        return;
    }
    {
        size_t n0 = c->diag->all.len;
        ped(c, i, loc, "ISO C forbids braced-groups within expressions");
        /* gcc: after the body, before the statement-with-no-effect warnings
         * it defers to the end and before its scope closes */
        if (k >= 1 && ntag(c, k - 1) == N_SCOPE_END) {
            size_t at = c->dm[k - 1], j;
            Diagnostic **dd = c->diag->all.data;
            for (j = c->dm[cfirst(c, i)]; j < at && j < n0; j++)
                if (!strcmp(dd[j]->id, "unused-value")) {
                    at = j;
                    break;
                }
            choist_at(c, n0, at);
        }
    }
    c->ty[i] = TYPE_B(VOID);
    c->ef[i] = EF_SIDE;
    if ((e = stmt_expr_value(c, i)) != NO_NODE) {
        if (node_err(c, e)) {
            set_err(c, i);
            return;
        }
        c->ty[i] = rvt(c, e);
    }
}

/* ---- __builtin_va_arg, __builtin_convertvector, offsetof, types_compatible_p ----------- */

void e_va_arg(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t, vl = c->tt.va_list, et;
    bool ok = false;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[1]);
    if (node_err(c, k[0]) || is_err(c, t)) {
        set_err(c, i);
        return;
    }
    et = c->ty[k[0]];
    if (vl && type_compatible(TT, mainv(c, et), mainv(c, vl)))
        ok = true;
    else if (vl && is_array(c, vl) && is_ptr(c, rvt(c, k[0])) &&
             type_compatible(TT, mainv(c, rvt(c, k[0])),
                             type_ptr(TT, mainv(c, elem_of(c, vl)))))
        ok = true;
    if (!ok) {
        cerror(c, first_loc(c, k[1]), "first argument to 'va_arg' not of type "
                                     "'va_list'");
        set_err(c, i);
        return;
    }
    if (!complete(c, t)) {
        if (is_func(c, t))
            cerror(c, first_loc(c, k[1]), "second argument to 'va_arg' is a "
                   "function type %s", type_q(TT, t));
        else
            cerror(c, first_loc(c, k[1]), "second argument to 'va_arg' is of "
                   "incomplete type %s", type_q(TT, t));
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ENUM && cexpr_cxx_compat(c, i))
        cwarn(c, first_loc(c, k[1]), DW_CPP_COMPAT, "C++ requires promoted "
              "type, not enum type, in 'va_arg'");
    c->ty[i] = t;
    c->ef[i] = EF_SIDE;
}

static bool cv_elem_ok(Checker *c, TypeId vec)
{
    TypeId e = type_canon(TT, type_base(TT, vec));
    return type_is_integer(TT, e) || type_is_float(TT, e);
}

static uint64_t cv_count(Checker *c, TypeId vec)
{
    bool ok;
    uint64_t es = type_size(TT, type_base(TT, vec), &ok);
    return es ? type_size(TT, vec, &ok) / es : 0;
}

void e_convertvector(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[1]);
    if (node_err(c, k[0]) || is_err(c, t)) {
        set_err(c, i);
        return;
    }
    {
        /* c_build_vec_convert */
        TypeId a = type_canon(TT, c->ty[k[0]]), v = type_canon(TT, t);
        bool av = type_ckind(TT, a) == TY_VECTOR, tv = type_ckind(TT, v) == TY_VECTOR;
        if (!av || !cv_elem_ok(c, a)) {
            cerror(c, cnode_loc(c, i), "'__builtin_convertvector' first argument "
                   "must be an integer or floating vector");
            set_err(c, i);
            return;
        }
        if (!tv || !cv_elem_ok(c, v)) {
            cerror(c, first_loc(c, k[1]), "'__builtin_convertvector' second "
                   "argument must be an integer or floating vector type");
            set_err(c, i);
            return;
        }
        if (cv_count(c, a) != cv_count(c, v)) {
            cerror(c, cnode_loc(c, i), "'__builtin_convertvector' number of "
                   "elements of the first argument vector and the second "
                   "argument vector type should be the same");
            set_err(c, i);
            return;
        }
    }
    c->ty[i] = t;
    c->ef[i] = c->ef[k[0]] & EF_PROP;
}

/* gcc's input_location when offsetof's member is a bit-field: a struct,
 * union or enum specifier in the type name leaves it at the tag (or '{');
 * a later token that starts a line, or a plain typedef name, leaves the
 * line change's location.  peek: the ',' after the type name. */
static SrcLoc offsetof_bf_loc(Checker *c, uint32_t ty, uint32_t peek)
{
    uint32_t t, tag = 0;
    for (t = first_tok(c, ty); t < peek; t++) {
        int kw = tckw(c, t);
        if (kw == CK_STRUCT || kw == CK_UNION || kw == CK_ENUM)
            tag = t + 1;
    }
    for (t = tag + 1; tag && t <= peek; t++)
        if (c->u->toks[t].t.flags & TF_BOL)
            tag = 0;
    return tag ? ctok_loc(c, tag) : cinput_loc(c, peek);
}

/* One step of an offsetof designator, for fold_offsetof_1's bounds check. */
typedef struct OffStep {
    bool arr, known, cons, last, ptr;
    TypeId ty;
    uint64_t n, idx;
} OffStep;

/* fold_offsetof_1: an index beyond the array's last element (one past it is
 * fine when nothing is selected from the element) warns, unless the array is
 * a "poor man's flexible array": every component on the way back to the
 * start is the last member of its struct (or in a union). */
static void offsetof_bounds(Checker *c, const OffStep *st, uint32_t ns, SrcLoc loc)
{
    uint32_t j, q;
    if (!diag_enabled(c->diag, DW_ARRAY_BOUNDS_V))
        return;
    for (j = 0; j < ns; j++) {
        uint64_t up;
        if (!st[j].arr || !st[j].known || !st[j].cons)
            continue;
        up = st[j].n - 1;
        if (j + 1 == ns)
            up++;                   /* the outermost reference: one past */
        if (up >= st[j].idx)
            continue;
        for (q = j; q > 0 && !st[q - 1].arr && !st[q - 1].ptr && st[q - 1].last;)
            q--;
        if (q == 0)
            continue;               /* reached the base: a trailing array */
        cwarn(c, loc, DW_ARRAY_BOUNDS_V, "index %llu denotes an offset greater "
              "than size of %s", (unsigned long long)st[j].idx,
              type_q(TT, st[j].ty));
    }
}

void e_offsetof(Checker *c, uint32_t i)
{
    uint32_t k[64], n = nkids(c, i, k, 64), j;
    TypeId t, cur;
    uint64_t off = 0;
    bool konst = true, nonconst_addr = false, varsz = false;
    OffStep st[64] = {{0}};
    uint32_t ns = 0;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[0]);
    if (is_err(c, t)) {
        set_err(c, i);
        return;
    }
    cur = t;
    for (j = 1; j < n; j++) {
        unsigned tag = ntag(c, k[j]);
        if (tag == N_NAME || tag == N_DESIG_FIELD) {
            uint32_t name = cnode_ident(c, k[j]);
            SrcLoc loc = cnode_loc(c, k[j]);
            const Field *f;
            uint64_t o = 0;
            unsigned q = 0;
            if (tag == N_DESIG_FIELD && c->nodes[k[j]].flags && is_array(c, cur))
                cur = elem_of(c, cur);
            if (!is_record(c, cur)) {
                if (!is_err(c, cur))
                    cerror(c, loc, "request for member '%s' in something not a "
                           "structure or union", cident(c, name));
                set_err(c, i);
                return;
            }
            if (!complete(c, cur)) {
                incomplete_error(c, loc, NO_NODE, cur);
                set_err(c, i);
                return;
            }
            f = find_field(c, cur, name, &o, &q);
            if (!f) {
                cerror(c, loc, "%s has no member named '%s'", type_q(TT, cur),
                       cident(c, name));
                set_err(c, i);
                return;
            }
            if (c->sx)
                csx_field(c, (uint32_t)(f - c->tt.fields.data),
                          c->nodes[k[j]].tok, CIX_REF);
            if (f->flags & FF_BITFIELD) {
                cerror(c, offsetof_bf_loc(c, k[0], first_tok(c, k[1]) - 1),
                       "attempt to take address of bit-field structure member "
                       "'%s'", cident(c, name));
                set_err(c, i);
                return;
            }
            off += o / 8;
            if (ns < 64) {
                const Record *rec = type_record(TT, type_canon(TT, cur));
                st[ns].arr = false;
                st[ns].last = (rec->flags & RF_UNION) ||
                              f == &TT->fields.data[rec->fields + rec->nfields - 1];
                ns++;
            }
            cur = f->ty;
        } else if (tag == N_DESIG_INDEX) {
            uint32_t e = first_child(c, k[j]);
            if (e == NO_NODE || node_err(c, e)) {
                set_err(c, i);
                return;
            }
            if (tkind(c, cur) == TY_PTR) {   /* *(p + n): reported once the
                                              * whole designator is known */
                nonconst_addr = true;
                konst = false;
                cur = pointee(c, cur);
                if (ns < 64) {
                    st[ns].arr = false;
                    st[ns].last = false;
                    st[ns++].ptr = true;
                }
                continue;
            }
            if (!is_array(c, cur)) {
                cerror(c, cnode_loc(c, k[j]), "subscripted value is neither "
                       "array nor pointer nor vector");
                set_err(c, i);
                return;
            }
            if (!is_int(c, rvt(c, e))) {
                cerror(c, cnode_loc(c, k[j]), "array subscript is not an "
                       "integer");
                set_err(c, i);
                return;
            }
            c->cv[k[j]] = 0;    /* else the element size: the index is not constant */
            if (has_ival(c, e) && !var_size(c, elem_of(c, cur)))
                off += (uint64_t)(int64_t)c->cv[e] * elem_size(c, type_ptr(TT,
                                                                 elem_of(c, cur)));
            else {
                konst = false;
                if (var_size(c, elem_of(c, cur)))
                    varsz = true;
                else
                    c->cv[k[j]] = elem_size(c, type_ptr(TT, elem_of(c, cur)));
            }
            if (ns < 64) {
                const TypeEnt *te = type_ent(TT, type_canon(TT, cur));
                st[ns].arr = true;
                st[ns].ty = cur;
                st[ns].known = tkind(c, cur) == TY_ARRAY && te->n &&
                               !(te->flags & TF_INCOMPLETE);
                st[ns].n = te->n;
                st[ns].cons = has_ival(c, e) && (int64_t)c->cv[e] >= 0;
                st[ns].idx = (uint64_t)c->cv[e];
                ns++;
            }
            cur = elem_of(c, cur);
        }
    }
    if (nonconst_addr) {
        cerror(c, offsetof_bf_loc(c, k[0], first_tok(c, k[1]) - 1),
               "cannot apply 'offsetof' to a non constant address");
        set_err(c, i);
        return;
    }
    offsetof_bounds(c, st, ns, offsetof_bf_loc(c, k[0], first_tok(c, k[1]) - 1));
    c->ty[i] = size_type(c);
    if (konst)
        set_ice(c, i, size_type(c), off);
    else
        c->cv[i] = varsz ? ~(uint64_t)0 : off;  /* the constant part, for the T-AST */
}

void e_types_compat(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId a, b;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    a = type_of_typename(c, k[0]);
    b = type_of_typename(c, k[1]);
    if (is_err(c, a) || is_err(c, b)) {
        set_err(c, i);
        return;
    }
    set_ice(c, i, TYPE_B(INT), type_compatible(TT, mainv(c, a), mainv(c, b)));
}

/* ---- __builtin_has_attribute ---------------------------------------------------------- */

/* The field a member-access node names (no diagnostics), and its record. */
const Field *member_field_of(Checker *c, uint32_t x, TypeId *recp)
{
    uint32_t d = first_child(c, x);
    TypeId rt;
    uint64_t off;
    unsigned q;
    if (d == NO_NODE || node_err(c, d))
        return NULL;
    rt = c->ty[d];
    if (c->nodes[x].flags & NF_ARROW) {
        if (type_ckind(TT, rt) != TY_PTR && type_ckind(TT, rt) != TY_ARRAY)
            return NULL;
        rt = pointee(c, rt);
    }
    rt = type_canon(TT, rt);
    if (!is_record(c, rt))
        return NULL;
    if (recp)
        *recp = rt;
    return find_field(c, rt, cnode_ident(c, x), &off, &q);
}

/* The attribute sets of the typedefs t goes through, appended to out[0..n)
 * (at most max).  An array without an aligned attribute of its own takes
 * its element type's (gcc propagates the user-alignment bit). */
static unsigned typedef_asets(Checker *c, TypeId t, uint32_t *out, unsigned n,
                              unsigned max)
{
    bool al = false;
    for (;;) {
        while (type_kind(TT, t) == TY_TYPEDEF) {
            uint32_t s = cdecl_typedef_aset(c, t);
            if (s && n < max) {
                out[n++] = s;
                al |= cdecl_aset_has(c, s, "aligned", NULL);
            }
            t = type_ent(TT, t)->base;
        }
        if (al || type_kind(TT, t) != TY_ARRAY)
            return n;
        t = type_ent(TT, t)->base;
    }
}

/* The sets of attribute names that belong to the expression e: its declaration
 * (a symbol, a member), and its type's record.  strip: look through the
 * pointers and arrays of the type (what 'copy' does). */
unsigned cexpr_asets(Checker *c, uint32_t e, bool strip, uint32_t out[3])
{
    unsigned n = 0;
    uint32_t x = strip_paren(c, e);
    TypeId t;
    bool comma = false;
    if (x == NO_NODE)
        return 0;
    while (ntag(c, x) == N_BINARY && npunct(c, x) == P_COMMA) {
        uint32_t bk[3];
        if (nkids(c, x, bk, 3) < 2)
            break;
        x = strip_paren(c, bk[1]);     /* a comma expression: its value */
        comma = true;
    }
    if (ntag(c, x) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, x));
        if (ref != SYM_NONE && csym(c, ref)->aset)
            out[n++] = csym(c, ref)->aset;
    } else if (ntag(c, x) == N_MEMBER_EXPR) {
        const Field *f = member_field_of(c, x, NULL);
        if (f && f->aset)
            out[n++] = f->aset;
    }
    if (node_err(c, x))
        return n;
    t = c->ty[x];
    if (comma && type_ckind(TT, t) == TY_ARRAY)    /* C: it decays */
        t = type_ptr(TT, type_base(TT, t));
    while (strip && (type_ckind(TT, t) == TY_PTR || type_ckind(TT, t) == TY_ARRAY))
        t = type_base(TT, t);
    n = typedef_asets(c, t, out, n, 3);
    t = type_canon(TT, t);
    if (is_record(c, t) && type_record(TT, t)->aset && n < 3)
        out[n++] = type_record(TT, t)->aset;
    return n;
}

void e_has_attr(Checker *c, uint32_t i)
{
    uint32_t k[2], sets[3], n = 0, j;
    char an[32], args[24];
    bool has = false, nonnull = false;
    if (nkids(c, i, k, 2) < 2 || ntag(c, k[1]) != N_ATTR_ITEM) {
        set_err(c, i);
        return;
    }
    {
        size_t len;
        char raw[48];
        const char *tx = ttext(c, c->nodes[k[1]].tok, &len);
        if (len >= sizeof raw)
            len = sizeof raw - 1;
        memcpy(raw, tx, len);
        raw[len] = 0;
        cdecl_attr_name(raw, an, sizeof an);
        cdecl_attr_args(c, k[1], args, sizeof args);
        nonnull = !strcmp(an, "nonnull");
        if (!attr_is_known(an)) {
            cerror(c, ctok_loc(c, c->nodes[k[1]].tok), "unknown attribute "
                   "'%s'", raw);
            set_err(c, i);
            return;
        }
    }
    if (!strcmp(an, "mode")) {
        cwarn(c, ctok_loc(c, c->nodes[k[1]].tok), DW_ATTRIBUTES, "'mode' attribute "
              "not supported in '__builtin_has_attribute'");
        set_ice(c, i, TYPE_B(INT), 0);
        return;
    }
    if (!strcmp(an, "aligned") && !strcmp(args, "0"))
        cwarn(c, cnode_loc(c, i), DW_ATTRIBUTES, "requested alignment '0' is "
              "not a positive power of 2");
    if (!strcmp(an, "aligned") && args[0] && !isdigit((unsigned char)args[0])) {
        uint32_t ak[2];
        if (nkids(c, k[1], ak, 2) >= 1 && !node_err(c, ak[0]) &&
            !is_intcst(c, ak[0])) {
            cerror(c, cdecl_line_start_loc(c, c->nodes[i].tok), "requested "
                   "alignment is not an integer constant");
            set_ice(c, i, TYPE_B(INT), 0);     /* gcc goes on with 0 */
            return;
        }
    }
    if (!strcmp(an, "aligned") && args[0] >= '1' && args[0] <= '9') {
        unsigned long long av = strtoull(args, NULL, 10);
        if (av & (av - 1))
            cerror(c, cdecl_line_start_loc(c, c->nodes[i].tok), "requested "
                   "alignment '%s' is not a positive power of 2", args);
    }
    if (!strcmp(an, "vector_size")) {
        /* a property of the (vector) type itself, never of a declaration */
        TypeId t;
        if (ntag(c, k[0]) == N_TYPE_NAME)
            t = type_of_typename(c, k[0]);
        else if (node_err(c, k[0])) {
            set_err(c, i);
            return;
        } else
            t = c->ty[k[0]];
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
        t = type_canon(TT, t);
        set_ice(c, i, TYPE_B(INT),
                type_kind(TT, t) == TY_VECTOR &&
                (!args[0] || (uint64_t)atoll(args) == type_ent(TT, t)->n));
        return;
    }
    if (ntag(c, k[0]) == N_TYPE_NAME) {
        TypeId t = type_of_typename(c, k[0]);
        uint32_t q, tmp = 0;
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
        n = typedef_asets(c, t, sets, n, 2);
        t = type_canon(TT, t);
        if (is_record(c, t))
            sets[n++] = type_record(TT, t)->aset;
        /* attributes written in the type name itself */
        for (q = cfirst(c, k[0]); q < k[0]; q++)
            if (ntag(c, q) == N_ATTRIBUTE)
                cdecl_attrs_names(c, q, &tmp);
        if (tmp)
            sets[n++] = tmp;
    } else {
        if (node_err(c, k[0])) {
            set_err(c, i);
            return;
        }
        n = cexpr_asets(c, k[0], false, sets);
    }
    if ((!strcmp(an, "alloc_align") || !strcmp(an, "alloc_size")) && args[0]) {
        /* gcc applies the handler to the operand: a function that does not
         * return a pointer ignores it, one that has the attribute with other
         * arguments keeps its own */
        TypeId ft = ntag(c, k[0]) == N_TYPE_NAME ? type_of_typename(c, k[0])
                                                : c->ty[k[0]];
        if (!is_err(c, ft) && type_ckind(TT, type_canon(TT, ft)) != TY_FUNC)
            cwarn(c, cdecl_line_start_loc(c, c->nodes[i].tok), DW_ATTRIBUTES,
                  "'%s' attribute only applies to function types", an);
        else if (!is_err(c, ft) && type_ckind(TT, type_canon(TT, ft)) == TY_FUNC) {
            TypeId rt = type_base(TT, type_canon(TT, ft));
            char was[96];
            if (type_ckind(TT, type_canon(TT, rt)) != TY_PTR)
                cwarn(c, cdecl_line_start_loc(c, c->nodes[i].tok), DW_ATTRIBUTES, "'%s' attribute "
                      "ignored on a function returning %s", an, type_q(TT, rt));
            else
                for (j = 0; j < n; j++)
                    if (cdecl_aset_first_arg(c, sets[j], an, was, sizeof was)) {
                        if (strcmp(was, args))
                            cdecl_alloc_conflict(c, cdecl_line_start_loc(c, c->nodes[i].tok), an, args, was);
                        break;
                    }
        }
    }
    for (j = 0; j < n; j++)
        {
            has |= cdecl_aset_has(c, sets[j], an, args[0] || nonnull ? args : NULL);
        }
    set_ice(c, i, TYPE_B(INT), has);
}

/* ---- _Generic ------------------------------------------------------------------------ */

void e_generic_assoc(Checker *c, uint32_t i)
{
    copy_node(c, i, i - 1);
}

void e_generic(Checker *c, uint32_t i)
{
    uint32_t k[64], n = nkids(c, i, k, 64), j, match = NO_NODE, deflt = NO_NODE;
    TypeId sel, ts[64];
    bool bad = false;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    if (!cexpr_in_extension(c, i))
        cped11(c, cnode_loc(c, i), "ISO C99 does not support '_Generic'");
    if (node_err(c, k[0])) {
        set_err(c, i);
        return;
    }
    sel = rvt(c, k[0]);
    sel = mainv(c, sel);
    for (j = 1; j < n; j++) {
        uint32_t tn = first_child(c, k[j]);
        SrcLoc loc;
        uint32_t m;
        ts[j] = 0;
        if (tn == NO_NODE)
            continue;
        if (ntag(c, tn) != N_TYPE_NAME) {
            if (deflt != NO_NODE) {
                Diagnostic *d = cerror_d(c, cnode_loc(c, k[j]), "duplicate "
                                         "'default' case in '_Generic'");
                cnote(c, d, cnode_loc(c, deflt), "original 'default' is here");
                bad = true;
            } else {
                deflt = k[j];
            }
            continue;
        }
        loc = first_loc(c, tn);
        ts[j] = type_of_typename(c, tn);
        if (is_err(c, ts[j])) {
            ts[j] = 0;
            bad = true;
            continue;
        }
        if (is_func(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has function type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        if (!complete(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has incomplete type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        if (var_size(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has variable length type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        for (m = 1; m < j; m++)
            if (ts[m] && type_compatible(TT, ts[m], ts[j])) {
                Diagnostic *d = cerror_d(c, loc, "'_Generic' specifies two "
                                         "compatible types");
                cnote(c, d, first_loc(c, first_child(c, k[m])),
                      "compatible type is here");
                bad = true;
                break;
            }
    }
    for (j = 1; j < n; j++) {
        if (!ts[j] || !type_compatible(TT, sel, ts[j]))
            continue;
        if (match != NO_NODE) {
            Diagnostic *d = cerror_d(c, cnode_loc(c, k[j]), "'_Generic' "
                                     "selector matches multiple associations");
            cnote(c, d, cnode_loc(c, match), "other match is here");
            bad = true;
        } else {
            match = k[j];
        }
    }
    if (match == NO_NODE)
        match = deflt;
    if (match == NO_NODE) {
        cerror(c, first_loc(c, k[0]), "'_Generic' selector of type %s is not "
               "compatible with any association", type_q(TT, sel));
        set_err(c, i);
        return;
    }
    if (bad) {
        set_err(c, i);
        return;
    }
    copy_node(c, i, match);
}

void e_addr_label(Checker *c, uint32_t i)
{
    uint32_t tok = c->nodes[i].tok;
    ped(c, i, cinput_loc(c, tok + 1), "taking the address of a "
                                                    "label is non-standard");
    if (!in_function(c) || c->kr_decls) {
        cerror(c, cinput_loc(c, tok + 1), "label '%s' referenced outside of "
               "any function", cident(c, cnode_ident(c, i)));
        c->ty[i] = type_ptr(TT, TYPE_B(VOID));
        c->ck[i] = K_ADDR;
        c->ef[i] = EF_NPC;
        return;
    }
    c->ty[i] = type_ptr(TT, TYPE_B(VOID));
    c->ck[i] = K_ADDR;
    c->cb[i] = CB_NODE | i;
}

