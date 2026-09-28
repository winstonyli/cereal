/* hygiene.c - macro definition and invocation hygiene.
 *
 * Definition-time checks look at the replacement list in isolation:
 * operands that need parentheses, statement-like bodies that are not a
 * single statement, reserved names.  Invocation-time checks combine the
 * definition with the actual arguments: side effects in an argument whose
 * parameter is evaluated more than once (or never). */
#include "analysis.h"

#include <string.h>

typedef struct MacroInfo {
    int *evals;           /* evaluated occurrences of each parameter */
    int *mentions;        /* all occurrences (incl. # and ##) */
} MacroInfo;

struct HygieneState {
    /* call-site arguments already reported (dedupe re-expansions) */
    VEC(SrcLoc) reported;
};

/* ---- token classification ------------------------------------------ */

static bool is_decl_keyword(const Token *t)
{
    static const char *const kw[] = {
        "typedef", "struct", "union", "enum", "static", "extern", "auto",
        "register", "inline", "const", "volatile", "restrict", "void", "char",
        "short", "int", "long", "float", "double", "signed", "unsigned",
        "_Bool", "_Complex", "__attribute__", "__attribute", "__declspec",
        "__inline", "__inline__", "__restrict", "__restrict__", "__const",
        "__volatile__", "__thread", "__extension__", "__asm__", "asm",
        "__asm", "_Static_assert", NULL};
    int i;
    if (t->kind != TK_IDENT)
        return false;
    for (i = 0; kw[i]; i++)
        if (tok_is_ident(t, kw[i]))
            return true;
    return false;
}

static bool is_stmt_keyword(const Token *t)
{
    static const char *const kw[] = {"if", "else", "for", "while", "do",
                                     "switch", "return", "break", "continue",
                                     "goto", "case", "default", NULL};
    int i;
    if (t->kind != TK_IDENT)
        return false;
    for (i = 0; kw[i]; i++)
        if (tok_is_ident(t, kw[i]))
            return true;
    return false;
}

static bool is_assign(const Token *t)
{
    if (t->kind != TK_PUNCT)
        return false;
    switch (t->punct) {
    case P_ASSIGN: case P_MUL_ASSIGN: case P_DIV_ASSIGN: case P_MOD_ASSIGN:
    case P_ADD_ASSIGN: case P_SUB_ASSIGN: case P_SHL_ASSIGN:
    case P_SHR_ASSIGN: case P_AND_ASSIGN: case P_XOR_ASSIGN:
    case P_OR_ASSIGN:
        return true;
    default:
        return false;
    }
}

/* Does the token end an operand (so a following + - * & is binary)? */
static bool ends_operand(const Token *t)
{
    if (!t)
        return false;
    switch (t->kind) {
    case TK_IDENT:
        return !is_c_keyword(t->text, t->len);
    case TK_PPNUM: case TK_CHAR: case TK_STRING:
        return true;
    case TK_PUNCT:
        return t->punct == P_RPAREN || t->punct == P_RBRACKET ||
               t->punct == P_INC || t->punct == P_DEC;
    default:
        return false;
    }
}

/* Binary operators with lower precedence than a postfix/unary operand. */
static bool is_binary_op(Punct p)
{
    switch (p) {
    case P_STAR: case P_SLASH: case P_PERCENT: case P_PLUS: case P_MINUS:
    case P_SHL: case P_SHR: case P_LT: case P_GT: case P_LE: case P_GE:
    case P_EQEQ: case P_NE: case P_AMP: case P_CARET: case P_PIPE:
    case P_ANDAND: case P_OROR: case P_QUESTION:
        return true;
    default:
        return false;
    }
}

static bool unevaluated_context(Token **v, int i)
{
    /* sizeof x, sizeof(x), typeof(x), _Alignof(x) */
    const Token *p = i > 0 ? v[i - 1] : NULL, *pp = i > 1 ? v[i - 2] : NULL;
    if (p && (tok_is_ident(p, "sizeof")))
        return true;
    if (p && tok_is_punct(p, P_LPAREN) && pp &&
        (tok_is_ident(pp, "sizeof") || tok_is_ident(pp, "typeof") ||
         tok_is_ident(pp, "__typeof__") || tok_is_ident(pp, "__typeof") ||
         tok_is_ident(pp, "_Alignof") || tok_is_ident(pp, "__alignof__")))
        return true;
    return false;
}

static int param_index(const Macro *m, const Token *t)
{
    int i;
    if (t->kind != TK_IDENT)
        return -1;
    for (i = 0; i < m->nparams; i++)
        if (m->params[i] == t->ident)
            return i;
    return -1;
}

/* ---- definition checks --------------------------------------------- */

static bool feature_test_macro(const char *s)
{
    size_t n = strlen(s);
    static const char *const names[] = {
        "_FILE_OFFSET_BITS", "_TIME_BITS", "_REENTRANT", "_THREAD_SAFE",
        "_FORTIFY_SOURCE", "__STDC_FORMAT_MACROS", "__STDC_LIMIT_MACROS",
        "__STDC_CONSTANT_MACROS", "_WIN32_WINNT", "_CRT_SECURE_NO_WARNINGS",
        NULL};
    int i;
    for (i = 0; names[i]; i++)
        if (!strcmp(s, names[i]))
            return true;
    if (n > 7 && !strcmp(s + n - 7, "_SOURCE"))
        return true;
    if (!strncmp(s, "__STDC_WANT_", 12))
        return true;
    return false;
}

static void check_name(Analysis *a, Macro *m)
{
    const char *s = m->name->str;
    Diagnostic *d = NULL;
    if (is_c_keyword(s, m->name->len)) {
        d = diag_report(a->diag, DL_WARNING, "macro-reserved-name", m->name_loc,
                        "macro name '%s' is a keyword (undefined behavior if "
                        "a standard header is included, C99 7.1.2p4)", s);
    } else if (s[0] == '_' && (s[1] == '_' || (s[1] >= 'A' && s[1] <= 'Z')) &&
               !feature_test_macro(s)) {
        d = diag_report(a->diag, DL_WARNING, "macro-reserved-name", m->name_loc,
                        "macro name '%s' is a reserved identifier (C99 7.1.3)",
                        s);
    }
    if (d)
        diag_set_range(d, m->name_loc, m->name_loc + m->name->len);
}

static const char *spell(Analysis *a, const Token *t)
{
    return tok_str(a->arena, t);
}

static bool is_qualifier(const Token *t)
{
    return tok_is_ident(t, "const") || tok_is_ident(t, "volatile") ||
           tok_is_ident(t, "restrict") || tok_is_ident(t, "__restrict");
}

/* An identifier that can start or continue a declaration's specifiers. */
static bool type_word(Macro *m, const Token *t)
{
    if (!t || t->kind != TK_IDENT || param_index(m, t) >= 0)
        return false;
    return is_decl_keyword(t) || !is_c_keyword(t->text, t->len);
}

static bool operand_start(const Token *t)
{
    return t && (t->kind == TK_IDENT || t->kind == TK_PPNUM ||
                 t->kind == TK_CHAR || t->kind == TK_STRING ||
                 tok_is_punct(t, P_LPAREN));
}

/* Parameters that stand for type names or declared identifiers are not
 * expressions; parenthesizing them would be wrong. */
static void classify_params(Macro *m, Token **v, int n, bool *not_expr)
{
    int i;
    for (i = 0; i < n; i++) {
        const Token *t = v[i], *prev = i ? v[i - 1] : NULL,
                    *pprev = i > 1 ? v[i - 2] : NULL,
                    *next = i + 1 < n ? v[i + 1] : NULL,
                    *nnext = i + 2 < n ? v[i + 2] : NULL;
        int pi = param_index(m, t);
        if (pi < 0)
            continue;
        /* type: `P x`, `P *)`, `P **`, `P * const`, `(P) operand` */
        if (next && next->kind == TK_IDENT && !is_c_keyword(next->text, next->len))
            not_expr[pi] = true;
        else if (next && is_qualifier(next))
            not_expr[pi] = true;
        else if (next && tok_is_punct(next, P_STAR) && nnext &&
                 (tok_is_punct(nnext, P_RPAREN) || tok_is_punct(nnext, P_STAR) ||
                  is_qualifier(nnext)))
            not_expr[pi] = true;
        else if (next && tok_is_punct(next, P_STAR) && nnext &&
                 nnext->kind == TK_IDENT && i + 3 < n &&
                 (tok_is_punct(v[i + 3], P_SEMI) || tok_is_punct(v[i + 3], P_ASSIGN) ||
                  tok_is_punct(v[i + 3], P_LBRACKET) ||
                  tok_is_punct(v[i + 3], P_COMMA)) &&
                 (!prev || tok_is_punct(prev, P_LBRACE) ||
                  tok_is_punct(prev, P_SEMI) || tok_is_punct(prev, P_LPAREN) ||
                  tok_is_punct(prev, P_COMMA) || type_word(m, prev)))
            not_expr[pi] = true; /* T *name; */
        else if (prev && tok_is_punct(prev, P_LPAREN) && next &&
                 tok_is_punct(next, P_RPAREN) && i + 2 < n &&
                 operand_start(v[i + 2]) &&
                 !(pprev && (tok_is_ident(pprev, "sizeof") || pprev->kind == TK_IDENT)))
            not_expr[pi] = true;
        /* declared name: `type P =`, `type *P;`, `struct P {` */
        if (prev && (type_word(m, prev) || tok_is_ident(prev, "struct") ||
                     tok_is_ident(prev, "union") || tok_is_ident(prev, "enum")) &&
            !tok_is_ident(prev, "return") && !tok_is_ident(prev, "sizeof") &&
            !tok_is_ident(prev, "case"))
            not_expr[pi] = true;
        if (prev && tok_is_punct(prev, P_STAR) && pprev &&
            (type_word(m, pprev) || tok_is_punct(pprev, P_STAR)) && next &&
            (tok_is_punct(next, P_ASSIGN) || tok_is_punct(next, P_SEMI) ||
             tok_is_punct(next, P_LBRACKET)))
            not_expr[pi] = true;
        if (prev && tok_is_punct(prev, P_STAR) && pprev &&
            param_index(m, pprev) >= 0 && next &&
            (tok_is_punct(next, P_ASSIGN) || tok_is_punct(next, P_SEMI)))
            not_expr[pi] = true; /* T *name = ... */
    }
}

static void check_params(Analysis *a, Macro *m, Token **v, int n, MacroInfo *mi)
{
    int i;
    bool *not_expr = NEW_ARRAY(a->arena, bool, m->nparams + 1);
    classify_params(m, v, n, not_expr);
    for (i = 0; i < n; i++) {
        const Token *t = v[i], *prev = i ? v[i - 1] : NULL,
                    *next = i + 1 < n ? v[i + 1] : NULL;
        int pi = param_index(m, t);
        const Token *bad = NULL;
        Diagnostic *d;
        if (pi < 0)
            continue;
        if ((prev && (tok_is_punct(prev, P_HASH) || tok_is_punct(prev, P_HASHHASH))) ||
            (next && tok_is_punct(next, P_HASHHASH)))
            continue; /* # x, x ## y: not evaluated as an expression */
        if (!unevaluated_context(v, i))
            mi->evals[pi]++;
        if (t->ident == a->pp->id_va_args || not_expr[pi])
            continue;
        if (prev && (tok_is_punct(prev, P_DOT) || tok_is_punct(prev, P_ARROW)))
            continue; /* member designator */
        if (next && tok_is_punct(next, P_LPAREN))
            continue; /* callee: X(...) in X-macro lists, f(args) */
        if (prev) {
            if (prev->kind == TK_PUNCT) {
                if (is_binary_op((Punct)prev->punct) || prev->punct == P_BANG ||
                    prev->punct == P_TILDE || prev->punct == P_INC ||
                    prev->punct == P_DEC || prev->punct == P_RPAREN)
                    if (prev->punct != P_QUESTION)
                        bad = prev;
            } else if (tok_is_ident(prev, "sizeof")) {
                bad = prev;
            }
        }
        if (!bad && next && next->kind == TK_PUNCT) {
            if ((is_binary_op((Punct)next->punct) && next->punct != P_QUESTION) ||
                next->punct == P_LBRACKET || next->punct == P_DOT ||
                next->punct == P_ARROW || next->punct == P_INC ||
                next->punct == P_DEC)
                bad = next;
        }
        if (!bad)
            continue;
        /* `(T)x` is only a cast if the parenthesized part looks like a type;
         * `f(a) x` is not valid C, so treat ')' before x as a cast */
        d = diag_report(a->diag, DL_WARNING, "macro-unparenthesized-param",
                        t->loc,
                        "macro parameter '%s' is an operand of '%s' but is not "
                        "parenthesized; an argument like 'a + b' changes the "
                        "meaning", t->ident->str, spell(a, bad));
        diag_set_range(d, t->loc, t->loc + t->rawlen);
        if (d)
            d->fixit = arena_printf(a->arena, "(%s)", t->ident->str);
    }
}

static bool wrapped_in_parens(Token **v, int n)
{
    int depth = 0, k;
    if (!tok_is_punct(v[0], P_LPAREN))
        return false;
    for (k = 0; k < n; k++) {
        if (tok_is_punct(v[k], P_LPAREN))
            depth++;
        else if (tok_is_punct(v[k], P_RPAREN) && --depth == 0)
            return k == n - 1;
    }
    return false;
}

static void check_body(Analysis *a, Macro *m, Token **v, int n)
{
    int depth = 0, i, top_semis = 0, last_semi = -1;
    bool decl = false, stmt_kw = false, top_brace = false, top_else = false;
    bool binop = false, unbalanced = false, self_ref = false;
    const Token *binop_tok = NULL;
    Diagnostic *d;

    if (n == 0)
        return;
    for (i = 0; i < n; i++) {
        const Token *t = v[i], *prev = i ? v[i - 1] : NULL;
        if (t->kind == TK_IDENT && t->ident == m->name &&
            !(prev && (tok_is_punct(prev, P_DOT) || tok_is_punct(prev, P_ARROW))))
            self_ref = true;
        if (t->kind == TK_PUNCT) {
            switch (t->punct) {
            case P_LPAREN: case P_LBRACKET:
                depth++;
                continue;
            case P_LBRACE:
                if (depth == 0)
                    top_brace = true;
                depth++;
                continue;
            case P_RPAREN: case P_RBRACKET: case P_RBRACE:
                if (--depth < 0) {
                    unbalanced = true;
                    depth = 0;
                }
                continue;
            default:
                break;
            }
        }
        if (depth != 0)
            continue;
        if (tok_is_punct(t, P_SEMI)) {
            top_semis++;
            last_semi = i;
        } else if (is_decl_keyword(t)) {
            decl = true;
        } else if (is_stmt_keyword(t)) {
            stmt_kw = true;
            if (tok_is_ident(t, "else"))
                top_else = true;
        } else if (t->kind == TK_PUNCT && !binop_tok) {
            Punct p = (Punct)t->punct;
            bool binary = is_binary_op(p) &&
                          (p == P_QUESTION ||
                           !(p == P_STAR || p == P_AMP || p == P_PLUS ||
                             p == P_MINUS) ||
                           ends_operand(prev));
            if (binary && i > 0) {
                binop = true;
                binop_tok = t;
            } else if (is_assign(t) && i > 0) {
                binop = true;
                binop_tok = t;
            }
        }
    }
    if (depth != 0)
        unbalanced = true;

    if (unbalanced) {
        diag_report(a->diag, DL_REMARK, "macro-unbalanced", m->name_loc,
                    "replacement list of '%s' has unbalanced delimiters",
                    m->name->str);
        return;
    }
    if (self_ref)
        diag_report(a->diag, DL_REMARK, "macro-self-reference", m->name_loc,
                    "'%s' refers to itself; the inner occurrence is not "
                    "expanded again (C99 6.10.3.4p2)", m->name->str);
    if (decl)
        return; /* declarations and types: out of scope for these checks */

    /* statement-like bodies */
    if (tok_is_ident(v[0], "if") && !top_else) {
        d = diag_report(a->diag, DL_WARNING, "macro-dangling-else", v[0]->loc,
                        "body of '%s' is an 'if' without 'else'; 'if (c) %s; "
                        "else ...' binds the else to the macro's if",
                        m->name->str, macro_signature(a->arena, m));
        diag_note(a->diag, d, v[0]->loc, "wrap the body in 'do { ... } while (0)'");
        return;
    }
    if (tok_is_ident(v[0], "if"))
        return; /* if ... else ...: a single statement */
    if (tok_is_ident(v[0], "do")) {
        if (tok_is_punct(v[n - 1], P_SEMI))
            diag_report(a->diag, DL_WARNING, "macro-trailing-semicolon",
                        v[n - 1]->loc,
                        "do-while(0) body of '%s' ends with ';'; the ';' at "
                        "the call site makes an extra empty statement",
                        m->name->str);
        return;
    }
    if (tok_is_ident(v[0], "case") || tok_is_ident(v[0], "default"))
        return;
    if ((top_semis > 0 && last_semi != n - 1) || top_semis > 1 ||
        (top_brace && tok_is_punct(v[0], P_LBRACE))) {
        d = diag_report(a->diag, DL_WARNING, "macro-multi-statement", v[0]->loc,
                        "body of '%s' is not a single statement; under "
                        "'if (c) %s;' only the first part is conditional",
                        m->name->str, macro_signature(a->arena, m));
        diag_note(a->diag, d, v[0]->loc, "wrap the body in 'do { ... } while (0)'");
        return;
    }
    if (top_semis == 1 && last_semi == n - 1) {
        diag_report(a->diag, DL_WARNING, "macro-trailing-semicolon",
                    v[n - 1]->loc,
                    "body of '%s' ends with ';'; uses like 'if (c) %s; else' "
                    "will not compile", m->name->str,
                    macro_signature(a->arena, m));
        return;
    }
    if (stmt_kw || top_brace)
        return;

    /* expression-like body */
    if (binop && !wrapped_in_parens(v, n)) {
        d = diag_report(a->diag, DL_WARNING, "macro-unparenthesized-body",
                        binop_tok->loc,
                        "replacement list of '%s' has a top-level '%s' but is "
                        "not parenthesized; '%s' in an expression may bind "
                        "differently", m->name->str, spell(a, binop_tok),
                        m->name->str);
        diag_set_range(d, v[0]->loc, v[n - 1]->loc + v[n - 1]->rawlen);
        if (d)
            diag_note(a->diag, d, v[0]->loc, "wrap the replacement list in "
                                             "parentheses");
    }
}

/* ---- listeners ------------------------------------------------------ */

static Token **body_array(Analysis *a, const Macro *m)
{
    Token **v = NEW_ARRAY(a->arena, Token *, m->body_len + 1);
    Token *t;
    int i = 0;
    for (t = m->body; t->kind != TK_EOF; t = t->next)
        v[i++] = t;
    return v;
}

static MacroInfo *info_of(Analysis *a, Macro *m)
{
    MacroInfo *mi = m->user;
    if (!mi) {
        Token **v = body_array(a, m);
        int i;
        mi = NEW(a->arena, MacroInfo);
        mi->evals = NEW_ARRAY(a->arena, int, m->nparams + 1);
        mi->mentions = NEW_ARRAY(a->arena, int, m->nparams + 1);
        m->user = mi;
        for (i = 0; i < m->body_len; i++) {
            int pi = param_index(m, v[i]);
            const Token *prev = i ? v[i - 1] : NULL,
                        *next = i + 1 < m->body_len ? v[i + 1] : NULL;
            if (pi < 0)
                continue;
            if ((prev && (tok_is_punct(prev, P_HASH) ||
                          tok_is_punct(prev, P_HASHHASH))) ||
                (next && tok_is_punct(next, P_HASHHASH))) {
                mi->mentions[pi]++; /* used as text, deliberately */
                continue;
            }
            if (!unevaluated_context(v, i))
                mi->evals[pi]++;
        }
    }
    return mi;
}

static void on_define(void *ctx, Macro *m, Macro *replaced)
{
    Analysis *a = ctx;
    Token **v;
    int i;
    (void)replaced;
    if (m->predefined || !an_user_file(m->file))
        return;
    check_name(a, m);
    v = body_array(a, m);
    if (m->funclike) {
        MacroInfo scratch;
        scratch.evals = NEW_ARRAY(a->arena, int, m->nparams + 1);
        check_params(a, m, v, m->body_len, &scratch);
        for (i = 0; i < m->nparams; i++) {
            Token *t;
            bool used = false;
            for (t = m->body; t->kind != TK_EOF; t = t->next)
                if (t->kind == TK_IDENT && t->ident == m->params[i])
                    used = true;
            if (!used)
                diag_report(a->diag, DL_REMARK, "macro-unused-param",
                            m->param_locs[i],
                            "parameter '%s' of '%s' is never used",
                            m->params[i]->str, m->name->str);
        }
    }
    check_body(a, m, v, m->body_len);
}

typedef enum { SE_NONE, SE_CALL, SE_MODIFY } SideEffect;

static SideEffect side_effects(const Token *arg, const Token **where)
{
    SideEffect r = SE_NONE;
    const Token *t;
    for (t = arg; t->kind != TK_EOF; t = t->next) {
        if (tok_is_punct(t, P_INC) || tok_is_punct(t, P_DEC) || is_assign(t)) {
            *where = t;
            return SE_MODIFY;
        }
        if (t->kind == TK_IDENT && tok_is_punct(t->next, P_LPAREN) &&
            !is_c_keyword(t->text, t->len) && r == SE_NONE &&
            !(t->ident->macro && t->ident->macro->funclike)) {
            *where = t;
            r = SE_CALL;
        }
    }
    return r;
}

static bool already_reported(HygieneState *h, SrcLoc loc)
{
    size_t i;
    for (i = 0; i < h->reported.len; i++)
        if (h->reported.data[i] == loc)
            return true;
    vec_push(&h->reported, loc);
    return false;
}

static void on_expand(void *ctx, Expansion *e, Token **args, int nargs)
{
    Analysis *a = ctx;
    Macro *m = e->macro;
    MacroInfo *mi;
    SrcLoc site;
    int i;
    if (!m->funclike || m->builtin || nargs == 0 || e->in_directive)
        return;
    site = prov_expansion_loc(e->name_prov, e->name_loc);
    if (!an_user_loc(a, site))
        return;
    mi = info_of(a, m);
    for (i = 0; i < nargs && i < m->nparams; i++) {
        const Token *where = NULL;
        SideEffect se = side_effects(args[i], &where);
        Diagnostic *d = NULL;
        SrcLoc loc;
        if (se == SE_NONE)
            continue;
        loc = pp_expansion_loc(args[i]) == site ? args[i]->loc : site;
        if (mi->evals[i] >= 2) {
            if (already_reported(a->hyg, loc))
                continue;
            d = diag_report(a->diag, DL_WARNING,
                            se == SE_MODIFY ? "macro-multi-eval"
                                            : "macro-multi-eval-call",
                            loc,
                            "argument %d of '%s' %s and is evaluated %d times",
                            i + 1, m->name->str,
                            se == SE_MODIFY ? "has side effects"
                                            : "contains a function call",
                            mi->evals[i]);
        } else if (mi->evals[i] == 0 && se == SE_MODIFY &&
                   mi->mentions[i] == 0) {
            if (already_reported(a->hyg, loc))
                continue;
            d = diag_report(a->diag, DL_WARNING, "macro-discarded-side-effect",
                            loc,
                            "argument %d of '%s' has side effects but is never "
                            "evaluated", i + 1, m->name->str);
        }
        if (d) {
            Token *t = args[i];
            while (t->next && t->next->kind != TK_EOF)
                t = t->next;
            if (pp_expansion_loc(args[i]) == site)
                diag_set_range(d, args[i]->loc, t->loc + t->rawlen);
            diag_note(a->diag, d, m->name_loc, "'%s' is defined here",
                      macro_signature(a->arena, m));
        }
    }
}

void hygiene_attach(Analysis *a)
{
    PPListener l;
    memset(&l, 0, sizeof l);
    a->hyg = NEW(a->arena, HygieneState);
    l.ctx = a;
    l.define = on_define;
    l.expand = on_expand;
    pp_add_listener(a->pp, l);
}

void hygiene_finish(Analysis *a)
{
    vec_free(&a->hyg->reported);
}
