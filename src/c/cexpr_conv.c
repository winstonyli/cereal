/* cexpr_conv.c - implicit conversions, constants through conversions, sizeof, casts
 * (split from cexpr.c; see its header comment). */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- implicit conversions (gcc's convert_for_assignment) ----------------------------------- */


void sp(char *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, 640, fmt, ap);
    va_end(ap);
}

/* %qv: the qualifiers in q. */
/* a function type's const / volatile are the attributes const / noreturn */
const char *fqual_str(char *b, unsigned q)
{
    b[0] = 0;
    if (q & TQ_CONST)
        strcat(b, "__attribute__((const))");
    if (q & TQ_VOLATILE)
        strcat(b, b[0] ? " __attribute__((noreturn))" : "__attribute__((noreturn))");
    return b;
}

const char *qual_str(char *b, unsigned q)
{
    b[0] = 0;
    if (q & TQ_CONST)
        strcat(b, "const");
    if (q & TQ_VOLATILE)
        strcat(b, b[0] ? " volatile" : "volatile");
    if (q & TQ_RESTRICT)
        strcat(b, b[0] ? " restrict" : "restrict");
    if (q & TQ_ATOMIC)
        strcat(b, b[0] ? " _Atomic" : "_Atomic");
    return b;
}

/* gcc's strip_array_types: the element type of (nested) arrays. */
TypeId strip_arr(Checker *c, TypeId t)
{
    t = type_canon(TT, t);
    while (is_array(c, t))
        t = type_canon(TT, type_base(TT, t));
    return t;
}

/* gcc's TYPE_QUALS (arrays carry their element's qualifiers). */
unsigned gq(Checker *c, TypeId t)
{
    return TYPE_QUALS(strip_arr(c, t));
}

/* TYPE_MAIN_VARIANT, keeping _Atomic (and leaving the elements of arrays
 * unqualified). */
TypeId mvt(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    unsigned a = TYPE_QUALS(k) & TQ_ATOMIC;
    if (is_array(c, k)) {
        const TypeEnt *e = type_ent(TT, k);
        TypeId el = mvt(c, e->base);
        if (e->kind == TY_VLA)
            return type_vla_x(TT, el, e->n, e->flags & TF_SIZED, e->extra);
        if (e->flags & TF_INCOMPLETE)
            return type_array_incomplete(TT, el);
        return type_array(TT, el, e->n);
    }
    return TYPE_UNQUAL(k) | a;
}

/* c_common_signed_type / c_common_unsigned_type of a main variant. */
void sign_map(Checker *c, TypeId t, TypeId *uns, TypeId *sgn)
{
    TypeId m = TYPE_UNQUAL(type_canon(TT, t));
    unsigned bits;
    *uns = *sgn = m;
    switch (tkind(c, m)) {
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR:
        *uns = TYPE_B(UCHAR);
        *sgn = TYPE_B(SCHAR);
        break;
    case TY_SHORT: case TY_USHORT:
        *uns = TYPE_B(USHORT);
        *sgn = TYPE_B(SHORT);
        break;
    case TY_INT: case TY_UINT:
        *uns = TYPE_B(UINT);
        *sgn = TYPE_B(INT);
        break;
    case TY_LONG: case TY_ULONG:
        *uns = TYPE_B(ULONG);
        *sgn = TYPE_B(LONG);
        break;
    case TY_LLONG: case TY_ULLONG:
        *uns = TYPE_B(ULLONG);
        *sgn = TYPE_B(LLONG);
        break;
    case TY_INT128: case TY_UINT128:
        *uns = TYPE_B(UINT128);
        *sgn = TYPE_B(INT128);
        break;
    case TY_ENUM:
        bits = int_bits(c, m);
        if (bits == 8) {
            *uns = TYPE_B(UCHAR);
            *sgn = TYPE_B(SCHAR);
        } else if (bits == 16) {
            *uns = TYPE_B(USHORT);
            *sgn = TYPE_B(SHORT);
        } else if (bits == 32) {
            *uns = TYPE_B(UINT);
            *sgn = TYPE_B(INT);
        } else if (bits == 64) {
            *uns = TYPE_B(ULONG);
            *sgn = TYPE_B(LONG);
        }
        /* a type already of the wanted signedness maps to itself */
        if (is_signed(c, m))
            *sgn = m;
        else
            *uns = m;
        break;
    default:
        break;
    }
}

/* gcc's INTEGER_TYPE: an integer that is neither _Bool nor an enumeration. */
bool gcc_integer(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_CHAR && k <= TY_UINT128;
}

/* The parameter locations of the functions declared so far: gcc's
 * DECL_ARGUMENTS of the declaration it keeps (the definition's if seen, else
 * the first declaration's). */
typedef struct PLoc {
    uint32_t ref, n;
    bool def;
    SrcLoc *loc;
} PLoc;
typedef struct PLocVec {
    VEC(PLoc) v;
    uint32_t *idx;           /* symbol ref -> index + 1 into v */
    size_t idxcap;
} PLocVec;

void cexpr_free_params(Checker *c)
{
    PLocVec *v = c->plocs;
    size_t k;
    if (!v)
        return;
    for (k = 0; k < v->v.len; k++)
        free(v->v.data[k].loc);
    vec_free(&v->v);
    free(v->idx);
    free(v);
    c->plocs = NULL;
}

void cexpr_record_params(Checker *c, uint32_t declared, uint32_t ref, bool def)
{
    PLocVec *v = c->plocs;
    uint32_t fn = NO_NODE, kids[64], n, j, np = 0, m;
    PLoc *pl = NULL, nw;
    SrcLoc *locs;
    if (ref & SYM_LOCAL)
        return;
    if (!v) {
        v = xcalloc(1, sizeof *v);
        c->plocs = v;
    }
    if (ref < v->idxcap && v->idx[ref])
        pl = &v->v.data[v->idx[ref] - 1];
    if (pl && (pl->def || !def))
        return;
    /* the function declarator: the declared node's declarator sibling */
    for (j = cfirst(c, c->par[declared]); j < declared; j++)
        if (ntag(c, j) == N_FUNC)
            fn = j;
    if (fn == NO_NODE)
        return;
    n = nkids(c, fn, kids, 64);
    locs = xcalloc(n + 1, sizeof *locs);
    for (j = 0; j < n && j < 64; j++) {
        uint32_t q = kids[j];
        if (!is_real_param(c, q))
            continue;
        locs[np] = first_loc(c, q);
        for (m = cfirst(c, q); m <= q; m++)
            if (ntag(c, m) == N_NAME) {
                locs[np] = cnode_loc(c, m);
                break;
            }
        np++;
    }
    if (pl) {
        free(pl->loc);
        pl->loc = locs;
        pl->n = np;
        pl->def = def;
        return;
    }
    nw.ref = ref;
    nw.n = np;
    nw.def = def;
    nw.loc = locs;
    if (ref >= v->idxcap) {
        size_t nc = v->idxcap ? v->idxcap : 1024;
        while (nc <= ref)
            nc *= 2;
        v->idx = xrealloc(v->idx, nc * sizeof *v->idx);
        memset(v->idx + v->idxcap, 0, (nc - v->idxcap) * sizeof *v->idx);
        v->idxcap = nc;
    }
    vec_push(&v->v, nw);
    v->idx[ref] = (uint32_t)v->v.len;
}

SrcLoc param_loc(Checker *c, uint32_t ref, uint32_t idx, uint32_t at,
                        SrcLoc dflt)
{
    const PLocVec *v = c->plocs;
    (void)at;
    if (v && ref < v->idxcap && v->idx[ref]) {
        const PLoc *p = &v->v.data[v->idx[ref] - 1];
        return idx < p->n ? p->loc[idx] : dflt;
    }
    return dflt;
}

SrcLoc conv_note_loc(const Conv *x)
{
    if (x->ci->note_loc)
        return x->ci->note_loc;
    if (x->ci->fsym)
        return param_loc(x->c, x->ci->fsym - 1, (uint32_t)x->ci->parmnum - 1,
                         x->expr, x->eloc);
    return x->eloc;
}


/* expansion_point_location_if_in_system_header (PR c/67730): a system
 * header macro such as NULL still warns at its use. */
SrcLoc exp_if_system(Checker *c, SrcLoc l, uint32_t expr)
{
    SrcFile *sf = srcmgr_file_of(c->sm, l);
    if (sf && sf->system_header) {
        const PTok *t = &c->u->toks[first_tok(c, expr)];
        if (t->exp)
            return t->exp;
    }
    return l;
}

/* One diagnostic of a conversion, the text per context in m[] (indexed by
 * CONV_*); the notes gcc adds follow a reported one. */
void conv_diag(Conv *x, int rk, DiagOpt opt, char (*m)[640],
                      bool near)
{
    Checker *c = x->c;
    int ctx = x->ci->context;
    SrcLoc l = ctx == CONV_ARG ? x->eloc : x->loc;
    Diagnostic *d;
    l = exp_if_system(c, l, x->expr);
    if (rk == RK_PED)
        d = cpedwarn(c, l, opt, "%s", m[ctx]);
    else
        d = cwarn_d(c, DL_WARNING, l, opt, "%s", m[ctx]);
    if (!d)
        return;
    if (ctx == CONV_ARG)
        cnote(c, d, conv_note_loc(x), "expected %s but argument is of type %s",
              type_q(TT, x->type), type_q(TT, x->rhstype));
    else if (ctx == CONV_INIT && near && x->ci->near && *x->ci->near)
        cnote(c, d, l, "(near initialization for '%s')", x->ci->near);
}

/* comptypes_internal's enum_and_int_p: compatible types that differ in
 * being an enum on one side and an integer type on the other. */
bool cexpr_enum_int_mix(Checker *c, TypeId a, TypeId b, int depth)
{
    TypeKind ka, kb;
    a = type_canon(TT, a);
    b = type_canon(TT, b);
    ka = tkind(c, a);
    kb = tkind(c, b);
    if ((ka == TY_ENUM) != (kb == TY_ENUM))
        return type_is_integer(TT, a) && type_is_integer(TT, b);
    if (ka != kb || depth > 8)
        return false;
    if (ka == TY_PTR || ka == TY_ARRAY)
        return cexpr_enum_int_mix(c, type_base(TT, a), type_base(TT, b), depth + 1);
    if (ka == TY_FUNC) {
        uint32_t k, n = type_ent(TT, a)->n;
        if (n != type_ent(TT, b)->n)
            return false;
        if (cexpr_enum_int_mix(c, type_base(TT, a), type_base(TT, b), depth + 1))
            return true;
        for (k = 0; k < n; k++)
            if (cexpr_enum_int_mix(c, type_params(TT, a)[k], type_params(TT, b)[k],
                             depth + 1))
                return true;
    }
    return false;
}

/* comp_target_types of two pointer types (a pedantic note for arrays of
 * differently qualified elements). */
int comp_target(Conv *x, TypeId lt, TypeId rt)
{
    Checker *c = x->c;
    TypeId mvl = type_base(TT, type_canon(TT, lt));
    TypeId mvr = type_base(TT, type_canon(TT, rt));
    bool val_ped = true, val;
    if (is_array(c, mvl) && is_array(c, mvr))
        val_ped = type_compatible(TT, mvl, mvr);
    val = type_compatible(TT, mvt(c, mvl), mvt(c, mvr));
    if (val && cexpr_cxx_compat(c, x->expr) &&
        cexpr_enum_int_mix(c, mvl, mvr, 0))
        cwarn(c, x->loc, DW_CPP_COMPAT, "pointer target types incompatible in "
              "C++");
    if (val && !val_ped)
        cped2x(c, x->loc, "invalid use of pointers to arrays with "
                  "different qualifiers in ISO C before C2X");
    return val;
}

/* comp_target_types of two pointer targets (-, comparison, ?:): arrays of
 * differently qualified elements are compatible, with a pedantic note. */
bool targets_compat(Checker *c, SrcLoc loc, TypeId pa, TypeId pb)
{
    TypeId a = type_canon(TT, pa), b = type_canon(TT, pb);
    bool ped = !(is_array(c, a) && is_array(c, b)) || type_compatible(TT, a, b);
    bool val = type_compatible(TT, mvt(c, a), mvt(c, b));
    if (val && !ped)
        cped2x(c, loc, "invalid use of pointers to arrays with different "
                  "qualifiers in ISO C before C2X");
    return val;
}

/* The location of gcc's built-in declarations ("<built-in>"). */
static SrcLoc builtin_loc(Checker *c)
{
    uint32_t k, n = srcmgr_nfiles(c->sm);
    for (k = 0; k < n; k++) {
        SrcFile *f = srcmgr_file(c->sm, k);
        if (f && f->kind == SF_VIRTUAL && !strcmp(f->name, "<built-in>"))
            return f->base;
    }
    return 0;
}

/* maybe_warn_builtin_no_proto_arg: an argument of a call to a built-in
 * declared without a prototype that does not promote to the built-in's
 * parameter type. */
void builtin_noproto_arg(Conv *x)
{
    Checker *c = x->c;
    TypeId pt = x->type, at = x->rhstype, pr = TYPE_UNQUAL(at);
    TypeKind pk = tkind(c, pt), ak = tkind(c, at);
    int pc, ac;
    Diagnostic *d;
    if (is_int(c, at)) {
        /* gcc keeps the argument's own type node (typedef and all) when the
         * promotion changes nothing; an enum as wide as int stays itself */
        TypeId pm = type_int_promote(TT, at);
        if (ak == TY_ENUM && int_bits(c, at) < int_bits(c, TYPE_B(INT)))
            pr = TYPE_B(INT);
        else if (ak != TY_ENUM && mainv(c, pm) != mainv(c, at))
            pr = mainv(c, pm);
    } else if (is_flt(c, at) && ak != TY_DOUBLE && ak != TY_LDOUBLE &&
               float_prec(c, ak) <= 53 && ak >= TY_FLOAT16 && ak <= TY_FLOAT)
        pr = TYPE_B(DOUBLE);
    pc = gcc_integer(c, pt) ? 1 : pk == TY_ENUM ? 2 : pk == TY_BOOL ? 3
         : is_flt(c, pt) ? 4 : 5;
    ac = gcc_integer(c, at) ? 1 : ak == TY_ENUM ? 2 : ak == TY_BOOL ? 3
         : is_flt(c, at) ? 4 : 5;
    if (pc == 1 && ac == 2 && int_bits(c, pt) == int_bits(c, at))
        return;
    if ((pc == ac || (pc == 1 && ac == 2)) &&
        TYPE_UNQUAL(type_canon(TT, pt)) == TYPE_UNQUAL(type_canon(TT, pr)))
        return;
    d = cwarn_d(c, DL_WARNING, x->eloc, DW_BUILTIN_DECLARATION_MISMATCH,
                pr == mainv(c, at)
                ? "'%s' argument %d type is %s where %s is expected in a call "
                  "to built-in function declared without prototype"
                : "'%s' argument %d promotes to %s where %s is expected in a "
                  "call to built-in function declared without prototype",
                x->ci->fname, x->ci->parmnum, type_q(TT, pr), type_q(TT, pt));
    if (d)
        cnote(c, d, builtin_loc(c), "built-in '%s' declared here",
              x->ci->fname);
}

/* -Woverflow for a constant stored into a bit-field of width w: gcc converts
 * to the bit-field's own (narrower) type, which prints as 'unsigned char:3'.
 * True when it warned. */
bool cexpr_bf_overflow(Checker *c, SrcLoc loc, uint32_t n, TypeId ft,
                       TypeId rt, unsigned w)
{
    unsigned su_bits;
    bool tu, su;
    int64_t sv;
    uint64_t uv, mask, r;
    const char *base;
    char tname[64], vs[32], rs[32];
    if (!type_is_integer(TT, rt) || type_int_bits(TT, rt) > 64)
        return false;
    su_bits = type_int_bits(TT, ft);
    if (w >= su_bits)
        return false;
    tu = !type_is_signed(TT, ft);
    su = !type_is_signed(TT, rt);
    sv = cexpr_sval(c, n);
    uv = (uint64_t)sv;
    mask = ((uint64_t)1 << w) - 1;
    {
        bool fits;
        if (su)
            fits = tu ? uv <= mask : uv <= (mask >> 1);
        else if (tu)
            fits = sv >= 0 && uv <= mask;
        else
            fits = sv >= -(int64_t)(mask >> 1) - 1 && sv <= (int64_t)(mask >> 1);
        if (fits)
            return false;
    }
    r = uv & mask;
    if (!tu && (r >> (w - 1)))
        r |= ~mask;
    base = w <= 8 ? (tu ? "unsigned char" : "signed char")
         : w <= 16 ? (tu ? "short unsigned int" : "short int")
         : w <= 32 ? (tu ? "unsigned int" : "int")
                   : (tu ? "long unsigned int" : "long int");
    snprintf(tname, sizeof tname, "'%s:%u'", base, w);
    if (su)
        snprintf(vs, sizeof vs, "%" PRIu64, uv);
    else
        snprintf(vs, sizeof vs, "%" PRId64, sv);
    if (tu)
        snprintf(rs, sizeof rs, "%" PRIu64, r);
    else
        snprintf(rs, sizeof rs, "%" PRId64, (int64_t)r);
    if (su) {
        cwarn(c, loc, DW_OVERFLOW, "conversion from %s to %s "
              "changes value from '%s' to '%s'", type_q(TT, rt), tname, vs, rs);
        return true;
    } else if (tu) {
        bool sfit = sv >= -(int64_t)(mask >> 1) - 1 && sv <= (int64_t)(mask >> 1);
        if (!sfit) {
            cwarn(c, loc, DW_OVERFLOW, "unsigned conversion from "
                  "%s to %s changes value from '%s' to '%s'", type_q(TT, rt),
                  tname, vs, rs);
            return true;
        }
    } else {
        bool ufit = sv >= 0 && uv <= mask;
        if (!ufit || c->opt.pedantic) {
            cwarn(c, loc, DW_OVERFLOW, "overflow in conversion "
                  "from %s to %s changes value from '%s' to '%s'",
                  type_q(TT, rt), tname, vs, rs);
            return true;
        }
    }
    return false;
}

/* ---- constants through conversions ---------------------------------------------- */

bool is_decimal_flt(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_DEC32 && k <= TY_DEC128;
}

/* A floating value converted to integer type t (saturating). */
bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out)
{
    return float_to_int_bits(c, f, t, int_bits(c, t), out);
}

/* bits: the width to saturate to (a bit-field's, or t's own) */
bool float_to_int_bits(Checker *c, long double f, TypeId t, unsigned bits,
                       uint64_t *out)
{
    if (bits == 0 || bits > 64)
        return false;
    if (f != f) {
        *out = 0;
        return true;
    }
    if (tkind(c, t) == TY_BOOL) {
        *out = f != 0;
        return true;
    }
    if (is_signed(c, t)) {
        int64_t max = bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1;
        int64_t min = -max - 1;
        if (f >= (long double)max)
            *out = (uint64_t)max;
        else if (f <= (long double)min)
            *out = (uint64_t)min;
        else
            *out = (uint64_t)(int64_t)f;
    } else {
        uint64_t max = bits == 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
        if (f <= 0)
            *out = 0;
        else if (f >= (long double)max)
            *out = max;
        else
            *out = (uint64_t)f;
    }
    return true;
}

/* Node i (whose type is already set to `to`) gets the value of node a
 * converted to `to`, as a cast or the usual conversions would. */
void conv_const(Checker *c, uint32_t i, uint32_t a, TypeId to)
{
    TypeId from = rvt(c, a);
    long double f;
    c->ck[i] = K_NONE;
    c->cv[i] = 0;
    c->cb[i] = 0;
    if (is_complex(c, to)) {
        long double re, im;
        if (cplx_conv(c, a, to, &re, &im))
            cplx_set(c, i, re, im);
        return;
    }
    if (is_complex(c, from) && (is_int(c, to) || is_flt(c, to))) {
        long double re, im, o;
        TypeId ct;
        if (!cplx_get(c, a, &re, &im, &ct) || !part_conv(c, ct, to, re, &o))
            return;
        if (is_flt(c, to)) {
            c->ck[i] = K_FLOAT;
            c->cv[i] = fpush(c, o);
        } else {
            /* an imaginary literal cast directly to an integer is an ICE */
            bool lit = ntag(c, strip_paren(c, a)) == N_NUMBER;
            c->ck[i] = lit ? K_ICE : K_FOLD;
            c->cv[i] = cexpr_trunc(c, to, cplx_u(o));
            c->ef[i] |= lit ? EF_INTOPS : EF_NOPCST;
        }
        return;
    }
    if (is_int(c, to)) {
        if (has_ival(c, a) && int_bits(c, to) <= 64 && int_bits(c, from) <= 64) {
            uint64_t v = tkind(c, to) == TY_BOOL ? c->cv[a] != 0
                                                 : cexpr_trunc(c, to, c->cv[a]);
            c->cv[i] = v;
            if (c->ck[a] == K_ICE) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS | (c->ef[a] & EF_OVERFLOW);
            } else {
                c->ck[i] = K_FOLD;
                if (c->ef[a] & (EF_CST | EF_NOPCST | EF_REALCST))
                    c->ef[i] |= EF_NOPCST;
                else if (c->ef[a] & EF_INTOPS)
                    c->ef[i] |= EF_INTOPS;
                c->ef[i] |= c->ef[a] & EF_OVERFLOW;
            }
        } else if (c->ck[a] == K_FLOAT &&
                   float_to_int(c, c->fv.data[c->cv[a]], to, &c->cv[i])) {
            uint64_t ov;
            if (c->ef[a] & EF_REALCST) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS;
            } else {
                c->ck[i] = K_FOLD;
                c->ef[i] |= EF_NOPCST;
            }
            /* fold_convert_const_int_from_real: TREE_OVERFLOW */
            if (tkind(c, to) != TY_BOOL &&
                (float_ovf(c, c->fv.data[c->cv[a]], to, &ov) ||
                 c->fv.data[c->cv[a]] != c->fv.data[c->cv[a]]))
                c->ef[i] |= EF_OVERFLOW;
        } else if (c->ck[a] == K_ADDR &&
                   (is_ptr(c, from) || tkind(c, from) == TY_NULLPTR)) {
            if (c->cb[a] == 0) {
                c->ck[i] = K_FOLD;
                c->cv[i] = tkind(c, to) == TY_BOOL ? c->cv[a] != 0
                                                   : cexpr_trunc(c, to, c->cv[a]);
                c->ef[i] |= EF_NOPCST;
            } else if (int_bits(c, from) == int_bits(c, to) &&
                       tkind(c, to) != TY_BOOL) {
                c->ck[i] = K_ADDR;
                c->cb[i] = c->cb[a];
                c->cv[i] = c->cv[a];
            }
        }
        return;
    }
    if (is_flt(c, to)) {
        if (!is_decimal_flt(c, to) && !is_decimal_flt(c, from) &&
            fval(c, a, &f)) {
            c->ck[i] = K_FLOAT;
            c->cv[i] = fpush(c, fround(c, to, f));
        }
        return;
    }
    if (tkind(c, to) == TY_NULLPTR) {
        if (c->ck[a] == K_ADDR || has_ival(c, a))
            c->ck[i] = K_ADDR;      /* the null pointer */
        return;
    }
    if (is_ptr(c, to)) {
        if (has_ival(c, a) && int_bits(c, from) <= 64) {
            c->ck[i] = K_ADDR;
            c->cv[i] = cexpr_trunc(c, to, c->cv[a]);
        } else if (c->ck[a] == K_ADDR &&
                   (is_ptr(c, from) || tkind(c, from) == TY_NULLPTR ||
                    (is_int(c, from) && int_bits(c, from) == int_bits(c, to)))) {
            c->ck[i] = K_ADDR;
            c->cb[i] = c->cb[a];
            c->cv[i] = c->cv[a];
        }
    }
}

/* ---- sizeof, _Alignof ------------------------------------------------------------ */

/* Is t variably modified in a way that makes its size a run-time value? */
bool var_size(Checker *c, TypeId t)
{
    for (;;) {
        TypeId ct = type_canon(TT, t);
        TypeKind k = tkind(c, ct);
        if (k == TY_VLA)
            return true;
        if (k == TY_ARRAY) {
            t = type_base(TT, ct);
            continue;
        }
        if (k == TY_STRUCT || k == TY_UNION)
            return (type_record(TT, ct)->flags & RF_VLA) != 0;
        return false;
    }
}

void set_ice(Checker *c, uint32_t i, TypeId t, uint64_t v)
{
    c->ty[i] = t;
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, t, v);
    c->ef[i] = EF_INTOPS;
}


/* Whether x (parentheses ignored) names an object. */
static bool obj_ident(Checker *c, uint32_t x)
{
    uint32_t s = strip_paren(c, x), ref;
    if (s == NO_NODE || ntag(c, s) != N_IDENT)
        return false;
    ref = lookup_ord(c, cnode_ident(c, s));
    return ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ;
}

/* The outermost sizeof around node i, or NO_NODE. */
uint32_t sizeof_outer(const Checker *c, uint32_t i)
{
    uint32_t r = NO_NODE, p;
    unsigned depth;
    for (depth = 0; depth < 256 && (p = c->par[i]) != NO_NODE; depth++) {
        if (ntag(c, p) == N_SIZEOF_EXPR || ntag(c, p) == N_SIZEOF_TYPE ||
            ntag(c, p) == N_ALIGNOF_EXPR || ntag(c, p) == N_ALIGNOF_TYPE ||
            ntag(c, p) == N_TYPEOF)
            r = p;
        i = p;
    }
    return r;
}

/* A sizeof of a variable length type is evaluated: the functions named in it
 * are used (pop_maybe_used). */
static void sizeof_marks_used(Checker *c, uint32_t sz)
{
    uint32_t k;
    for (k = cfirst(c, sz); k < sz; k++)
        if (ntag(c, k) == N_IDENT && c->cb[k]) {
            CSym *s = csym(c, c->cb[k] - 1);
            if (s->kind == CS_FUNC)
                s->flags |= CSF_CUSED;
        }
}

/* typeof of a variably modified type is evaluated like sizeof of a VLA. */
void cexpr_typeof_used(Checker *c, uint32_t n, TypeId t)
{
    if (type_is_vm(TT, t) && sizeof_outer(c, n) == NO_NODE)
        sizeof_marks_used(c, n);
}

void e_sizeof(Checker *c, uint32_t i, bool align)
{
    uint32_t a = first_child(c, i);
    unsigned tag = ntag(c, i);
    bool is_type = tag == N_SIZEOF_TYPE || tag == N_ALIGNOF_TYPE;
    const char *op = align ? "__alignof__" : "sizeof";
    SrcLoc loc;
    TypeId t;
    uint64_t v = 1;
    bool ok;
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    if (align && !cexpr_in_extension(c, i)) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok, &len);
        if (len == 8 && !memcmp(s, "_Alignof", 8)) {
            size_t n0 = c->diag->all.len;
            cped11(c, cnode_loc(c, i), "ISO C99 does not support '_Alignof'");
            choist(c, i, n0);
            if (!is_type)
                cpedantic(c, cnode_loc(c, i),
                          "ISO C does not allow '_Alignof (expression)'");
        }
    }
    if (is_type) {
        t = type_of_typename(c, a);
        loc = first_loc(c, a);
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
    } else {
        if (node_err(c, a)) {
            /* c_alignof_expr of an erroneous operand is a constant 1 */
            if (align)
                set_ice(c, i, size_type(c), 1);
            else
                set_err(c, i);
            return;
        }
        t = c->ty[a];
        loc = align ? cnode_loc(c, i) : first_loc(c, a);
        if (c->ef[a] & EF_BITFIELD) {
            if (align) {
                cerror(c, loc, "'__alignof' applied to a bit-field");
                set_ice(c, i, size_type(c), 1);
                return;
            }
            cerror(c, loc, "'sizeof' applied to a bit-field");
        }
    }
    if (!is_type && !align) {
        uint32_t s = strip_paren(c, a);
        /* *&x folds to x (PR c/82167) */
        for (;;) {
            uint32_t k1[3], k2[3];
            uint32_t in;
            if (s == NO_NODE || ntag(c, s) != N_UNARY ||
                npunct(c, s) != P_STAR || nkids(c, s, k1, 3) != 1)
                break;
            in = strip_paren(c, k1[0]);
            if (in == NO_NODE || ntag(c, in) != N_UNARY ||
                npunct(c, in) != P_AMP || nkids(c, in, k2, 3) != 1)
                break;
            s = strip_paren(c, k2[0]);
        }
        if (s != NO_NODE && ntag(c, s) == N_IDENT) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, s));
            if (ref != SYM_NONE && (csym(c, ref)->flags & CSF_ARRAY_PARM)) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc,
                                        DW_SIZEOF_ARRAY_ARGUMENT, "'sizeof' on "
                                        "array function parameter '%s' will "
                                        "return size of %s",
                                        cident(c, cnode_ident(c, s)),
                                        type_q(TT, t));
                cnote(c, d, csym(c, ref)->loc, "declared here");
            }
        }
    }
    uint32_t fref = SYM_NONE;
    if (is_func(c, t) && align && !is_type) {
        uint32_t fs = strip_paren(c, a);
        if (fs != NO_NODE && ntag(c, fs) == N_IDENT)
            fref = lookup_ord(c, cnode_ident(c, fs));
        if (fref != SYM_NONE && csym(c, fref)->kind != CS_FUNC)
            fref = SYM_NONE;
    }
    if (fref != SYM_NONE) {
        /* c_alignof_expr: a function's DECL_ALIGN_UNIT (1 unless aligned) */
        v = csym(c, fref)->ualign ? csym(c, fref)->ualign : 1;
    } else if (is_func(c, t)) {
        if (!align)
            ped_arith(c, i, loc, "invalid application of 'sizeof' to a function "
                                 "type");
        else
            ped(c, i, loc, "ISO C does not permit '_Alignof' applied to a "
                           "function type");
        v = align ? 8 : 1;
    } else if (is_void(c, t)) {
        char buf[96];
        snprintf(buf, sizeof buf, "invalid application of '%s' to a void type",
                 op);
        ped_arith(c, i, loc, buf);
    } else if (align && !is_type && !complete(c, t) && obj_ident(c, a)) {
        /* c_alignof_expr takes a variable's DECL_ALIGN: an incomplete object
         * is accepted */
        v = tkind(c, type_canon(TT, t)) == TY_ARRAY ? type_align(TT, t) : 1;
    } else if (!complete(c, t)) {
        cerror(c, loc, "invalid application of '%s' to incomplete type %s", op,
               type_q(TT, t));
        set_err(c, i);
        return;
    } else if (!align) {
        if (var_size(c, t)) {
            if (sizeof_outer(c, i) == NO_NODE)
                sizeof_marks_used(c, i);
            c->ty[i] = size_type(c);
            return;
        }
        v = type_size(TT, t, &ok);
        if (!ok)
            v = 0;
    } else {
        v = type_align(TT, t);
        if (!is_type) {
            uint32_t s = strip_paren(c, a);
            if (s != NO_NODE && ntag(c, s) == N_IDENT) {
                uint32_t ref = lookup_ord(c, cnode_ident(c, s));
                /* aligned (n) sets a variable's alignment, even below the type's */
                if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ &&
                    csym(c, ref)->align)
                    v = csym(c, ref)->align;
            } else if (s != NO_NODE && ntag(c, s) == N_MEMBER_EXPR) {
                TypeId rec = 0;
                const Field *f = member_field_of(c, s, &rec);
                if (f) {
                    bool pk = (f->flags & FF_PACKED) ||
                              (type_record(TT, rec)->flags & RF_PACKED);
                    if (pk)
                        v = f->align ? f->align : 1;
                    else if (f->align > v)
                        v = f->align;
                }
            }
        }
    }
    set_ice(c, i, size_type(c), v);
}

/* ---- casts ----------------------------------------------------------------------- */


/* gcc's TREE_CODE class of a type for -Wbad-function-cast: all integer
 * types but _Bool and enums share INTEGER_TYPE. */
static int cast_class(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    if (k == TY_BOOL || k == TY_ENUM)
        return (int)k;
    if (is_int(c, t))
        return -1;
    if (is_flt(c, t))
        return -2;
    if (is_complex(c, t))
        return -3;
    return (int)k;
}

/* gcc's handle_warn_cast_qual: a pointer cast that drops a qualifier of a
 * target type, or that is unsafe through an unqualified intermediate level. */
/* gcc's build_c_cast -Wcast-align=strict: the target is more aligned than
 * the source (-Wcast-align alone is a no-op on targets that allow unaligned
 * access). */
static void cast_align(Checker *c, SrcLoc loc, TypeId pt, TypeId po)
{
    bool ok;
    if (is_void(c, po) || is_func(c, po) || is_void(c, pt) || is_func(c, pt) ||
        !diag_enabled(c->diag, DW_CAST_ALIGN_V) ||
        diag_option_level(c->diag, DW_CAST_ALIGN_V, 1) < 2)
        return;
    type_size(TT, po, &ok);
    if (!ok)
        return;                 /* an opaque type's alignment is unknown */
    type_size(TT, pt, &ok);
    if (ok && type_align(TT, pt) > type_align(TT, po))
        cwarn(c, loc, DW_CAST_ALIGN_V, "cast increases required alignment of "
              "target type");
}

/* gcc's c_safe_arg_type_equiv_p: two parameter or return types a call
 * cannot tell apart (a void stands for the end of a prototype). */
static bool safe_arg_equiv(Checker *c, TypeId a, TypeId b)
{
    if (!a || !b)
        return !a && !b;
    a = TYPE_UNQUAL(type_canon(TT, a));
    b = TYPE_UNQUAL(type_canon(TT, b));
    if (is_ptr(c, a) && is_ptr(c, b))
        return true;
    if (is_int(c, a) && is_int(c, b) &&
        (tkind(c, a) == TY_BOOL) == (tkind(c, b) == TY_BOOL) &&
        tkind(c, a) != TY_BOOL && int_bits(c, a) == int_bits(c, b) &&
        (is_signed(c, a) == is_signed(c, b) ||
         int_bits(c, a) >= 32))
        return true;
    return type_compatible(TT, a, b);
}

/* gcc's c_safe_function_type_cast_p; fa, fb: the function types. */
static bool safe_function_cast(Checker *c, TypeId fa, TypeId fb)
{
    const TypeEnt *ea = type_ent(TT, fa), *eb = type_ent(TT, fb);
    bool pa = !(ea->flags & TF_NOPROTO), pb = !(eb->flags & TF_NOPROTO);
    uint32_t na = pa ? ea->n + !(ea->flags & TF_VARIADIC) : 0,
             nb = pb ? eb->n + !(eb->flags & TF_VARIADIC) : 0, j;
    const TypeId *qa = type_params(TT, fa), *qb = type_params(TT, fb);
    TypeId ra = type_base(TT, fa), rb = type_base(TT, fb);
    if ((is_void(c, ra) && pa && !ea->n && !(ea->flags & TF_VARIADIC)) ||
        (is_void(c, rb) && pb && !eb->n && !(eb->flags & TF_VARIADIC)))
        return true;
    if (!safe_arg_equiv(c, ra, rb))
        return false;
    for (j = 0; j < na && j < nb; j++)
        if (!safe_arg_equiv(c, j < ea->n ? qa[j] : 0, j < eb->n ? qb[j] : 0))
            return false;
    return true;
}

static void cast_function_type(Checker *c, SrcLoc loc, TypeId t, TypeId ot)
{
    TypeId pt = type_canon(TT, pointee(c, t)), po = type_canon(TT, pointee(c, ot));
    if (!is_func(c, pt) || !is_func(c, po) ||
        !diag_enabled(c->diag, DW_CAST_FUNCTION_TYPE) ||
        safe_function_cast(c, pt, po))
        return;
    cwarn(c, loc, DW_CAST_FUNCTION_TYPE, "cast between incompatible function "
          "types from %s to %s", type_q(TT, ot),
          type_q(TT, type_canon(TT, t)));
}

static void cast_qual(Checker *c, SrcLoc loc, TypeId t, TypeId ot)
{
    TypeId it = t, io = ot;
    unsigned discarded = 0, added = 0, qnoat = TQ_CONST | TQ_VOLATILE | TQ_RESTRICT | TQ_ATOMIC;
    bool is_const;
    char b[48];
    if (!diag_enabled(c->diag, DW_CAST_QUAL))
        return;
    do {
        it = type_canon(TT, type_base(TT, type_canon(TT, it)));
        io = type_canon(TT, type_base(TT, type_canon(TT, io)));
        if (is_func(c, it) && is_func(c, io))
            added |= tquals(c, it) & ~tquals(c, io) & qnoat;
        else
            discarded |= tquals(c, strip_arr(c, io)) & ~tquals(c, strip_arr(c, it)) & qnoat;
    } while (tkind(c, it) == TY_PTR && tkind(c, io) == TY_PTR);
    if (added)
        cwarn(c, loc, DW_CAST_QUAL, "cast adds '%s%s%s' qualifier to function "
              "type", added & TQ_CONST ? "__attribute__((const))" : "",
              (added & TQ_CONST) && (added & TQ_VOLATILE) ? " " : "",
              added & TQ_VOLATILE ? "__attribute__((noreturn))" : "");
    if (discarded)
        cwarn(c, loc, DW_CAST_QUAL, "cast discards '%s' qualifier from pointer "
              "target type", qual_str(b, discarded));
    if (added || discarded)
        return;
    it = t;
    io = ot;
    /* only when the types are otherwise the same */
    for (;;) {
        it = type_canon(TT, type_base(TT, type_canon(TT, it)));
        io = type_canon(TT, type_base(TT, type_canon(TT, io)));
        if (tkind(c, it) == TY_PTR && tkind(c, io) == TY_PTR)
            continue;
        if (tkind(c, it) == TY_PTR || tkind(c, io) == TY_PTR ||
            !type_compatible(TT, mainv(c, it), mainv(c, io)))
            return;
        break;
    }
    it = t;
    io = ot;
    is_const = (tquals(c, pointee(c, it)) & TQ_CONST) != 0;
    do {
        it = type_canon(TT, type_base(TT, type_canon(TT, it)));
        io = type_canon(TT, type_base(TT, type_canon(TT, io)));
        if (!is_func(c, it) && (tquals(c, it) & ~tquals(c, io) & qnoat) &&
            !is_const) {
            cwarn(c, loc, DW_CAST_QUAL, "to be safe all intermediate pointers "
                  "in cast from %s to %s must be 'const' qualified",
                  type_q(TT, ot), type_q(TT, t));
            break;
        }
        if (is_const)
            is_const = (tquals(c, it) & TQ_CONST) != 0;
    } while (tkind(c, it) == TY_PTR && tkind(c, io) == TY_PTR);
}

/* gcc points at the tag of a struct/union/enum type name */
static SrcLoc cast_tag_loc(Checker *c, uint32_t i, uint32_t tn)
{
    SrcLoc el = after_loc(c, i);
    uint32_t m;
    for (m = cfirst(c, tn); m < tn; m++)
        if (ntag(c, m) == N_TAG && c->par[m] != NO_NODE &&
            (ntag(c, c->par[m]) == N_STRUCT || ntag(c, c->par[m]) == N_ENUM) &&
            c->par[c->par[m]] != NO_NODE &&
            ntag(c, c->par[c->par[m]]) == N_SPECS &&
            c->par[c->par[c->par[m]]] == tn)
            el = cnode_loc(c, m);
    return el;
}

void e_cast(Checker *c, uint32_t i)
{
    uint32_t k[2], a;
    SrcLoc loc = cnode_loc(c, i);
    TypeId t, ot;
    TypeKind tk;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    a = k[1];
    t = type_of_typename(c, k[0]);
    if (is_err(c, t) || node_err(c, a)) {
        set_err(c, i);
        return;
    }
    tk = tkind(c, t);
    if (tk == TY_ARRAY || tk == TY_VLA) {
        cerror(c, loc, "cast specifies array type");
        set_err(c, i);
        return;
    }
    if (tk == TY_FUNC) {
        cerror(c, loc, "cast specifies function type");
        set_err(c, i);
        return;
    }
    if (tk == TY_ENUM && !complete(c, t)) {
        cerror(c, cast_tag_loc(c, i, k[0]), "conversion to incomplete type");
        set_err(c, i);
        return;
    }
    if (tk == TY_VOID) {
        if (!rvalue_ok(c, a)) {
            set_err(c, i);
            return;
        }
        c->ty[i] = unqual(c, t);
        c->ef[i] = c->ef[a] & EF_PROP;
        return;
    }
    ot = rvt(c, a);
    if (is_void(c, ot)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (!rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = unqual(c, t);
    c->ef[i] = c->ef[a] & EF_PROP;
    if (tk == TY_VECTOR) {
        /* an integer or a vector of the vector's size converts */
        bool v = tkind(c, ot) == TY_VECTOR, ok = true;
        if (!v && !(is_int(c, ot) && tkind(c, ot) != TY_BOOL))
            cerror(c, after_loc(c, i), "cannot convert value to a vector");
        else if (type_size(TT, type_canon(TT, t), &ok) !=
                 type_size(TT, type_canon(TT, ot), &ok))
            cerror(c, after_loc(c, i), "cannot convert a value of type %s to "
                   "vector type %s which has different size",
                   type_q(TT, ot), type_q(TT, type_canon(TT, t)));
        else
            return;
        set_err(c, i);
        return;
    }
    if (tkind(c, ot) == TY_VECTOR && tk != TY_UNION && !is_record(c, t)) {
        bool ok = true;
        if (is_int(c, t) && tk != TY_BOOL) {
            if (type_size(TT, type_canon(TT, t), &ok) ==
                type_size(TT, type_canon(TT, ot), &ok))
                return;
            cerror(c, after_loc(c, i), "cannot convert a vector of type %s to "
                   "type %s which has different size", type_q(TT, ot),
                   type_q(TT, t));
        } else if (tk == TY_BOOL)
            cerror(c, after_loc(c, i), "used vector type where scalar is "
                   "required");
        else if (is_flt(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where a "
                   "floating-point was expected");
        else if (is_ptr(c, t))
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        else
            cerror(c, after_loc(c, i), "aggregate value used where a complex "
                   "was expected");
        set_err(c, i);
        return;
    }
    if (is_record(c, t) || tk == TY_UNION) {
        if (mainv(c, t) == mainv(c, ot)) {
            ped(c, i, loc, "ISO C forbids casting nonscalar to the same type");
            return;
        }
        if (tk == TY_UNION) {
            const Record *r = type_record(TT, type_canon(TT, t));
            uint32_t f;
            for (f = 0; f < r->nfields; f++) {
                const Field *fl = &c->tt.fields.data[r->fields + f];
                if (type_compatible(TT, mainv(c, fl->ty), mainv(c, ot)))
                    break;
            }
            if (f == r->nfields) {
                cerror(c, loc, "cast to union type from type not present in "
                               "union");
                set_err(c, i);
                return;
            }
            ped(c, i, loc, "ISO C forbids casts to union type");
            return;
        }
        {
            cerror(c, cast_tag_loc(c, i, k[0]),
                   "conversion to non-scalar type requested");
        }
        set_err(c, i);
        return;
    }
    if (is_record(c, ot)) {
        if (tk == TY_BOOL) {
            cerror(c, after_loc(c, i), "used %s type value where scalar is "
                   "required", tkind(c, ot) == TY_UNION ? "union" : "struct");
            set_err(c, i);
            return;
        }
        if (is_ptr(c, t)) {
            /* the cast keeps its type after the error */
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
            c->ty[i] = type_canon(TT, c->ty[i]);
            return;
        }
        if (is_int(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where an integer was "
                           "expected");
        else if (is_flt(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where a floating-point was "
                           "expected");
        else if (is_ptr(c, t))
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        else
            cerror(c, after_loc(c, i), "aggregate value used where a complex was "
                           "expected");
        set_err(c, i);
        return;
    }
    if (tkind(c, ot) == TY_NULLPTR && tk != TY_NULLPTR && !is_ptr(c, t) &&
        tk != TY_BOOL) {
        cerror(c, after_loc(c, i), "aggregate value used where %s was "
               "expected", is_int(c, t) ? "an integer"
               : is_flt(c, t) ? "a floating-point" : "a complex");
        set_err(c, i);
        return;
    }
    if (tk == TY_NULLPTR && tkind(c, ot) != TY_NULLPTR &&
        !(c->ef[a] & EF_NPC)) {
        SrcLoc el = after_loc(c, i);
        Diagnostic *d = cerror_d(c, el, "conversion from %s to %s",
                                 type_q(TT, ot), type_q(TT, type_canon(TT, t)));
        cnote(c, d, el, "only %s or a null pointer constant can be converted "
              "to %s", type_q(TT, type_canon(TT, t)),
              type_q(TT, type_canon(TT, t)));
        set_err(c, i);
        return;
    }
    if (is_ptr(c, ot) && (is_flt(c, t) || is_complex(c, t))) {
        cerror(c, after_loc(c, i), is_flt(c, t) ? "pointer value used where a "
               "floating-point was expected" : "pointer value used where a "
               "complex was expected");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, t) && !is_ptr(c, ot) && !is_int(c, ot) &&
        tkind(c, ot) != TY_NULLPTR) {
        cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        c->ty[i] = type_canon(TT, c->ty[i]);
        return;
    }
    if (is_ptr(c, t) && is_ptr(c, ot)) {
        TypeId pt = pointee(c, t), po = pointee(c, ot);
        cast_qual(c, loc, t, ot);
        cast_align(c, loc, pt, po);
        cast_function_type(c, loc, t, ot);
        if (is_func(c, pt) && !is_func(c, po) && !(c->ef[a] & EF_NPC))
            ped(c, i, loc, "ISO C forbids conversion of object pointer to "
                           "function pointer type");
        else if (!is_func(c, pt) && is_func(c, po))
            ped(c, i, loc, "ISO C forbids conversion of function pointer to "
                           "object pointer type");
    } else if (is_ptr(c, ot) && is_int(c, t) &&
               tk != TY_BOOL && tk != TY_ENUM &&
               int_bits(c, t) != int_bits(c, ot)) {
        cwarn(c, loc, DW_POINTER_TO_INT_CAST,
              "cast from pointer to integer of different size");
    }
    packed_ptr_check_x(c, t, a, noop_ptr_cast(c, t, a) ? 1 : 3, 0, false);
    if (is_ptr(c, t))
        alias_cast(c, a, c->ty[i]);
    if (tk == TY_BOOL && (is_ptr(c, ot) || is_int(c, ot) || is_flt(c, ot) || is_complex(c, ot)))
        cexpr_truth_warn(c, a, cinput_loc(c, c->nodes[i].tok));
    /* -Wbad-function-cast: a call cast to a type of another tree code */
    if (ntag(c, strip_paren(c, a)) == N_CALL && diag_enabled(c->diag, DW_BAD_FUNCTION_CAST) &&
        cast_class(c, t) != cast_class(c, ot))
        cwarn(c, loc, DW_BAD_FUNCTION_CAST, "cast from function call of type "
              "%s to non-matching type %s", type_q(TT, ot), type_q(TT, t));
    if (!(is_ptr(c, t) && is_ptr(c, ot)) &&
        is_ptr(c, t) && is_int(c, ot) && tkind(c, ot) != TY_BOOL &&
        tkind(c, ot) != TY_ENUM && int_bits(c, t) != int_bits(c, ot) &&
        c->ck[a] != K_ICE && c->ck[a] != K_FOLD) {
        cwarn(c, loc, DW_INT_TO_POINTER_CAST,
              "cast to pointer from integer of different size");
    }
    conv_const(c, i, a, c->ty[i]);
    if (is_ptr(c, t) && c->ck[i] == K_ADDR && c->cb[i] == 0 &&
        c->cv[i] == 0 && is_intcst(c, a) && !constvar_in(c, a) &&
        !(c->ef[a] & EF_OVERFLOW) &&
        !(ntag(c, strip_paren(c, a)) == N_BINARY &&
          npunct(c, strip_paren(c, a)) == P_COMMA) &&
        is_void(c, pointee(c, t)) && tquals(c, pointee(c, t)) == 0)
        c->ef[i] |= EF_NPC;
}

