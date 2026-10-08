/* cmerge.c - duplicate declarations: merging a new declaration of a name
 * with the earlier one (gcc's duplicate_decls / merge_decls and the
 * diagnostics of diagnose_mismatched_decls).  Called from pushdecl in
 * cdecl.c; the shared helpers are in cdecl_int.h. */
#include "c/cdecl_int.h"

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

/* gcc's tree code of a symbol. */
static int dcode(const CSym *s)
{
    return s->kind * 2 + (s->kind == CS_OBJ && (s->flags & CSF_PARAM) ? 1 : 0);
}

/* The type a typedef names (a typedef symbol's ty is its own entry). */
TypeId cdecl_typedef_under(Checker *c, TypeId t)
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

/* The note of cdecl_locate_old_decl. */
void cdecl_locate_old_decl(Checker *c, Diagnostic *d, const CSym *o)
{
    if (!d)
        return;
    if (sym_defined(o) || o->kind == CS_ENUMCONST)
        cnote(c, d, o->loc, "previous definition of '%s' with type %s",
              sname(c, o), type_q_decl(TT, o->ty));
    else if (o->flags & CSF_IMPLICIT)
        cnote(c, d, o->loc, "previous implicit declaration of '%s' with type "
              "%s", sname(c, o), type_q_decl(TT, o->ty));
    else
        cnote(c, d, o->loc, "previous declaration of '%s' with type %s",
              sname(c, o), type_q_decl(TT, o->ty));
}

/* -Wtraditional: a file-scope declaration without 'static' after a static one. */
static void nonstatic_follows_static(Checker *c, CSym *nw, const CSym *o)
{
    Diagnostic *d;
    if (cin_system(c, nw->loc))
        return;
    d = cwarn_d(c, DL_WARNING, nw->loc, "traditional", "non-static declaration "
                "of '%s' follows static declaration", sname(c, nw));
    if (d)
        cdecl_locate_old_decl(c, d, o);
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

static bool has_err_param(Checker *c, TypeId t)
{
    TypeId ct = type_canon(TT, t);
    uint32_t k;
    for (k = 0; k < (uint32_t)type_ent(TT, ct)->n; k++)
        if (is_err(c, type_params(TT, ct)[k]))
            return true;
    return false;
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
        cdecl_locate_old_decl(c, d, o);
        return false;
    }
    if (o->kind == CS_ENUMCONST) {
        d = cerror_d(c, nw->loc, "redeclaration of enumerator '%s'",
                     sname(c, nw));
        cdecl_locate_old_decl(c, d, o);
        return false;
    }
    {
        TypeId a = o->kind == CS_TYPEDEF ? cdecl_typedef_under(c, oldtype) : oldtype;
        TypeId b = o->kind == CS_TYPEDEF ? cdecl_typedef_under(c, newtype) : newtype;
        /* gcc's noreturn is a volatile function type: 'volatile' written on
         * one declaration agrees with 'noreturn' on the other */
        if (nw->kind == CS_FUNC && type_ckind(TT, a) == TY_FUNC &&
            type_ckind(TT, b) == TY_FUNC) {
            if (!(TYPE_QUALS(a) & TQ_VOLATILE) && (TYPE_QUALS(b) & TQ_VOLATILE) &&
                (o->flags & CSF_NORETURN))
                a |= TQ_VOLATILE;
            else if (!(TYPE_QUALS(b) & TQ_VOLATILE) &&
                     (TYPE_QUALS(a) & TQ_VOLATILE) && (nw->flags & CSF_NORETURN))
                b |= TQ_VOLATILE;
            /* a qualifier written on a function (a typedef of function type)
             * is a flag of its declaration, not part of the type */
            if ((TYPE_QUALS(a) ^ TYPE_QUALS(b)) & (TQ_CONST | TQ_VOLATILE)) {
                a &= ~(TypeId)(TQ_CONST | TQ_VOLATILE);
                b &= ~(TypeId)(TQ_CONST | TQ_VOLATILE);
            }
        }
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
                cdecl_locate_old_decl(c, d, o);
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
        TypeId a = cdecl_typedef_under(c, oldtype), b = cdecl_typedef_under(c, newtype);
        if (type_canon(TT, a) != type_canon(TT, b)) {
            d = cerror_d(c, nw->loc, "redefinition of typedef '%s' with "
                         "different type", sname(c, nw));
            cdecl_locate_old_decl(c, d, o);
            return false;
        }
        if (type_is_vm(TT, b)) {
            d = cerror_d(c, nw->loc, "redefinition of typedef '%s' with "
                         "variably modified type", sname(c, nw));
            cdecl_locate_old_decl(c, d, o);
        } else {
            d = cped11(c, nw->loc, "redefinition of typedef '%s'",
                          sname(c, nw));
            cdecl_locate_old_decl(c, d, o);
        }
        return true;
    } else if (nw->kind == CS_FUNC) {
        if (sym_defined(nw)) {
            /* an extern inline (gnu_inline) definition may be overridden */
            if (sym_defined(o) &&
                (!extern_inline(o) || extern_inline(nw) ||
                 !(o->flags & CSF_GNU_INLINE))) {
                d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
                cdecl_locate_old_decl(c, d, o);
                return false;
            }
        } else if (sym_defined(o) && !is_prototype(c, oldtype) &&
                   !o->olddef_merged &&
                   is_prototype(c, newtype) &&
                   (type_ent(TT, type_canon(TT, oldtype))->n ||
                    !has_err_param(c, newtype))) {
            if (!validate_proto_after_old_defn(c, nw, o, &d)) {
                cdecl_locate_old_decl(c, d, o);
                return false;
            }
        }
        if (sym_public(o) && !sym_public(nw)) {
            if (!extern_inline(o)) {
                d = cerror_d(c, nw->loc, "static declaration of '%s' follows "
                             "non-static declaration", sname(c, nw));
                cdecl_locate_old_decl(c, d, o);
            }
            return false;
        } else if (sym_public(nw) && !sym_public(o)) {
            if (!ofile) {
                d = cerror_d(c, nw->loc, "non-static declaration of '%s' "
                             "follows static declaration", sname(c, nw));
                cdecl_locate_old_decl(c, d, o);
                return false;
            }
            nonstatic_follows_static(c, nw, o);
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
            cdecl_locate_old_decl(c, d, o);
            return false;
        }
        if (sym_defined(nw) && sym_defined(o)) {
            d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
            cdecl_locate_old_decl(c, d, o);
            return false;
        }
        if (nfile && sym_public(nw) != sym_public(o)) {
            if (sym_external(nw)) {
                if (!ofile) {
                    d = cerror_d(c, nw->loc, "extern declaration of '%s' "
                                 "follows declaration with no linkage",
                                 sname(c, nw));
                    cdecl_locate_old_decl(c, d, o);
                    return false;
                }
                if (sym_public(nw))
                    nonstatic_follows_static(c, nw, o);
            } else {
                if (sym_public(nw))
                    d = cerror_d(c, nw->loc, "non-static declaration of '%s' "
                                 "follows static declaration", sname(c, nw));
                else
                    d = cerror_d(c, nw->loc, "static declaration of '%s' "
                                 "follows non-static declaration",
                                 sname(c, nw));
                cdecl_locate_old_decl(c, d, o);
                return false;
            }
        } else if (!nfile) {
            if (sym_external(nw)) {
                /* extern with initializer at block scope: an error already */
            } else if (sym_external(o)) {
                d = cerror_d(c, nw->loc, "declaration of '%s' with no linkage "
                             "follows extern declaration", sname(c, nw));
                cdecl_locate_old_decl(c, d, o);
            } else {
                d = cerror_d(c, nw->loc, "redeclaration of '%s' with no "
                             "linkage", sname(c, nw));
                cdecl_locate_old_decl(c, d, o);
            }
            return false;
        }
    }
    if (nw->kind == CS_OBJ && nfile && ofile && !sym_external(nw) &&
        !sym_external(o) && !cin_system(c, nw->loc)) {
        d = cwarn_d(c, DL_WARNING, nw->loc, "c++-compat", "duplicate "
                    "declaration of '%s' is invalid in C++", sname(c, nw));
        cdecl_locate_old_decl(c, d, o);
    }
    if (nw->kind == CS_OBJ && (nw->flags & CSF_PARAM) &&
        !(o->flags & CSF_FWD)) {
        d = cerror_d(c, nw->loc, "redefinition of parameter '%s'",
                     sname(c, nw));
        cdecl_locate_old_decl(c, d, o);
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
        cdecl_locate_old_decl(c, wd, note_new ? &tmp : o);
    }
    return true;
}

/* handle_weak_attribute + declare_weak for a declaration of an object or
 * function: an inline function ignores it, a symbol without external linkage
 * cannot be weak. */
void cdecl_weak_apply(Checker *c, CSym *s, bool is_inline)
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

/* Whether the specifiers sn or the declarator idecl give the function
 * noinline (noipa implies it), unless the attribute was ignored. */
static bool given_noinline(Checker *c, uint32_t sn, uint32_t idecl)
{
    unsigned k;
    bool r = false;
    static const char *const names[] = {"noinline", "noipa"};
    for (k = 0; k < 2; k++)
        r = r || (sn != NO_NODE && attrs_item_named(c, sn, names[k])) ||
            attrs_item_named(c, idecl, names[k]);
    for (k = 0; k < c->nign; k++)
        if (!strcmp(c->ign[k], "noinline"))
            r = false;
    return r;
}

/* The deallocator named by a malloc attribute `holder` writes (into out). */
static bool attrs_malloc_dealloc(Checker *c, uint32_t holder, char *out,
                                 size_t n)
{
    Kids k;
    uint32_t j;
    bool found = false;
    if (holder == NO_NODE)
        return false;
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
            if (!strcmp(an, "malloc")) {
                char *cm;
                cdecl_attr_args(c, it.p[q], out, n);
                cm = strchr(out, ',');
                if (cm)
                    *cm = 0;
                /* gcc's own deallocators imply no noinline */
                found = out[0] && !isdigit((unsigned char)out[0]) &&
                        strncmp(out, "__builtin_", 10) &&
                        strcmp(out, "free") && strcmp(out, "realloc");
            }
        }
        kids_free(&it);
    }
    kids_free(&k);
    return found;
}

/* The first token of loc's line, where gcc's input_location is. */
static SrcLoc bol_loc(Checker *c, SrcLoc loc)
{
    uint32_t line, col = 1, len;
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    const char *t;
    if (!f)
        return loc;
    srcmgr_linecol(f, loc, &line, &col);
    t = srcmgr_line_text(f, line, &len);
    for (col = 1; col <= len && (t[col - 1] == ' ' || t[col - 1] == '	'); col++)
        ;
    return srcmgr_loc_of(f, line, col);
}

/* An inline function given the noinline attribute (malloc with a
 * deallocator implies it; with optimization it is ignored instead). */
void cdecl_inline_given(Checker *c, const CSym *s, bool is_inline,
                        uint32_t sn, uint32_t idecl)
{
    char dn[64];
    if (s->kind == CS_FUNC && is_inline &&
        (attrs_malloc_dealloc(c, sn, dn, sizeof dn) ||
         attrs_malloc_dealloc(c, idecl, dn, sizeof dn))) {
        if (c->opt.optimize)
            cwarn(c, bol_loc(c, s->loc), "attributes", "'malloc (%s)' "
                  "attribute ignored on functions declared 'inline'", dn);
        else
            cwarn(c, s->loc, "attributes", "inline function '%s' given "
                  "attribute 'noinline'", sname(c, s));
        return;
    }
    if (s->kind == CS_FUNC && is_inline && given_noinline(c, sn, idecl))
        cwarn(c, s->loc, "attributes", "inline function '%s' given attribute "
              "'noinline'", sname(c, s));
}

/* diagnose_mismatched_decls: an inline declaration after one with
 * noinline, or noinline after an inline one. */
/* Append the options of one optimize("...") argument string (quotes and all
 * as cdecl_attr_args writes it) to acc, comma separated. */
static void opt_acc(char *acc, size_t n, const char *arg)
{
    size_t l = strlen(acc);
    for (; *arg && l + 2 < n; arg++) {
        if (*arg == '"')
            continue;
        acc[l++] = *arg == ' ' ? ',' : *arg;
    }
    if (l + 2 < n)
        acc[l++] = ',';
    acc[l] = 0;
}

static int opt_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The accumulated options in a form that is equal exactly when gcc's
 * optimization nodes are: an -O level makes the order significant (it only
 * defaults flags not set so far), otherwise the order does not matter. */
static void opt_canon(char *acc)
{
    char *tok[64], *s, *e;
    size_t nt = 0, k;
    bool lvl = false;
    for (s = acc; *s && nt < 64; s = e) {
        e = strchr(s, ',');
        if (!e)
            break;
        *e++ = 0;
        if (*s) {
            tok[nt++] = s;
            lvl = lvl || *s == 'O' || (*s >= '0' && *s <= '9');
        }
    }
    if (!lvl)
        qsort(tok, nt, sizeof *tok, opt_cmp);
    {
        char out[512];
        size_t l = 0;
        for (k = 0; k < nt && l + 2 < sizeof out; k++)
            l += (size_t)snprintf(out + l, sizeof out - l, "%s,", tok[k]);
        out[l] = 0;
        memcpy(acc, out, l + 1);
    }
}

static void attrs_opt_args(Checker *c, uint32_t holder, char *acc, size_t n)
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
            char an[32], arg[256];
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (strcmp(an, "optimize"))
                continue;
            cdecl_attr_args(c, it.p[q], arg, sizeof arg);
            opt_acc(acc, n, arg);
        }
        kids_free(&it);
    }
    kids_free(&k);
}

static void aset_opt_args(const Checker *c, uint32_t set, char *acc, size_t n)
{
    size_t k, ord[32], m = 0;
    if (!set)
        return;
    for (k = c->ahead.data[set - 1]; k && m < 32; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, "optimize"))
            ord[m++] = k;
    while (m--)
        opt_acc(acc, n, c->anames.data[ord[m] - 1].arg);
}

/* Whether the optimize options written on the new declaration differ from
 * the definition's (none at all counts as different). */
static bool opt_mismatch(Checker *c, const CSym *o, uint32_t sn, uint32_t idecl)
{
    char a[512] = "", b[512] = "";
    aset_opt_args(c, o->aset, a, sizeof a);
    if (!*a)
        return true;
    if (sn != NO_NODE)
        attrs_opt_args(c, sn, b, sizeof b);
    attrs_opt_args(c, idecl, b, sizeof b);
    opt_canon(a);
    opt_canon(b);
    return strcmp(a, b) != 0;
}

void cdecl_inline_follows(Checker *c, const CSym *nw, uint32_t ltok,
                          uint32_t sn, uint32_t idecl)
{
    uint32_t ref = lookup_ord(c, nw->name);
    const CSym *o;
    Diagnostic *d = NULL;
    bool nw_noinline;
    if (ref == SYM_NONE || csym(c, ref)->kind != CS_FUNC)
        return;
    o = csym(c, ref);
    nw_noinline = given_noinline(c, sn, idecl);
    if ((nw->flags & CSF_INLINE) && !(o->flags & CSF_INLINE) &&
        cdecl_aset_has(c, o->aset, "noinline", NULL))
        d = cwarn_d(c, DL_WARNING, iloc(c, ltok), "attributes", "inline declaration "
                    "of '%s' follows declaration with attribute 'noinline'",
                    sname(c, nw));
    else if ((o->flags & CSF_INLINE) && nw_noinline)
        d = cwarn_d(c, DL_WARNING, nw->loc, "attributes", "declaration of "
                    "'%s' with attribute 'noinline' follows inline "
                    "declaration", sname(c, nw));
    else if ((o->flags & CSF_DEFINED) &&
             ((sn != NO_NODE && attrs_item_named(c, sn, "optimize")) ||
              attrs_item_named(c, idecl, "optimize")) &&
             opt_mismatch(c, o, sn, idecl))
        d = cwarn_d(c, DL_WARNING, iloc(c, ltok), "attributes", "optimization "
                    "attribute on '%s' follows definition but the attribute "
                    "doesn't match", sname(c, nw));
    cdecl_locate_old_decl(c, d, o);
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
    if (nw->kind == CS_TYPEDEF) {
        m.ty = o->ty;
        if (infunc)
            m.flags |= CSF_USED;   /* gcc: a redeclared local typedef is used */
    } else
        m.ty = type_composite(TT, newtype, oldtype);
    if ((!sym_defined(nw) && sym_defined(o)) || (old_proto && !new_proto))
        m.loc = o->loc;
    m.flags |= o->flags & (CSF_DEFINED | CSF_USED | CSF_CUSED | CSF_NORETURN | CSF_THREAD |
                           CSF_INLINE | CSF_BLOCK_EXTERN | CSF_TENTATIVE |
                           CSF_WEAK | CSF_WEAKREF | CSF_ADDR_WARNED | CSF_DEPRECATED |
                           CSF_UNAVAILABLE | CSF_INNER_COMP | CSF_GNU_INLINE | CSF_PURE | CSF_CONSTFN);
    m.olddef_merged = o->olddef_merged ||
                      (new_def && !new_proto && !sym_defined(o));
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
    if (o->fmtarg)
        m.fmtarg = o->fmtarg;
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

bool cdecl_duplicate_decls(Checker *c, CSym *nw, bool nfile, uint32_t oldref,
                           bool implicit_int)
{
    TypeId nt, ot;
    CSym *o = csym(c, oldref);
    if (!diagnose_mismatched(c, nw, nfile, o, ref_file_scope(oldref),
                             implicit_int, &nt, &ot)) {
        c->redecl_failed = true;
        return false;
    }
    c->vis_old = *o;
    c->vis_old_ok = true;
    merge_decls(c, nw, o, nt, ot);
    return true;
}
