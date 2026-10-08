/* cstmt_warn.c - statement-level warnings: -Wunused-value,
 * -Wmisleading-indentation and -Wduplicated-branches / -Wduplicated-cond
 * (split from cstmt.c; the walk and the per-unit state stay there). */
#include <ctype.h>
#include "c/cstmt_int.h"

#include <stdio.h>
#include <string.h>

/* ---- -Wunused-value (emit_side_effect_warnings) --------------------------------- */

static bool is_comma(const Checker *c, uint32_t e)
{
    return tg(c, e) == N_BINARY && c->u->toks[c->nodes[e].tok].t.punct == P_COMMA;
}

static bool void_ty(Checker *c, uint32_t e)
{
    return type_ckind(TT, c->ty[e]) == TY_VOID;
}

/* The location gcc gives an expression that might have none: a bare
 * declaration has no location, the caller's is used. */
static SrcLoc value_loc(Checker *c, uint32_t e, SrcLoc dloc)
{
    uint32_t kids[4];
    if (c->ef[e] & EF_GCCFOLD)
        return dloc;
    switch (tg(c, e)) {
    case N_IDENT: case N_NUMBER: case N_CHAR: case N_STRING:
        return dloc;
    case N_MEMBER_EXPR:
        if (is_array_ty(c, c->ty[e]))
            return first_loc(c, e);
        return expr_loc(c, e);
    case N_INDEX:
        if (is_array_ty(c, c->ty[e]))
            return first_loc(c, e);
        return expr_loc(c, e);
    case N_COND:
        if (node_children(c->nodes, e, kids, 4) == 3)
            return ctok_loc(c, last_tok(c, kids[1]) + 1);
        return cnode_loc(c, e);
    default:
        return expr_loc(c, e);
    }
}

/* A cast gcc builds as a NOP_EXPR (and warn_if_unused_value looks through). */
static bool nop_cast(Checker *c, uint32_t e)
{
    uint32_t kids[4], in;
    TypeId to = type_canon(TT, c->ty[e]), from;
    node_children(c->nodes, e, kids, 4);
    in = strip_paren(c, kids[1]);
    if (tg(c, in) == N_ASSIGN)
        return true;
    from = type_canon(TT, cexpr_rvalue_type(c, in));
    if (type_ckind(TT, to) == TY_PTR && type_ckind(TT, from) == TY_PTR)
        return true;
    if (type_is_integer(TT, to) && type_is_integer(TT, from))
        return type_int_bits(TT, to) >= type_int_bits(TT, from);
    return false;
}

/* Complex arithmetic whose operands differ in type: gcc converts one of them
 * through a SAVE_EXPR, so the result has side effects and the warning is
 * "value computed is not used" (PR c/97748).  A real / complex division is
 * expanded differently. */
static bool mixed_complex(Checker *c, uint32_t e)
{
    uint32_t kids[4];
    int op;
    TypeId l, r;
    if (tg(c, e) != N_BINARY || type_ckind(TT, c->ty[e]) != TY_COMPLEX)
        return false;
    op = c->u->toks[c->nodes[e].tok].t.punct;
    if (op != P_PLUS && op != P_MINUS && op != P_STAR && op != P_SLASH)
        return false;
    node_children(c->nodes, e, kids, 4);
    l = type_canon(TT, cexpr_rvalue_type(c, kids[0]));
    r = type_canon(TT, cexpr_rvalue_type(c, kids[1]));
    if (l == r)
        return false;
    if (type_ckind(TT, l) == TY_COMPLEX && type_ckind(TT, r) == TY_COMPLEX &&
        (tg(c, strip_paren(c, kids[0])) == N_NUMBER ||
         tg(c, strip_paren(c, kids[1])) == N_NUMBER))
        return false;                    /* a constant is converted by folding */
    return !(op == P_SLASH && type_ckind(TT, l) != TY_COMPLEX);
}

void unused_value(Checker *c, uint32_t e, SrcLoc dloc)
{
    uint32_t kids[4];
    e = strip_paren(c, e);
    if (node_err(c, e) || void_ty(c, e))
        return;
    if (tg(c, e) == N_COND && node_children(c->nodes, e, kids, 4) == 2)
        return;     /* a ?: b is a SAVE_EXPR with side effects for gcc */
    if (!(c->ef[e] & EF_SIDE)) {
        SrcLoc loc;
        if (is_comma(c, e)) {
            uint32_t r = e;
            while (is_comma(c, r)) {
                node_children(c->nodes, r, kids, 4);
                r = strip_paren(c, kids[1]);
            }
            loc = tg(c, r) == N_IDENT ? dloc : cnode_loc(c, e);
        } else
            loc = value_loc(c, e, dloc);
        cwarn(c, loc, "unused-value", mixed_complex(c, e) ?
              "value computed is not used" : "statement with no effect");
        return;
    }
    if (is_comma(c, e)) {
        uint32_t r = e;
        SrcLoc cl = cnode_loc(c, e);
        while (is_comma(c, r)) {
            cl = cnode_loc(c, r);
            node_children(c->nodes, r, kids, 4);
            r = strip_paren(c, kids[1]);
        }
        if (!(c->ef[r] & EF_SIDE) && !void_ty(c, r) && !node_err(c, r) &&
            tg(c, r) != N_CAST)
            cwarn(c, cl, "unused-value", "right-hand operand of comma "
                  "expression has no effect");
        return;
    }
    for (;;) {
        e = strip_paren(c, e);
        if (node_err(c, e) || void_ty(c, e))
            return;
        switch (tg(c, e)) {
        case N_ASSIGN: case N_POSTFIX: case N_CALL: case N_VA_ARG:
        case N_STMT_EXPR: case N_COND:
            return;
        case N_UNARY: {
            unsigned op = c->u->toks[c->nodes[e].tok].t.punct;
            if (op == P_INC || op == P_DEC)
                return;
            break;
        }
        case N_CAST:
            if (nop_cast(c, e)) {
                node_children(c->nodes, e, kids, 4);
                e = kids[1];
                continue;
            }
            break;
        case N_BINARY:
            if (is_comma(c, e)) {
                node_children(c->nodes, e, kids, 4);
                if (c->ck[strip_paren(c, kids[1])] != K_NONE)
                    return;     /* let people do '(foo (), 0)' */
                e = kids[1];
                continue;
            }
            if (c->u->toks[c->nodes[e].tok].t.punct == P_ANDAND ||
                c->u->toks[c->nodes[e].tok].t.punct == P_OROR) {
                uint32_t r;
                node_children(c->nodes, e, kids, 4);
                r = strip_paren(c, kids[1]);
                if (is_comma(c, r)) {   /* 'c && (foo (), 0)': no warning */
                    while (is_comma(c, r)) {
                        node_children(c->nodes, r, kids, 4);
                        r = strip_paren(c, kids[1]);
                    }
                    if (c->ck[r] != K_NONE)
                        return;
                }
            }
            break;
        default:
            break;
        }
        if (TYPE_QUALS(c->ty[e]) & TQ_VOLATILE)
            switch (tg(c, e)) {
            case N_IDENT: case N_MEMBER_EXPR: case N_INDEX: case N_UNARY:
                return;
            default:
                break;
            }
        cwarn(c, value_loc(c, e, dloc), "unused-value", "value computed is "
              "not used");
        return;
    }
}

static void stmt_empty(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i], kids[32], n, e = i;
    if (c->nodes[i].size != 1 || p == NOB)
        return;
    while (p != NOB && (tg(c, p) == N_LABEL || tg(c, p) == N_CASE ||
                        tg(c, p) == N_DEFAULT)) {  /* c_parser_if_body: labels first */
        i = p;
        p = c->par[p];
    }
    if (p == NOB)
        return;
    if (tg(c, p) == N_DO) {
        cwarn(c, cnode_loc(c, e), "empty-body", "suggest braces around empty "
              "body in 'do' statement");
        return;
    }
    if (tg(c, p) != N_IF)
        return;
    n = node_children(c->nodes, p, kids, 32);
    if (n == 6 && kids[3] == i)
        cwarn(c, cnode_loc(c, e), "empty-body", "suggest braces around empty "
              "body in an 'if' statement");
    else if (n == 9 && kids[6] == i)
        cwarn(c, cnode_loc(c, e), "empty-body", "suggest braces around empty "
              "body in an 'else' statement");
}

/* -Wdeclaration-after-statement (gcc's last_stmt in
 * c_parser_compound_statement_nostart): a declaration directly in a block
 * whose previous item is a statement; labels reset it. */
static void decl_after_stmt(Checker *c, uint32_t i)
{
    uint32_t par = c->par[i], prev;
    if (par == NOB || tg(c, par) != N_COMPOUND || cfirst(c, i) == 0)
        return;
    prev = cfirst(c, i) - 1;
    if (c->par[prev] != par)
        return;
    switch (tg(c, prev)) {
    case N_DECL: case N_BODY: case N_LOCAL_LABEL: case N_STATIC_ASSERT:
    case N_PRAGMA: case N_SCOPE:
        return;
    case N_LABEL: case N_CASE: case N_DEFAULT: {
        /* the label resets it; a statement after the label sets it again */
        uint32_t kids[32], n = node_children(c->nodes, prev, kids, 32), k;
        unsigned t = tg(c, prev);
        bool stmt = false;
        if (t == N_LABEL) {
            for (k = 0; k < n; k++)
                stmt |= !is_attr_kid(c, kids[k]);
        } else {
            stmt = n > (t == N_CASE && (c->nodes[prev].flags & NF_RANGE) ? 2u
                                                                         : t == N_CASE ? 1u : 0u);
        }
        if (!stmt)
            return;
        break;
    }
    default:
        break;
    }
    {
        uint32_t t = first_tok(c, i);
        SrcLoc l = ctok_loc(c, t);
        /* a system header's macro (bool): gcc reports at its expansion */
        if (c->u->toks[t].exp && cin_system(c, l))
            l = c->u->toks[t].exp;
        cc90(c, l, "declaration-after-statement",
             "ISO C90 forbids mixed declarations and code");
    }
}

/* After cdecl_node for a non-expression node. */
void cstmt_node(Checker *c, uint32_t i)
{
    CStmt *s = c->stmt;
    if (c->quiet || !s || !s->fs.len)
        return;
    switch (tg(c, i)) {
    case N_DECLARED:
        stmt_declared(c, s, i);
        break;
    case N_STRUCT:
        stmt_struct_defined(c, s, i);
        break;
    case N_DECL:
        if (c->par[i] != NOB && tg(c, c->par[i]) == N_FOR) {
            cc90(c, ctok_loc(c, c->nodes[c->par[i]].tok), NULL,
                 "ISO C90 does not support 'for' loop initial declarations");
            for_loop_decls(c, c->par[i]);
        }
        decl_after_stmt(c, i);
        break;
    case N_GOTO: {
        uint32_t tok = c->nodes[i].tok;
        uint32_t name = cnode_ident(c, i);
        if (name && c->func_sym != SYM_NONE)
            use_label(c, s, name, ctok_loc(c, tok - 1), cinput_loc(c, tok), i);
        break;
    }
    case N_GOTO_EXPR: {
        uint32_t e = i - 1;
        if (!node_err(c, e)) {
            TypeId t = cexpr_rvalue_type(c, e);
            if (type_ckind(TT, t) != TY_PTR &&
                !(c->ef[e] & EF_NPC))
                cerror(c, first_loc(c, e), "computed goto must be pointer "
                       "type");
        }
        break;
    }
    case N_ASM:
        cexpr_asm(c, i);
        break;
    case N_BREAK:
        stmt_bc(c, i, true);
        break;
    case N_CONTINUE:
        stmt_bc(c, i, false);
        break;
    case N_RETURN:
        stmt_return(c, i);
        break;
    case N_LABEL:
        label_attrs(c, s, i);
        label_at_end(c, i);
        break;
    case N_CASE: case N_DEFAULT:
        label_at_end(c, i);
        break;
    case N_LOCAL_LABEL:
        local_labels(c, s, i);
        {
            /* gcc warns once for a run of them, at the last */
            uint32_t par = c->par[i], j = i + 1;
            while (j < par && c->par[j] != par)
                j = c->par[j];
            if (j >= par || tg(c, j) != N_LOCAL_LABEL)
                cpedantic(c, cnode_loc(c, i), "ISO C forbids label "
                          "declarations");
        }
        break;
    case N_IF:
        stmt_if(c, i);
        break;
    case N_EXPR_STMT:
        stmt_empty(c, i);
        if (c->nodes[i].size > 1 &&
            !(i + 1 < c->nn && tg(c, i + 1) == N_SCOPE_END &&
              c->par[i + 1] != NOB && c->par[c->par[i + 1]] != NOB &&
              tg(c, c->par[c->par[i + 1]]) == N_STMT_EXPR))
            unused_value(c, i - 1, cnode_loc(c, i));
        break;
    default:
        break;
    }
}

/* ---- the end of scopes and functions ---------------------------------------------- */

/* ---- -Wmisleading-indentation --------------------------------------------------- */

typedef struct {
    SrcFile *f;
    uint32_t line, vcol;
} TokPos;

static bool tok_from_macro(const Checker *c, uint32_t tok)
{
    return (c->u->toks[tok].t.flags &
            (TF_ORIGIN_BODY | TF_ORIGIN_ARG | TF_PASTED | TF_SYNTH)) != 0;
}

/* The file, line and display column (tabs to multiples of 8) of a token. */
static bool tok_pos(Checker *c, uint32_t tok, TokPos *p, bool spell)
{
    SrcLoc loc = ctok_loc(c, tok);
    uint32_t col, len, i, dc = 0;
    if (!spell && tok_from_macro(c, tok) && c->u->toks[tok].exp)
        loc = c->u->toks[tok].exp;      /* the expansion point */
    const char *text;
    p->f = srcmgr_file_of(c->sm, loc);
    if (!p->f)
        return false;
    srcmgr_linecol(p->f, loc, &p->line, &col);
    text = srcmgr_line_text(p->f, p->line, &len);
    if (!text) {
        p->vcol = col;
        return true;
    }
    for (i = 0; i + 1 < col && i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\t')
            dc = (dc + 8) & ~7u;
        else if ((ch & 0xC0) != 0x80)
            dc++;
    }
    p->vcol = dc + 1;
    return true;
}


static bool tok_is_p(const Checker *c, uint32_t tok, Punct p)
{
    const Tok *t = &c->u->toks[tok].t;
    return t->kind == TK_PUNCT && t->punct == p;
}

static bool tok_is_kw(const Checker *c, uint32_t tok, int kw)
{
    const Tok *t = &c->u->toks[tok].t;
    return t->kind == TK_IDENT && (ident_by_id(c->in, t->aux)->ckw & 0xFF) == kw;
}

/* The display column (tabs to multiples of 8) of byte col (0-based) of a line. */
static uint32_t line_vcol(const char *text, uint32_t len, uint32_t col)
{
    uint32_t i, dc = 0;
    for (i = 0; i < col && i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '	')
            dc = (dc + 8) & ~7u;
        else if ((ch & 0xC0) != 0x80)
            dc++;
    }
    return dc + 1;
}

/* What lies in the source between the end of token `last` and the start of
 * token n besides white space and comments: a preprocessing directive (1;
 * gcc then does not compare the indentation), or the text of a macro
 * invocation that expanded to nothing (2; *at its first character). */
static int gap_scan(Checker *c, uint32_t last, uint32_t n, TokPos *at)
{
    SrcLoc ll = ctok_loc(c, last), nl = ctok_loc(c, n);
    SrcFile *f = srcmgr_file_of(c->sm, ll);
    uint32_t l1, c1, l2, c2, L, len, tl;
    bool in_block = false, found = false;
    if (!f || srcmgr_file_of(c->sm, nl) != f)
        return 0;
    srcmgr_linecol(f, ll, &l1, &c1);
    srcmgr_linecol(f, nl, &l2, &c2);
    tl = c->u->toks[last].t.len;
    c1 += tl - 1;                       /* 1-based column of the last byte */
    if (l2 < l1 || (l2 == l1 && c2 <= c1))
        return 0;
    for (L = l1; L <= l2; L++) {
        const char *text = srcmgr_line_text(f, L, &len);
        uint32_t i = L == l1 ? c1 : 0, e = L == l2 ? c2 - 1 : len;
        bool first_nws = L != l1;
        if (!text)
            return 0;
        for (; i < e && i < len; i++) {
            char ch = text[i];
            if (in_block) {
                if (ch == '*' && i + 1 < len && text[i + 1] == '/') {
                    in_block = false;
                    i++;
                }
                continue;
            }
            if (ch == ' ' || ch == '\t' || ch == '\r')
                continue;
            if (ch == '/' && i + 1 < len && text[i + 1] == '*') {
                in_block = true;
                i++;
                continue;
            }
            if (ch == '/' && i + 1 < len && text[i + 1] == '/')
                break;
            if (ch == '#' && first_nws)
                return 1;
            first_nws = false;
            if (!found) {
                found = true;
                at->f = f;
                at->line = L;
                at->vcol = line_vcol(text, len, i);
            }
        }
    }
    return found ? 2 : 0;
}

/* The first token-like character after token last in its own file (the
 * invocation of a nested macro inside a macro body, which expands away). */
static void after_text(Checker *c, uint32_t last, TokPos *at)
{
    SrcLoc ll = ctok_loc(c, last);
    SrcFile *f = srcmgr_file_of(c->sm, ll);
    uint32_t l, col, len, i;
    bool blk = false;
    if (!f)
        return;
    srcmgr_linecol(f, ll, &l, &col);
    i = col - 1 + c->u->toks[last].t.len;
    for (;; l++, i = 0) {
        const char *text = srcmgr_line_text(f, l, &len);
        if (!text)
            return;
        for (; i < len; i++) {
            char ch = text[i];
            if (blk) {
                if (ch == '*' && i + 1 < len && text[i + 1] == '/') {
                    blk = false;
                    i++;
                }
            } else if (ch == '/' && i + 1 < len && text[i + 1] == '*') {
                blk = true;
                i++;
            } else if (ch != ' ' && ch != 9 && ch != 13 && ch != 92) {
                at->f = f;
                at->line = l;
                at->vcol = line_vcol(text, len, i);
                return;
            }
        }
    }
}

/* warn_for_misleading_indentation: guard token g (if, else, while, for), the
 * body statement node, and the last token of the body. */
static void misleading(Checker *c, uint32_t g, uint32_t body, uint32_t last,
                       const char *kw)
{
    uint32_t b = first_tok(c, body), n = last + 1, l;
    TokPos gp, bp, np, lp;
    Diagnostic *d;
    bool spell;
    if (tg(c, body) == N_GOTO)
        b = c->nodes[body].tok - 1;     /* the 'goto' before the label name */
    if (n >= c->u->ntoks || c->u->toks[n].t.kind == TK_EOF)
        return;
    if (tok_is_p(c, b, P_LBRACE) ||
        tok_is_p(c, n, P_SEMI) || tok_is_p(c, n, P_RBRACE) ||
        tok_is_kw(c, n, CK_ELSE))
        return;
    spell = tok_from_macro(c, g) && tok_from_macro(c, n) &&
            c->u->toks[g].exp && c->u->toks[g].exp == c->u->toks[n].exp;
    if (spell && ((c->u->toks[b].t.flags | c->u->toks[n].t.flags) & TF_ORIGIN_ARG))
        return;                 /* spelled at the use, not in the body */
    if (!tok_pos(c, g, &gp, spell) || !tok_pos(c, b, &bp, spell) ||
        !tok_pos(c, n, &np, spell) || np.f != bp.f)
        return;
    if (spell && bp.f == gp.f && bp.line < gp.line && np.line <= bp.line)
        return;                 /* body and next in a nested macro defined above */
    if (spell && (np.f != bp.f || np.line < bp.line))
        after_text(c, last, &np);   /* n is spelled in a nested macro: its invocation */
    switch (spell ? 0 : gap_scan(c, last, n, &np)) {
    case 1:
        return;
    case 2:
        break;                  /* np: where the empty expansion starts */
    default:
        break;
    }
    for (l = g; l > 0 && !(c->u->toks[l].t.flags & TF_BOL); l--)
        ;
    if (!tok_pos(c, l, &lp, spell))
        return;
    if (np.line == bp.line) {
        if (gp.line == bp.line && !(c->u->toks[g].t.flags & TF_BOL))
            return;
    } else if (tok_is_p(c, b, P_SEMI)) {
        /* an empty body: the next statement indented past the guard line */
        /* a following block counts from the guard line's own column */
        if (np.vcol < lp.vcol || (np.vcol == lp.vcol && !tok_is_p(c, n, P_LBRACE)))
            return;
        if (gp.line != bp.line) {
            /* a ';' alone on its line is the body, indented as one; text
             * (a comment) before it makes it look misplaced */
            uint32_t tl2, k2;
            const char *tx2 = srcmgr_line_text(bp.f, bp.line, &tl2);
            for (k2 = 0; tx2 && k2 < tl2 && (tx2[k2] == ' ' || tx2[k2] == 9); k2++)
                ;
            if (!tx2 || line_vcol(tx2, tl2, k2) == bp.vcol)
                return;
        }
    } else if (!(bp.vcol == np.vcol && bp.vcol > lp.vcol) ||
               (bp.vcol == gp.vcol && strcmp(kw, "else")))  /* body under the guard itself */
        return;
    d = cwarn_d(c, DL_WARNING, ctok_loc(c, g), "misleading-indentation",
                "this '%s' clause does not guard...", kw);
    cnote(c, d, ctok_loc(c, n), "...this statement, but the latter is "
          "misleadingly indented as if it were guarded by the '%s'", kw);
}

/* The macro invocation a macro-originated token belongs to (gcc: the
 * outermost expansion point). */
static SrcLoc tok_expansion(const Checker *c, uint32_t tok)
{
    return ptok_loc(&c->u->toks[tok]);
}

/* Which macro definition a body token was spelled in: the first line of
 * its logical #define line (the macro an argument token was substituted
 * into is not tracked: 0). */
static uint64_t tok_macro(const Checker *c, uint32_t tok)
{
    const PTok *t = &c->u->toks[tok];
    SrcFile *f;
    uint32_t line, col, len;
    const char *text;
    if (!(t->t.flags & TF_ORIGIN_BODY) || (t->t.flags & TF_ORIGIN_ARG))
        return 0;
    f = srcmgr_file_of(c->sm, t->t.loc);
    if (!f)
        return 1;
    srcmgr_linecol(f, t->t.loc, &line, &col);
    while (line > 1) {
        text = srcmgr_line_text(f, line - 1, &len);
        while (text && len && (text[len - 1] == 10 || text[len - 1] == 13))
            len--;
        if (!text || !len || text[len - 1] != 92)
            break;
        line--;
    }
    return ((uint64_t)(uintptr_t)f << 20) ^ line;
}

/* ---- -Wduplicated-branches ------------------------------------------------------ */

/* gcc's operand_equal_p (OEP_LEXICOGRAPHIC) on the trees the C front end
 * builds: same shape, same declarations, equal constants (folded), same
 * types in casts, commutative operands in either order, and, for nodes
 * that carry a location, the same macro (a node written in one macro
 * never equals one from another macro or from plain source). */

static unsigned dup_punct(const Checker *c, uint32_t i)
{
    return c->u->toks[c->nodes[i].tok].t.punct;
}

static bool dup_same_tok(const Checker *c, uint32_t a, uint32_t b)
{
    const Tok *x = &c->u->toks[a].t, *y = &c->u->toks[b].t;
    return x->len == y->len &&
           !memcmp(tok_text_raw(c->sm, c->in, x), tok_text_raw(c->sm, c->in, y),
                   x->len);
}

static bool dup_commutes(Checker *c, uint32_t e, uint32_t l, uint32_t r)
{
    switch (dup_punct(c, e)) {
    case P_PLUS: case P_STAR: case P_AMP: case P_PIPE: case P_CARET:
    case P_EQEQ: case P_NE:
        return type_is_arith(TT, c->ty[l]) && type_is_arith(TT, c->ty[r]);
    default:
        return false;
    }
}

static bool dup_stmts_eq(Checker *c, uint32_t a, uint32_t b);
static bool dup_flatten(Checker *c, uint32_t s, uint32_t *out, uint32_t *n,
                        uint32_t max);
static bool dup_stmt(Checker *c, uint32_t a, uint32_t b);

static bool dup_expr(Checker *c, uint32_t a, uint32_t b)
{
    uint32_t ka[8], kb[8], na, nb, j;
    a = strip_paren(c, a);
    b = strip_paren(c, b);
    if (c->ck[a] == K_ERR || c->ck[b] == K_ERR)
        return false;
    if (c->ck[a] == c->ck[b] &&
        type_canon(TT, c->ty[a]) == type_canon(TT, c->ty[b])) {
        if ((c->ck[a] == K_ICE || c->ck[a] == K_FOLD) && c->cv[a] == c->cv[b])
            return true;
        if (c->ck[a] == K_FLOAT && c->fv.data[c->cv[a]] == c->fv.data[c->cv[b]])
            return true;
    }
    if (tg(c, a) != tg(c, b))
        return false;
    switch (tg(c, a)) {
    case N_IDENT:
        return cnode_ident(c, a) == cnode_ident(c, b);
    case N_STRING: {
        uint32_t t;
        if (node_pieces(c, a) != node_pieces(c, b))
            return false;
        for (t = 0; t < node_pieces(c, a); t++)
            if (!dup_same_tok(c, c->nodes[a].tok + t, c->nodes[b].tok + t))
                return false;
        return true;
    }
    case N_STMT_EXPR: {
        /* only a block of one statement compares equal */
        uint32_t la[2], lb[2], ma = 0, mb = 0;
        na = node_children(c->nodes, a, ka, 8);
        nb = node_children(c->nodes, b, kb, 8);
        return na == 1 && nb == 1 && dup_flatten(c, ka[0], la, &ma, 2) &&
               dup_flatten(c, kb[0], lb, &mb, 2) && ma == 1 && mb == 1 &&
               dup_stmt(c, la[0], lb[0]);
    }
    case N_BINARY: case N_ASSIGN: case N_UNARY: case N_POSTFIX: case N_CALL:
    case N_INDEX: case N_CAST: case N_MEMBER_EXPR:
        break;
    case N_COND:
        if (c->dup_no_cond)
            return false;
        break;
    default:
        return false;
    }
    if (tok_macro(c, c->nodes[a].tok) != tok_macro(c, c->nodes[b].tok))
        return false;
    na = node_children(c->nodes, a, ka, 8);
    nb = node_children(c->nodes, b, kb, 8);
    if (na != nb || na >= 8)
        return false;
    switch (tg(c, a)) {
    case N_BINARY: case N_ASSIGN: case N_UNARY: case N_POSTFIX:
        if (dup_punct(c, a) != dup_punct(c, b))
            return false;
        if (tg(c, a) == N_BINARY && dup_commutes(c, a, ka[0], ka[1]) &&
            dup_expr(c, ka[0], kb[1]) && dup_expr(c, ka[1], kb[0]))
            return true;
        break;
    case N_CAST:
        if (type_canon(TT, c->ty[a]) != type_canon(TT, c->ty[b]))
            return false;
        return dup_expr(c, ka[na - 1], kb[nb - 1]);
    case N_MEMBER_EXPR:
        if (cnode_ident(c, a) != cnode_ident(c, b) ||
            dup_punct(c, a - 0) != dup_punct(c, b - 0))
            return false;
        break;
    default:
        break;
    }
    for (j = 0; j < na; j++)
        if (!dup_expr(c, ka[j], kb[j]))
            return false;
    return true;
}

/* The statements of a branch with nested blocks flattened and empty
 * statements dropped; false if one is of a kind not compared. */
/* The expression of a statement expression that is a single expression
 * statement (gcc then sees just the expression), or NO_NODE. */
uint32_t stmt_expr_single(Checker *c, uint32_t s)
{
    uint32_t k[8], l[2], e[2], m = 0;
    if (node_children(c->nodes, s, k, 8) != 1 ||
        !dup_flatten(c, k[0], l, &m, 2) || m != 1 || tg(c, l[0]) != N_EXPR_STMT ||
        node_children(c->nodes, l[0], e, 2) != 1)
        return NO_NODE;
    return e[0];
}

static bool dup_flatten(Checker *c, uint32_t s, uint32_t *out, uint32_t *n,
                        uint32_t max)
{
    uint32_t k[256], nk, j;
    switch (tg(c, s)) {
    case N_COMPOUND:
        nk = node_children(c->nodes, s, k, 256);
        if (nk >= 256)
            return false;
        for (j = 0; j < nk; j++) {
            unsigned t = tg(c, k[j]);
            if (t == N_SCOPE || t == N_SCOPE_END || t == N_BODY)
                continue;
            if (!dup_flatten(c, k[j], out, n, max))
                return false;
        }
        return true;
    case N_EXPR_STMT:
        if (c->nodes[s].size <= 1)
            return true;
        /* fall through */
    case N_RETURN: case N_IF: case N_BREAK: case N_CONTINUE:
        if (*n >= max)
            return false;
        out[(*n)++] = s;
        return true;
    default:
        return false;
    }
}

static bool dup_stmt(Checker *c, uint32_t a, uint32_t b)
{
    uint32_t ka[8], kb[8], na, nb;
    if (tg(c, a) != tg(c, b))
        return false;
    na = node_children(c->nodes, a, ka, 8);
    nb = node_children(c->nodes, b, kb, 8);
    switch (tg(c, a)) {
    case N_BREAK: case N_CONTINUE:
        return true;
    case N_EXPR_STMT: case N_RETURN:
        if (na != nb || na >= 8)
            return false;
        return na == 0 || dup_expr(c, ka[na - 1], kb[nb - 1]);
    case N_IF:
        if (na != nb || (na != 6 && na != 9))
            return false;
        return dup_expr(c, ka[1], kb[1]) && dup_stmts_eq(c, ka[3], kb[3]) &&
               (na == 6 || dup_stmts_eq(c, ka[6], kb[6]));
    default:
        return false;
    }
}

static bool dup_stmts_eq(Checker *c, uint32_t a, uint32_t b)
{
    uint32_t la[64], lb[64], na = 0, nb = 0, j;
    if (!dup_flatten(c, a, la, &na, 64) || !dup_flatten(c, b, lb, &nb, 64))
        return false;
    if (na != nb)
        return false;
    for (j = 0; j < na; j++)
        if (!dup_stmt(c, la[j], lb[j]))
            return false;
    return true;
}

static bool dup_const(const Checker *c, uint32_t e)
{
    return c->ck[e] == K_ICE || c->ck[e] == K_FOLD;
}

/* A null value of any pointer or integer type (the arms are converted to
 * the result type before they are compared). */
static bool dup_isnull(const Checker *c, uint32_t e)
{
    return (dup_const(c, e) || (c->ck[e] == K_ADDR && !c->cb[e])) && !c->cv[e];
}

/* The arms of the ?: node i are the same expression.  build_conditional_expr
 * (immediate) compares the arms already converted to the result type and
 * does not see through statement expressions or nested ?:; the later walk
 * (not immediate) compares them as written.  A __builtin_constant_p
 * condition is folded away first. */
bool cstmt_cond_identical(Checker *c, uint32_t i, bool immediate)
{
    uint32_t k[4], x, y, cd, f[4];
    bool ok;
    if (node_children(c->nodes, i, k, 4) != 3)
        return false;
    cd = strip_paren(c, k[0]);
    if (tg(c, cd) == N_CALL && node_children(c->nodes, cd, f, 4) >= 1 &&
        tg(c, f[0]) == N_IDENT && c->u->toks[c->nodes[f[0]].tok].t.len == 20 &&
        !memcmp(tok_text_raw(c->sm, c->in, &c->u->toks[c->nodes[f[0]].tok].t),
                "__builtin_constant_p", 20))
        return false;
    x = strip_paren(c, k[1]);
    y = strip_paren(c, k[2]);
    if (!immediate)
        return dup_expr(c, x, y);
    if (tg(c, x) == N_STMT_EXPR || tg(c, x) == N_COND ||
        tg(c, y) == N_STMT_EXPR || tg(c, y) == N_COND)
        return false;
    if (dup_isnull(c, x) && dup_isnull(c, y))
        return !dup_const(c, cd) || !dup_const(c, x);
    if ((c->ck[x] == K_ICE || c->ck[x] == K_FOLD) && c->ck[y] == c->ck[x])
        /* a constant condition folds the ?: away */
        return c->cv[x] == c->cv[y] && !dup_const(c, cd);
    c->dup_no_cond = true;     /* a ?: inside the arms is not comparable */
    ok = dup_expr(c, x, y);
    c->dup_no_cond = false;
    return ok;
}

/* The if statements with identical, non-empty branches, and the ?:
 * expressions whose equal arms have side effects (gcc: c_genericize's
 * pre-order walk, so by first token; nodes are post-order). */
/* An arm has side effects (a statement expression counts by its only
 * statement: cereal flags every one). */
static bool dup_side(Checker *c, uint32_t e)
{
    uint32_t k[8], l[2], m = 0;
    e = strip_paren(c, e);
    if (tg(c, e) != N_STMT_EXPR)
        return c->ef[e] & EF_SIDE;
    if (node_children(c->nodes, e, k, 8) != 1 || !dup_flatten(c, k[0], l, &m, 2) ||
        m != 1 || tg(c, l[0]) != N_EXPR_STMT)
        return true;
    return c->ef[node_children(c->nodes, l[0], k, 8) ? k[0] : l[0]] & EF_SIDE;
}

static uint32_t dup_key(const Checker *c, uint32_t k)
{
    return tg(c, k) == N_IF ? c->nodes[k].tok : first_tok(c, k);
}

void cstmt_dup_branches(Checker *c, uint32_t scope, uint32_t end)
{
    uint32_t k, n = 0, cap = 16, *v, j, p;
    if (!diag_enabled(c->diag, "duplicated-branches"))
        return;
    v = xmalloc(cap * sizeof *v);
    for (k = cfirst(c, scope); k <= end; k++) {
        uint32_t kids[8];
        if (tg(c, k) == N_IF)
            ;
        else if (tg(c, k) == N_COND && (c->ef[k] & EF_SIDE) &&
                 node_children(c->nodes, k, kids, 8) == 3 &&
                 (dup_side(c, kids[1]) || dup_side(c, kids[2])))
            ;
        else
            continue;
        if (n == cap)
            v = xrealloc(v, (cap *= 2) * sizeof *v);
        v[n++] = k;
    }
    for (j = 1; j < n; j++) {
        uint32_t x = v[j];
        for (p = j; p > 0 && dup_key(c, v[p - 1]) > dup_key(c, x); p--)
            v[p] = v[p - 1];
        v[p] = x;
    }
    for (j = 0; j < n; j++) {
        uint32_t kids[16], nk = node_children(c->nodes, v[j], kids, 16);
        uint32_t l[64], m = 0, o[64], q = 0;
        if (tg(c, v[j]) == N_COND) {
            if (cstmt_cond_identical(c, v[j], false))
                cwarn(c, cexpr_colon_loc(c, v[j], kids[1], kids[2]),
                      "duplicated-branches", "this condition has identical "
                      "branches");
            continue;
        }
        if (nk != 9)
            continue;
        if (!dup_flatten(c, kids[3], l, &m, 64) ||
            !dup_flatten(c, kids[6], o, &q, 64) || !m || !q)
            continue;
        if (dup_stmts_eq(c, kids[3], kids[6]))
            cwarn(c, ctok_loc(c, c->nodes[v[j]].tok + 1), "duplicated-branches",
                  "this condition has identical branches");
    }
    free(v);
}

/* -Wduplicated-cond: a condition repeated in an if / else-if chain (the
 * parser compares each new condition with the earlier ones; conditions
 * with side effects are not compared). */
/* What gcc's fold makes of an if condition: a comparison, with the constant
 * (if any) on the right, > and >= turned into < and <=, a constant bound
 * moved into the constant, and ! pushed into the comparison. */
typedef struct {
    int op;                     /* P_EQEQ P_NE P_LT P_LE P_GT P_GE */
    uint32_t l, r;              /* r is NO_NODE when rc */
    bool rc;
    int64_t cv;
} DupCmp;

static int dup_mirror(int op)
{
    switch (op) {
    case P_LT: return P_GT;
    case P_GT: return P_LT;
    case P_LE: return P_GE;
    case P_GE: return P_LE;
    default: return op;
    }
}

static int dup_negate(int op)
{
    switch (op) {
    case P_EQEQ: return P_NE;
    case P_NE: return P_EQEQ;
    case P_LT: return P_GE;
    case P_GE: return P_LT;
    case P_LE: return P_GT;
    default: return P_LE;
    }
}

static bool dup_intlike(Checker *c, uint32_t e)
{
    TypeId t = c->ty[e];
    return type_is_integer(TT, t) || type_ckind(TT, t) == TY_PTR;
}

static bool dup_cmp_of(Checker *c, uint32_t e, DupCmp *d)
{
    uint32_t k[3];
    unsigned n;
    bool neg = false;
    e = strip_paren(c, e);
    while (tg(c, e) == N_UNARY && dup_punct(c, e) == P_BANG &&
           node_children(c->nodes, e, k, 3) == 1) {
        uint32_t in = strip_paren(c, k[0]);
        neg = !neg;
        if (tg(c, in) == N_BINARY) {
            switch (dup_punct(c, in)) {
            case P_EQEQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE:
                e = in;
                goto cmp;
            default:
                break;
            }
        }
        e = in;
        if (tg(c, e) == N_UNARY && dup_punct(c, e) == P_BANG)
            continue;
        d->op = neg ? P_EQEQ : P_NE;        /* !v is v == 0 */
        if (!dup_intlike(c, e))
            return false;
        d->l = e;
        d->r = NO_NODE;
        d->rc = true;
        d->cv = 0;
        goto norm;
    }
    if (tg(c, e) == N_BINARY) {
        switch (dup_punct(c, e)) {
        case P_EQEQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE:
            goto cmp;
        default:
            break;
        }
    }
    if (!dup_intlike(c, e) || dup_const(c, e))
        return false;
    d->op = P_NE;
    d->l = e;
    d->r = NO_NODE;
    d->rc = true;
    d->cv = 0;
    goto norm;
cmp:
    n = node_children(c->nodes, e, k, 3);
    if (n != 2)
        return false;
    if (!dup_intlike(c, k[0]) || !dup_intlike(c, k[1])) {
        /* floats swap operands, but ! cannot invert (NaN) */
        if (neg || !type_is_arith(TT, c->ty[k[0]]) ||
            !type_is_arith(TT, c->ty[k[1]]))
            return false;
    }
    d->op = dup_punct(c, e);
    if (neg)
        d->op = dup_negate(d->op);
    d->l = strip_paren(c, k[0]);
    d->r = strip_paren(c, k[1]);
    d->rc = false;
    if (dup_const(c, d->l) && !dup_const(c, d->r)) {
        uint32_t t = d->l;
        d->l = d->r;
        d->r = t;
        d->op = dup_mirror(d->op);
    }
    if (dup_const(c, d->r)) {
        d->rc = true;
        d->cv = (int64_t)c->cv[d->r];
        d->r = NO_NODE;
    }
norm:
    if (d->rc) {
        bool sgn = type_is_signed(TT, c->ty[d->l]);
        for (;;) {
            uint32_t m[3], x;
            d->l = strip_paren(c, d->l);
            if (tg(c, d->l) == N_UNARY && dup_punct(c, d->l) == P_MINUS &&
                node_children(c->nodes, d->l, m, 3) == 1 && sgn) {
                d->l = m[0];
                d->op = dup_mirror(d->op);
                d->cv = -d->cv;
                continue;
            }
            if (tg(c, d->l) != N_BINARY ||
                (dup_punct(c, d->l) != P_PLUS && dup_punct(c, d->l) != P_MINUS) ||
                node_children(c->nodes, d->l, m, 3) != 2 ||
                !(sgn || d->op == P_EQEQ || d->op == P_NE) ||
                !type_is_integer(TT, c->ty[m[0]]) ||
                !type_is_integer(TT, c->ty[m[1]]))
                break;
            if (dup_const(c, m[1]) && !dup_const(c, m[0]))
                x = m[0];
            else if (dup_punct(c, d->l) == P_PLUS && dup_const(c, m[0]) &&
                     !dup_const(c, m[1]))
                x = m[1];
            else
                break;
            {
                uint32_t kc = x == m[0] ? m[1] : m[0];
                int64_t v = (int64_t)c->cv[kc];
                d->cv = dup_punct(c, d->l) == P_PLUS ? d->cv - v : d->cv + v;
            }
            d->l = x;
        }
        if (d->op == P_LT)
            d->op = P_LE, d->cv--;
        else if (d->op == P_GT)
            d->op = P_GE, d->cv++;
    } else if (d->op == P_GT || d->op == P_GE) {
        uint32_t t = d->l;
        d->l = d->r;
        d->r = t;
        d->op = dup_mirror(d->op);
    }
    return true;
}

/* The same condition after folding (or structurally the same). */
static bool dup_cond_same(Checker *c, uint32_t a, uint32_t b)
{
    DupCmp x, y;
    if (dup_expr(c, a, b))
        return true;
    if (!dup_cmp_of(c, a, &x) || !dup_cmp_of(c, b, &y) || x.op != y.op ||
        x.rc != y.rc)
        return false;
    if (x.rc)
        return x.cv == y.cv && dup_expr(c, x.l, y.l);
    return dup_expr(c, x.l, y.l) && dup_expr(c, x.r, y.r);
}

typedef struct { uint32_t cond, prev; } DupCond;

static int dupcond_cmp(const void *x, const void *y)
{
    uint32_t a = ((const DupCond *)x)->cond, b = ((const DupCond *)y)->cond;
    return a < b ? -1 : a > b;
}

/* Where gcc puts the condition: a value that is not already a truth value
 * is wrapped in "!= 0" at the start of the expression, so an arithmetic
 * operator's location is its first token. */
static SrcLoc dupcond_loc(Checker *c, uint32_t e)
{
    if (tg(c, e) == N_BINARY)
        switch (dup_punct(c, e)) {
        case P_EQEQ: case P_NE: case P_LT: case P_LE: case P_GT: case P_GE:
        case P_ANDAND: case P_OROR: case P_COMMA:
            break;
        default:
            if (!dup_const(c, e))
                return ctok_loc(c, first_tok(c, e));
        }
    return cnode_loc(c, e);
}

void cstmt_dup_cond(Checker *c, uint32_t scope, uint32_t end)
{
    uint32_t k, nw = 0, cap = 8, j;
    DupCond *w;
    if (!diag_enabled(c->diag, "duplicated-cond"))
        return;
    w = xmalloc(cap * sizeof *w);
    for (k = cfirst(c, scope); k <= end; k++) {
        uint32_t kids[16], seen[32], n, ns = 0, cur = k;
        if (tg(c, k) != N_IF)
            continue;
        if (c->par[k] != NOB && tg(c, c->par[k]) == N_IF &&
            node_children(c->nodes, c->par[k], kids, 16) == 9 &&
            kids[6] == k)
            continue;           /* not the head of its chain */
        for (;;) {
            uint32_t cond;
            n = node_children(c->nodes, cur, kids, 16);
            if (n < 6)
                break;
            cond = strip_paren(c, kids[1]);
            if (c->ef[cond] & EF_SIDE)
                ns = 0;         /* the chain's earlier tests may not hold now */
            if (!node_err(c, cond) && !(c->ef[cond] & EF_SIDE) &&
                !dup_const(c, cond)) {
                for (j = 0; j < ns; j++)
                    if (dup_cond_same(c, seen[j], cond)) {
                        if (nw == cap)
                            w = xrealloc(w, (cap *= 2) * sizeof *w);
                        w[nw].cond = cond;
                        w[nw++].prev = seen[j];
                        break;
                    }
                if (ns < 32)
                    seen[ns++] = cond;
            }
            if (n != 9 || tg(c, kids[6]) != N_IF)
                break;
            cur = kids[6];
        }
    }
    /* gcc warns as the parser reaches each condition */
    qsort(w, nw, sizeof *w, dupcond_cmp);
    for (j = 0; j < nw; j++) {
        Diagnostic *dg = cwarn_d(c, DL_WARNING, dupcond_loc(c, w[j].cond),
                                 "duplicated-cond", "duplicated 'if' "
                                 "condition");
        if (dg)
            cnote(c, dg, dupcond_loc(c, w[j].prev), "previously used here");
    }
    free(w);
}

/* warn_for_multistatement_macros: the body of a guard starts in a macro
 * expansion and the token after it is from the same expansion. */
static void multistatement(Checker *c, uint32_t g, uint32_t body,
                           uint32_t last, const char *kw)
{
    uint32_t b = first_tok(c, body), n = last + 1;
    uint64_t kb, kn, kg;
    Diagnostic *d;
    if (!diag_enabled(c->diag, "multistatement-macros") ||
        n >= c->u->ntoks || c->u->toks[n].t.kind == TK_EOF)
        return;
    /* the body proper starts after its labels */
    while (b + 1 < n) {
        if (c->u->toks[b].t.kind == TK_IDENT && tok_is_p(c, b + 1, P_COLON))
            b += 2;
        else if (tok_is_kw(c, b, CK_DEFAULT) && tok_is_p(c, b + 1, P_COLON))
            b += 2;
        else if (tok_is_kw(c, b, CK_CASE)) {
            while (b < n && !tok_is_p(c, b, P_COLON))
                b++;
            b++;
        } else
            break;
    }
    if (b >= n || tok_is_p(c, b, P_LBRACE))
        return;
    if (!tok_from_macro(c, b) || !tok_from_macro(c, n) ||
        tok_expansion(c, b) != tok_expansion(c, n))
        return;
    if (tok_is_p(c, n, P_SEMI))
        return;
    kb = tok_macro(c, b);
    kn = tok_macro(c, n);
    if (kb && kn) {
        uint32_t k, prev = b;
        if (kb != kn)
            return;
        /* one expansion: the spelled locations never go back */
        for (k = b + 1; k <= n; k++)
            if (tok_macro(c, k) == kb) {
                if (c->u->toks[k].t.loc < c->u->toks[prev].t.loc)
                    return;
                prev = k;
            }
    }
    if (tok_from_macro(c, g) && tok_expansion(c, g) == tok_expansion(c, b)) {
        /* a guard from the same invocation: the body must not belong to a
         * macro that the guard's own macro was expanded inside */
        uint32_t k;
        kg = tok_macro(c, g);
        if (!kg || !kb || kg == kb)
            return;
        for (k = g; k-- > 0;)
            if (tok_from_macro(c, k) && tok_macro(c, k) == kb &&
                tok_expansion(c, k) == tok_expansion(c, b))
                return;
    }
    d = cwarn_d(c, DL_WARNING, ctok_loc(c, b), "multistatement-macros",
                "macro expands to multiple statements");
    cnote(c, d, ctok_loc(c, g), "some parts of macro expansion are not "
          "guarded by this '%s' clause", kw);
}

/* The SCOPE_END i closed the body of an if/else/while/for. */
void misleading_scope_end(Checker *c, uint32_t i, uint32_t p)
{
    uint32_t kids[16], n = node_children(c->nodes, p, kids, 16), k = NOB;
    uint32_t body = NOB, g = c->nodes[p].tok;
    const char *kw = NULL;
    uint32_t j;
    for (j = 0; j < n; j++)
        if (kids[j] == i)
            k = j;
    if (k == NOB || k == 0 || i + 1 == p)
        return;
    body = kids[k - 1];
    switch (tg(c, p)) {
    case N_IF:
        if (n == 6 && k == 4)
            kw = "if";
        else if (n == 9 && k == 7) {
            kw = "else";
            g = c->nodes[kids[5]].tok;
            if (g == 0)
                return;
            g--;
        }
        break;
    case N_WHILE:
        if (k == 4)
            kw = "while";
        break;
    case N_FOR:
        if (k == 6)
            kw = "for";
        break;
    case N_SWITCH:
        if (k == 4)
            kw = "switch";
        break;
    default:
        break;
    }
    if (kw) {
        if (tg(c, p) != N_SWITCH)
            misleading(c, g, body, c->nodes[i].tok, kw);
        multistatement(c, g, body, c->nodes[i].tok, kw);
    }
}
