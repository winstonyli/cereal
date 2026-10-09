/* cstmt.c - statements and function-level checks.
 *
 * A port of the statement parts of gcc 13: c-typeck.cc (c_finish_return,
 * c_start_switch, do_case, c_finish_switch, c_finish_bc_stmt), c-decl.cc
 * (labels, jumps into the scope of variably modified types, the for-loop
 * declaration rules), c-common.cc (c_add_case_label, c_do_switch_warnings)
 * and the warnings of c-parser.cc's statement parsers (-Wdangling-else,
 * -Wempty-body, -Wparentheses, label at the end of a block).
 *
 * The walk is post-order, so a few diagnostics gcc gives before a
 * statement's body (a label's duplicate check, a case label) have to be
 * made early: cstmt_enter runs at the first node of a LABEL/DEFAULT
 * subtree, and cstmt_expr after the expressions that decide things (the
 * case values, the controlling expressions).  See docs/TYPES.md. */
#include <ctype.h>
#include "c/cstmt_int.h"

#include <stdio.h>
#include <string.h>


static CStmt *st(Checker *c)
{
    if (!c->stmt) {
        c->stmt = xcalloc(1, sizeof(CStmt));
        ((CStmt *)c->stmt)->cur = NOB;
    }
    return c->stmt;
}

static FuncState *top(CStmt *s)
{
    return &s->fs.data[s->fs.len - 1];
}

/* ---- locations ---------------------------------------------------------- */

static const char *label_name(Checker *c, const CLabel *l)
{
    return cident(c, l->name);
}

/* ---- blocks ---------------------------------------------------------------- */

/* Does block b enclose block x (or is it x)? */
static bool encl(const CStmt *s, uint32_t b, uint32_t x)
{
    uint32_t db;
    if (b == NOB)
        return true;
    db = s->blocks.data[b].depth;
    while (x != NOB && s->blocks.data[x].depth > db)
        x = s->blocks.data[x].parent;
    return x == b;
}

static uint32_t depth_of(const CStmt *s, uint32_t b)
{
    return s->blocks.data[b].depth;
}

/* ---- the per-unit state ------------------------------------------------------ */

static void lmap_ensure(Checker *c, CStmt *s, uint32_t name)
{
    if (name >= s->nmap) {
        uint32_t n = c->nidents > name + 1 ? c->nidents : name + 1;
        if (n < s->nmap * 2)
            n = s->nmap * 2;
        s->lmap = xrealloc(s->lmap, n * sizeof *s->lmap);
        memset(s->lmap + s->nmap, 0, (n - s->nmap) * sizeof *s->lmap);
        s->nmap = n;
    }
}

static uint32_t lmap_get(const CStmt *s, uint32_t name)
{
    return name < s->nmap ? s->lmap[name] : 0;
}

static void lmap_set(Checker *c, CStmt *s, uint32_t name, uint32_t v)
{
    lmap_ensure(c, s, name);
    s->lmap[name] = v;
}

static void enumset_free(CStmt *s)
{
    free(s->en.name);
    free(s->en.val);
    free(s->en.unused);
    memset(&s->en, 0, sizeof s->en);
}

void cstmt_unit_begin(Checker *c)
{
    CStmt *s = c->stmt;
    size_t k;
    if (!s)
        return;
    for (k = 0; k < s->labels.len; k++)
        if (s->labels.data[k].name < s->nmap)
            s->lmap[s->labels.data[k].name] = 0;
    s->blocks.len = s->unsafe.len = s->gotos.len = s->labels.len = 0;
    s->sw.len = s->cases.len = s->fs.len = s->tmp.len = 0;
    s->cur = NOB;
    enumset_free(s);
}

void cstmt_free(Checker *c)
{
    CStmt *s = c->stmt;
    if (!s)
        return;
    vec_free(&s->blocks);
    vec_free(&s->unsafe);
    vec_free(&s->gotos);
    vec_free(&s->labels);
    vec_free(&s->sw);
    vec_free(&s->cases);
    vec_free(&s->fs);
    vec_free(&s->tmp);
    free(s->lmap);
    enumset_free(s);
    free(s);
    c->stmt = NULL;
}

/* ---- jumps ------------------------------------------------------------------ */

/* Puts the unsafe declarations selected by the caller in s->tmp in gcc's
 * order: those of `first` (common-scope declarations made after a forward
 * goto) first, then innermost scope outwards, newest first. */
static void sort_unsafe(CStmt *s, bool first_group)
{
    size_t i, j;
    uint32_t *a = s->tmp.data;
    for (i = 1; i < s->tmp.len; i++) {
        uint32_t v = a[i];
        const CUnsafe *u = &s->unsafe.data[v & 0x7FFFFFFFu];
        bool vg = first_group && (v >> 31);
        for (j = i; j > 0; j--) {
            uint32_t w = a[j - 1];
            const CUnsafe *x = &s->unsafe.data[w & 0x7FFFFFFFu];
            bool wg = first_group && (w >> 31);
            bool before;
            if (vg != wg)
                before = vg;
            else if (depth_of(s, u->block) != depth_of(s, x->block))
                before = depth_of(s, u->block) > depth_of(s, x->block);
            else
                before = u->seq > x->seq;
            if (!before)
                break;
            a[j] = w;
        }
        a[j] = v;
    }
}

static void warn_about_goto(Checker *c, SrcLoc gloc, const CLabel *l,
                            const CUnsafe *u)
{
    Diagnostic *d;
    if (u->vm)
        d = cerror_d(c, gloc, "jump into scope of identifier with variably "
                     "modified type");
    else
        d = cwarn_d(c, DL_WARNING, gloc, "jump-misses-init", "jump skips "
                    "variable initialization");
    if (!d)
        return;
    cnote(c, d, l->loc, "label '%s' defined here", label_name(c, l));
    cnote(c, d, u->loc, "'%s' declared here",
          u->anon ? "({anonymous})" : cident(c, u->name));
}

/* check_earlier_gotos / the decls_in_scope loop of lookup_label_for_goto:
 * a goto at gloc (in block gblock, event gseq) to the defined label l. */
static void jump_checks(Checker *c, CStmt *s, const CLabel *l, SrcLoc gloc,
                        uint32_t gblock, uint32_t gseq)
{
    FuncState *f = top(s);
    size_t k;
    uint32_t x;
    s->tmp.len = 0;
    for (k = f->unsafe_base; k < s->unsafe.len; k++) {
        const CUnsafe *u = &s->unsafe.data[k];
        uint32_t v = (uint32_t)k;
        if (u->seq >= l->def_seq || !encl(s, u->block, l->def_block))
            continue;
        if (encl(s, u->block, gblock)) {
            if (u->seq < gseq)
                continue;
            v |= 0x80000000u;    /* declared in the common scope after */
        }
        vec_push(&s->tmp, v);
    }
    if (s->tmp.len) {
        sort_unsafe(s, true);
        for (k = 0; k < s->tmp.len; k++)
            warn_about_goto(c, gloc, l,
                            &s->unsafe.data[s->tmp.data[k] & 0x7FFFFFFFu]);
    }
    for (x = l->def_block; x != NOB && !encl(s, x, gblock);
         x = s->blocks.data[x].parent)
        if (s->blocks.data[x].stmtexpr) {
            Diagnostic *d = cerror_d(c, gloc, "jump into statement "
                                     "expression");
            cnote(c, d, l->loc, "label '%s' defined here", label_name(c, l));
            break;
        }
}

static void check_earlier_gotos(Checker *c, CStmt *s, uint32_t li)
{
    CLabel *l = &s->labels.data[li];
    uint32_t g = l->ghead;
    while (g) {
        CGoto gt = s->gotos.data[g - 1];
        jump_checks(c, s, &s->labels.data[li], gt.loc, gt.block, gt.seq);
        g = gt.next;
    }
    l->ghead = l->gtail = 0;
}

/* ---- labels ----------------------------------------------------------------- */

static uint32_t new_label(Checker *c, CStmt *s, uint32_t name, SrcLoc loc,
                          uint32_t key, bool defined, bool declared,
                          uint32_t block)
{
    CLabel l;
    memset(&l, 0, sizeof l);
    l.name = name;
    l.prev = lmap_get(s, name);
    l.block = block;
    l.key = key;
    l.fn = (uint32_t)s->fs.len;
    l.loc = loc;
    l.defined = defined;
    l.declared = declared;
    if (declared)
        top(s)->ndeclared++;
    vec_push(&s->labels, l);
    lmap_set(c, s, name, (uint32_t)s->labels.len);
    if (c->sx)
        csx_label_new(c, (uint32_t)s->labels.len - 1, name, loc);
    return (uint32_t)s->labels.len - 1;
}

/* define_label, at the first node of a LABEL. */
static void define_label(Checker *c, CStmt *s, uint32_t node, uint32_t key)
{
    uint32_t name = cnode_ident(c, node);
    SrcLoc loc = cnode_loc(c, node);
    uint32_t idx = lmap_get(s, name), fn = (uint32_t)s->fs.len;
    FuncState *f = top(s);
    if (!name)
        return;
    if (lookup_ord(c, name) != SYM_NONE && !cin_system(c, loc))
        cwarn(c, loc, "traditional", "traditional C lacks a separate "
              "namespace for labels, identifier '%s' conflicts",
              cident(c, name));
    if (idx) {
        CLabel *l = &s->labels.data[idx - 1];
        if (c->sx && (l->fn == fn || l->declared))
            csx_label(c, idx - 1, cnode(c, node)->tok, CIX_DEF);
        if ((l->fn == fn && l->defined) || (l->fn != fn && l->declared)) {
            Diagnostic *d = cerror_d(c, loc, "duplicate label '%s'",
                                     cident(c, name));
            cnote(c, d, l->loc, "previous %s of '%s' with type 'void'",
                  l->defined ? "definition" : "declaration", cident(c, name));
            return;
        }
        if (l->fn == fn) {
            l->loc = loc;
            l->defined = true;
            l->def_seq = ++f->seq;
            l->def_block = s->cur;
            check_earlier_gotos(c, s, idx - 1);
            return;
        }
    }
    idx = new_label(c, s, name, loc, key, true, false, f->fblock);
    if (c->sx)
        csx_label(c, idx, cnode(c, node)->tok, CIX_DEF);
    s->labels.data[idx].def_seq = ++f->seq;
    s->labels.data[idx].def_block = s->cur;
}

/* lookup_label_for_goto: a goto (or &&label) at gloc naming ident; uloc is
 * gcc's input_location then. */
void use_label(Checker *c, CStmt *s, uint32_t name, SrcLoc gloc,
                      SrcLoc uloc, uint32_t key)
{
    uint32_t idx = lmap_get(s, name), fn = (uint32_t)s->fs.len;
    FuncState *f = top(s);
    CLabel *l;
    if (idx && (s->labels.data[idx - 1].fn == fn ||
                s->labels.data[idx - 1].declared)) {
        l = &s->labels.data[idx - 1];
        if (!l->defined)
            l->loc = uloc;
    } else {
        idx = new_label(c, s, name, uloc, key, false, false, f->fblock) + 1;
        l = &s->labels.data[idx - 1];
    }
    if (c->sx)
        csx_label(c, idx - 1, c->nodes[key].tok, CIX_REF);
    if (l->fn != fn) {
        l->used = true;
        return;
    }
    if (!l->defined) {
        CGoto g;
        g.loc = gloc;
        g.block = s->cur;
        g.seq = ++f->seq;
        g.next = 0;
        vec_push(&s->gotos, g);
        l = &s->labels.data[idx - 1];
        if (l->gtail)
            s->gotos.data[l->gtail - 1].next = (uint32_t)s->gotos.len;
        else
            l->ghead = (uint32_t)s->gotos.len;
        l->gtail = (uint32_t)s->gotos.len;
    } else
        jump_checks(c, s, l, gloc, s->cur, ++f->seq);
    s->labels.data[idx - 1].used = true;
}

static void emit_label(Checker *c, CLabel *l)
{
    l->emitted = true;
    if (l->used && !l->defined)
        cerror(c, l->loc, "label '%s' used but not defined",
               label_name(c, l));
    else if (!l->used) {
        if (l->defined)
            cwarn(c, l->loc, "unused-label", "label '%s' defined but not "
                  "used", label_name(c, l));
        else
            cwarn(c, l->loc, "unused-label", "label '%s' declared but not "
                  "defined", label_name(c, l));
    }
}

/* pop_scope's label bindings for the scope opened at scope_node: those
 * newer than the declaration min_key (newest first). */
void cstmt_emit_labels(Checker *c, uint32_t scope_node, int64_t min_key)
{
    CStmt *s = c->stmt;
    uint32_t blk;
    size_t k;
    if (!s || !s->fs.len || !c->cv[scope_node])
        return;
    blk = (uint32_t)c->cv[scope_node] - 1;
    for (k = s->labels.len; k-- > top(s)->label_base;) {
        CLabel *l = &s->labels.data[k];
        if (l->block == blk && !l->emitted && (int64_t)l->key > min_key)
            emit_label(c, l);
    }
}

/* ---- the state's entry points ------------------------------------------------ */

static void new_block(Checker *c, CStmt *s, uint32_t scope_node)
{
    CBlock b;
    uint32_t p = c->par[scope_node];
    b.parent = s->cur;
    b.depth = s->cur == NOB ? 0 : s->blocks.data[s->cur].depth + 1;
    b.stmtexpr = p != NOB && tg(c, p) == N_COMPOUND && c->par[p] != NOB &&
                 tg(c, c->par[p]) == N_STMT_EXPR;
    vec_push(&s->blocks, b);
    s->cur = (uint32_t)s->blocks.len - 1;
    c->cv[scope_node] = s->cur + 1;
}

void cstmt_scope_open(Checker *c, uint32_t i)
{
    CStmt *s;
    if (c->quiet)
        return;
    s = st(c);
    if (c->nodes[i].flags & NF_PARAMS) {
        FuncState f;
        f.label_base = (uint32_t)s->labels.len;
        f.block_base = (uint32_t)s->blocks.len;
        f.unsafe_base = (uint32_t)s->unsafe.len;
        f.goto_base = (uint32_t)s->gotos.len;
        f.sw_base = (uint32_t)s->sw.len;
        f.case_base = (uint32_t)s->cases.len;
        f.ndeclared = 0;
        f.rv = f.rnull = f.abn = false;
        f.sym = SYM_NONE;
        f.seq = 0;
        f.fblock = NOB;
        vec_push(&s->fs, f);
        new_block(c, s, i);
        top(s)->fblock = s->cur;
        return;
    }
    if (s->fs.len)
        new_block(c, s, i);
}

/* ---- switch --------------------------------------------------------------------- */

static int cmp_val(uint64_t a, bool asg, uint64_t b, bool bsg)
{
    if (asg == bsg) {
        if (asg)
            return (int64_t)a < (int64_t)b ? -1 : (int64_t)a > (int64_t)b;
        return a < b ? -1 : a > b;
    }
    if (asg) {
        if ((int64_t)a < 0)
            return -1;
        return a < b ? -1 : a > b;
    }
    if ((int64_t)b < 0)
        return 1;
    return a < b ? -1 : a > b;
}

static int cmp_key(const CSwitch *sw, bool sgn, uint64_t a, uint64_t b)
{
    (void)sw;
    return cmp_val(a, sgn, b, sgn);
}

/* The signed type of the width of the unsigned type t. */
static TypeId signed_of(Checker *c, TypeId t)
{
    switch (type_ckind(TT, t)) {
    case TY_UINT: return TYPE_B(INT);
    case TY_ULONG: return TYPE_B(LONG);
    case TY_ULLONG: return TYPE_B(LLONG);
    case TY_UINT128: return TYPE_B(INT128);
    default: return t;
    }
}

static void fmt_val(char *buf, size_t n, uint64_t v, bool sg)
{
    if (sg)
        snprintf(buf, n, "%lld", (long long)(int64_t)v);
    else
        snprintf(buf, n, "%llu", (unsigned long long)v);
}

/* convert_and_check for an integer constant: value v of type S converted
 * to T (in a case label). */
static void conv_check(Checker *c, SrcLoc loc, TypeId T, TypeId S,
                       uint64_t v, uint64_t *out)
{
    bool ssg = type_is_signed(TT, S), tsg = type_is_signed(TT, T);
    char a[32], b[32];
    *out = cexpr_trunc(c, T, v);
    if (type_int_bits(TT, T) > 64 || type_int_bits(TT, S) > 64 ||
        cexpr_fits(c, v, S, T))
        return;
    fmt_val(a, sizeof a, v, ssg);
    fmt_val(b, sizeof b, *out, tsg);
    if (tsg) {
        bool warn = !cexpr_fits(c, v, S, type_to_unsigned(TT, T));
        if (!warn && c->opt.pedantic &&
            type_int_bits(TT, S) != type_int_bits(TT, T))
            warn = true;
        if (warn) {
            const char *s1 = type_q(TT, S);
            char sb1[96];
            snprintf(sb1, sizeof sb1, "%s", s1);
            cwarn(c, loc, "overflow", "overflow in conversion from %s to %s "
                  "changes value from '%s' to '%s'", sb1, type_q(TT, T), a, b);
        }
    } else if (!cexpr_fits(c, v, S, signed_of(c, T))) {
        char sb1[96];
        snprintf(sb1, sizeof sb1, "%s", type_q(TT, S));
        cwarn(c, loc, "overflow", "%sconversion from %s to %s changes value "
              "from '%s' to '%s'", ssg ? "unsigned " : "", sb1,
              type_q(TT, T), a, b);
    }
}

static bool starts_typename_cast(Checker *c, uint32_t e)
{
    uint32_t t = first_tok(c, e);
    const Tok *tk = &c->u->toks[t].t;
    uint32_t f = cfirst(c, e);
    return tk->kind == TK_PUNCT && tk->punct == P_LPAREN &&
           (tg(c, f) == N_SPECS || tg(c, f) == N_TYPESPEC ||
            tg(c, f) == N_TYPE_NAME);
}

/* truth_value_p of the expression's tree code. */
static bool is_truth_value(Checker *c, uint32_t e)
{
    for (;;) {
        e = strip_paren(c, e);
        if (tg(c, e) == N_BINARY && c->u->toks[c->nodes[e].tok].t.kind ==
                                        TK_PUNCT &&
            c->u->toks[c->nodes[e].tok].t.punct == P_COMMA) {
            e = e - 1;             /* the right operand is the last child */
            continue;
        }
        break;
    }
    if (c->ck[e] == K_ICE || c->ck[e] == K_FOLD)
        return false;
    if (tg(c, e) == N_BINARY) {
        switch (c->u->toks[c->nodes[e].tok].t.punct) {
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR:
            return true;
        default:
            return false;
        }
    }
    if (tg(c, e) == N_UNARY &&
        c->u->toks[c->nodes[e].tok].t.punct == P_BANG)
        return true;
    return false;
}

static void start_switch(Checker *c, CStmt *s, uint32_t sw_node, uint32_t e)
{
    CSwitch sw;
    TypeId t;
    memset(&sw, 0, sizeof sw);
    sw.node = sw_node;
    sw.block = s->cur;
    sw.loc = cnode_loc(c, sw_node);
    sw.cond = e;
    sw.cbase = (uint32_t)s->cases.len;
    sw.ty = TYPE_B(INT);
    if (node_err(c, e) || !cexpr_rvalue_ok(c, e)) {
        sw.cond_err = true;
        sw.orig = ERRT;
    } else {
        t = cexpr_rvalue_type(c, e);
        if (!type_is_integer(TT, t)) {
            cerror(c, first_loc(c, e), "switch quantity not an integer");
            sw.orig = ERRT;
        } else {
            uint32_t p = strip_paren(c, e);
            TypeId ct = type_canon(TT, t);
            bool cast_p = starts_typename_cast(c, e);
            sw.orig = t;
            if ((type_ckind(TT, ct) == TY_LONG || type_ckind(TT, ct) == TY_ULONG) &&
                !cin_system(c, first_loc(c, e)))
                cwarn(c, first_loc(c, e), "traditional", "'long' switch "
                      "expression not converted to 'int' in ISO C");
            if (tg(c, p) == N_CHAR &&
                type_ckind(TT, c->ty[p]) == TY_INT)
                sw.orig = TYPE_B(CHAR);   /* a narrow character constant */
            if (type_ckind(TT, t) == TY_BOOL ||
                (is_truth_value(c, e) &&
                 !(type_ckind(TT, ct) == TY_INT && cast_p)))
                sw.bool_cond = true;
            /* an explicit cast to int: INTEGER_TYPE, not boolean */
            if (sw.bool_cond && cast_p && type_ckind(TT, ct) == TY_INT &&
                type_ckind(TT, t) != TY_BOOL)
                sw.bool_cond = false;
            sw.ty = (c->ef[e] & EF_BFPROMOTE) ? TYPE_B(INT)
                                              : type_int_promote(TT, t);
        }
    }
    vec_push(&s->sw, sw);
}

/* c_check_switch_jump_warnings; true if there was an error. */
static bool switch_jump_checks(Checker *c, CStmt *s, const CSwitch *sw,
                               SrcLoc case_loc)
{
    FuncState *f = top(s);
    bool saw_error = false;
    size_t k;
    uint32_t x;
    s->tmp.len = 0;
    for (k = f->unsafe_base; k < s->unsafe.len; k++) {
        const CUnsafe *u = &s->unsafe.data[k];
        if (encl(s, u->block, s->cur) && !encl(s, u->block, sw->block))
            vec_push(&s->tmp, (uint32_t)k);
    }
    if (s->tmp.len) {
        sort_unsafe(s, false);
        for (k = 0; k < s->tmp.len; k++) {
            const CUnsafe *u = &s->unsafe.data[s->tmp.data[k]];
            Diagnostic *d;
            if (u->vm) {
                saw_error = true;
                d = cerror_d(c, case_loc, "switch jumps into scope of "
                             "identifier with variably modified type");
            } else
                d = cwarn_d(c, DL_WARNING, case_loc, "jump-misses-init",
                            "switch jumps over variable initialization");
            if (d) {
                cnote(c, d, sw->loc, "switch starts here");
                cnote(c, d, u->loc, "'%s' declared here",
                      cident(c, u->name));
            }
        }
    }
    for (x = s->cur; x != NOB && !encl(s, x, sw->block);
         x = s->blocks.data[x].parent)
        if (s->blocks.data[x].stmtexpr) {
            Diagnostic *d = cerror_d(c, case_loc, "switch jumps into "
                                     "statement expression");
            cnote(c, d, sw->loc, "switch starts here");
            saw_error = true;
            break;
        }
    return saw_error;
}

/* The value of a case label expression (check_case_value and
 * convert_and_check); false if there is none. */
static bool case_value(Checker *c, SrcLoc loc, const CSwitch *sw, uint32_t e,
                       uint64_t *out)
{
    TypeId rt, pt;
    if (node_err(c, e))
        return false;
    rt = cexpr_rvalue_type(c, e);
    if (type_ckind(TT, rt) == TY_PTR) {
        cerror(c, loc, "pointers are not permitted as case values");
        return false;
    }
    if (c->ck[e] != K_ICE && c->ck[e] != K_FOLD) {
        cerror(c, loc, "case label does not reduce to an integer constant");
        return false;
    }
    if (!type_is_integer(TT, c->ty[e])) {
        cerror(c, loc, "case label does not reduce to an integer constant");
        return false;
    }
    if (c->ef[e] & EF_OVERFLOW)
        cconst_overflow(c, loc);
    pt = type_int_promote(TT, c->ty[e]);
    conv_check(c, loc, sw->ty, pt, cexpr_trunc(c, pt, c->cv[e]), out);
    return true;
}

/* The first case with lo >= key; n if none. */
static size_t case_lower_bound(const CStmt *s, const CSwitch *sw, bool sgn,
                               uint64_t key)
{
    size_t lo = sw->cbase, hi = s->cases.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cmp_key(sw, sgn, s->cases.data[mid].lo, key) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* c_add_case_label; low/high are the expression nodes (NOB: none). */
static void add_case_label(Checker *c, CStmt *s, SrcLoc loc, uint32_t lowe,
                           uint32_t highe)
{
    CSwitch *sw = &s->sw.data[s->sw.len - 1];
    bool sgn = type_is_signed(TT, sw->ty);
    uint64_t lo = 0, hi = 0;
    bool have_hi = false, dup = false;
    size_t pos, node = 0;
    CCase cs;
    if (sw->cond_err)
        return;
    if (lowe == NOB) {
        if (sw->has_default) {
            Diagnostic *d = cerror_d(c, loc, "multiple default labels in one "
                                     "switch");
            cnote(c, d, sw->def_loc, "this is the first default label");
            return;
        }
        sw->has_default = true;
        sw->def_loc = loc;
        return;
    }
    if (!case_value(c, loc, sw, lowe, &lo))
        return;
    if (highe != NOB) {
        if (!case_value(c, loc, sw, highe, &hi))
            return;
        have_hi = true;
        if (hi == lo)
            have_hi = false;
        else if (cmp_key(sw, sgn, lo, hi) >= 0)
            cwarn(c, loc, "", "empty range specified");
    }
    pos = case_lower_bound(s, sw, sgn, lo);
    if (pos < s->cases.len && s->cases.data[pos].lo == lo) {
        dup = true;
        node = pos;
    } else {
        if (pos > sw->cbase) {
            const CCase *pr = &s->cases.data[pos - 1];
            if (pr->range && cmp_key(sw, sgn, pr->hi, lo) >= 0) {
                dup = true;
                node = pos - 1;
            }
        }
        if (!dup && pos < s->cases.len && have_hi &&
            cmp_key(sw, sgn, s->cases.data[pos].lo, hi) <= 0) {
            dup = true;
            node = pos;
        }
    }
    if (dup) {
        Diagnostic *d;
        SrcLoc old = s->cases.data[node].loc;
        if (have_hi) {
            d = cerror_d(c, loc, "duplicate (or overlapping) case value");
            cnote(c, d, old, "this is the first entry overlapping that "
                  "value");
        } else {
            d = cerror_d(c, loc, "duplicate case value");
            cnote(c, d, old, "previously used here");
        }
        return;
    }
    cs.lo = lo;
    cs.hi = have_hi ? hi : lo;
    cs.loc = loc;
    cs.range = have_hi;
    vec_push(&s->cases, cs);
    sw = &s->sw.data[s->sw.len - 1];
    if (pos < s->cases.len - 1)
        memmove(&s->cases.data[pos + 1], &s->cases.data[pos],
                (s->cases.len - 1 - pos) * sizeof(CCase));
    s->cases.data[pos] = cs;
}

/* do_case: lowe / highe (NOB: none); node is the CASE/DEFAULT. */
static void do_case(Checker *c, CStmt *s, uint32_t node, uint32_t lowe,
                    uint32_t highe)
{
    SrcLoc loc = cnode_loc(c, node);
    if (lowe != NOB && !node_err(c, lowe) && c->ck[lowe] == K_FOLD)
        cpedantic(c, loc, "case label is not an integer constant expression");
    if (highe != NOB && !node_err(c, highe) && c->ck[highe] == K_FOLD)
        cpedantic(c, cinput_loc(c, last_tok(c, highe) + 1),
                  "case label is not an integer constant expression");
    if (!s->sw.len || s->sw.len <= top(s)->sw_base) {
        if (lowe != NOB)
            cerror(c, loc, "case label not within a switch statement");
        else
            cerror(c, loc, "'default' label not within a switch "
                   "statement");
        return;
    }
    if (switch_jump_checks(c, s, &s->sw.data[s->sw.len - 1], loc))
        return;
    add_case_label(c, s, loc, lowe, highe);
}

/* ---- enumerations ------------------------------------------------------------------ */

static void enum_collect(Checker *c, CStmt *s, TypeId et)
{
    CEnumSet *e = &s->en;
    TypeId ce = type_canon(TT, et);
    uint32_t cap = 0, k;
    if (e->ty == ce && e->n)
        return;
    enumset_free(s);
    e->ty = ce;
    for (k = 0; k < c->gsyms.len + c->lsyms.len; k++) {
        const CSym *sy = k < c->gsyms.len ? &c->gsyms.data[k]
                                          : &c->lsyms.data[k - c->gsyms.len];
        if (sy->kind != CS_ENUMCONST || type_canon(TT, sy->ty) != ce)
            continue;
        if (e->n == cap) {
            cap = cap ? cap * 2 : 8;
            e->name = xrealloc(e->name, cap * sizeof *e->name);
            e->val = xrealloc(e->val, cap * sizeof *e->val);
            e->unused = xrealloc(e->unused, cap);
        }
        e->name[e->n] = sy->name;
        e->val[e->n] = sy->val;
        e->unused[e->n] = (sy->flags & CSF_ATTR_UNUSED) != 0;
        e->n++;
    }
}

/* c_do_switch_warnings, at the end of the switch. */
static void finish_switch(Checker *c, CStmt *s)
{
    CSwitch sw = s->sw.data[s->sw.len - 1];
    bool sgn = type_is_signed(TT, sw.ty);
    size_t k, n = s->cases.len - sw.cbase;
    CCase *cs = s->cases.data + sw.cbase;
    int sw_enum = diag_option_state(c->diag, "switch");
    int sw_enum_all = diag_option_state(c->diag, "switch-enum");
    if (sw.orig != ERRT && n) {
        unsigned ob = type_int_bits(TT, sw.orig), tb = type_int_bits(TT, sw.ty);
        bool osg = type_is_signed(TT, sw.orig);
        if (ob <= 64 && tb <= 64 && (ob != tb || osg != sgn)) {
            uint64_t mn, mx;
            if (osg) {
                mx = ob >= 64 ? UINT64_MAX >> 1 : (UINT64_C(1) << (ob - 1)) - 1;
                mn = ~mx;
            } else {
                mn = 0;
                mx = ob >= 64 ? UINT64_MAX : (UINT64_C(1) << ob) - 1;
            }
            for (k = 0; k < n; k++) {
                bool lo_lt = cmp_val(cs[k].lo, sgn, mn, osg) < 0;
                bool lo_gt = cmp_val(cs[k].lo, sgn, mx, osg) > 0;
                bool hi_lt = cmp_val(cs[k].hi, sgn, mn, osg) < 0;
                bool hi_gt = cmp_val(cs[k].hi, sgn, mx, osg) > 0;
                if (lo_lt) {
                    if (!cs[k].range || hi_lt)
                        cwarn(c, cs[k].loc, "switch-outside-range", "case "
                              "label value is less than minimum value for "
                              "type");
                    else
                        cwarn(c, cs[k].loc, "switch-outside-range", "lower "
                              "value in case label range less than minimum "
                              "value for type");
                }
                if (hi_gt) {
                    if (!cs[k].range || lo_gt)
                        cwarn(c, cs[k].loc, "switch-outside-range", "case "
                              "label value exceeds maximum value for type");
                    else
                        cwarn(c, cs[k].loc, "switch-outside-range", "upper "
                              "value in case label range exceeds maximum "
                              "value for type");
                }
            }
        }
    }
    if (!sw.has_default)
        cwarn(c, sw.loc, "switch-default", "switch missing default case");
    bool covers_bool = false;
    if (sw.bool_cond && n && sw.has_default) {
        /* a default after labels for both 0 and 1 can never be taken */
        bool z = false, o = false;
        uint32_t kb;
        for (kb = 0; kb < n; kb++) {
            z |= cmp_val(cs[kb].lo, sgn, 0, false) <= 0 &&
                 cmp_val(cs[kb].hi, sgn, 0, false) >= 0;
            o |= cmp_val(cs[kb].lo, sgn, 1, false) <= 0 &&
                 cmp_val(cs[kb].hi, sgn, 1, false) >= 0;
        }
        covers_bool = z && o;
    }
    if (sw.bool_cond && n &&
        ((sgn && (int64_t)cs[0].lo < 0) ||
         cmp_val(cs[n - 1].hi, sgn, 1, false) > 0 || covers_bool))
        cwarn(c, sw.loc, "switch-bool", "switch condition has boolean value");
    if (sw.orig != ERRT && type_ckind(TT, sw.orig) == TY_ENUM &&
        (sw_enum > 0 || sw_enum_all > 0)) {
        CEnumSet *e;
        uint32_t q;
        enum_collect(c, s, sw.orig);
        e = &s->en;
        for (q = 0; q < e->n; q++) {
            uint64_t v = cexpr_trunc(c, sw.ty, e->val[q]);
            bool found = false;
            for (k = 0; k < n && !found; k++)
                found = cmp_key(&sw, sgn, cs[k].lo, v) <= 0 &&
                        cmp_key(&sw, sgn, v, cs[k].hi) <= 0;
            if (found || e->unused[q])      /* PR c++/105497 */
                continue;
            if (!sw.has_default && sw_enum > 0)
                cwarn(c, sw.loc, "switch", "enumeration value '%s' not "
                      "handled in switch", cident(c, e->name[q]));
            else if (sw_enum_all > 0)
                cwarn(c, sw.loc, "switch-enum", "enumeration value '%s' not "
                      "handled in switch", cident(c, e->name[q]));
        }
        if (sw_enum > 0 || sw_enum_all > 0)
            for (k = 0; k < n; k++) {
                int pass;
                for (pass = 0; pass < (cs[k].range ? 2 : 1); pass++) {
                    uint64_t v = pass ? cs[k].hi : cs[k].lo;
                    bool found = false;
                    char buf[32];
                    for (q = 0; q < e->n && !found; q++)
                        found = cexpr_trunc(c, sw.ty, e->val[q]) == v;
                    if (found)
                        continue;
                    fmt_val(buf, sizeof buf, v, sgn);
                    if (tg(c, strip_paren(c, sw.cond)) == N_CAST)
                        /* a cast has no original type to name */
                        cwarn(c, cs[k].loc, sw_enum > 0 ? "switch"
                              : "switch-enum", "case value '%s' not in "
                              "enumerated type", buf);
                    else
                        cwarn(c, cs[k].loc, sw_enum > 0 ? "switch"
                              : "switch-enum", "case value '%s' not in "
                              "enumerated type %s", buf, type_q(TT, sw.orig));
                }
            }
    }
    s->cases.len = sw.cbase;
    s->sw.len--;
}

/* ---- conditions ---------------------------------------------------------------------- */

/* c_objc_common_truthvalue_conversion's errors and the -Wparentheses
 * warning of c_common_truthvalue_conversion. */
/* Is there an unparenthesized assignment the truth-value conversion sees
 * (through the arms of ?:)? */
static bool assign_truth(Checker *c, uint32_t e)
{
    uint32_t kids[4], n;
    switch (tg(c, e)) {
    case N_ASSIGN:
        return c->u->toks[c->nodes[e].tok].t.punct == P_ASSIGN;
    case N_COND:
        n = node_children(c->nodes, e, kids, 4);
        if (n == 3)
            return assign_truth(c, kids[1]) || assign_truth(c, kids[2]);
        return n == 2 && assign_truth(c, kids[1]);
    default:
        return false;
    }
}

static void cond_check(Checker *c, uint32_t e)
{
    TypeId t;
    if (node_err(c, e) || !cexpr_rvalue_ok(c, e))
        return;
    t = cexpr_rvalue_type(c, e);
    switch (type_ckind(TT, t)) {
    case TY_STRUCT:
        cerror(c, first_loc(c, e), "used struct type value where scalar is "
               "required");
        return;
    case TY_UNION:
        cerror(c, first_loc(c, e), "used union type value where scalar is "
               "required");
        return;
    case TY_VECTOR:
        cerror(c, first_loc(c, e), "used vector type where scalar is "
               "required");
        return;
    case TY_VOID:
        cerror(c, first_loc(c, e), "void value not ignored as it ought to "
               "be");
        return;
    default:
        break;
    }
    if (assign_truth(c, e))
        cwarn(c, first_loc(c, e), "parentheses", "suggest parentheses around "
              "assignment used as truth value");    cexpr_truth_warn(c, e, first_loc(c, e));
}

static uint32_t child_index(Checker *c, uint32_t p, uint32_t i, uint32_t *n)
{
    uint32_t kids[32], cnt = node_children(c->nodes, p, kids, 32), k;
    *n = cnt;
    for (k = 0; k < cnt; k++)
        if (kids[k] == i)
            return k;
    return NOB;
}


/* gcc's built-in functions that do not return (also undeclared, in their
 * library spelling). */
static bool builtin_noreturn(const char *n)
{
    static const char *const x[] = {"abort", "exit", "_exit", "_Exit",
                                    "quick_exit", "longjmp", "unreachable",
                                    "trap"};
    size_t k;
    if (!strncmp(n, "__builtin_", 10)) {
        n += 10;
        if (!strcmp(n, "unwind_resume") || !strcmp(n, "eh_return") ||
            !strcmp(n, "__unreachable"))
            return true;
    } else if (!strcmp(n, "unreachable") || !strcmp(n, "trap"))
        return false;
    for (k = 0; k < sizeof x / sizeof *x; k++)
        if (!strcmp(n, x[k]))
            return true;
    return false;
}

/* After the expression node i was checked. */
void cstmt_expr(Checker *c, uint32_t i)
{
    CStmt *s = c->stmt;
    uint32_t p;
    if (c->quiet || !s || !s->fs.len)
        return;
    if (tg(c, i) == N_CALL) {
        uint32_t kids[4], f;
        node_children(c->nodes, i, kids, 4);
        f = strip_paren(c, kids[0]);
        if (tg(c, f) == N_IDENT && cnode_ident(c, f)) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, f));
            if (ref != SYM_NONE && (csym(c, ref)->flags & CSF_NORETURN))
                top(s)->abn = true;
            else if ((ref == SYM_NONE || (csym(c, ref)->flags & CSF_IMPLICIT)) &&
                     builtin_noreturn(cident(c, cnode_ident(c, f))))
                top(s)->abn = true;
        }
    }
    if (tg(c, i) == N_ADDR_LABEL) {
        uint32_t tok = c->nodes[i].tok;
        if (c->func_sym != SYM_NONE && !c->kr_decls && cnode_ident(c, i))
            use_label(c, s, cnode_ident(c, i), ctok_loc(c, tok - 1),
                      cinput_loc(c, tok), i);
        p = c->par[i];     /* an if/while/do condition is still a truth value */
        if (p != NOB && (tg(c, p) == N_IF || tg(c, p) == N_WHILE ||
                         tg(c, p) == N_DO))
            cond_check(c, i);
        return;
    }
    p = c->par[i];
    if (p == NOB)
        return;
    switch (tg(c, p)) {
    case N_IF: case N_WHILE: case N_DO:
        cond_check(c, i);
        break;
    case N_FOR: {
        uint32_t n, k = child_index(c, p, i, &n);
        if (k == 2)
            cond_check(c, i);
        else if (k == 1 || k == 3)      /* the increment has its own location */
            unused_value(c, i, k == 3 ? first_loc(c, i) : cnode_loc(c, p));
        break;
    }
    case N_SWITCH:
        start_switch(c, s, p, i);
        break;
    case N_EXPR_STMT:
        if (!node_err(c, i))
            cexpr_rvalue_ok(c, i);
        break;
    case N_CASE: {
        bool range = c->nodes[p].flags & NF_RANGE;
        bool first = cfirst(c, i) == cfirst(c, p);
        if (first && range)
            cpedantic(c, cnode_loc(c, p), "range expressions in switch "
                      "statements are non-standard");
        else if (first)
            do_case(c, s, p, i, NOB);
        else if (range) {
            uint32_t kids[4], lo;
            node_children(c->nodes, p, kids, 4);
            lo = kids[0];
            do_case(c, s, p, lo, i);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- the enter hook: labels and default ---------------------------------------- */

/* Called before visiting node i: a LABEL or DEFAULT whose subtree starts at
 * i is processed now, outermost first. */
void cstmt_enter(Checker *c, uint32_t i)
{
    CStmt *s = c->stmt;
    uint32_t a;
    size_t k;
    if (c->quiet || !s || !s->fs.len)
        return;
    s->tmp.len = 0;
    for (a = i; a != NOB && cfirst(c, a) == i; a = c->par[a])
        if (tg(c, a) == N_LABEL || tg(c, a) == N_DEFAULT)
            vec_push(&s->tmp, a);
    if (!s->tmp.len)
        return;
    {
        size_t n = s->tmp.len;
        uint32_t *nodes = xmalloc(n * sizeof *nodes);
        memcpy(nodes, s->tmp.data, n * sizeof *nodes);
        for (k = n; k-- > 0;) {
            c->cur_node = nodes[k];
            if (tg(c, nodes[k]) == N_LABEL)
                define_label(c, s, nodes[k], i);
            else
                do_case(c, s, nodes[k], NOB, NOB);
        }
        c->cur_node = i;
        free(nodes);
    }
}

/* ---- statements -------------------------------------------------------------------------- */

static bool has_bare_ifelse(Checker *c, uint32_t stmt)
{
    for (;;) {
        uint32_t kids[32], n;
        switch (tg(c, stmt)) {
        case N_IF:
            n = node_children(c->nodes, stmt, kids, 32);
            return n >= 9;
        case N_WHILE: case N_FOR: case N_SWITCH:
            n = node_children(c->nodes, stmt, kids, 32);
            if (n < 3)
                return false;
            stmt = kids[n - 3];
            continue;
        case N_LABEL: case N_DEFAULT: case N_CASE:
            n = node_children(c->nodes, stmt, kids, 32);
            if (!n || kids[n - 1] + 1 != stmt ||
                cexpr_is_expr(tg(c, kids[n - 1])) ||
                tg(c, kids[n - 1]) == N_ATTRIBUTE)
                return false;
            stmt = kids[n - 1];
            continue;
        default:
            return false;
        }
    }
}

/* The declared-here note of a return statement's diagnostics. */
static SrcLoc func_loc(Checker *c)
{
    const CSym *f = csym(c, c->func_sym);
    return f->def_loc ? f->def_loc : f->loc;
}

/* pedwarn (loc, warn_return_type >= 0 ? OPT_Wreturn_type : 0, ...) */
static Diagnostic *return_pedwarn(Checker *c, SrcLoc loc, const char *msg)
{
    int st = diag_option_state(c->diag, "return-type");
    if (st == 0)
        return NULL;
    return cpedwarn(c, loc, st > 0 ? "return-type" : "", "%s", msg);
}

static bool is_local_var(Checker *c, uint32_t id)
{
    uint32_t ref = lookup_ord(c, id);
    const CSym *sy;
    if (ref == SYM_NONE)
        return false;
    sy = csym(c, ref);
    return sy->kind == CS_OBJ && sy->linkage == LK_NONE &&
           sy->sc != SC_STATIC && sy->sc != SC_EXTERN &&
           !(sy->flags & CSF_THREAD);
}

/* Is the lvalue chain of e (member and array element accesses) rooted in
 * a local variable? */
static bool lvalue_root_local(Checker *c, uint32_t e)
{
    for (;;) {
        e = strip_paren(c, e);
        switch (tg(c, e)) {
        case N_IDENT:
            return is_local_var(c, cnode_ident(c, e));
        case N_MEMBER_EXPR:
            if (c->nodes[e].flags & NF_ARROW)
                return false;
            e = e - 1;
            continue;
        case N_INDEX: {
            uint32_t kids[4];
            node_children(c->nodes, e, kids, 4);
            if (type_ckind(TT, c->ty[kids[0]]) != TY_ARRAY &&
                type_ckind(TT, c->ty[kids[0]]) != TY_VLA)
                return false;
            e = kids[0];
            continue;
        }
        default:
            return false;
        }
    }
}

bool is_array_ty(Checker *c, TypeId t)
{
    TypeKind k = type_ckind(TT, t);
    return k == TY_ARRAY || k == TY_VLA;
}

/* The -Wreturn-local-addr peel of c_finish_return. */
static void return_local_addr(Checker *c, uint32_t e, SrcLoc loc)
{
    bool top = true, istop;
    for (;;) {
        uint32_t kids[4];
        e = strip_paren(c, e);
        istop = top;
        top = false;
        switch (tg(c, e)) {
        case N_CAST:
            node_children(c->nodes, e, kids, 4);
            e = kids[1];
            continue;
        case N_BINARY: {
            unsigned op = c->u->toks[c->nodes[e].tok].t.punct;
            uint32_t l, r;
            if (op != P_PLUS && op != P_MINUS)
                return;
            node_children(c->nodes, e, kids, 4);
            l = kids[0];
            r = kids[1];
            if (op == P_MINUS) {
                uint32_t q = strip_paren(c, r);
                while (tg(c, q) == N_CAST &&
                       type_ckind(TT, cexpr_rvalue_type(c, q)) != TY_PTR) {
                    uint32_t k2[4];
                    node_children(c->nodes, q, k2, 4);
                    q = strip_paren(c, k2[1]);
                }
                if (type_ckind(TT, cexpr_rvalue_type(c, q)) == TY_PTR)
                    return;
                e = l;
            } else if (type_ckind(TT, cexpr_rvalue_type(c, l)) == TY_PTR ||
                       is_array_ty(c, c->ty[l]))
                e = l;
            else if (type_ckind(TT, cexpr_rvalue_type(c, r)) == TY_PTR ||
                     is_array_ty(c, c->ty[r]))
                e = r;
            else if (c->ck[l] == K_ICE || c->ck[l] == K_FOLD)
                e = r;
            else
                e = l;
            continue;
        }
        case N_UNARY:
            if (c->u->toks[c->nodes[e].tok].t.punct != P_AMP)
                return;
            if (lvalue_root_local(c, e - 1))
                cwarn(c, loc, "return-local-addr", "function returns address "
                      "of local variable");
            return;
        case N_ADDR_LABEL:
            cwarn(c, loc, "return-local-addr", "function returns address of "
                  "label");
            return;
        case N_IDENT: case N_MEMBER_EXPR: case N_INDEX:
            /* an array decays to the address of its first element */
            if (is_array_ty(c, c->ty[e]) && lvalue_root_local(c, e))
                cwarn(c, istop ? first_loc(c, e) : loc, "return-local-addr", "function returns address "
                      "of local variable");
            return;
        default:
            return;
        }
    }
}

void stmt_return(Checker *c, uint32_t i)
{
    TypeId fty, valtype;
    uint32_t e = c->nodes[i].size > 1 ? i - 1 : NOB;
    SrcLoc kloc = cnode_loc(c, i), loc;
    const CSym *f;
    bool is_void;
    if (c->func_sym == SYM_NONE)
        return;
    f = csym(c, c->func_sym);
    fty = type_canon(TT, f->ty);
    if (type_ckind(TT, fty) != TY_FUNC)
        return;
    valtype = type_base(TT, fty);
    is_void = type_ckind(TT, valtype) == TY_VOID;
    loc = e != NOB ? expr_loc(c, e) : kloc;
    if (st(c)->fs.len) {
        if (e == NOB)
            top(st(c))->rnull = true;
        else
            top(st(c))->rv = true;
    }
    if (f->flags & CSF_NORETURN)
        cwarn(c, loc, "", "function declared 'noreturn' has a 'return' "
              "statement");
    if (e == NOB) {
        if (!is_void && valtype != ERRT) {
            Diagnostic *d = return_pedwarn(c, loc, "'return' with no value, "
                                           "in function returning non-void");
            cnote(c, d, func_loc(c), "declared here");
        }
        return;
    }
    if (is_void) {
        Diagnostic *d;
        if (node_err(c, e))
            return;
        if (type_ckind(TT, c->ty[e]) != TY_VOID)
            d = return_pedwarn(c, exp_if_system(c, loc, e), "'return' with a value, in function "
                               "returning void");
        else
            d = cpedantic(c, loc, "ISO C forbids 'return' with expression, "
                          "in function returning void");
        cnote(c, d, func_loc(c), "declared here");
        return;
    }
    {
        ConvInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.context = CONV_RETURN;
        ci.loc = loc;
        if (node_err(c, e) || (valtype == ERRT))
            return;
        /* messages name the return type as declared (typedefs kept) */
        if (!cexpr_assign_check(c, e,
                type_ent(TT, f->ty)->kind == TY_FUNC ? type_base(TT, f->ty)
                                                     : valtype, &ci))
            return;
        if (type_ckind(TT, valtype) == TY_PTR)
            return_local_addr(c, e, loc);
    }
}

/* break and continue. */
void stmt_bc(Checker *c, uint32_t i, bool is_break)
{
    uint32_t p;
    bool loop = false, sw = false;
    for (p = c->par[i]; p != NOB && tg(c, p) != N_FUNC_DEF; p = c->par[p]) {
        unsigned t = tg(c, p);
        if (t == N_WHILE || t == N_DO || t == N_FOR)
            loop = true;
        else if (t == N_SWITCH)
            sw = true;
    }
    if (is_break && !loop && !sw)
        cerror(c, cnode_loc(c, i), "break statement not within loop or "
               "switch");
    else if (!is_break && !loop)
        cerror(c, cnode_loc(c, i), "continue statement not within a loop");
}

/* check_for_loop_decls, after the initial declaration of a for. */
void for_loop_decls(Checker *c, uint32_t fornode)
{
    SrcLoc floc = cnode_loc(c, fornode);
    uint32_t first = c->scopes.data[c->scopes.len - 1].log, k;
    for (k = (uint32_t)c->log.len; k-- > first;) {
        const Bind *b = &c->log.data[k];
        if (b->ns == NS_ORD) {
            const CSym *sy = csym(c, b->ref);
            if (!sy->name)
                continue;
            if (sy->kind == CS_OBJ) {
                if (sy->sc == SC_STATIC)
                    cerror(c, sy->loc, "declaration of static variable '%s' "
                           "in 'for' loop initial declaration",
                           cident(c, sy->name));
                else if (sy->sc == SC_EXTERN)
                    cerror(c, sy->loc, "declaration of 'extern' variable '%s' "
                           "in 'for' loop initial declaration",
                           cident(c, sy->name));
            } else
                cerror(c, floc, "declaration of non-variable '%s' in 'for' "
                       "loop initial declaration", cident(c, sy->name));
        } else {
            switch (type_ckind(TT, b->ref)) {
            case TY_STRUCT:
                cerror(c, floc, "'struct %s' declared in 'for' loop initial "
                       "declaration", cident(c, b->ident));
                break;
            case TY_UNION:
                cerror(c, floc, "'union %s' declared in 'for' loop initial "
                       "declaration", cident(c, b->ident));
                break;
            case TY_ENUM:
                cerror(c, floc, "'enum %s' declared in 'for' loop initial "
                       "declaration", cident(c, b->ident));
                break;
            default:
                break;
            }
        }
    }
}

bool is_attr_kid(Checker *c, uint32_t k)
{
    return tg(c, k) == N_ATTRIBUTE || tg(c, k) == N_ATTR_STMT;
}

/* "label at end of compound statement" for a label without a statement. */
void label_at_end(Checker *c, uint32_t i)
{
    uint32_t kids[32], n = node_children(c->nodes, i, kids, 32), k;
    SrcLoc loc;
    unsigned t = tg(c, i);
    if (t == N_LABEL) {
        for (k = 0; k < n; k++)
            if (!is_attr_kid(c, kids[k]))
                return;
        loc = cnode_loc(c, i);
    } else if (t == N_DEFAULT) {
        if (n)
            return;
        loc = cnode_loc(c, i);
    } else {
        uint32_t ne = c->nodes[i].flags & NF_RANGE ? 2 : 1;
        if (n > ne)
            return;
        loc = first_loc(c, kids[0]);
    }
    cpedantic(c, loc, "label at end of compound statement");
}

void label_attrs(Checker *c, CStmt *s, uint32_t i)
{
    uint32_t kids[32], n = node_children(c->nodes, i, kids, 32), k, name;
    uint32_t idx;
    bool unused = false;
    for (k = 0; k < n; k++) {
        uint32_t a, j;
        if (tg(c, kids[k]) != N_ATTRIBUTE)
            continue;
        for (a = cfirst(c, kids[k]); a < kids[k]; a++)
            if (tg(c, a) == N_ATTR_ITEM) {
                const Tok *t = cnode_tok(c, a);
                const char *nm;
                if (t->kind != TK_IDENT)
                    continue;
                nm = cident(c, t->aux);
                j = 0;
                if (!strcmp(nm, "unused") || !strcmp(nm, "__unused__"))
                    unused = true;
                (void)j;
            }
    }
    name = cnode_ident(c, i);
    idx = lmap_get(s, name);
    if (unused && idx && s->labels.data[idx - 1].fn == s->fs.len)
        s->labels.data[idx - 1].used = true;
}

void local_labels(Checker *c, CStmt *s, uint32_t i)
{
    uint32_t kids[64], n = node_children(c->nodes, i, kids, 64), k;
    for (k = 0; k < n; k++) {
        uint32_t name = cnode_ident(c, kids[k]);
        uint32_t idx = lmap_get(s, name);
        SrcLoc loc;
        if (!name)
            continue;
        loc = cinput_loc(c, c->nodes[kids[k]].tok + 1);
        if (idx && !s->labels.data[idx - 1].popped &&
            s->labels.data[idx - 1].block == s->cur) {
            const CLabel *l = &s->labels.data[idx - 1];
            Diagnostic *d = cerror_d(c, loc, "duplicate label declaration "
                                     "'%s'", cident(c, name));
            cnote(c, d, l->loc, "previous %s of '%s' with type 'void'",
                  l->defined ? "definition" : "declaration", cident(c, name));
            continue;
        }
        idx = new_label(c, s, name, loc, kids[k], false, true, s->cur);
        if (c->sx)
            csx_label(c, idx, c->nodes[kids[k]].tok, CIX_DECL);
    }
}

/* A struct/union defined in a block with variably modified members: gcc
 * declares the tag (at its name, else the keyword), then one nameless
 * declaration per member whose type is a pointer to a variably modified
 * type (at the member). */
void stmt_struct_defined(Checker *c, CStmt *s, uint32_t i)
{
    TypeId t = c->ty[i];
    const Record *r;
    CUnsafe u;
    uint32_t tag, k;
    TypeKind tk = type_ckind(TT, t);
    if ((tk != TY_STRUCT && tk != TY_UNION) || !(c->nodes[i].flags & NF_BODY))
        return;
    r = type_record(TT, t);
    if (!(r->flags & RF_VMOD))
        return;
    memset(&u, 0, sizeof u);
    tag = NO_NODE;
    for (k = cfirst(c, i); k < i; k++)
        if (tg(c, k) == N_TAG && c->par[k] == i) {
            tag = k;
            break;
        }
    u.block = s->cur;
    u.loc = ctok_loc(c, tag != NO_NODE ? c->nodes[tag].tok : c->nodes[i].tok);
    u.vm = true;
    u.anon = true;
    u.seq = ++top(s)->seq;
    vec_push(&s->unsafe, u);
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        TypeId ft = type_canon(TT, f->ty);
        for (;;) {
            TypeKind fk = type_ckind(TT, ft);
            if (fk == TY_ARRAY || fk == TY_VLA) {
                ft = type_canon(TT, type_base(TT, ft));
            } else if (fk == TY_PTR && type_is_vm(TT, type_base(TT, ft)) &&
                       type_ckind(TT, type_base(TT, ft)) != TY_STRUCT &&
                       type_ckind(TT, type_base(TT, ft)) != TY_UNION) {
                u.loc = f->loc;
                u.seq = ++top(s)->seq;
                vec_push(&s->unsafe, u);
                ft = type_canon(TT, type_base(TT, ft));
            } else {
                break;
            }
        }
    }
}

void stmt_declared(Checker *c, CStmt *s, uint32_t i)
{
    uint32_t ref = c->cb[i];
    const CSym *sy;
    CUnsafe u;
    bool vm;
    if (!ref)
        return;
    sy = csym(c, ref - 1);
    if (!sy->name || (sy->kind != CS_OBJ && sy->kind != CS_TYPEDEF &&
                      sy->kind != CS_FUNC))
        return;
    vm = !(sy->ty == ERRT) && type_is_vm(TT, sy->ty);
    if (!vm && sy->kind == CS_FUNC)
        return;
    if (!vm) {
        uint32_t p = c->par[i];
        if (sy->kind != CS_OBJ || sy->sc == SC_STATIC || sy->sc == SC_EXTERN ||
            sy->linkage != LK_NONE || (sy->flags & CSF_PARAM) ||
            p == NOB || tg(c, p) != N_INIT_DECL || p == i + 1 ||
            diag_option_state(c->diag, "jump-misses-init") <= 0)
            return;
    }
    u.block = s->cur;
    u.loc = sy->loc;
    u.name = sy->name;
    u.vm = vm;
    u.anon = false;
    if (vm) {
        /* gcc also declares, before the object, one nameless declaration
         * for every pointer whose target is variably modified */
        TypeId t = sy->ty;
        for (;;) {
            TypeId b = type_canon(TT, t);
            TypeKind k = type_ckind(TT, b);
            if (sy->kind == CS_FUNC ||
                (sy->kind != CS_TYPEDEF && type_ent(TT, t)->kind == TY_TYPEDEF))
                break;
            if (k == TY_ARRAY || k == TY_VLA || k == TY_FUNC) {
                t = type_base(TT, t);
            } else if (k == TY_PTR && type_is_vm(TT, type_base(TT, b)) &&
                       type_ckind(TT, type_base(TT, b)) != TY_STRUCT &&
                       type_ckind(TT, type_base(TT, b)) != TY_UNION) {
                CUnsafe a = u;
                /* a typedef name's own declaration covers its pointers */
                if (type_ent(TT, type_base(TT, t))->kind == TY_TYPEDEF)
                    break;
                a.anon = true;
                a.seq = ++top(s)->seq;
                vec_push(&s->unsafe, a);
                t = type_base(TT, t);
            } else
                break;
        }
    }
    u.seq = ++top(s)->seq;
    vec_push(&s->unsafe, u);
}

/* IF, WHILE, ... finished. */
void stmt_if(Checker *c, uint32_t i)
{
    uint32_t kids[32], n = node_children(c->nodes, i, kids, 32);
    if (n == 6 && has_bare_ifelse(c, kids[3]))
        cwarn(c, ctok_loc(c, c->nodes[i].tok + 1), "dangling-else", "suggest "
              "explicit braces to avoid ambiguous 'else'");
}

/* Before scope_close for the SCOPE_END at i. */
void cstmt_scope_end(Checker *c, uint32_t i)
{
    CStmt *s = c->stmt;
    uint32_t p;
    if (c->quiet || !s || !s->fs.len)
        return;
    p = c->par[i];
    if (p != NOB && (tg(c, p) == N_IF || tg(c, p) == N_WHILE ||
                     tg(c, p) == N_FOR || tg(c, p) == N_SWITCH))
        misleading_scope_end(c, i, p);
    if (p != NOB && tg(c, p) == N_SWITCH && i + 1 == p && s->sw.len &&
        s->sw.len > top(s)->sw_base)
        finish_switch(c, s);
    if (p != NOB && tg(c, p) == N_FUNC_DEF && s->cur == top(s)->fblock)
        top(s)->sym = c->func_sym;
}

static bool is_main_name(Checker *c, const CSym *s)
{
    return s->name && !strcmp(cident(c, s->name), "main");
}

/* After scope_close. */
void cstmt_scope_end_post(Checker *c, uint32_t i)
{
    CStmt *s = c->stmt;
    FuncState *f;
    size_t k;
    (void)i;
    if (c->quiet || !s || !s->fs.len || s->cur == NOB)
        return;
    f = top(s);
    if (s->cur != f->fblock) {
        if (f->ndeclared)
            for (k = s->labels.len; k-- > f->label_base;) {
                CLabel *l = &s->labels.data[k];
                if (l->declared && !l->popped && l->block == s->cur) {
                    l->popped = true;
                    lmap_set(c, s, l->name, l->prev);
                }
            }
        s->cur = s->blocks.data[s->cur].parent;
        return;
    }
    /* no return statement in a static non-void function */
    if (c->par[i] != NOB && tg(c, c->par[i]) == N_FUNC_DEF &&
        f->sym != SYM_NONE && !f->rv && !f->rnull && !f->abn) {
        const CSym *fs = csym(c, f->sym);
        TypeId fty = type_canon(TT, fs->ty);
        uint32_t fd = c->par[i], kids[8], nk, q;
        bool implicit_int = true;
        nk = node_children(c->nodes, fd, kids, 8);
        if (nk > 0 && tg(c, kids[0]) == N_SPECS) {
            uint32_t sk[16], ns = node_children(c->nodes, kids[0], sk, 16);
            for (q = 0; q < ns; q++) {
                unsigned tq = tg(c, sk[q]);
                if (tq == N_TYPESPEC || tq == N_TYPEDEF_NAME ||
                    tq == N_STRUCT || tq == N_ENUM || tq == N_TYPEOF ||
                    tq == N_ATOMIC_TYPE)
                    implicit_int = false;
            }
        }
        if (type_ckind(TT, fty) == TY_FUNC &&
            type_ckind(TT, type_base(TT, fty)) != TY_VOID &&
            type_base(TT, fty) != ERRT && fs->linkage == LK_INTERNAL &&
            !implicit_int && !is_main_name(c, fs) &&
            !(fs->flags & CSF_NORETURN) &&
            !cdecl_aset_has(c, fs->aset, "naked", NULL))
            cwarn(c, cinput_loc(c, c->nodes[i].tok), "return-type", "no return statement in "
                  "function returning non-void");
    }
    /* the function ends: what the scope scan did not report, then drop its
     * state */
    for (k = s->labels.len; k-- > f->label_base;) {
        CLabel *l = &s->labels.data[k];
        if (!l->emitted)
            emit_label(c, l);
        if (!l->popped)
            lmap_set(c, s, l->name, l->prev);
    }
    s->cur = s->blocks.data[s->cur].parent;
    s->labels.len = f->label_base;
    s->blocks.len = f->block_base;
    s->unsafe.len = f->unsafe_base;
    s->gotos.len = f->goto_base;
    s->sw.len = f->sw_base;
    s->cases.len = f->case_base;
    s->fs.len--;
}
