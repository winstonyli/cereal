/* parse.c - recursive-descent C99 + GNU parser (parse.h). */
#include "c/parse.h"

#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#include "c/ckw.h"
#include "c/fuzzy.h"
#include "c/lit.h"
#include "c/target.h"

#define NO_TOK UINT32_MAX

static const PTok eof_tok = {{TK_EOF, 0, TF_BOL, 0, 0, 0}, 0, 0};

static bool is_p(const PTok *t, Punct x);

/* ---- tokens ------------------------------------------------------------- */

/* Make toks[i] available; false past the end of the input. */
/* c_lex_one_token's -Wc++-compat: a C++ keyword used as an identifier. */
static void check_cxx_keyword(Parser *p, const Tok *t, size_t i);

static bool fill(Parser *p, size_t i)
{
    while (p->toks.len <= i) {
        PTok pt;
        if (p->src_done)
            return false;
        memset(&pt, 0, sizeof pt);
        if (!p->src(p->src_ctx, &pt.t, &pt.exp)) {
            p->src_done = true;
            return false;
        }
        vec_push(&p->toks, pt);
        if (pt.t.kind == TK_IDENT && !p->unwind)
            check_cxx_keyword(p, &pt.t, p->toks.len - 1);
        if (is_p(&pt, P_LBRACKET)) {    /* '[[': look one token ahead */
            size_t k = p->toks.len;
            if (fill(p, k) && is_p(&p->toks.data[k], P_LBRACKET))
                p->toks.data[k - 1].stdattr = 1;
        }
    }
    return true;
}

/* The first non-pragma token at or after i (pragmas are items of their
 * own, taken where a declaration or statement may start: item_pragmas). */
static uint32_t skip_prag(Parser *p, uint32_t i)
{
    while (fill(p, i) && p->toks.data[i].t.kind == TK_PRAGMA)
        i++;
    return i;
}

/* Index of the current token (== toks.len at the end). */
static uint32_t ci(Parser *p)
{
    return skip_prag(p, p->pos);
}

/* Tokens are returned by value: toks may move when more are read. */
static PTok tok_at(Parser *p, uint32_t i)
{
    if (p->unwind || !fill(p, i))
        return eof_tok;
    return p->toks.data[i];
}

static PTok ct(Parser *p)
{
    return tok_at(p, ci(p));
}

static PTok pk(Parser *p, int k)
{
    uint32_t i = ci(p);
    while (k-- > 0)
        i = skip_prag(p, i + 1);
    return tok_at(p, i);
}

static uint32_t adv(Parser *p)
{
    uint32_t i = ci(p);
    if (!p->unwind && fill(p, i))
        p->pos = i + 1;
    return i;
}

static bool is_p(const PTok *t, Punct x)
{
    return t->t.kind == TK_PUNCT && t->t.punct == x;
}

static bool at(Parser *p, Punct x)
{
    PTok t = ct(p);
    return is_p(&t, x);
}

static bool at_eof(Parser *p)
{
    return ct(p).t.kind == TK_EOF;
}

static bool accept(Parser *p, Punct x)
{
    if (!at(p, x))
        return false;
    adv(p);
    return true;
}

static int ckw_of(Parser *p, const PTok *t)
{
    unsigned k;
    if (t->stdattr)
        return CK_ATTRIBUTE;
    if (t->t.kind != TK_IDENT)
        return CK_NONE;
    k = ident_by_id(p->in, t->t.aux)->ckw;
    if ((k & CKW_GNU_ONLY) && !p->gnu)
        return CK_NONE;
    return (int)(k & 0xFF);
}

static int ckw(Parser *p)
{
    PTok t = ct(p);
    return ckw_of(p, &t);
}

/* An identifier that is not a keyword. */
static bool is_name(Parser *p, const PTok *t)
{
    return t->t.kind == TK_IDENT && !ckw_of(p, t);
}

static bool is_typedef_name(Parser *p, const PTok *t)
{
    return is_name(p, t) && scope_lookup(&p->scope, t->t.aux) == SYM_TYPEDEF;
}

/* ---- diagnostics ---------------------------------------------------------- */

static SrcLoc tok_loc(Parser *p, uint32_t i)
{
    if (i < p->toks.len)
        return p->toks.data[i].exp ? p->toks.data[i].exp
                                   : p->toks.data[i].t.loc;
    return p->toks.len ? p->toks.data[p->toks.len - 1].exp : 0;
}

/* Where gcc reports a token: its spelling (a macro body token at its
 * definition). */
static SrcLoc spell_loc(Parser *p, uint32_t i)
{
    return i < p->toks.len && !p->diag->track0 ? p->toks.data[i].t.loc
                                              : tok_loc(p, i);
}

/* The file of the last token read (the end of input is its end). */
static SrcFile *eof_file(Parser *p)
{
    return p->toks.len ? srcmgr_file_of(p->sm, p->toks.data[p->toks.len - 1].t.loc)
                       : NULL;
}

/* gcc's location of the end-of-input token: the end of the file, printed
 * without a column (c_parser_require reports a missing token there). */
static SrcLoc eof_loc(Parser *p)
{
    SrcFile *f = eof_file(p);
    return f ? f->base + f->size : tok_loc(p, p->toks.len);
}

/* gcc's input_location at the end of input, where c_parser_error reports:
 * the first token of the last line read (cb_line_change). */
static SrcLoc eof_input_loc(Parser *p)
{
    SrcFile *f = eof_file(p);
    uint32_t line, col = 1;
    if (!f)
        return tok_loc(p, p->toks.len);
    srcmgr_linecol(f, p->toks.data[p->toks.len - 1].t.loc, &line, &col);
    {
        SrcLoc ls = srcmgr_loc_of(f, line, 1);
        const char *b = f->buf + (ls - f->base);
        col = 1;
        while (b[col - 1] == ' ' || b[col - 1] == '	')
            col++;
    }
    return srcmgr_loc_of(f, line, col);
}

/* How gcc's c_parse_error names the offending token: " before ..." or
 * " at end of input".  Keywords read like identifiers, punctuators are
 * spelled canonically (digraphs included) and followed by "token". */
static const char *tok_desc(Parser *p, uint32_t i, char *buf, size_t n)
{
    const Tok *t;
    const char *s;
    if (!fill(p, i))
        return " at end of input";
    t = &p->toks.data[i].t;
    s = tok_text_raw(p->sm, p->in, t);
    switch (t->kind) {
    case TK_EOF:
        return " at end of input";
    case TK_PPNUM:
        return " before numeric constant";
    case TK_STRING:
        return " before string constant";
    case TK_PRAGMA:
        return " before '#pragma'";
    case TK_IDENT:
        snprintf(buf, n, " before '%.*s'", (int)t->len, s);
        return buf;
    case TK_CHAR: {
        Lit l;
        unsigned v;
        const char *pre = s[0] == 'L' ? "L" : s[0] == 'U' ? "U"
                        : s[0] == 'u' ? (s[1] == '8' ? "u8" : "u") : "";
        lit_char(&target_x86_64, s, t->len, &l);
        v = (unsigned)l.v;
        if (v <= 255 && v > 32 && v < 127)
            snprintf(buf, n, " before %s'%c'", pre, (int)v);
        else
            snprintf(buf, n, " before %s'\\x%x'", pre, v);
        return buf;
    }
    case TK_PUNCT:
        snprintf(buf, n, " before '%s' token", punct_spelling[t->punct]);
        return buf;
    default:
        snprintf(buf, n, " before '%.*s' token", (int)t->len, s);
        return buf;
    }
}

static Diagnostic *vperr(Parser *p, uint32_t i, SrcLoc loc, const char *fmt,
                         va_list ap)
{
    Diagnostic *d;
    void (*chain)(void *, SrcLoc **, int *) = p->diag->include_chain;
    if (p->unwind || p->hush || (p->have_err && i <= p->last_err))
        return NULL; /* one error per place: no cascades */
    p->have_err = true;
    p->err_live = true;
    p->nerrs++;
    p->last_err = i;
    p->errors++;
    p->diag->include_chain = NULL; /* the preprocessor has moved on */
    d = diag_vreport(p->diag, DL_ERROR, "", loc, fmt, ap);
    p->diag->include_chain = chain;
    if (d && !p->diag->track0 && i < p->toks.len && p->toks.data[i].exp &&
        p->toks.data[i].exp != p->toks.data[i].t.loc) {
        /* a macro body token: gcc names the macro at the invocation (the
         * outermost one; nested expansions are not tracked) */
        const char *s = srcmgr_ptr(p->sm, p->toks.data[i].exp);
        int n = 0;
        while (isalnum((unsigned char)s[n]) || s[n] == '_')
            n++;
        if (n)
            diag_note(p->diag, d, p->toks.data[i].exp,
                      "in expansion of macro '%.*s'", n, s);
    }
    return d;
}

static Diagnostic *perr(Parser *p, uint32_t i, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = vperr(p, i, spell_loc(p, i), fmt, ap);
    va_end(ap);
    return d;
}

static Diagnostic *perr_at(Parser *p, uint32_t i, SrcLoc loc, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = vperr(p, i, loc, fmt, ap);
    va_end(ap);
    return d;
}

/* Like perr, but at the end of the token before i when that is plain
 * source text (gcc's c_parser_require for a missing ';'). */
static Diagnostic *perr_after_prev(Parser *p, uint32_t i, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    SrcLoc loc = spell_loc(p, i);
    if (i > 0 && i - 1 < p->toks.len &&
        (!p->toks.data[i - 1].exp ||
         p->toks.data[i - 1].exp == p->toks.data[i - 1].t.loc))
        loc = p->toks.data[i - 1].t.loc + p->toks.data[i - 1].t.len;
    va_start(ap, fmt);
    d = vperr(p, i, loc, fmt, ap);
    va_end(ap);
    return d;
}

static void check_cxx_keyword(Parser *p, const Tok *t, size_t i)
{
    static const char *const kw[] = {"alignas", "alignof", "bool", "catch",
        "char8_t", "char16_t", "char32_t", "class", "consteval", "constexpr",
        "constinit", "const_cast", "decltype", "delete", "dynamic_cast",
        "explicit", "export", "false", "friend", "mutable", "namespace", "new",
        "noexcept", "nullptr", "operator", "private", "protected", "public",
        "reinterpret_cast", "static_assert", "static_cast", "template", "this",
        "thread_local", "throw", "true", "try", "typename", "typeid", "using",
        "virtual", "concept", "requires", "co_await", "co_yield", "co_return"};
    size_t k, n = t->len;
    const char *s;
    if (n < 3 || n > 16 || !diag_enabled(p->diag, "c++-compat"))
        return;
    s = tok_text_raw(p->sm, p->in, t);
    for (k = 0; k < sizeof kw / sizeof *kw; k++)
        if (strlen(kw[k]) == n && !memcmp(s, kw[k], n)) {
            PTok pt = p->toks.data[i];
            void (*chain)(void *, SrcLoc **, int *) = p->diag->include_chain;
            if (ckw_of(p, &pt))
                return;
            p->diag->include_chain = NULL;
            diag_report(p->diag, DL_WARNING, "c++-compat", tok_loc(p, (uint32_t)i),
                        "identifier '%s' conflicts with C++ keyword", kw[k]);
            p->diag->include_chain = chain;
            return;
        }
}

static void pwarn(Parser *p, uint32_t i, const char *fmt, ...)
{
    va_list ap;
    void (*chain)(void *, SrcLoc **, int *) = p->diag->include_chain;
    if (p->unwind)
        return;
    p->diag->include_chain = NULL;
    va_start(ap, fmt);
    diag_vreport(p->diag, DL_WARNING, "", tok_loc(p, i), fmt, ap);
    va_end(ap);
    p->diag->include_chain = chain;
}

static void expected(Parser *p, const char *what)
{
    char buf[160];
    uint32_t i = ci(p);
    if (!fill(p, i) && p->toks.len) {
        /* c_parser_error at the end of input: input_location */
        va_list none;
        (void)none;
        perr_at(p, i, eof_input_loc(p), "expected %s%s", what,
                tok_desc(p, i, buf, sizeof buf));
        return;
    }
    perr(p, i, "expected %s%s", what, tok_desc(p, i, buf, sizeof buf));
}

/* c_parser_require of a token that never moves "after the previous": at the
 * end of input it is reported at the end-of-file location. */
static void expected_req(Parser *p, const char *what)
{
    char buf[160];
    uint32_t i = ci(p);
    if (!fill(p, i) && p->toks.len)
        perr_at(p, i, eof_loc(p), "expected %s%s", what,
                tok_desc(p, i, buf, sizeof buf));
    else
        expected(p, what);
}

static bool expect(Parser *p, Punct x)
{
    char what[16], buf[160];
    if (accept(p, x))
        return true;
    snprintf(what, sizeof what, "'%s'", punct_spelling[x]);
    /* gcc's c_parser_require puts a missing closing token, ';', ',' or
     * ':' after the previous token, unless an error is already pending */
    if (!p->err_live && (x == P_RPAREN || x == P_RBRACKET || x == P_SEMI ||
                         x == P_COMMA || x == P_COLON))
        perr_after_prev(p, ci(p), "expected %s%s", what,
                        tok_desc(p, ci(p), buf, sizeof buf));
    else if (!fill(p, ci(p)) && p->toks.len)
        perr_at(p, ci(p), eof_loc(p), "expected %s%s", what,
                tok_desc(p, ci(p), buf, sizeof buf));
    else
        expected(p, what);
    return false;
}

/* ---- nodes ---------------------------------------------------------------- */

static uint32_t nmark(Parser *p)
{
    return (uint32_t)p->nodes.len;
}

static void emit(Parser *p, NodeTag tag, uint32_t tok, uint32_t start,
                 unsigned flags)
{
    Node n;
    n.tag = (uint8_t)tag;
    n.flags = (uint8_t)flags;
    n.aux = 0;
    n.tok = tok;
    n.size = (uint32_t)p->nodes.len - start + 1;
    vec_push(&p->nodes, n);
}

static void leaf(Parser *p, NodeTag tag, uint32_t tok)
{
    emit(p, tag, tok, nmark(p), 0);
}

static void set_aux(Parser *p, uint32_t n)
{
    vec_last(&p->nodes).aux = (uint16_t)(n > 0xFFFF ? 0xFFFF : n);
}

/* Scopes, with their markers in the tree (ast.h). */
static void open_scope(Parser *p, uint32_t tok, unsigned flags)
{
    scope_push(&p->scope);
    scope_push(&p->tags);
    emit(p, N_SCOPE, tok, nmark(p), flags);
}

static void close_scope(Parser *p, SymSaveVec *save)
{
    leaf(p, N_SCOPE_END, p->pos ? p->pos - 1 : 0);
    scope_pop(&p->scope, save);
    scope_pop(&p->tags, NULL);
}

/* ---- recovery ------------------------------------------------------------- */

/* Skip to the end of the statement: past a ';' or up to a '}' at this
 * nesting level. */
static void sync_stmt(Parser *p)
{
    int depth = 0;
    p->err_live = false;
    while (!at_eof(p)) {
        PTok t = ct(p);
        if (t.t.kind == TK_PUNCT) {
            if (depth == 0 && t.t.punct == P_SEMI) {
                adv(p);
                break;
            }
            if (depth == 0 && t.t.punct == P_RBRACE)
                break;
            if (t.t.punct == P_LPAREN || t.t.punct == P_LBRACKET ||
                t.t.punct == P_LBRACE)
                depth++;
            else if ((t.t.punct == P_RPAREN || t.t.punct == P_RBRACKET ||
                      t.t.punct == P_RBRACE) && depth > 0)
                depth--;
        }
        adv(p);
    }
}

/* Skip to the end of an external declaration: past a ';' at the top, or
 * past a '}' that closes a brace opened while skipping. */
static void sync_top(Parser *p)
{
    int depth = 0;
    p->err_live = false;
    while (!at_eof(p)) {
        PTok t = ct(p);
        adv(p);
        if (t.t.kind != TK_PUNCT)
            continue;
        if (depth == 0 && t.t.punct == P_SEMI)
            break;
        if (t.t.punct == P_LBRACE)
            depth++;
        else if (t.t.punct == P_RBRACE && (depth == 0 || --depth == 0))
            break;
    }
}

/* Pragmas where an item may start become items. */
static void item_pragmas(Parser *p)
{
    while (!p->unwind && fill(p, p->pos) &&
           p->toks.data[p->pos].t.kind == TK_PRAGMA) {
        leaf(p, N_PRAGMA, p->pos);
        p->pos++;
    }
}

/* ---- forward declarations ------------------------------------------------ */

static void parse_expr(Parser *p);
static void parse_assign(Parser *p);
static void parse_cond(Parser *p);
static void parse_cast(Parser *p);
static void type_name(Parser *p);
static void initializer(Parser *p);
static void compound(Parser *p, bool push);
static void statement(Parser *p);
static void declaration(Parser *p, bool top);
static void attributes(Parser *p);
static void member_declarator(Parser *p);

/* ---- classification -------------------------------------------------------- */

/* Can t start a specifier-qualifier list (a type name)? */
static bool is_type_start(Parser *p, const PTok *t)
{
    switch (ckw_of(p, t)) {
    case CK_CONST: case CK_VOLATILE: case CK_RESTRICT: case CK_ATOMIC:
    case CK_VOID: case CK_CHAR: case CK_SHORT: case CK_INT: case CK_LONG:
    case CK_FLOAT: case CK_DOUBLE: case CK_SIGNED: case CK_UNSIGNED:
    case CK_BOOL: case CK_COMPLEX: case CK_IMAGINARY: case CK_INT128:
    case CK_FLOATN: case CK_DECIMAL: case CK_AUTO_TYPE:
    case CK_STRUCT: case CK_UNION: case CK_ENUM: case CK_TYPEOF:
    case CK_ATTRIBUTE: case CK_ALIGNAS: case CK_GIMPLE:
        return true;
    case CK_NONE:
        return is_typedef_name(p, t);
    default:
        return false;
    }
}

/* A type name after '(' (sizeof, cast): gcc's
 * c_parser_next_tokens_start_typename has no '[[' (attributes there are
 * only for declarations). */
static bool is_typename_start(Parser *p, const PTok *t)
{
    return !t->stdattr && is_type_start(p, t);
}

/* Storage-class specifiers a compound literal may carry (C2X). */
static bool is_clit_storage(Parser *p, const PTok *t)
{
    int k = ckw_of(p, t);
    return k == CK_STATIC || k == CK_REGISTER || k == CK_THREAD_LOCAL;
}

/* Can t start declaration specifiers? */
static bool is_decl_start(Parser *p, const PTok *t)
{
    switch (ckw_of(p, t)) {
    case CK_TYPEDEF: case CK_EXTERN: case CK_STATIC: case CK_AUTO:
    case CK_REGISTER: case CK_THREAD_LOCAL: case CK_INLINE:
    case CK_NORETURN: case CK_STATIC_ASSERT:
        return true;
    default:
        return is_type_start(p, t);
    }
}

/* Where gcc looks for a type: how hard it tries to spot an unknown type
 * name (c_parser_next_tokens_start_typename's lookahead kinds). */
typedef enum {
    LA_ID,          /* never: the identifier is the declared name */
    LA_DECL,        /* `A B`, `A *B`, A not declared */
    LA_DECL_TOP,    /* the same at file scope, whether or not A is declared */
    LA_TYPE         /* a type is required: any undeclared identifier */
} Lookahead;

/* An identifier at token i that gcc reports as an unknown type name. */
static bool unknown_type(Parser *p, uint32_t i, Lookahead la)
{
    PTok t = tok_at(p, i), n;
    SymKind k;
    if (la == LA_ID || !is_name(p, &t))
        return false;
    k = scope_lookup(&p->scope, t.t.aux);
    if (k == SYM_TYPEDEF || (k != SYM_NONE && la != LA_DECL_TOP))
        return false;
    if (la == LA_TYPE)
        return true;
    n = tok_at(p, i + 1);
    return is_name(p, &n) || is_p(&n, P_STAR);
}

/* lookup_name_fuzzy (FUZZY_LOOKUP_TYPENAME): a visible typedef name or a
 * type keyword close to the identifier. */
static const char *fuzzy_typename(Parser *p, const char *goal)
{
    static const char *const kws[] = {
        "_Atomic", "_Bool", "_Complex", "_Decimal128", "_Decimal32",
        "_Decimal64", "_Float128", "_Float128x", "_Float16", "_Float32",
        "_Float32x", "_Float64", "_Float64x", "__int128", "char", "const",
        "double", "enum", "float", "int", "long", "restrict", "short",
        "signed", "struct", "typeof", "union", "unsigned", "void", "volatile"
    };
    Best b;
    size_t k;
    bool res_ok = goal[0] == '_';
    best_init(&b, goal, &p->fuzzy_work);
    for (k = p->scope.log.len; k-- > 0;) {
        const SymEnt *e = &p->scope.log.data[k];
        const Ident *id;
        if (e->kind != SYM_TYPEDEF || p->scope.top[e->ident] == 0 ||
            p->scope.log.data[p->scope.top[e->ident] - 1].kind != SYM_TYPEDEF)
            continue;
        if (p->fuzzy_work > SC_BUDGET)
            return NULL;
        id = ident_by_id(p->in, e->ident);
        if (!res_ok && reserved_name(id->str))
            continue;
        best_consider_n(&b, id->str, id->len);
    }
    for (k = 0; k < sizeof kws / sizeof *kws; k++)
        best_consider(&b, kws[k]);
    return best_get(&b);
}

/* c_parser_declaration_or_fndef's unknown type name error: a tag of that
 * name wants its keyword, else a close spelling is suggested. */
static void unknown_type_error(Parser *p, const PTok *t)
{
    static const char *const kw[] = {"struct", "union", "enum"};
    SymKind k = t->t.aux < p->tags.ntop ? scope_lookup(&p->tags, t->t.aux)
                                        : SYM_NONE;
    const char *name = tok_text_raw(p->sm, p->in, &t->t), *sug;
    int len = (int)t->t.len;
    if (k >= SYM_TAG_STRUCT) {
        perr(p, ci(p), "unknown type name '%.*s'; use '%s' keyword to refer "
             "to the type", len, name, kw[k - SYM_TAG_STRUCT]);
        return;
    }
    {
        char buf[SC_MAXLEN + 1];
        size_t n = t->t.len < SC_MAXLEN ? t->t.len : SC_MAXLEN;
        memcpy(buf, name, n);
        buf[n] = 0;
        sug = fuzzy_typename(p, buf);
    }
    if (sug)
        perr(p, ci(p), "unknown type name '%.*s'; did you mean '%s'?", len,
             name, sug);
    else
        perr(p, ci(p), "unknown type name '%.*s'", len, name);
}

/* Can the token at ci start a declaration (block scope)? */
static bool is_decl_start_la(Parser *p, const PTok *t)
{
    return is_decl_start(p, t) || unknown_type(p, ci(p), LA_DECL);
}

/* Token index just past the attributes starting at i (i if none). */
static uint32_t skip_attrs_ahead(Parser *p, uint32_t i)
{
    for (;;) {
        PTok t = tok_at(p, i);
        int depth = 0;
        if (ckw_of(p, &t) != CK_ATTRIBUTE)
            return i;
        if (t.stdattr) {                /* [[ ... ]] */
            for (;;) {
                t = tok_at(p, i);
                if (t.t.kind == TK_EOF)
                    return i;
                if (is_p(&t, P_LBRACKET))
                    depth++;
                else if (is_p(&t, P_RBRACKET) && --depth <= 0) {
                    i = skip_prag(p, i + 1);
                    break;
                }
                i = skip_prag(p, i + 1);
            }
            continue;
        }
        i = skip_prag(p, i + 1);
        for (;;) {
            t = tok_at(p, i);
            if (t.t.kind == TK_EOF)
                return i;
            if (is_p(&t, P_LPAREN))
                depth++;
            else if (is_p(&t, P_RPAREN) && --depth <= 0) {
                i = skip_prag(p, i + 1);
                break;
            }
            i = skip_prag(p, i + 1);
        }
    }
}

/* Source column 0 (for brace recovery). */
static bool at_col0(Parser *p, uint32_t i)
{
    const PTok *t;
    SrcLoc loc;
    char c;
    if (!fill(p, i))
        return false;
    t = &p->toks.data[i];
    if (!(t->t.flags & TF_BOL))
        return false;
    loc = t->exp ? t->exp : t->t.loc;
    if (!loc)
        return false;
    c = *srcmgr_ptr(p->sm, loc - 1);
    return c == '\n' || c == '\r' || c == 0;
}

/* ---- attributes, asm ---------------------------------------------------- */

/* The standard attributes gcc 13 knows (an unknown one's arguments are
 * skipped as balanced tokens, not parsed). */
static bool std_attr_known(Parser *p, uint32_t tok)
{
    static const char *const known[] = {"deprecated", "fallthrough",
        "maybe_unused", "nodiscard", "noreturn", "_Noreturn"};
    PTok t = tok_at(p, tok);
    const Ident *id = ident_by_id(p->in, t.t.aux);
    const char *s = id->str;
    size_t n = id->len, k;
    if (n > 4 && !strncmp(s, "__", 2) && !strcmp(s + n - 2, "__")) {
        s += 2;
        n -= 4;
    }
    for (k = 0; k < sizeof known / sizeof *known; k++)
        if (strlen(known[k]) == n && !strncmp(s, known[k], n))
            return true;
    return false;
}

/* [[ name [(args)] , ... ]] (no 'ns::' before C2X) */
static void std_attribute(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    if (!expect(p, P_LBRACKET)) {
        emit(p, N_ATTRIBUTE, kw, start, NF_ERROR);
        return;
    }
    while (!at(p, P_RBRACKET) && !at_eof(p)) {
        PTok t = ct(p);
        if (t.t.kind == TK_IDENT) {
            uint32_t s = nmark(p), name = adv(p);
            if (accept(p, P_LPAREN)) {
                if (!std_attr_known(p, name)) {
                    int depth = 0;      /* c_parser_balanced_token_sequence */
                    while (!at_eof(p) && !(depth == 0 && at(p, P_RPAREN))) {
                        if (at(p, P_LPAREN) || at(p, P_LBRACKET) ||
                            at(p, P_LBRACE))
                            depth++;
                        else if (at(p, P_RPAREN) || at(p, P_RBRACKET) ||
                                 at(p, P_RBRACE))
                            depth--;
                        adv(p);
                    }
                } else if (!at(p, P_RPAREN)) {
                    parse_assign(p);
                    while (accept(p, P_COMMA))
                        parse_assign(p);
                }
                expect(p, P_RPAREN);
            }
            emit(p, N_ATTR_ITEM, name, s, 0);
        } else if (!is_p(&t, P_COMMA)) {
            expected(p, "attribute name");
            break;
        }
        if (!accept(p, P_COMMA))
            break;
    }
    if (!at(p, P_RBRACKET)) {
        /* gcc: c_parser_skip_until_found(']'), balancing brackets */
        int depth = 0;
        bool hush = p->hush;
        expect(p, P_RBRACKET);
        while (!at_eof(p) && !at(p, P_SEMI) && !at(p, P_RBRACE) &&
               !at(p, P_LBRACE)) {
            if (at(p, P_LPAREN) || at(p, P_LBRACKET))
                depth++;
            else if (at(p, P_RPAREN))
                depth--;
            else if (at(p, P_RBRACKET) && depth-- <= 0) {
                adv(p);
                break;
            }
            adv(p);
        }
        p->hush = true;                 /* parser->error is set */
        expect(p, P_RBRACKET);
        p->hush = hush;
        emit(p, N_ATTRIBUTE, kw, start, 0);
        return;
    } else {
        expect(p, P_RBRACKET);
    }
    expect(p, P_RBRACKET);
    emit(p, N_ATTRIBUTE, kw, start, 0);
}

static void attribute(Parser *p)
{
    uint32_t start, kw;
    if (ct(p).stdattr) {
        std_attribute(p);
        return;
    }
    start = nmark(p);
    kw = adv(p);
    if (!expect(p, P_LPAREN) || !expect(p, P_LPAREN)) {
        emit(p, N_ATTRIBUTE, kw, start, NF_ERROR);
        return;
    }
    while (!at(p, P_RPAREN) && !at_eof(p)) {
        PTok t = ct(p);
        if (t.t.kind == TK_IDENT) {
            uint32_t s = nmark(p), name = adv(p);
            if (accept(p, P_LPAREN)) {
                if (!at(p, P_RPAREN)) {
                    parse_assign(p);
                    while (accept(p, P_COMMA))
                        parse_assign(p);
                }
                expect(p, P_RPAREN);
            }
            emit(p, N_ATTR_ITEM, name, s, 0);
        } else if (!is_p(&t, P_COMMA)) {
            expected(p, "attribute name");
            break;
        }
        if (!accept(p, P_COMMA))
            break;
    }
    expect(p, P_RPAREN);
    expect(p, P_RPAREN);
    emit(p, N_ATTRIBUTE, kw, start, 0);
}

static void attributes(Parser *p)
{
    while (ckw(p) == CK_ATTRIBUTE)
        attribute(p);
}

static void string_lit(Parser *p)
{
    uint32_t first = ci(p), n = 0;
    if (ct(p).t.kind != TK_STRING) {
        expected(p, "string literal");
        leaf(p, N_ERROR, first);
        return;
    }
    while (ct(p).t.kind == TK_STRING) {
        adv(p);
        n++;
    }
    leaf(p, N_STRING, first);
    set_aux(p, n);
}

/* asm-label: asm ( string ) */
static void asm_label(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    expect(p, P_LPAREN);
    string_lit(p);
    expect(p, P_RPAREN);
    emit(p, N_ASM_LABEL, kw, start, 0);
}

/* asm [qualifiers] ( string [: outputs [: inputs [: clobbers [: labels]]]] ) */
static void asm_stmt(Parser *p, bool top)
{
    uint32_t start = nmark(p), kw = adv(p);
    int section;
    uint32_t seen[3] = { 0, 0, 0 };     /* volatile, inline, goto: token + 1 */
    for (;;) {
        int k = ckw(p);
        int q = k == CK_VOLATILE ? 0 : k == CK_INLINE ? 1 : 2;
        if (k != CK_VOLATILE && k != CK_INLINE && k != CK_GOTO)
            break;
        if (seen[q]) {
            Diagnostic *d = perr(p, ci(p), "duplicate 'asm' qualifier '%s'",
                                 q == 0 ? "volatile" : q == 1 ? "inline"
                                                              : "goto");
            if (d)
                diag_note(p->diag, d, tok_loc(p, seen[q] - 1),
                          "first seen here");
        } else {
            seen[q] = ci(p) + 1;
        }
        leaf(p, N_QUAL, adv(p));
    }
    if (!expect(p, P_LPAREN)) {
        sync_stmt(p);
        emit(p, N_ASM, kw, start, NF_ERROR);
        return;
    }
    string_lit(p);
    for (section = 1; section <= 4; section++) {
        uint32_t s = nmark(p), colon = ci(p);
        if (!accept(p, P_COLON))
            break;
        while (!at(p, P_COLON) && !at(p, P_RPAREN) && !at_eof(p)) {
            if (section <= 2) {
                uint32_t o = nmark(p), c;
                if (accept(p, P_LBRACKET)) {
                    leaf(p, N_NAME, adv(p));
                    expect(p, P_RBRACKET);
                }
                c = ci(p);
                string_lit(p);
                expect(p, P_LPAREN);
                parse_expr(p);
                expect(p, P_RPAREN);
                emit(p, N_ASM_OPERAND, c, o, 0);
            } else if (section == 3) {
                string_lit(p);
            } else {
                PTok t = ct(p);
                if (!is_name(p, &t)) {
                    expected(p, "label");
                    break;
                }
                leaf(p, N_NAME, adv(p));
            }
            if (!accept(p, P_COMMA))
                break;
        }
        emit(p, N_ASM_SECTION, colon, s, 0);
        set_aux(p, (uint32_t)section);
    }
    expect(p, P_RPAREN);
    if (!expect(p, P_SEMI))
        sync_stmt(p);
    emit(p, top ? N_TOP_ASM : N_ASM, kw, start, 0);
}

/* ---- declaration specifiers ------------------------------------------- */

typedef struct Specs {
    bool any;                   /* something was there */
    bool type;                  /* a type specifier */
    bool is_typedef;
    bool gimple;                /* __GIMPLE: the body is not C */
} Specs;

static void struct_spec(Parser *p);
static void enum_spec(Parser *p);

/* _Alignas ( type-name | constant-expression ), typeof (...) */
static void paren_type_or_expr(Parser *p, NodeTag tag)
{
    uint32_t start = nmark(p), kw = adv(p);
    PTok t;
    if (!expect(p, P_LPAREN)) {
        emit(p, tag, kw, start, NF_ERROR);
        return;
    }
    t = ct(p);
    if (is_type_start(p, &t))
        type_name(p);
    else
        parse_expr(p);
    expect(p, P_RPAREN);
    emit(p, tag, kw, start, 0);
}

static void specs(Parser *p, Specs *s, Lookahead la)
{
    uint32_t start = nmark(p), first = ci(p);
    memset(s, 0, sizeof *s);
    for (;;) {
        PTok t = ct(p);
        int k = ckw_of(p, &t);
        switch (k) {
        case CK_TYPEDEF:
            s->is_typedef = true;
            /* fallthrough */
        case CK_EXTERN: case CK_STATIC: case CK_AUTO: case CK_REGISTER:
        case CK_THREAD_LOCAL:
            leaf(p, N_STORAGE, adv(p));
            break;
        case CK_CONST: case CK_VOLATILE: case CK_RESTRICT:
            leaf(p, N_QUAL, adv(p));
            break;
        case CK_ATOMIC: {
            PTok n = pk(p, 1);
            if (is_p(&n, P_LPAREN)) {
                uint32_t s0 = nmark(p), kw = adv(p);
                adv(p);
                type_name(p);
                expect(p, P_RPAREN);
                emit(p, N_ATOMIC_TYPE, kw, s0, 0);
                s->type = true;
            } else {
                leaf(p, N_QUAL, adv(p));
            }
            break;
        }
        case CK_INLINE: case CK_NORETURN:
            leaf(p, N_FUNCSPEC, adv(p));
            break;
        case CK_GIMPLE:
            /* gcc diagnoses it without -fgimple and goes on; the pass
             * list is skipped and the body is not parsed as C */
            perr(p, ci(p), "'__GIMPLE' only valid with '-fgimple'");
            adv(p);
            s->gimple = true;
            if (at(p, P_LPAREN)) {
                int depth = 0;
                do {
                    if (at(p, P_LPAREN))
                        depth++;
                    else if (at(p, P_RPAREN))
                        depth--;
                    adv(p);
                } while (depth > 0 && !at_eof(p));
            }
            break;
        case CK_VOID: case CK_CHAR: case CK_SHORT: case CK_INT: case CK_LONG:
        case CK_FLOAT: case CK_DOUBLE: case CK_SIGNED: case CK_UNSIGNED:
        case CK_BOOL: case CK_COMPLEX: case CK_IMAGINARY: case CK_INT128:
        case CK_FLOATN: case CK_DECIMAL: case CK_AUTO_TYPE:
            leaf(p, N_TYPESPEC, adv(p));
            s->type = true;
            break;
        case CK_STRUCT: case CK_UNION:
            struct_spec(p);
            s->type = true;
            break;
        case CK_ENUM:
            enum_spec(p);
            s->type = true;
            break;
        case CK_TYPEOF:
            paren_type_or_expr(p, N_TYPEOF);
            s->type = true;
            break;
        case CK_ALIGNAS:
            paren_type_or_expr(p, N_ALIGNAS);
            break;
        case CK_ATTRIBUTE:
            attribute(p);
            break;
        case CK_EXTENSION:
            adv(p);
            break;
        case CK_NONE:
            /* a typedef name is a type specifier only where no other
             * type specifier came before: `T T;` declares a T */
            if (!s->type && is_typedef_name(p, &t)) {
                leaf(p, N_TYPEDEF_NAME, adv(p));
                s->type = true;
                break;
            }
            if (!s->type && unknown_type(p, ci(p), la)) {
                /* as gcc: diagnosed, then parsed as if it were a type */
                unknown_type_error(p, &t);
                emit(p, N_TYPEDEF_NAME, adv(p), nmark(p), NF_ERROR);
                s->type = true;
                break;
            }
            goto done;
        default:
            goto done;
        }
        s->any = true;
    }
done:
    emit(p, N_SPECS, first, start, 0);
}

static void member_decl(Parser *p)
{
    uint32_t start = nmark(p), first = ci(p);
    Specs s;
    specs(p, &s, LA_DECL);
    if (!s.any) {
        expected(p, "specifier-qualifier-list");
        sync_stmt(p);
        emit(p, N_MEMBER_DECL, first, start, NF_ERROR);
        return;
    }
    if (!at(p, P_SEMI) && !at(p, P_RBRACE))
        for (;;) {
            uint32_t m = nmark(p), mfirst = ci(p);
            unsigned flags = 0;
            PTok t;
            if (!at(p, P_COLON))
                member_declarator(p); /* names are members, not in scope */
            t = ct(p);
            if (!(at(p, P_COLON) || at(p, P_COMMA) || at(p, P_SEMI) ||
                  at(p, P_RBRACE) ||
                  (ckw_of(p, &t) == CK_ATTRIBUTE && !t.stdattr))) {
                expected(p, "':', ',', ';', '}' or '__attribute__'");
                emit(p, N_MEMBER, mfirst, m, flags);
                break;
            }
            if (accept(p, P_COLON)) {
                flags |= NF_BITFIELD;
                parse_cond(p);
            }
            attributes(p);
            emit(p, N_MEMBER, mfirst, m, flags);
            if (accept(p, P_COMMA))
                continue;
            if (!at(p, P_SEMI) && !at(p, P_RBRACE))
                expected(p, "',', ';' or '}'");
            break;
        }
    if (at(p, P_RBRACE)) { /* GCC: a warning */
        pwarn(p, ci(p), "no semicolon at end of struct or union");
    } else if (!expect(p, P_SEMI)) {
        sync_stmt(p);
        emit(p, N_MEMBER_DECL, first, start, NF_ERROR);
        return;
    }
    emit(p, N_MEMBER_DECL, first, start, 0);
}

static void static_assert_decl(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    if (!expect(p, P_LPAREN)) {
        sync_stmt(p);
        emit(p, N_STATIC_ASSERT, kw, start, NF_ERROR);
        return;
    }
    parse_cond(p);
    if (accept(p, P_COMMA))
        string_lit(p);
    expect(p, P_RPAREN);
    if (!expect(p, P_SEMI))
        sync_stmt(p);
    emit(p, N_STATIC_ASSERT, kw, start, 0);
}

/* A tag is declared where first seen (a use declares it too); a visible
 * one of any kind is left alone. */
static void tag_declare(Parser *p, uint32_t ident, SymKind kind)
{
    if (scope_lookup(&p->tags, ident) == SYM_NONE)
        scope_declare(&p->tags, ident, kind);
}

static void struct_spec(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    unsigned flags = 0;
    PTok t;
    attributes(p);
    t = ct(p);
    if (is_name(p, &t)) {
        tag_declare(p, t.t.aux, ckw_of(p, &p->toks.data[kw]) == CK_UNION
                                    ? SYM_TAG_UNION : SYM_TAG_STRUCT);
        leaf(p, N_TAG, adv(p));
    }
    attributes(p);
    if (at(p, P_LBRACE)) {
        leaf(p, N_OPEN, adv(p));
        set_aux(p, ckw_of(p, &p->toks.data[kw]) == CK_UNION);
        flags |= NF_BODY;
        for (;;) {
            item_pragmas(p);
            if (at(p, P_RBRACE) || at_eof(p))
                break;
            if (accept(p, P_SEMI))
                continue; /* GNU: stray ';' */
            if (ckw(p) == CK_STATIC_ASSERT)
                static_assert_decl(p);
            else
                member_decl(p);
        }
        if (!expect(p, P_RBRACE))
            flags |= NF_ERROR;
        attributes(p);
    } else if (!is_name(p, &t)) {
        expected(p, "'{'");
        flags |= NF_ERROR;
    }
    emit(p, N_STRUCT, kw, start, flags);
}

static void enum_spec(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    unsigned flags = 0;
    PTok t;
    attributes(p);
    t = ct(p);
    if (is_name(p, &t)) {
        tag_declare(p, t.t.aux, SYM_TAG_ENUM);
        leaf(p, N_TAG, adv(p));
    }
    attributes(p);
    {   /* enum e : type (C2X underlying type) */
        PTok n1 = pk(p, 1);
        if (at(p, P_COLON) && is_type_start(p, &n1)) {
            adv(p);
            type_name(p);
        }
    }
    if (at(p, P_LBRACE)) {
        leaf(p, N_OPEN, adv(p));
        set_aux(p, 2);
        flags |= NF_BODY;
        if (at(p, P_RBRACE)) {
            /* gcc: "nicer error for enum {}" */
            perr(p, ci(p), "empty enum is invalid");
            flags |= NF_ERROR;
        }
        while (!at(p, P_RBRACE) && !at_eof(p)) {
            PTok e = ct(p);
            uint32_t s, name;
            if (!is_name(p, &e)) {
                expected(p, "enumerator");
                sync_stmt(p);
                flags |= NF_ERROR;
                break;
            }
            s = nmark(p);
            name = adv(p);
            attributes(p);
            if (accept(p, P_ASSIGN))
                parse_cond(p);
            emit(p, N_ENUMERATOR, name, s, 0);
            /* in scope after its enumerator (value included) */
            scope_declare(&p->scope, e.t.aux, SYM_ORDINARY);
            if (!accept(p, P_COMMA))
                break;
        }
        if (!expect(p, P_RBRACE))
            flags |= NF_ERROR;
        attributes(p);
    } else if (!is_name(p, &t)) {
        expected(p, "'{'");
        flags |= NF_ERROR;
    }
    emit(p, N_ENUM, kw, start, flags);
}

/* ---- declarators ---------------------------------------------------------- */

enum { DK_NONE, DK_PTR, DK_ARRAY, DK_FUNC };
enum { DCL_NAMED, DCL_ABSTRACT, DCL_EITHER };

typedef struct DeclInfo {
    uint32_t name;              /* token of the name, NO_TOK if none */
    uint8_t inner;              /* the derivation next to the name */
    bool kr;                    /* ... a K&R identifier list */
    uint32_t save_start, save_len; /* ... its parameters (p->saved) */
    bool any;                   /* produced nodes */
    bool failed;                /* an error in a parameter list: gcc gives up
                                   on the declarator */
} DeclInfo;

static void declarator(Parser *p, int mode, DeclInfo *di);
static void declarator_init(DeclInfo *d);

/* After '(' in a declarator that may be abstract: parameters (not a
 * nested declarator)?  C99 6.7.5.3p11: a typedef name there is a type. */
static bool paren_is_params(Parser *p)
{
    uint32_t i = skip_attrs_ahead(p, skip_prag(p, ci(p) + 1));
    PTok t = tok_at(p, i);
    if (is_p(&t, P_RPAREN) || is_p(&t, P_ELLIPSIS))
        return true;
    if (ckw_of(p, &t) == CK_ATTRIBUTE)
        return false;
    return is_decl_start(p, &t);
}

/* ( parameter-type-list | identifier-list ) after the '(' */
static void params(Parser *p, unsigned *flags)
{
    PTok t = ct(p), n = pk(p, 1);
    uint32_t list0;
    if (is_p(&t, P_RPAREN))
        return;
    if (is_name(p, &t) && !is_typedef_name(p, &t) &&
        (is_p(&n, P_COMMA) || is_p(&n, P_RPAREN))) {
        *flags |= NF_KR;
        for (;;) {
            PTok x = ct(p);
            if (!is_name(p, &x)) {
                expected(p, "identifier");
                break;
            }
            leaf(p, N_KR_IDENT, adv(p));
            scope_declare(&p->scope, x.t.aux, SYM_ORDINARY);
            if (!accept(p, P_COMMA))
                break;
        }
        return;
    }
    list0 = nmark(p);
    for (;;) {
        uint32_t s = nmark(p), first = ci(p);
        Specs sp;
        DeclInfo d;
        if (accept(p, P_ELLIPSIS)) {
            *flags |= NF_VARIADIC;
            break;
        }
        specs(p, &sp, LA_TYPE);
        if (!sp.any) {
            expected(p, "declaration specifiers or '...'");
            p->nodes.len = s;
            break;
        }
        declarator_init(&d);
        declarator(p, DCL_EITHER, &d);
        attributes(p);
        if (d.name != NO_TOK)
            scope_declare(&p->scope, p->toks.data[d.name].t.aux,
                          sp.is_typedef ? SYM_TYPEDEF : SYM_ORDINARY);
        emit(p, N_PARAM, first, s, 0);
        if (at(p, P_SEMI)) {
            /* GNU forward declarations: everything so far in this list */
            uint32_t i = nmark(p);
            p->nodes.data[i - 1].flags |= NF_SEMI;
            while (i > list0) {
                Node *nd = &p->nodes.data[i - 1];
                nd->flags |= NF_FWD;
                i -= nd->size;
            }
            adv(p);
            attributes(p);
            if (at(p, P_RPAREN))
                break;
            continue;
        }
        if (!accept(p, P_COMMA))
            break;
    }
}

static void direct_declarator(Parser *p, int mode, DeclInfo *di)
{
    uint32_t start = nmark(p);
    PTok t = ct(p);
    if (is_name(p, &t) && mode != DCL_ABSTRACT) {
        di->name = adv(p);
        leaf(p, N_NAME, di->name);
        di->any = true;
    } else if (is_p(&t, P_LPAREN) &&
               (mode == DCL_NAMED || !paren_is_params(p))) {
        adv(p);
        attributes(p);
        declarator(p, mode, di);
        expect(p, P_RPAREN);
    }
    for (;;) {
        if (ct(p).stdattr) {            /* int f [[attr]] (void) */
            attribute(p);
            continue;
        }
        if (at(p, P_LBRACKET)) {
            uint32_t lb = adv(p);
            unsigned flags = 0;
            for (;;) {
                int k = ckw(p);
                if (k == CK_STATIC) {
                    adv(p);
                    flags |= NF_STATIC;
                } else if (k == CK_CONST || k == CK_VOLATILE ||
                           k == CK_RESTRICT || k == CK_ATOMIC) {
                    leaf(p, N_QUAL, adv(p));
                } else if (k == CK_ATTRIBUTE) {
                    attribute(p);
                } else {
                    break;
                }
            }
            if (at(p, P_STAR) && (t = pk(p, 1), is_p(&t, P_RBRACKET))) {
                adv(p);
                flags |= NF_STAR;
            } else if (!at(p, P_RBRACKET)) {
                parse_assign(p);
            }
            expect(p, P_RBRACKET);
            emit(p, N_ARRAY, lb, start, flags);
            if (di->inner == DK_NONE)
                di->inner = DK_ARRAY;
            di->any = true;
        } else if (at(p, P_LPAREN)) {
            uint32_t lp = adv(p), save = (uint32_t)p->saved.len;
            unsigned flags = 0;
            bool adjacent = di->inner == DK_NONE;
            uint32_t nerrs = p->nerrs;
            open_scope(p, lp, 0); /* function prototype scope */
            params(p, &flags);
            /* only this declarator's own parameters are saved (not
             * those of a function declarator among them) */
            p->saved.len = save;
            close_scope(p, adjacent ? &p->saved : NULL);
            expect(p, P_RPAREN);
            if (p->nerrs != nerrs)
                di->failed = true;
            emit(p, N_FUNC, lp, start, flags);
            if (adjacent) {
                di->inner = DK_FUNC;
                di->kr = (flags & NF_KR) != 0;
                di->save_start = save;
                di->save_len = (uint32_t)p->saved.len - save;
            }
            di->any = true;
        } else {
            break;
        }
    }
}

static void declarator(Parser *p, int mode, DeclInfo *di)
{
    if (at(p, P_STAR)) {
        uint32_t start = nmark(p), star = adv(p);
        for (;;) {
            int k = ckw(p);
            if (k == CK_CONST || k == CK_VOLATILE || k == CK_RESTRICT ||
                k == CK_ATOMIC)
                leaf(p, N_QUAL, adv(p));
            else if (k == CK_ATTRIBUTE)
                attribute(p);
            else
                break;
        }
        declarator(p, mode, di);
        emit(p, N_PTR, star, start, 0);
        if (di->inner == DK_NONE)
            di->inner = DK_PTR;
        di->any = true;
        return;
    }
    direct_declarator(p, mode, di);
}

static void declarator_init(DeclInfo *d)
{
    memset(d, 0, sizeof *d);
    d->name = NO_TOK;
}

static void member_declarator(Parser *p)
{
    DeclInfo d;
    declarator_init(&d);
    declarator(p, DCL_NAMED, &d);
    if (d.name == NO_TOK)
        expected(p, "identifier or '('");
}

static void type_name(Parser *p)
{
    uint32_t start = nmark(p), first = ci(p);
    Specs s;
    DeclInfo d;
    specs(p, &s, LA_TYPE);
    if (!s.any)
        expected(p, "type name");
    declarator_init(&d);
    declarator(p, DCL_ABSTRACT, &d);
    emit(p, N_TYPE_NAME, first, start, 0);
}

/* ---- initializers ------------------------------------------------------ */

static void init_list(Parser *p)
{
    uint32_t start = nmark(p), lb = adv(p);
    unsigned flags = 0;
    while (!at(p, P_RBRACE) && !at_eof(p)) {
        uint32_t s = nmark(p), first = ci(p);
        bool desig = false, old = false;
        int nd = 0;
        bool dot = false;
        for (;;) {
            PTok t = ct(p), n;
            if (is_p(&t, P_DOT)) {
                dot = true;
                adv(p);
                t = ct(p);
                if (t.t.kind != TK_IDENT) {
                    expected(p, "field name");
                    break;
                }
                leaf(p, N_DESIG_FIELD, adv(p));
            } else if (is_p(&t, P_LBRACKET)) {
                uint32_t d = nmark(p), lbk = adv(p);
                parse_cond(p);
                if (accept(p, P_ELLIPSIS)) {
                    parse_cond(p);
                    expect(p, P_RBRACKET);
                    emit(p, N_DESIG_RANGE, lbk, d, 0);
                } else {
                    expect(p, P_RBRACKET);
                    emit(p, N_DESIG_INDEX, lbk, d, 0);
                }
            } else if (!desig && is_name(p, &t) &&
                       (n = pk(p, 1), is_p(&n, P_COLON))) {
                leaf(p, N_DESIG_FIELD, adv(p)); /* GNU: field: value */
                adv(p);
                old = true;
                desig = true;
                break;
            } else {
                break;
            }
            desig = true;
            nd++;
        }
        if (desig && !old && !accept(p, P_ASSIGN) && (dot || nd > 1)) {
            /* only a single array designator may omit the '=' */
            uint32_t at0 = ci(p);
            int depth = 0;
            expected(p, "'='");
            while (!at_eof(p)) {
                PTok t = ct(p);
                if (t.t.kind == TK_PUNCT) {
                    if (depth == 0 && (t.t.punct == P_COMMA ||
                                       t.t.punct == P_RBRACE))
                        break;
                    if (t.t.punct == P_LPAREN || t.t.punct == P_LBRACKET ||
                        t.t.punct == P_LBRACE)
                        depth++;
                    else if ((t.t.punct == P_RPAREN ||
                              t.t.punct == P_RBRACKET ||
                              t.t.punct == P_RBRACE) && depth > 0)
                        depth--;
                }
                adv(p);
            }
            p->err_live = false;
            leaf(p, N_ERROR, at0);
            flags |= NF_ERROR;
        } else {
            initializer(p);
        }
        if (desig)
            emit(p, N_DESIGNATED, first, s, 0);
        if (!accept(p, P_COMMA))
            break;
    }
    if (!expect(p, P_RBRACE)) {
        flags |= NF_ERROR;
        sync_stmt(p);
    }
    emit(p, N_INIT_LIST, lb, start, flags);
}

static void initializer(Parser *p)
{
    if (at(p, P_LBRACE))
        init_list(p);
    else
        parse_assign(p);
}

/* ---- expressions ----------------------------------------------------------- */

static int binary_prec(const PTok *t)
{
    if (t->t.kind != TK_PUNCT)
        return 0;
    switch (t->t.punct) {
    case P_OROR: return 4;
    case P_ANDAND: return 5;
    case P_PIPE: return 6;
    case P_CARET: return 7;
    case P_AMP: return 8;
    case P_EQEQ: case P_NE: return 9;
    case P_LT: case P_GT: case P_LE: case P_GE: return 10;
    case P_SHL: case P_SHR: return 11;
    case P_PLUS: case P_MINUS: return 12;
    case P_STAR: case P_SLASH: case P_PERCENT: return 13;
    default: return 0;
    }
}

static bool is_assign_op(const PTok *t)
{
    if (t->t.kind != TK_PUNCT)
        return false;
    switch (t->t.punct) {
    case P_ASSIGN: case P_MUL_ASSIGN: case P_DIV_ASSIGN: case P_MOD_ASSIGN:
    case P_ADD_ASSIGN: case P_SUB_ASSIGN: case P_SHL_ASSIGN:
    case P_SHR_ASSIGN: case P_AND_ASSIGN: case P_XOR_ASSIGN:
    case P_OR_ASSIGN:
        return true;
    default:
        return false;
    }
}

static void postfix_tail(Parser *p, uint32_t start)
{
    for (;;) {
        PTok t = ct(p);
        if (t.t.kind != TK_PUNCT)
            return;
        /* an erroneous primary: gcc's pending error silences the rest of
         * the postfix expression */
        if (p->nodes.len && p->err_live &&
            p->nodes.data[p->nodes.len - 1].tag == N_ERROR && !p->hush) {
            p->hush = true;
            p->hushed = true;
            postfix_tail(p, start);
            p->hush = false;
            return;
        }
        switch (t.t.punct) {
        case P_LBRACKET: {
            uint32_t lb = adv(p);
            parse_expr(p);
            expect(p, P_RBRACKET);
            emit(p, N_INDEX, lb, start, 0);
            break;
        }
        case P_LPAREN: {
            uint32_t lp = adv(p);
            if (!at(p, P_RPAREN)) {
                parse_assign(p);
                while (accept(p, P_COMMA))
                    parse_assign(p);
            }
            expect(p, P_RPAREN);
            emit(p, N_CALL, lp, start, 0);
            break;
        }
        case P_DOT: case P_ARROW: {
            PTok f;
            adv(p);
            f = ct(p);
            if (f.t.kind != TK_IDENT) {
                expected(p, "field name");
                emit(p, N_MEMBER_EXPR, ci(p), start, NF_ERROR);
                return;
            }
            emit(p, N_MEMBER_EXPR, adv(p), start,
                 t.t.punct == P_ARROW ? NF_ARROW : 0);
            break;
        }
        case P_INC: case P_DEC:
            emit(p, N_POSTFIX, adv(p), start, 0);
            break;
        default:
            return;
        }
    }
}

/* __builtin_offsetof ( type-name , member-designator ) */
static void offsetof_expr(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    PTok t;
    expect(p, P_LPAREN);
    type_name(p);
    expect(p, P_COMMA);
    t = ct(p);
    if (t.t.kind == TK_IDENT)
        leaf(p, N_NAME, adv(p));
    else
        expected(p, "field name");
    for (;;) {
        if (at(p, P_DOT) || at(p, P_ARROW)) {
            bool arrow = at(p, P_ARROW);    /* gcc: 'a->b' is 'a[0].b' */
            uint32_t d = nmark(p);
            adv(p);
            t = ct(p);
            if (t.t.kind != TK_IDENT) {
                expected(p, "field name");
                break;
            }
            emit(p, N_DESIG_FIELD, adv(p), d, arrow);
        } else if (at(p, P_LBRACKET)) {
            uint32_t d = nmark(p), lb = adv(p);
            parse_expr(p);
            expect(p, P_RBRACKET);
            emit(p, N_DESIG_INDEX, lb, d, 0);
        } else {
            break;
        }
    }
    expect(p, P_RPAREN);
    emit(p, N_OFFSETOF, kw, start, 0);
}

/* builtin ( expr , type-name ) / ( type-name , type-name ) */
static void builtin2(Parser *p, NodeTag tag, bool first_type)
{
    uint32_t start = nmark(p), kw = adv(p);
    expect(p, P_LPAREN);
    if (first_type)
        type_name(p);
    else
        parse_assign(p);
    expect(p, P_COMMA);
    type_name(p);
    expect(p, P_RPAREN);
    emit(p, tag, kw, start, 0);
}

/* __builtin_has_attribute ( expr | type-name , attribute ) */
static void has_attr_expr(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    PTok n = ct(p), n1 = pk(p, 1);
    expect(p, P_LPAREN);
    n = ct(p);
    if (is_typename_start(p, &n) || is_clit_storage(p, &n))
        type_name(p);
    else
        parse_assign(p);
    (void)n1;
    expect(p, P_COMMA);
    {
        PTok t = ct(p);
        if (t.t.kind == TK_IDENT) {
            uint32_t s = nmark(p), name = adv(p);
            if (accept(p, P_LPAREN)) {
                if (!at(p, P_RPAREN)) {
                    parse_assign(p);
                    while (accept(p, P_COMMA))
                        parse_assign(p);
                }
                expect(p, P_RPAREN);
            }
            emit(p, N_ATTR_ITEM, name, s, 0);
        } else {
            expected(p, "attribute name");
        }
    }
    expect(p, P_RPAREN);
    emit(p, N_HAS_ATTR, kw, start, 0);
}

static void generic_expr(Parser *p)
{
    uint32_t start = nmark(p), kw = adv(p);
    expect(p, P_LPAREN);
    parse_assign(p);
    while (accept(p, P_COMMA)) {
        uint32_t s = nmark(p), first = ci(p);
        if (ckw(p) == CK_DEFAULT)
            leaf(p, N_NONE, adv(p));
        else
            type_name(p);
        expect(p, P_COLON);
        parse_assign(p);
        emit(p, N_GENERIC_ASSOC, first, s, 0);
    }
    expect(p, P_RPAREN);
    emit(p, N_GENERIC, kw, start, 0);
}

static void primary(Parser *p)
{
    uint32_t start = nmark(p), i = ci(p);
    PTok t = ct(p);
    switch (t.t.kind) {
    case TK_IDENT:
        switch (ckw_of(p, &t)) {
        case CK_NONE:
            leaf(p, N_IDENT, adv(p));
            return;
        case CK_GENERIC:
            generic_expr(p);
            return;
        case CK_VA_ARG:
            builtin2(p, N_VA_ARG, false);
            return;
        case CK_CONVERTVECTOR:
            builtin2(p, N_CONVERTVECTOR, false);
            return;
        case CK_HAS_ATTRIBUTE:
            has_attr_expr(p);
            return;
        case CK_TYPES_COMPATIBLE:
            builtin2(p, N_TYPES_COMPAT, true);
            return;
        case CK_OFFSETOF:
            offsetof_expr(p);
            return;
        default:
            break;
        }
        break;
    case TK_PPNUM:
        leaf(p, N_NUMBER, adv(p));
        return;
    case TK_CHAR:
        leaf(p, N_CHAR, adv(p));
        return;
    case TK_STRING:
        string_lit(p);
        return;
    case TK_PUNCT:
        if (t.t.punct == P_LPAREN) {
            PTok n = pk(p, 1);
            uint32_t lp = adv(p);
            if (is_p(&n, P_LBRACE)) { /* GNU statement expression */
                compound(p, true);
                expect(p, P_RPAREN);
                emit(p, N_STMT_EXPR, lp, start, 0);
                return;
            }
            p->hushed = false;
            parse_expr(p);
            if (!at(p, P_RPAREN)) {
                /* gcc: c_parser_skip_until_found(')'), balancing */
                int depth = 0;
                bool hush = p->hush;
                p->hush = hush || p->hushed;    /* gcc: parser->error */
                expect(p, P_RPAREN);
                p->hush = hush;
                while (!at_eof(p) && !at(p, P_SEMI) && !at(p, P_RBRACE) &&
                       !at(p, P_LBRACE)) {
                    if (at(p, P_LPAREN) || at(p, P_LBRACKET))
                        depth++;
                    else if (at(p, P_RBRACKET))
                        depth--;
                    else if (at(p, P_RPAREN) && depth-- <= 0)
                        break;
                    adv(p);
                }
            }
            expect(p, P_RPAREN);
            emit(p, N_PAREN, lp, start, 0);
            return;
        }
        break;
    default:
        break;
    }
    expected(p, "expression");
    emit(p, N_ERROR, i, start, NF_ERROR);
}

static void unary(Parser *p)
{
    uint32_t start = nmark(p);
    PTok t = ct(p);
    int k = ckw_of(p, &t);
    if (t.t.kind == TK_PUNCT) {
        switch (t.t.punct) {
        case P_INC: case P_DEC: {
            uint32_t op = adv(p);
            parse_cast(p);   /* gcc: c_parser_cast_expression, so ++(T){...} parses */
            emit(p, N_UNARY, op, start, 0);
            return;
        }
        case P_AMP: case P_STAR: case P_PLUS: case P_MINUS: case P_TILDE:
        case P_BANG: {
            uint32_t op = adv(p);
            parse_cast(p);
            emit(p, N_UNARY, op, start, 0);
            return;
        }
        case P_ANDAND: { /* GNU: &&label */
            PTok n = pk(p, 1);
            if (is_name(p, &n)) {
                adv(p);
                emit(p, N_ADDR_LABEL, adv(p), start, 0);
                return;
            }
            break;
        }
        default:
            break;
        }
    }
    if (k == CK_SIZEOF || k == CK_ALIGNOF) {
        uint32_t kw = adv(p);
        PTok n0 = ct(p), n1 = pk(p, 1);
        if (is_p(&n0, P_LPAREN) &&
            (is_typename_start(p, &n1) || is_clit_storage(p, &n1))) {
            uint32_t s2 = nmark(p), lp = adv(p);
            type_name(p);
            expect(p, P_RPAREN);
            if (at(p, P_LBRACE)) { /* sizeof (T){...}: a compound literal */
                init_list(p);
                emit(p, N_COMPOUND_LIT, lp, s2, 0);
                postfix_tail(p, s2);
                emit(p, k == CK_SIZEOF ? N_SIZEOF_EXPR : N_ALIGNOF_EXPR, kw,
                     start, 0);
                return;
            }
            emit(p, k == CK_SIZEOF ? N_SIZEOF_TYPE : N_ALIGNOF_TYPE, kw, start,
                 0);
            return;
        }
        unary(p);
        emit(p, k == CK_SIZEOF ? N_SIZEOF_EXPR : N_ALIGNOF_EXPR, kw, start, 0);
        return;
    }
    if (k == CK_EXTENSION || k == CK_REAL || k == CK_IMAG) {
        uint32_t op = adv(p);
        parse_cast(p);
        emit(p, N_UNARY, op, start, 0);
        return;
    }
    primary(p);
    postfix_tail(p, start);
}

static void parse_cast(Parser *p)
{
    PTok n = pk(p, 1);
    if (at(p, P_LPAREN) && (is_typename_start(p, &n) || is_clit_storage(p, &n))) {
        uint32_t start = nmark(p), lp = adv(p);
        type_name(p);
        expect(p, P_RPAREN);
        if (at(p, P_LBRACE)) {
            init_list(p);
            emit(p, N_COMPOUND_LIT, lp, start, 0);
            postfix_tail(p, start);
            return;
        }
        parse_cast(p);
        emit(p, N_CAST, lp, start, 0);
        return;
    }
    unary(p);
}

static void binary(Parser *p, int min)
{
    uint32_t start = nmark(p);
    parse_cast(p);
    for (;;) {
        PTok t = ct(p);
        int prec = binary_prec(&t);
        uint32_t op;
        if (!prec || prec < min)
            return;
        op = adv(p);
        binary(p, prec + 1);
        emit(p, N_BINARY, op, start, 0);
    }
}

static void parse_cond(Parser *p)
{
    uint32_t start = nmark(p);
    binary(p, 4);
    if (at(p, P_QUESTION)) {
        uint32_t q = adv(p);
        unsigned flags = 0;
        if (at(p, P_COLON))
            flags |= NF_OMITTED; /* GNU: a ?: b */
        else
            parse_expr(p);
        expect(p, P_COLON);
        parse_cond(p);
        emit(p, N_COND, q, start, flags);
    }
}

static void parse_assign(Parser *p)
{
    uint32_t start = nmark(p);
    PTok t;
    parse_cond(p);
    t = ct(p);
    if (is_assign_op(&t)) {
        uint32_t op = adv(p);
        parse_assign(p);
        emit(p, N_ASSIGN, op, start, 0);
    }
}

static void parse_expr(Parser *p)
{
    uint32_t start = nmark(p);
    parse_assign(p);
    while (at(p, P_COMMA)) {
        uint32_t op = adv(p);
        parse_assign(p);
        emit(p, N_BINARY, op, start, 0);
    }
}

/* ---- statements -------------------------------------------------------- */

/* A substatement: a block of its own (C99 6.8.4p3, 6.8.5p5). */
static void substatement(Parser *p)
{
    open_scope(p, ci(p), 0);
    statement(p);
    close_scope(p, NULL);
}

static void end_stmt(Parser *p, NodeTag tag, uint32_t tok, uint32_t start)
{
    unsigned flags = 0;
    if (!expect(p, P_SEMI)) {
        sync_stmt(p);
        flags = NF_ERROR;
    }
    emit(p, tag, tok, start, flags);
}

static void paren_expr(Parser *p)
{
    expect(p, P_LPAREN);
    parse_expr(p);
    expect(p, P_RPAREN);
}

/* A label may end a compound statement (GNU, C23). */
static void label_body(Parser *p)
{
    PTok t;
    if (at(p, P_RBRACE))
        return;
    t = ct(p);
    {
        PTok n1 = pk(p, 1);
        if (is_name(p, &t) && is_p(&n1, P_COLON)) {   /* labels do not start declarations */
            statement(p);
            return;
        }
    }
    if (is_decl_start_la(p, &t) && ckw_of(p, &t) != CK_STATIC_ASSERT &&
        (ckw_of(p, &t) != CK_ATTRIBUTE || t.stdattr))
        declaration(p, false);  /* C2X; the checker pedwarns */
    else
        statement(p);
}

static void statement(Parser *p)
{
    uint32_t start = nmark(p), i = ci(p);
    PTok t = ct(p), n;
    switch (ckw_of(p, &t)) {
    case CK_NONE:
        break;
    case CK_IF:
        adv(p);
        open_scope(p, i, 0);
        paren_expr(p);
        substatement(p);
        if (ckw(p) == CK_ELSE) {
            adv(p);
            substatement(p);
        }
        close_scope(p, NULL);
        emit(p, N_IF, i, start, 0);
        return;
    case CK_SWITCH: case CK_WHILE:
        adv(p);
        open_scope(p, i, 0);
        paren_expr(p);
        substatement(p);
        close_scope(p, NULL);
        emit(p, ckw_of(p, &t) == CK_SWITCH ? N_SWITCH : N_WHILE, i, start, 0);
        return;
    case CK_DO:
        adv(p);
        open_scope(p, i, 0);
        substatement(p);
        if (ckw(p) == CK_WHILE) {
            adv(p);
            paren_expr(p);
        } else {
            expected(p, "'while'");
        }
        close_scope(p, NULL);
        end_stmt(p, N_DO, i, start);
        return;
    case CK_FOR: {
        PTok c;
        adv(p);
        open_scope(p, i, 0);
        expect(p, P_LPAREN);
        c = ct(p);
        if (accept(p, P_SEMI)) {
            leaf(p, N_NONE, i);
        } else if (is_decl_start_la(p, &c) || ckw_of(p, &c) == CK_EXTENSION) {
            declaration(p, false);
        } else {
            parse_expr(p);
            expect(p, P_SEMI);
        }
        if (at(p, P_SEMI))
            leaf(p, N_NONE, i);
        else
            parse_expr(p);
        expect(p, P_SEMI);
        if (at(p, P_RPAREN))
            leaf(p, N_NONE, i);
        else
            parse_expr(p);
        expect(p, P_RPAREN);
        substatement(p);
        close_scope(p, NULL);
        emit(p, N_FOR, i, start, 0);
        return;
    }
    case CK_GOTO:
        adv(p);
        if (accept(p, P_STAR)) { /* GNU: goto *expr */
            parse_expr(p);
            end_stmt(p, N_GOTO_EXPR, i, start);
            return;
        }
        n = ct(p);
        if (is_name(p, &n)) {
            uint32_t l = adv(p);
            end_stmt(p, N_GOTO, l, start);
            return;
        }
        expected(p, "label");
        end_stmt(p, N_GOTO, i, start);
        return;
    case CK_CONTINUE:
        adv(p);
        end_stmt(p, N_CONTINUE, i, start);
        return;
    case CK_BREAK:
        adv(p);
        end_stmt(p, N_BREAK, i, start);
        return;
    case CK_RETURN:
        adv(p);
        if (!at(p, P_SEMI))
            parse_expr(p);
        end_stmt(p, N_RETURN, i, start);
        return;
    case CK_CASE: {
        unsigned flags = 0;
        adv(p);
        parse_cond(p);
        if (accept(p, P_ELLIPSIS)) {
            parse_cond(p);
            flags |= NF_RANGE;
        }
        expect(p, P_COLON);
        label_body(p);
        emit(p, N_CASE, i, start, flags);
        return;
    }
    case CK_DEFAULT:
        adv(p);
        expect(p, P_COLON);
        label_body(p);
        emit(p, N_DEFAULT, i, start, 0);
        return;
    case CK_ASM:
        asm_stmt(p, false);
        return;
    case CK_ATTRIBUTE: /* __attribute__((fallthrough)); */
        attributes(p);
        end_stmt(p, N_ATTR_STMT, i, start);
        return;
    default:
        break;
    }
    if (is_p(&t, P_LBRACE)) {
        compound(p, true);
        return;
    }
    if (is_p(&t, P_SEMI)) {
        adv(p);
        emit(p, N_EXPR_STMT, i, start, 0);
        return;
    }
    n = pk(p, 1);
    if (is_name(p, &t) && is_p(&n, P_COLON)) {
        adv(p);
        adv(p);
        attributes(p);
        label_body(p);
        emit(p, N_LABEL, i, start, 0);
        return;
    }
    parse_expr(p);
    end_stmt(p, N_EXPR_STMT, i, start);
}

static void block_item(Parser *p)
{
    uint32_t start = nmark(p), i = ci(p);
    PTok t = ct(p), n;
    int k = ckw_of(p, &t);
    if (k == CK_STATIC_ASSERT) {
        static_assert_decl(p);
        return;
    }
    if (k == CK_LABEL) { /* GNU: __label__ a, b; */
        adv(p);
        for (;;) {
            PTok x = ct(p);
            if (!is_name(p, &x)) {
                expected(p, "identifier");
                break;
            }
            leaf(p, N_NAME, adv(p));
            if (!accept(p, P_COMMA))
                break;
        }
        end_stmt(p, N_LOCAL_LABEL, i, start);
        return;
    }
    if (k == CK_EXTENSION) {
        uint32_t j = i;
        PTok x;
        do {
            j = skip_prag(p, j + 1);
            x = tok_at(p, j);
        } while (ckw_of(p, &x) == CK_EXTENSION);
        if (is_decl_start(p, &x))
            declaration(p, false);
        else
            statement(p);
        return;
    }
    if (k == CK_ATTRIBUTE) {
        PTok x = tok_at(p, skip_attrs_ahead(p, i));
        if (is_p(&x, P_SEMI))
            statement(p); /* __attribute__((fallthrough)); */
        else
            declaration(p, false);
        return;
    }
    n = pk(p, 1);
    if (is_name(p, &t) && is_p(&n, P_COLON)) {
        statement(p);
        return;
    }
    if (is_decl_start_la(p, &t))
        declaration(p, false);
    else
        statement(p);
}

static void compound(Parser *p, bool push)
{
    uint32_t start = nmark(p), lb = adv(p);
    unsigned flags = 0;
    vec_push(&p->open_braces, lb);
    if (push)
        open_scope(p, lb, 0);
    else
        leaf(p, N_BODY, lb);
    for (;;) {
        uint32_t before;
        item_pragmas(p);
        if (at(p, P_RBRACE) || at_eof(p))
            break;
        before = p->pos;
        block_item(p);
        if (p->pos == before && !p->unwind) { /* no progress: skip */
            expected(p, "statement");
            adv(p);
        }
    }
    if (!accept(p, P_RBRACE)) {
        flags |= NF_ERROR;
        if (!p->unwind && !p->eof_stmt_err) {
            /* c_parser_error: gcc has recovered from any earlier error and
             * reports the unclosed body once, at input_location */
            p->eof_stmt_err = true;
            p->have_err = false;
            perr_at(p, ci(p), eof_input_loc(p), "expected declaration or "
                    "statement at end of input");
        }
    }
    if (push)
        close_scope(p, NULL);
    p->open_braces.len--;
    emit(p, N_COMPOUND, lb, start, flags);
}

/* ---- declarations -------------------------------------------------------- */

static void function_def(Parser *p, const DeclInfo *d, uint32_t start,
                         uint32_t first, unsigned flags)
{
    uint32_t i;
    /* the function is in scope in its body (recursion) */
    scope_declare(&p->scope, p->toks.data[d->name].t.aux, SYM_ORDINARY);
    emit(p, N_DECLARED, d->name, nmark(p), NF_BODY);
    open_scope(p, ci(p), NF_PARAMS);
    for (i = 0; i < d->save_len; i++) {
        const SymSave *s = &p->saved.data[d->save_start + i];
        scope_declare(&p->scope, s->ident, (SymKind)s->kind);
    }
    /* as gcc, whatever follows the declarator up to the '{' is a
     * declaration of a parameter (K&R), diagnosed as such */
    while (!at(p, P_LBRACE) && !at_eof(p)) {
        PTok t = ct(p);
        if (is_decl_start_la(p, &t)) {
            bool save = p->kr_params;
            p->kr_params = true;    /* no definition here (fndef_ok false) */
            declaration(p, false);
            p->kr_params = save;
        } else {
            uint32_t at0 = p->pos;
            expected(p, "declaration specifiers");
            sync_top(p);
            if (p->pos == at0) /* gcc's skip consumes a stray '}' */
                adv(p);
        }
    }
    p->fn_depth++;
    if (at(p, P_LBRACE) && p->gimple_body) {
        int depth = 0;
        do {
            if (at(p, P_LBRACE))
                depth++;
            else if (at(p, P_RBRACE))
                depth--;
            adv(p);
        } while (depth > 0 && !at_eof(p));
    } else if (at(p, P_LBRACE)) {
        compound(p, false);
    } else {
        expected_req(p, "'{'");
        flags |= NF_ERROR;
    }
    p->fn_depth--;
    close_scope(p, NULL);
    p->saved.len = d->save_start;
    emit(p, N_FUNC_DEF, first, start, flags);
}

static void declaration(Parser *p, bool top)
{
    uint32_t start = nmark(p), first = ci(p);
    unsigned flags = 0;
    Specs s;
    PTok t;
    int n;
    while (ckw(p) == CK_EXTENSION) {
        adv(p);
        flags |= NF_EXTENSION;
    }
    t = ct(p);
    switch (ckw_of(p, &t)) {
    case CK_STATIC_ASSERT:
        static_assert_decl(p);
        return;
    case CK_ASM:
        if (top) {
            asm_stmt(p, true);
            return;
        }
        break;
    default:
        break;
    }
    if (top && is_p(&t, P_SEMI)) { /* GNU: stray ';' at file scope */
        emit(p, N_EMPTY, adv(p), start, 0);
        return;
    }
    specs(p, &s, top ? LA_DECL_TOP : LA_DECL);
    if (!s.any) {
        /* no specifiers: at file scope gcc goes on to the declarator
         * (implicit int, accepted with a warning): f(void) {...}, x;
         * a stray token is diagnosed there */
        if (!(top && (is_name(p, &t) || is_p(&t, P_STAR) ||
                      is_p(&t, P_LPAREN)))) {
            if (top)
                expected(p, "identifier or '('");
            else
                expected(p, "declaration");
            if (top || p->kr_params)
                sync_top(p);
            else
                sync_stmt(p);
            emit(p, N_ERROR, first, start, NF_ERROR);
            set_aux(p, p->pos - first);
            return;
        }
    }
    if (accept(p, P_SEMI)) {
        emit(p, N_DECL, first, start, flags);
        return;
    }
    for (n = 0;; n++) {
        uint32_t is = nmark(p);
        DeclInfo d;
        declarator_init(&d);
        declarator(p, DCL_NAMED, &d);
        if (d.failed) {
            if (top || p->kr_params)
                sync_top(p);
            else
                sync_stmt(p);
            emit(p, N_DECL, first, start, flags | NF_ERROR);
            return;
        }
        if (d.name == NO_TOK) {
            expected(p, "identifier or '('");
            if (top || p->kr_params)
                sync_top(p);
            else
                sync_stmt(p);
            emit(p, N_DECL, first, start, flags | NF_ERROR);
            return;
        }
        /* a definition: '{', or a K&R declaration list (attributes
         * first belong to a declaration: f(x) __attribute__((...)); */
        t = ct(p);
        if (n == 0 && d.inner == DK_FUNC && !p->kr_params &&
            (at(p, P_LBRACE) ||
             (top && !(at(p, P_ASSIGN) || at(p, P_COMMA) || at(p, P_SEMI) ||
                       ckw_of(p, &t) == CK_ASM ||
                       (ckw_of(p, &t) == CK_ATTRIBUTE && !t.stdattr))) ||
             (d.kr && (t = tok_at(p, skip_attrs_ahead(p, ci(p))),
                       ckw_of(p, &t) != CK_ATTRIBUTE &&
                       is_decl_start(p, &t))))) {
            if (!top && at_col0(p, first)) {
                /* a function definition in column 0 inside a body: the
                 * body was not closed; end it here */
                Diagnostic *e = perr(p, first,
                                     "function definition inside a function "
                                     "body: missing '}'?");
                if (p->open_braces.len)
                    diag_note(p->diag, e,
                              tok_loc(p, vec_last(&p->open_braces)),
                              "this '{' is not closed");
                p->nodes.len = start;
                p->unwind = true;
                p->unwind_to = first;
                return;
            }
            p->gimple_body = s.gimple;
            function_def(p, &d, start, first, flags);
            p->gimple_body = false;
            return;
        }
        t = ct(p);
        if ((d.inner != DK_FUNC || p->kr_params) &&
            !(at(p, P_ASSIGN) || at(p, P_COMMA) || at(p, P_SEMI) ||
              ckw_of(p, &t) == CK_ASM ||
              (ckw_of(p, &t) == CK_ATTRIBUTE && !t.stdattr))) {
            /* gcc: not a declarator list or a function definition */
            if (n == 0 && !p->kr_params &&
                is_decl_start(p, &t)) { /* a missing ';' */
                char buf[160];
                perr_after_prev(p, ci(p), "expected ';'%s",
                                tok_desc(p, ci(p), buf, sizeof buf));
            } else {
                expected(p, "'=', ',', ';', 'asm' or '__attribute__'");
                /* gcc's c_parser_declaration_or_fndef just returns: in a
                 * block the statements go on from this very token */
                if (top || p->kr_params)
                    sync_top(p);
                else
                    p->err_live = false; /* error = false after each item */
            }
            /* gcc has not declared the name yet */
            emit(p, N_DECL, first, start, flags | NF_ERROR);
            return;
        }
        attributes(p);
        if (ckw(p) == CK_ASM)
            asm_label(p);
        attributes(p);
        scope_declare(&p->scope, p->toks.data[d.name].t.aux,
                      s.is_typedef ? SYM_TYPEDEF : SYM_ORDINARY);
        leaf(p, N_DECLARED, d.name);
        if (accept(p, P_ASSIGN))
            initializer(p);
        emit(p, N_INIT_DECL, d.name, is, 0);
        if (!accept(p, P_COMMA))
            break;
    }
    if (!accept(p, P_SEMI)) {
        expected(p, "',' or ';'");
        flags |= NF_ERROR;
        if (top || p->kr_params)
            sync_top(p);
        else
            sync_stmt(p);
    }
    emit(p, N_DECL, first, start, flags);
}

/* ---- units ------------------------------------------------------------------ */

void parser_init(Parser *p, SrcMgr *sm, Interner *in, DiagEngine *diag,
                 bool gnu, ParseSource src, void *ctx)
{
    static const char *const builtin_types[] = {
        "__builtin_va_list", "__int128_t", "__uint128_t", NULL};
    int i;
    memset(p, 0, sizeof *p);
    p->sm = sm;
    p->in = in;
    p->diag = diag;
    p->gnu = gnu;
    p->src = src;
    p->src_ctx = ctx;
    scope_init(&p->scope);
    scope_init(&p->tags);
    for (i = 0; builtin_types[i]; i++)
        scope_declare(&p->scope, intern_cstr(in, builtin_types[i])->id,
                      SYM_TYPEDEF);
}

void parser_free(Parser *p)
{
    vec_free(&p->toks);
    vec_free(&p->nodes);
    vec_free(&p->saved);
    vec_free(&p->open_braces);
    scope_free(&p->scope);
    scope_free(&p->tags);
}

bool parser_next(Parser *p, ParseUnit *u)
{
    uint32_t i;
    if (p->unit_end) { /* drop the previous unit's tokens */
        size_t rest = p->toks.len - p->unit_end;
        memmove(p->toks.data, p->toks.data + p->unit_end,
                rest * sizeof *p->toks.data);
        p->toks.len = rest;
        p->base += p->unit_end;
        p->pos -= p->unit_end;
        p->unit_end = 0;
    }
    p->nodes.len = 0;
    p->saved.len = 0;
    p->have_err = false;
    p->err_live = false;
    if (fill(p, p->pos) && p->toks.data[p->pos].t.kind == TK_PRAGMA) {
        leaf(p, N_PRAGMA, p->pos);
        p->pos++;
    } else {
        uint32_t before = p->pos;
        if (at_eof(p))
            return false;
        declaration(p, true);
        if (p->unwind) {
            p->unwind = false;
            p->pos = p->unwind_to;
        }
        if (p->pos == before) { /* no progress: skip a token */
            expected(p, "declaration");
            adv(p);
            p->nodes.len = 0;
            leaf(p, N_ERROR, before);
            set_aux(p, 1);
        }
    }
    p->unit_end = p->pos;
    /* nodes naming the end of input point at the last token */
    for (i = 0; i < p->nodes.len; i++)
        if (p->nodes.data[i].tok >= p->unit_end)
            p->nodes.data[i].tok = p->unit_end ? p->unit_end - 1 : 0;
    u->toks = p->toks.data;
    u->ntoks = p->unit_end;
    u->nodes = p->nodes.data;
    u->nnodes = (uint32_t)p->nodes.len;
    u->first_tok = p->base;
    p->units++;
    return true;
}
