/* cspec.c - the checker's declaration specifiers: storage classes,
 * qualifiers, type keywords and the combination rules (gcc's c_declspecs;
 * split from cdecl.c).  The shared readers are cdecl_int.h's. */
#include "c/cdecl_int.h"

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
    if (dupe && prev && !tfrom_macro(c, prev - 1) && !tfrom_macro(c, tok)) {
        const char *id = cc90_id(c, NULL);
        cwarn(c, loc, id ? id : "duplicate-decl-specifier",
              "duplicate '%s' declaration specifier", sp);
    }
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
            if (cc90_id(c, "long-long"))
                cwarn(c, loc, "long-long", "ISO C90 does not support 'long "
                      "long'");
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
            cc90(c, loc, NULL, "ISO C90 does not support complex types");
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
            if (kw == CK_BOOL)
                cc90(c, loc, NULL, "ISO C90 does not support boolean types");
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
            s->sat = true;
            s->sat_loc = loc;
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
            cped2x(c, loc, "ISO C does not support decimal floating-point "
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
        } else if (s->sat) {
            cerror(c, s->sat_loc, "'_Sat' is used without '_Fract' or "
                   "'_Accum'");
            cerror(c, s->sat_loc, "fixed-point types not supported for this "
                   "target");
            s->word = TW_INT;
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

void specs_visit(Checker *c, uint32_t i)
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
            cped2x(c, s.loc, "a label can only be part of a statement "
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
            if (ref != SYM_NONE && c->recs.len)
                cxx_typedef_in_struct(c, cnode_ident(c, n), nd->tok);
            if (ref != SYM_NONE) {
                csym(c, ref)->flags |= CSF_USED;
                if (csym(c, ref)->kind == CS_TYPEDEF &&
                    (csym(c, ref)->flags & CSF_ATTR_UNUSED))
                    s.attrs.unused = true;     /* TREE_USED of the type */
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
            if (!(nd->flags & NF_BODY) && find_child(c, n, N_TAG) != NO_NODE) {
                /* packed after a reference to a tag is dropped, at the tag */
                Attrs ta;
                memset(&ta, 0, sizeof ta);
                c->attr_quiet = true;
                attrs_of_children(c, n, &ta);
                c->attr_quiet = false;
                if (ta.packed)
                    cwarn(c, iloc(c, cnode(c, find_child(c, n, N_TAG))->tok),
                          "attributes", "'packed' attribute ignored");
            }
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
                cexpr_typeof_used(c, n, t);
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
                    /* c_sizeof_or_alignof_type (_Alignof) of the type */
                    TypeId at = c->ty[a];
                    TypeKind ak = type_ckind(TT, at);
                    bool ext = cexpr_in_extension(c, n);
                    if (ak == TY_FUNC) {
                        if (!ext)
                            cpedantic(c, loc, "ISO C does not permit "
                                      "'_Alignof' applied to a function type");
                    } else if (ak == TY_VOID) {
                        if (!ext)
                            cpedwarn(c, loc, "pointer-arith", "invalid "
                                     "application of '__alignof__' to a void "
                                     "type");
                    } else if (ak != TY_ERROR && !type_is_complete(TT, at))
                        cerror(c, loc, "invalid application of '__alignof__' "
                               "to incomplete type %s", type_q(TT, at));
                    else if (ak != TY_ERROR)
                        v = type_align(TT, at);
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
