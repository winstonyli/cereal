/* crecord.c - structs, unions and enums: the tags (start_struct, xref_tag),
 * finish_struct and finish_enum, the members, _Static_assert and
 * #pragma pack.  Called from the walk in cdecl.c (cdecl_node); the shared
 * helpers are in cdecl_int.h. */
#include "c/cdecl_int.h"

/* ---- structs, unions, enums ------------------------------------------------- */

uint32_t find_child(Checker *c, uint32_t i, unsigned tag)
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

/* -Wc++-compat: a typedef and a tag of the same name in one scope that are
 * not the same type; the earlier one is at old. */
void cdecl_typedef_tag_clash(Checker *c, uint32_t name, SrcLoc at, SrcLoc old)
{
    Diagnostic *d;
    if (cin_system(c, at))
        return;
    d = cwarn_d(c, DL_WARNING, at, "c++-compat", "using '%s' as both a typedef "
                "and a tag is invalid in C++", cident(c, name));
    if (d && old)
        cnote(c, d, old, "originally defined here");
}

/* pushtag of a new forward reference. */
static TypeId new_tag(Checker *c, int want, uint32_t name, SrcLoc loc)
{
    TypeId t = want == TY_ENUM
        ? type_new_enum(TT, name, loc)
        : type_new_record(TT, name, want == TY_UNION, loc);
    if (name) {
        uint32_t ob = cbound_here(c, NS_ORD, name);
        if (ob) {
            const CSym *o = csym(c, c->log.data[ob - 1].ref);
            if (o->kind == CS_TYPEDEF &&
                type_canon(TT, o->ty & ~(TypeId)TQ_MASK) != type_canon(TT, t))
                cdecl_typedef_tag_clash(c, name, loc, o->loc);
        }
    }
    cbind(c, NS_TAG, name, t);
    return t;
}

/* The unit's stray ';' in the struct body being defined, up to token upto
 * (gcc's parser diagnoses them as it goes). */
void cdecl_struct_semis(Checker *c, uint32_t upto)
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
        if (!c->recs.data[k].cleared &&
            type_canon(TT, c->recs.data[k].ty) == type_canon(TT, t))
            return true;
    return false;
}

void cdecl_tag_visit(Checker *c, uint32_t i)
{
    iloc_event(c, cnode(c, i)->tok);
}

/* c_cast_expr / the compound literal: the type name itself defines the tag
 * (ctsk_tagdef, or names it for the first time). */
static void cxx_defining_cast(Checker *c, uint32_t st, bool lit_only)
{
    if (cexpr_cxx_compat(c, st)) {
        uint32_t a = c->par[st], prev = st;
        for (; a != NO_NODE; prev = a, a = c->par[a]) {
            unsigned tg = ntag(c, a);
            if (tg == N_CAST || tg == N_COMPOUND_LIT) {
                if (first_child(c, a) == prev &&
                    (!lit_only || tg == N_COMPOUND_LIT))
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
}

/* start_struct / start_enum, at the '{'. */
void cdecl_open_visit(Checker *c, uint32_t i)
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
        if (being_defined(c, t)) {
            /* gcc diagnoses it in start_enum, before any later syntax error
             * in the unit (an empty inner enum), then clears the flag */
            size_t k;
            bool q = quiet_lift(c);
            cerror(c, loc, "nested redefinition of 'enum %s'",
                   name ? cident(c, name) : "");
            quiet_restore(c, q);
            for (k = 0; k < c->recs.len; k++)
                if (type_canon(TT, c->recs.data[k].ty) == type_canon(TT, t))
                    c->recs.data[k].cleared = true;
        }
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
    if (c->sx && name)
        csx_tag(c, t, cnode(c, tagn)->tok, CIX_DEF);
    if (cexpr_cxx_compat(c, st)) {
        /* in_sizeof / in_typeof / in_alignof: the parser is inside one */
        bool in_sz = false, in_ty = false, in_al = false;
        uint32_t a;
        for (a = c->par[st]; a != NO_NODE; a = c->par[a])
            switch (ntag(c, a)) {
            case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: in_sz = true; break;
            case N_TYPEOF: case N_HAS_ATTR: in_ty = true; break;
            case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE: in_al = true; break;
            default: break;
            }
        if (in_sz || in_ty || in_al)
            cwarn(c, loc, "c++-compat", "defining type in '%s' expression is "
                  "invalid in C++", in_sz ? "sizeof" : in_ty ? "typeof"
                                                              : "alignof");
    }
    cxx_defining_cast(c, st, false);
    if (c->recs.len && !vec_last(&c->recs).is_enum &&
        diag_enabled(c->diag, "c++-compat")) {
        if (want == TY_ENUM)
            type_enum(TT, t)->in_struct = true;
        else
            type_record(TT, t)->flags |= RF_IN_STRUCT;
    }
    rd.ty = t;
    rd.first = (uint32_t)c->fields.len;
    rd.first_td = (uint32_t)c->tdseen.len;
    vec_push(&c->recs, rd);
    c->ty[i] = t;
    if (created && cscope_kind(c) == SCK_PROTO)
        c->ef[i] |= 2;
}

/* -Wc++-compat: a type or enumerator defined inside a struct is used outside. */
void cxx_in_struct_use(Checker *c, SrcLoc at, const char *what,
                       const char *noted, SrcLoc def)
{
    Diagnostic *d;
    if (cin_system(c, at))
        return;
    d = cwarn_d(c, DL_WARNING, at, "c++-compat", "%s defined in struct or "
                "union is not visible in C++", what);
    if (d && def)
        cnote(c, d, def, "%s defined here", noted);
}

/* parser_xref_tag: 'struct S' without a body. */
static void xref_visit(Checker *c, uint32_t i, int want)
{
    uint32_t tagn = find_child(c, i, N_TAG), name;
    SrcLoc loc, xloc = 0;
    TypeId t, ref;
    unsigned kind = TSK_TAGREF;
    bool wrong = false;
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
        wrong = true;
    }
    if (ref) {
        t = ref;
        /* straight from the table: a name-only use is not a layout read */
        uint32_t x = type_ent(TT, type_canon(TT, t))->extra;
        if (want == TY_ENUM) {
            const Enum *e = &TT->enums.data[x];
            cdep_report(c, loc, name, e->dep, e->dmsg, &e->loc);
            if (e->in_struct && !c->recs.len)
                cxx_in_struct_use(c, loc, "enum type", "enum type", e->loc);
        } else {
            const Record *r = &TT->recs.data[x];
            cdep_report(c, loc, name, r->dep, r->dmsg, &r->loc);
            if ((r->flags & RF_IN_STRUCT) && !c->recs.len)
                cxx_in_struct_use(c, loc, tag_kw(want), tag_kw(want), r->loc);
        }
    } else {
        t = new_tag(c, want, name, loc);
        /* a tag of the wrong kind is a reference, not a first one */
        kind = wrong ? TSK_TAGREF : TSK_TAGFIRSTREF;
        cxx_defining_cast(c, i, true);
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
    if (c->sx)
        csx_tag(c, t, tt_tok, ref ? CIX_REF : CIX_DECL);
    c->ty[i] = t;
    c->cv[i] = kind | ((uint64_t)xloc << 8);
}

/* ---- finish_struct ----------------------------------------------------------- */

bool cdecl_flex_struct(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    Record *r;
    if (k != TY_STRUCT && k != TY_UNION)
        return false;
    r = type_record(TT, type_canon(TT, t));
    return r && (r->flags & RF_FLEXIBLE);
}

/* Whether member name was seen already in the record being finished (and
 * reported if so), else marks it seen: c->mseen[name] == c->mgen, a new
 * generation per record (struct_finish). */
static bool dup_add(Checker *c, uint32_t name, SrcLoc loc)
{
    if (name >= c->nmseen) {
        uint32_t n = c->nmseen ? c->nmseen : 1024;
        while (n <= name)
            n *= 2;
        c->mseen = xrealloc(c->mseen, n * sizeof *c->mseen);
        memset(c->mseen + c->nmseen, 0, (n - c->nmseen) * sizeof *c->mseen);
        c->nmseen = n;
    }
    if (c->mseen[name] == c->mgen) {
        cerror(c, loc, "duplicate member '%s'", cident(c, name));
        return true;
    }
    c->mseen[name] = c->mgen;
    return false;
}

static void dup_nested(Checker *c, TypeId t, int depth)
{
    Record *r = type_record(TT, type_canon(TT, t));
    uint32_t k;
    if (!r || !(r->flags & RF_COMPLETE) || depth > 16)
        return;
    for (k = 0; k < r->nfields; k++) {
        const Field *fl = &TT->fields.data[r->fields + k];
        if (fl->name)
            dup_add(c, fl->name, fl->loc);
        else if (tkind(c, fl->ty) == TY_STRUCT ||
                 tkind(c, fl->ty) == TY_UNION)
            dup_nested(c, fl->ty, depth + 1);
    }
}

bool is_rec(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_STRUCT || tkind(c, t) == TY_UNION;
}

/* -Wpacked at gcc's input_location when finish_struct runs: the last
 * struct/union/enum tag (or the record's own tag or '{') lexed, unless a
 * later token, up to the one after the attributes, starts a line. */
static SrcLoc finish_loc(Checker *c, uint32_t open, uint32_t close_tok)
{
    uint32_t k, tag, peek = close_tok + 1, last;
    int depth;
    for (open = cnode(c, open)->tok; open < close_tok; open++)
        if (tpunct(c, open) == P_LBRACE)
            break;
    tag = open;
    if (open > 0 && c->u->toks[open - 1].t.kind == TK_IDENT &&
        tckw(c, open - 1) == CK_NONE)
        tag = open - 1;
    for (k = open + 1; k < close_tok; k++) {
        int kw = tckw(c, k);
        if ((kw == CK_STRUCT || kw == CK_UNION || kw == CK_ENUM) &&
            k + 1 < close_tok)
            tag = k + 1;
    }
    /* trailing attributes */
    while (peek < c->u->ntoks && tckw(c, peek) == CK_ATTRIBUTE) {
        peek++;
        for (depth = 0; peek < c->u->ntoks; peek++) {
            int p = tpunct(c, peek);
            if (p == P_LPAREN)
                depth++;
            else if (p == P_RPAREN && --depth == 0) {
                peek++;
                break;
            }
        }
    }
    last = tag;
    for (k = tag + 1; k <= peek && k < c->u->ntoks; k++)
        if (c->u->toks[k].t.flags & TF_BOL)
            last = 0;
    return last ? tloc(c, tag) : cinput_loc(c, peek);
}

static void packed_unnecessary(Checker *c, TypeId t, uint32_t open,
                               uint32_t close_tok)
{
    const Record *r = type_record(TT, t);
    SrcLoc loc = finish_loc(c, open, close_tok);
    if (r->tag)
        cwarn(c, loc, "packed", "packed attribute is unnecessary for '%s'",
              cident(c, r->tag));
    else
        cwarn(c, loc, "packed", "packed attribute is unnecessary");
}

/* place_field and finalize_record_size: -Wpadded.  A field placed past the
 * end of the one before it, and the size rounded up to the alignment. */
static void padded_check(Checker *c, TypeId t, bool is_union, uint32_t open,
                         uint32_t close_tok)
{
    const Record *r = type_record(TT, t);
    uint64_t pos = 0, end = 0;
    uint32_t k;
    for (k = 0; k < r->nfields; k++) {
        const Field *fl = &TT->fields.data[r->fields + k];
        bool bf = (fl->flags & FF_BITFIELD) != 0;
        uint64_t bits = bf ? fl->width : type_size(TT, fl->ty, NULL) * 8;
        if (!is_union && !bf && fl->off_bits > pos)
            cwarn(c, fl->loc, "padded", "padding struct to align '%s'",
                  fl->name ? cident(c, fl->name) : "({anonymous})");
        if (is_union ? bits > end : fl->off_bits + bits > end)
            end = is_union ? bits : fl->off_bits + bits;
        pos = fl->off_bits + bits;
    }
    end = (end + 7) / 8;
    if (r->size > end)
        cwarn(c, finish_loc(c, open, close_tok), "padded", "padding struct "
              "size to alignment boundary with %llu bytes",
              (unsigned long long)(r->size - end));
}

/* TYPE_WARN_IF_NOT_ALIGN: the warn_if_not_aligned of a typedef (or one it
 * names), of a record, or of an array's element. */
static unsigned type_wina(Checker *c, TypeId t)
{
    for (;;) {
        const TypeEnt *e = type_ent(TT, t);
        size_t k;
        if (e->kind == TY_TYPEDEF) {
            for (k = c->wina_td.len; k-- > 0;)
                if (c->wina_td.data[k].key == TYPE_IDX(t))
                    return c->wina_td.data[k].wina;
            t = e->base;
        } else if (e->kind == TY_ARRAY) {
            t = e->base;
        } else if (e->kind == TY_STRUCT || e->kind == TY_UNION) {
            for (k = c->wina_rec.len; k-- > 0;)
                if (c->wina_rec.data[k].key == e->extra)
                    return c->wina_rec.data[k].wina;
            return 0;
        } else
            return 0;
    }
}

/* Does the type carry an aligned attribute (TYPE_ATTRIBUTES)? */
static bool type_user_aligned(Checker *c, TypeId t)
{
    for (;;) {
        const TypeEnt *e = type_ent(TT, t);
        (void)c;
        if (e->kind == TY_TYPEDEF) {
            if (e->flags & TF_ALIGNED)
                return true;
            t = e->base;
        } else if (e->kind == TY_STRUCT || e->kind == TY_UNION)
            return (TT->recs.data[e->extra].flags & RF_USER_ALIGN) != 0;
        else
            return false;
    }
}

/* place_field and finalize_record_size: -Wif-not-aligned and
 * -Wpacked-not-aligned. */
static void wina_check(Checker *c, TypeId t, const FieldIn *f, uint32_t m,
                       unsigned attr_wina, SrcLoc end)
{
    Record *r = type_record(TT, t);
    const Field *fl;
    unsigned rw = attr_wina, rp = 0;
    uint32_t k;
    bool on_w = diag_enabled(c->diag, "if-not-aligned"),
         on_p = diag_enabled(c->diag, "packed-not-aligned");
    if (!r || m != r->nfields)
        return;
    fl = TT->fields.data + r->fields;
    for (k = 0; k < m; k++) {
        unsigned w, p = 0;
        if (f[k].width >= 0)
            continue;
        w = f[k].wina > type_wina(c, f[k].ty) ? f[k].wina
                                              : type_wina(c, f[k].ty);
        if (!w && type_user_aligned(c, f[k].ty))
            p = type_align(TT, f[k].ty);
        if (w > rw)
            rw = w;
        if (p > rp)
            rp = p;
    }
    if (rw) {
        WinaEnt we = {(uint32_t)(r - TT->recs.data), (uint16_t)rw};
        vec_push(&c->wina_rec, we);
    }
    if (rw && !on_w)
        rw = 0;
    if (rp && !on_p)
        rp = 0;
    if (rw ? r->align < rw : rp && r->align < rp) {
        unsigned need = rw ? rw : rp;
        cwarn(c, end, rw ? "if-not-aligned" : "packed-not-aligned",
              "alignment %u of '%s %s' is less than %u", r->align,
              r->flags & RF_UNION ? "union" : "struct",
              r->tag ? cident(c, r->tag) : "", need);
    }
    for (k = 0; k < m; k++) {
        unsigned w, p = 0, v;
        if (f[k].width >= 0 || !f[k].name)
            continue;
        w = f[k].wina > type_wina(c, f[k].ty) ? f[k].wina
                                              : type_wina(c, f[k].ty);
        if (!w && type_user_aligned(c, f[k].ty))
            p = type_align(TT, f[k].ty);
        v = w ? w : p;
        if (!v || !(w ? on_w : on_p))
            continue;
        if ((fl[k].off_bits / 8) % v)
            cwarn(c, f[k].loc, w ? "if-not-aligned" : "packed-not-aligned",
                  "'%s' offset %llu in '%s %s' isn't aligned to %u",
                  cident(c, f[k].name),
                  (unsigned long long)(fl[k].off_bits / 8),
                  r->flags & RF_UNION ? "union" : "struct",
                  r->tag ? cident(c, r->tag) : "", v);
    }
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
    int keep_err = -1;
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
    cdecl_struct_semis(c, close_tok);
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
    if (a.nonstring)
        cwarn(c, iloc(c, close_tok), "attributes",
              "'nonstring' attribute does not apply to types");
    if (a.desig && want == TY_UNION)
        cerror(c, iloc(c, close_tok), "'designated_init' attribute is only "
               "valid on 'struct' type");
    n = (uint32_t)c->fields.len - rd.first;
    f = c->fields.data + rd.first;
    if (c->opt.pedantic) {
        for (k = 0; k < n; k++)
            if (f[k].name || (c->opt.std_year >= 2011 && type_is_record(TT, f[k].ty)))
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
        size_t q;
        for (q = rd.first_td; q < c->tdseen.len && f[k].name; q++)
            if (c->tdseen.data[q] == f[k].name) {
                if (!cin_system(c, f[k].loc))
                    cwarn(c, f[k].loc, "c++-compat", "using '%s' as both "
                          "field and typedef name is invalid in C++",
                          cident(c, f[k].name));
                break;
            }
    }
    c->tdseen.len = rd.first_td;
    if (n == 0 && !cin_system(c, loc))
        cwarn(c, loc, "c++-compat", "empty %s has size 0 in C, size 1 in C++",
              want == TY_UNION ? "union" : "struct");
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
                /* the member stays, erroneous (uses are silent) */
                f[k].ty = ERRT;
                keep_err = k;
            } else if (!(type_ent(TT, type_canon(TT, f[k].ty))->flags &
                         TF_FLEX)) {
                /* finish_struct gives a typedef'd `T[]` member its domain too */
                TypeId ct = type_canon(TT, f[k].ty);
                f[k].ty = type_array_flex(TT, type_base(TT, ct)) |
                          TYPE_QUALS(f[k].ty);
            }
        }
        if (c->opt.pedantic && want == TY_STRUCT && cdecl_flex_struct(c, f[k].ty))
            cpedantic(c, f[k].loc, "invalid use of structure with flexible "
                      "array member");
        if (f[k].name || is_rec(c, f[k].ty))
            saw_named = true;
    }
    if (!++c->mgen) {           /* wrapped: no stale marks may match */
        memset(c->mseen, 0, c->nmseen * sizeof *c->mseen);
        c->mgen = 1;
    }
    for (k = 0; k < n; k++) {
        if (f[k].name) {
            if (dup_add(c, f[k].name, f[k].loc))
                f[k].name = 0;
        } else if (is_rec(c, f[k].ty) && f[k].width < 0)
            dup_nested(c, f[k].ty, 0);
    }
    for (k = 0; k < n; k++)
        if (!is_err(c, f[k].ty) || (int)k == keep_err)
            f[m++] = f[k];
    csum_read_pack(c);
    type_complete_record(TT, t, f, m, c->pack, a.aligned, a.packed,
                         a.ms);
    if (c->sx)
        for (k = 0; k < m; k++)
            if (f[k].tok && f[k].name)
                csx_field(c, type_record(TT, t)->fields + k, f[k].tok - 1,
                          CIX_DEF);
    if (type_record(TT, t)->size > (uint64_t)INT64_MAX)
        cerror(c, loc, "type %s is too large", type_q(TT, t));
    /* gcc reports at the closing brace when it starts its line, else at the tag */
    wina_check(c, t, f, m, a.wina_al,
               cbol_tok(c, close_tok) == close_tok + 1 ? cinput_loc(c, close_tok)
                                                       : loc);
    if (diag_enabled(c->diag, "padded"))
        padded_check(c, t, want == TY_UNION, open, close_tok);
    if (a.packed && want != TY_UNION && diag_enabled(c->diag, "packed") &&
        type_packed_unnecessary(TT, t, f, m, c->pack, a.aligned, a.ms, -1))
        packed_unnecessary(c, t, open, close_tok);
    if (!a.packed && want != TY_UNION && diag_enabled(c->diag, "packed"))
        for (k = 0; k < m; k++)
            if (f[k].packed && !f[k].align && f[k].name &&
                type_align(TT, f[k].ty) != 1 &&
                type_packed_unnecessary(TT, t, f, m, c->pack, a.aligned, a.ms,
                                        (long)k))
                cwarn(c, f[k].loc, "attributes", "packed attribute is "
                      "unnecessary for '%s'", cident(c, f[k].name));
    if (want == TY_UNION)       /* a member of the other storage order */
        for (k = 0; k < m; k++) {
            TypeId ft = type_canon(TT, f[k].ty);
            while (tkind(c, ft) == TY_ARRAY || tkind(c, ft) == TY_VLA)
                ft = type_canon(TT, type_base(TT, ft));
            if (is_rec(c, ft) &&
                ((type_record(TT, ft)->flags & RF_SSO) != 0) != (a.sso == 1))
                cwarn(c, f[k].loc, "scalar-storage-order", "type punning "
                      "toggles scalar storage order");
        }
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
    if (a.may_alias)
        r->flags |= RF_MAYALIAS;
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

/* "enumconst NAME = VALUE (TYPE)": --dump-types and the hover text. */
void crecord_enumconst_line(Checker *c, const CSym *s, StrBuf *sb)
{
    if (type_is_signed(TT, s->vty))
        sb_printf(sb, "enumconst %s = %" PRId64 " (", sname(c, s), (int64_t)s->val);
    else
        sb_printf(sb, "enumconst %s = %" PRIu64 " (", sname(c, s), s->val);
    type_print(TT, sb, s->vty);
    sb_putc(sb, ')');
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

/* Attributes written on an enumerator (a CONST_DECL): decl_attributes finds
 * a handler that does not take it.  Probed on gcc 13. */
static void enumerator_attrs(Checker *c, uint32_t i, SrcLoc loc, uint32_t name,
                             uint32_t ref)
{
    static const char *const ignored[] = {
        "alias", "always_inline", "artificial", "assume", "cleanup", "cold",
        "common", "const", "constructor", "destructor", "error",
        "externally_visible", "fallthrough", "fentry_name", "fentry_section",
        "flatten", "gcc_struct", "gnu_inline", "hot", "ifunc", "mode",
        "ms_struct", "no_address_safety_analysis", "no_icf",
        "no_profile_instrument_function", "no_sanitize",
        "no_sanitize_address", "no_sanitize_coverage", "no_sanitize_thread",
        "no_sanitize_undefined", "no_stack_protector", "noclone", "nocommon",
        "nodirect_extern_access", "noinline", "noipa", "nonstring",
        "noreturn", "nothrow", "optimize", "packed", "pure", "retain",
        "returns_twice", "scalar_storage_order", "signed_bool_precision",
        "simd", "stack_protect", "target", "target_clones",
        "transaction_callable", "transaction_may_cancel_outer",
        "transaction_pure", "transaction_safe", "transaction_safe_dynamic",
        "transaction_unsafe", "transaction_wrap", "transparent_union", "used",
        "vector_mask", "visibility", "volatile", "warn_unused", "warning",
        "weak", "weakref"};
    static const char *const fnonly[] = {
        "access", "alloc_align", "alloc_size", "assume_aligned",
        "callee_pop_aggregate_return", "cdecl", "fastcall", "fd_arg",
        "fd_arg_read", "fd_arg_write", "force_align_arg_pointer",
        "format_arg", "indirect_return", "interrupt", "ms_abi",
        "no_caller_saved_registers", "nocf_check", "nonnull", "regparm",
        "returns_nonnull", "sentinel", "sseregparm", "stdcall", "sysv_abi",
        "thiscall", "warn_unused_result"};
    Kids k;
    uint32_t j;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[48];
            size_t f;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (!strcmp(an, "gnu") || attr_scope_of(c, c->nodes[it.p[q]].tok))
                continue;
            if (!strcmp(an, "unused") && ref != SYM_NONE)
                csym(c, ref)->flags |= CSF_ATTR_UNUSED;
            if (!attr_known(an) || !strcmp(an, "maybe_unused") ||
                !strcmp(an, "nodiscard")) {
                cwarn(c, loc, "attributes", "'%s' attribute directive "
                      "ignored", an);
                continue;
            }
            if (!strcmp(an, "malloc")) {
                cwarn(c, loc, "attributes", "'malloc' attribute ignored; "
                      "valid only for functions");
                continue;
            }
            if (!strcmp(an, "aligned")) {
                cerror(c, loc, "alignment may not be specified for '%s'",
                       cident(c, name));
                continue;
            }
            if (!strcmp(an, "section")) {
                cerror(c, loc, "section attribute not allowed for '%s'",
                       cident(c, name));
                continue;
            }
            for (f = 0; f < sizeof fnonly / sizeof *fnonly; f++)
                if (!strcmp(an, fnonly[f])) {
                    cwarn(c, loc, "attributes", "'%s' attribute only applies "
                          "to function types", an);
                    break;
                }
            if (f < sizeof fnonly / sizeof *fnonly)
                continue;
            for (f = 0; f < sizeof ignored / sizeof *ignored; f++)
                if (!strcmp(an, ignored[f])) {
                    cwarn(c, loc, "attributes", "'%s' attribute ignored", an);
                    break;
                }
        }
        kids_free(&it);
    }
    kids_free(&k);
}

void cdecl_enumerator_visit(Checker *c, uint32_t i)
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
        cped2x(c, vloc, "ISO C restricts enumerator values to range of "
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
    if (type_enum(TT, rd->ty)->in_struct)
        s.flags |= CSF_IN_STRUCT;
    {
        Attrs ea;
        memset(&ea, 0, sizeof ea);
        c->attr_quiet = true;       /* enumerator_attrs reports them */
        attrs_of_children(c, i, &ea);
        c->attr_quiet = false;
        if (ea.deprecated || ea.unavailable) {
            s.flags |= ea.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
            s.dep_msg = ea.dep_msg;
        }
    }
    ref = cdecl_pushdecl(c, &s, false, cnode(c, i)->tok);
    vec_push(&c->ecs, ref);
    enumerator_attrs(c, i, nloc, name, ref);
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
            StrBuf sb;
            memset(&sb, 0, sizeof sb);
            crecord_enumconst_line(c, csym(c, c->ecs.data[rd.first_ec + k]), &sb);
            fprintf(c->opt.dump, "%s\n", sb_cstr(&sb));
            sb_free(&sb);
        }
    }
    if (c->sx)
        csx_enum(c, t, c->ecs.data + rd.first_ec, ne);
    c->ecs.len = rd.first_ec;
    c->fields.len = rd.first;
    c->recs.len--;
    c->ty[i] = t;
    c->cv[i] = TSK_TAGDEF;
    if (c->ef[open] & 2)
        c->ef[i] |= 2;
}

/* handle_visibility_attribute on a tagged definition: C has no class types. */
static void tag_visibility(Checker *c, uint32_t i)
{
    uint32_t tg = find_child(c, i, N_TAG), j, etok = 0;
    Kids k;
    if (tg == NO_NODE)
        return;
    if (cnode(c, i)->tag == N_ENUM) {   /* gcc's input_location: the first enumerator */
        for (etok = cnode(c, i)->tok; tpunct(c, etok) != P_LBRACE; etok++)
            ;
        etok++;
    }
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[48];
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (!strcmp(an, "visibility") &&
                !attr_scope_of(c, c->nodes[it.p[q]].tok))
                cwarn(c, tloc(c, etok ? etok : cnode(c, tg)->tok),
                      "attributes", "'visibility' attribute ignored on types");
        }
        kids_free(&it);
    }
    kids_free(&k);
}

void cdecl_struct_visit(Checker *c, uint32_t i)
{
    uint32_t open = find_child(c, i, N_OPEN);
    int want;
    if (cnode(c, i)->tag == N_ENUM)
        want = TY_ENUM;
    else
        want = tckw(c, cnode(c, i)->tok) == CK_UNION ? TY_UNION : TY_STRUCT;
    if (want == TY_ENUM && open != NO_NODE) {   /* a trailing comma */
        Kids ek;
        uint32_t j, en = NO_NODE;
        kids_get(c, i, &ek);
        for (j = 0; j < ek.n; j++)
            if (ntag(c, ek.p[j]) == N_ENUMERATOR)
                en = ek.p[j];
        kids_free(&ek);
        if (en != NO_NODE && tpunct(c, last_tok(c, en) + 1) == P_COMMA)
            cc90(c, tloc(c, last_tok(c, en) + 1), NULL, "comma at end of "
                 "enumerator list");
    }
    if (want == TY_ENUM && find_child(c, i, N_TYPE_NAME) != NO_NODE) {
        uint32_t tn = find_child(c, i, N_TYPE_NAME);
        uint32_t tg_ = find_child(c, i, N_TAG);
        cped2x(c, tloc(c, tg_ != NO_NODE ? cnode(c, tg_)->tok
                                              : first_tok(c, tn) - 1),
                  "ISO C does not support specifying 'enum' underlying types "
                  "before C2X");
    }
    if (open != NO_NODE)
        tag_visibility(c, i);
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
        k = cdecl_scan_end(c, k, false);
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

void cdecl_member_visit(Checker *c, uint32_t i)
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
    si = cdecl_find_spec(c, sn);
    if (si < 0)
        return;
    cdecl_struct_semis(c, first_tok(c, md));
    cdecl_pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    cdecl_dep_spec_use(c, &sp);
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
    cdecl_grok(c, &sp, top, DC_FIELD, false, false, w, ltok, ltok, &g);
    if (g.what == GD_NONE)
        return;
    memset(&a, 0, sizeof a);
    attrs_of_children(c, i, &a);
    if (a.has_mode || a.vs_seen)
        g.ty = attr_apply_type(c, g.ty, &a, iloc(c, ltok));
    attrs_unknown_emit(c, &sp.attrs, ltok);
    if (top != NO_NODE)
        ptr_type_attrs(c, top, g.ty, ltok, &a);
    attrs_merge(&a, &sp.attrs);
    attrs_misapplied(c, &a, 'm', false, w == NO_NODE ? g.ty : 0, ltok);
    attrs_ctx_check(c, sp.node, g.ty, ltok, AC_F);
    attrs_ctx_check(c, i, g.ty, ltok, AC_F);
    attrs_section_check(c, &a, 'm', false, g.name, g.loc);
    strict_flex_check(c, sp.node, true, g.ty, g.name, g.loc, sp.tok0);
    strict_flex_check(c, i, true, g.ty, g.name, g.loc, NO_NODE);
    attrs_zcur_check(c, &a, false, g.loc);
    attrs_wina_check(c, &a, 'm', g.width >= 0, g.name, g.loc);
    if (g.width >= 0 && g.name && type_wina(c, g.ty))
        cerror(c, g.loc, "cannot declare bit-field '%s' with "
               "'warn_if_not_aligned' type", cident(c, g.name));
    memset(&fi, 0, sizeof fi);
    fi.wina = a.wina_al;
    fi.name = g.name;
    if (g.name)
        fi.tok = cnode(c, g.name_node)->tok + 1;
    fi.ty = g.ty;
    fi.width = g.width;
    fi.align = g.s.align > a.aligned ? g.s.align : a.aligned;
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

void cdecl_member_decl_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i);
    int si = sn != NO_NODE ? cdecl_find_spec(c, sn) : -1;
    if (si >= 0 && c->nodes[i].size == c->nodes[sn].size + 1) {
        Spec sp = c->specs.data[si];
        uint32_t ltok = sp.tok1;
        cdecl_struct_semis(c, first_tok(c, i));
        if (sp.kind == TSK_NONE && !sp.has_type && sp.default_int) {
            cpedantic(c, tloc(c, sp.tok0), "ISO C forbids member declarations "
                      "with no members");
            cdecl_shadow_tag(c, &sp, c->opt.pedantic ? 1 : 0, ltok);
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
                cdecl_grok(c, &sp, NO_NODE, DC_FIELD, false, false, NO_NODE, ltok,
                     ltok, &g);
                if (g.what != GD_NONE) {
                    FieldIn fi;
                    memset(&fi, 0, sizeof fi);
                    fi.ty = g.ty;
                    fi.width = -1;
                    fi.align = g.s.align > sp.attrs.aligned
                        ? g.s.align : sp.attrs.aligned;
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
    cdecl_pop_specs(c, i);
}

/* ---- _Static_assert, #pragma pack ---------------------------------------------- */

void cdecl_static_assert_visit(Checker *c, uint32_t i)
{
    uint32_t e = first_child(c, i), s = NO_NODE, k;
    SrcLoc aloc = tloc(c, cnode(c, i)->tok), vloc;
    Kids kk;
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        cdecl_struct_semis(c, cnode(c, i)->tok);
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++)
        if (ntag(c, kk.p[k]) == N_STRING)
            s = kk.p[k];
    kids_free(&kk);
    if (!in_extension(c, i)) {
        size_t n0 = c->diag->all.len;
        cped11(c, aloc, "ISO C99 does not support '_Static_assert'");
        if (c->opt.std_year >= 2011 && (s == NO_NODE || s == e))
            cped2x(c, aloc, "ISO C11 does not support omitting the string in "
                   "'_Static_assert'");
        choist(c, i, n0);
    }
    if (e == NO_NODE)
        return;
    if (s == e)
        return;
    vloc = value_loc(c, strip_paren(c, e), tloc(c, first_tok(c, e)));
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
    if (c->ef[e] & EF_OVERFLOW)        /* constant_expression_warning */
        cconst_overflow(c, aloc);
    if (cexpr_sval(c, e) == 0) {
        if (s != NO_NODE) {
            StrBuf sb;
            uint32_t p, np = node_pieces(c, s);
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

void cdecl_pragma_visit(Checker *c, uint32_t i)
{
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        cdecl_struct_semis(c, cnode(c, i)->tok);
    cpragma_apply(c, cnode(c, i)->tok);
}

