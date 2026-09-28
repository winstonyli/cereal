/* cond.c - conditional-compilation analysis.
 *
 * Configuration space: every #if/#elif/#ifdef/#ifndef/#else in a user file
 * (active or not) is turned into a condition over
 *   - boolean variables  defined(X)
 *   - integer variables  X  (with a small domain: 0, 1 and c-1, c, c+1 for
 *                            every constant c that X is compared with)
 *   - opaque {0,1} variables for calls such as __GNUC_PREREQ(4, 1)
 * with the C99 rule that an undefined X evaluates to 0.  Each branch is
 * checked by enumeration: dead if it can never be selected, redundant if
 * it is always selected once reached.
 *
 * Dynamic checks use preprocessor events: tested-but-never-defined macros
 * with a similar defined name (typos). */
#include "analysis.h"

#include <string.h>

/* ---- expression trees ----------------------------------------------- */

typedef enum { N_NUM, N_VAR, N_DEFINED, N_CALL, N_UN, N_BIN, N_TERN } NKind;

typedef struct Node {
    NKind kind;
    int op;                 /* Punct for N_UN/N_BIN */
    intmax_t num;
    Ident *id;              /* N_VAR/N_DEFINED */
    int var;                /* variable index after binding */
    const char *text;       /* N_CALL: canonical text */
    struct Node *a, *b, *c;
} Node;

typedef struct PX {
    Arena *arena;
    Analysis *an;
    const Tok *t;           /* array ends with a TK_EOF sentinel */
    bool ok;
} PX;

#define PTEXT(p, t) pp_text((p)->an->pp, (t))

static Node *px_cond(PX *p);

static Node *mk(PX *p, NKind k)
{
    Node *n = NEW(p->arena, Node);
    n->kind = k;
    n->var = -1;
    return n;
}

static bool num_value(PX *p, const Tok *t, intmax_t *out)
{
    const char *s = PTEXT(p, t);
    uint32_t i = 0, n = t->len;
    uintmax_t v = 0;
    int base = 10;
    if (n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (s[0] == '0') {
        base = 8;
    }
    for (; i < n; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else if (c == 'u' || c == 'U' || c == 'l' || c == 'L')
            break;
        else
            return false;
        if (d >= base)
            return false;
        v = v * (uintmax_t)base + (uintmax_t)d;
    }
    *out = (intmax_t)v;
    return true;
}

static Node *px_primary(PX *p)
{
    const Tok *t = p->t;
    Node *n;
    if (tok_is_punct(t, P_LPAREN)) {
        p->t = (t + 1);
        n = px_cond(p);
        if (!tok_is_punct(p->t, P_RPAREN)) {
            p->ok = false;
            return n;
        }
        p->t = (p->t + 1);
        return n;
    }
    if (tok_is_word(p->an->in, t, "defined")) {
        bool paren;
        p->t = (t + 1);
        paren = tok_is_punct(p->t, P_LPAREN);
        if (paren)
            p->t = (p->t + 1);
        if (p->t->kind != TK_IDENT) {
            p->ok = false;
            return mk(p, N_NUM);
        }
        n = mk(p, N_DEFINED);
        n->id = tok_ident(p->an->in, p->t);
        p->t = (p->t + 1);
        if (paren) {
            if (!tok_is_punct(p->t, P_RPAREN))
                p->ok = false;
            else
                p->t = (p->t + 1);
        }
        return n;
    }
    if (t->kind == TK_IDENT && tok_is_punct((t + 1), P_LPAREN)) {
        /* function-like macro call: opaque */
        StrBuf sb = {0};
        int depth = 0;
        const Tok *u = t;
        n = mk(p, N_CALL);
        n->id = tok_ident(p->an->in, t);
        do {
            if (tok_is_punct(u, P_LPAREN))
                depth++;
            else if (tok_is_punct(u, P_RPAREN))
                depth--;
            if (sb.len)
                sb_putc(&sb, ' ');
            sb_putn(&sb, PTEXT(p, u), u->len);
            u = (u + 1);
        } while (u->kind != TK_EOF && depth > 0);
        n->text = arena_strndup(p->arena, sb_cstr(&sb), sb.len);
        sb_free(&sb);
        p->t = u;
        return n;
    }
    if (t->kind == TK_IDENT) {
        n = mk(p, N_VAR);
        n->id = tok_ident(p->an->in, t);
        p->t = (t + 1);
        return n;
    }
    if (t->kind == TK_PPNUM) {
        n = mk(p, N_NUM);
        if (!num_value(p, t, &n->num))
            p->ok = false;
        p->t = (t + 1);
        return n;
    }
    if (t->kind == TK_CHAR) {
        n = mk(p, N_NUM);
        n->num = (unsigned char)PTEXT(p, t)[PTEXT(p, t)[0] == 'L' ? 2 : 1];
        p->t = (t + 1);
        return n;
    }
    p->ok = false;
    p->t = t->kind == TK_EOF ? t : (t + 1);
    return mk(p, N_NUM);
}

static Node *px_unary(PX *p)
{
    const Tok *t = p->t;
    if (tok_is_punct(t, P_BANG) || tok_is_punct(t, P_MINUS) ||
        tok_is_punct(t, P_PLUS) || tok_is_punct(t, P_TILDE)) {
        Node *n = mk(p, N_UN);
        n->op = t->punct;
        p->t = (t + 1);
        n->a = px_unary(p);
        return n;
    }
    return px_primary(p);
}

static int prec(const Tok *t)
{
    if (t->kind != TK_PUNCT)
        return -1;
    switch (t->punct) {
    case P_STAR: case P_SLASH: case P_PERCENT: return 10;
    case P_PLUS: case P_MINUS: return 9;
    case P_SHL: case P_SHR: return 8;
    case P_LT: case P_GT: case P_LE: case P_GE: return 7;
    case P_EQEQ: case P_NE: return 6;
    case P_AMP: return 5;
    case P_CARET: return 4;
    case P_PIPE: return 3;
    case P_ANDAND: return 2;
    case P_OROR: return 1;
    default: return -1;
    }
}

static Node *px_binary(PX *p, int min)
{
    Node *lhs = px_unary(p);
    for (;;) {
        int pr = prec(p->t);
        Node *n;
        if (pr < min || pr < 0)
            return lhs;
        n = mk(p, N_BIN);
        n->op = p->t->punct;
        p->t = (p->t + 1);
        n->a = lhs;
        n->b = px_binary(p, pr + 1);
        lhs = n;
    }
}

static Node *px_cond(PX *p)
{
    Node *c = px_binary(p, 1);
    if (tok_is_punct(p->t, P_QUESTION)) {
        Node *n = mk(p, N_TERN);
        p->t = (p->t + 1);
        n->a = c;
        n->b = px_cond(p);
        if (!tok_is_punct(p->t, P_COLON)) {
            p->ok = false;
            return n;
        }
        p->t = (p->t + 1);
        n->c = px_cond(p);
        return n;
    }
    return c;
}

static Node *parse_cond(Analysis *an, TokSpan toks)
{
    PX p;
    Node *n;
    Tok *arr = NEW_ARRAY(an->arena, Tok, toks.n + 1);
    if (toks.n)
        memcpy(arr, toks.t, sizeof(Tok) * toks.n);
    arr[toks.n].kind = TK_EOF;
    p.arena = an->arena;
    p.an = an;
    p.t = arr;
    p.ok = true;
    n = px_cond(&p);
    if (!p.ok || p.t->kind != TK_EOF)
        return NULL;
    return n;
}

static Node *make_defined(Arena *a, Ident *id, bool negate)
{
    Node *d = NEW(a, Node), *n;
    d->kind = N_DEFINED;
    d->id = id;
    d->var = -1;
    if (!negate)
        return d;
    n = NEW(a, Node);
    n->kind = N_UN;
    n->op = P_BANG;
    n->a = d;
    n->var = -1;
    return n;
}

/* ---- variables and enumeration -------------------------------------- */

typedef struct Var {
    int kind;             /* 0 defined(X), 1 value X, 2 opaque call */
    Ident *id;
    const char *text;
    intmax_t dom[24];
    int ndom;
    int defined_var;      /* for value vars: index of defined(X) or -1 */
} Var;

typedef struct Problem {
    Var vars[16];
    int nvars;
    bool too_big;
    Node *f[64];          /* conjunction, each possibly negated */
    bool neg[64];
    int nf;
} Problem;

static int find_var(Problem *pb, int kind, Ident *id, const char *text)
{
    int i;
    for (i = 0; i < pb->nvars; i++)
        if (pb->vars[i].kind == kind &&
            (kind == 2 ? !strcmp(pb->vars[i].text, text) : pb->vars[i].id == id))
            return i;
    if (pb->nvars == (int)ARRAY_LEN(pb->vars)) {
        pb->too_big = true;
        return 0;
    }
    memset(&pb->vars[pb->nvars], 0, sizeof pb->vars[0]);
    pb->vars[pb->nvars].kind = kind;
    pb->vars[pb->nvars].id = id;
    pb->vars[pb->nvars].text = text;
    pb->vars[pb->nvars].defined_var = -1;
    return pb->nvars++;
}

static void dom_add(Var *v, intmax_t x)
{
    int i;
    for (i = 0; i < v->ndom; i++)
        if (v->dom[i] == x)
            return;
    if (v->ndom < (int)ARRAY_LEN(v->dom))
        v->dom[v->ndom++] = x;
}

static void bind(Problem *pb, Node *n)
{
    if (!n)
        return;
    switch (n->kind) {
    case N_DEFINED:
        n->var = find_var(pb, 0, n->id, NULL);
        break;
    case N_VAR:
        n->var = find_var(pb, 1, n->id, NULL);
        break;
    case N_CALL:
        n->var = find_var(pb, 2, NULL, n->text);
        break;
    default:
        bind(pb, n->a);
        bind(pb, n->b);
        bind(pb, n->c);
    }
}

/* collect comparison constants into the domains of value variables */
static void collect_consts(Problem *pb, Node *n)
{
    if (!n)
        return;
    if (n->kind == N_BIN && n->a && n->b) {
        Node *v = NULL, *c = NULL;
        if (n->a->kind == N_VAR && n->b->kind == N_NUM)
            v = n->a, c = n->b;
        else if (n->b->kind == N_VAR && n->a->kind == N_NUM)
            v = n->b, c = n->a;
        if (v && v->var >= 0) {
            dom_add(&pb->vars[v->var], c->num - 1);
            dom_add(&pb->vars[v->var], c->num);
            dom_add(&pb->vars[v->var], c->num + 1);
        }
    }
    collect_consts(pb, n->a);
    collect_consts(pb, n->b);
    collect_consts(pb, n->c);
}

static bool eval(Problem *pb, const int *asg, Node *n, intmax_t *out)
{
    intmax_t x, y;
    switch (n->kind) {
    case N_NUM:
        *out = n->num;
        return true;
    case N_DEFINED:
    case N_CALL:
        *out = asg[n->var];
        return true;
    case N_VAR: {
        Var *v = &pb->vars[n->var];
        *out = v->dom[asg[n->var]];
        return true;
    }
    case N_UN:
        if (!eval(pb, asg, n->a, &x))
            return false;
        switch (n->op) {
        case P_BANG: *out = !x; break;
        case P_MINUS: *out = -x; break;
        case P_TILDE: *out = ~x; break;
        default: *out = x;
        }
        return true;
    case N_TERN:
        if (!eval(pb, asg, n->a, &x))
            return false;
        return eval(pb, asg, x ? n->b : n->c, out);
    case N_BIN:
        if (!eval(pb, asg, n->a, &x))
            return false;
        if (n->op == P_ANDAND && !x) {
            *out = 0;
            return true;
        }
        if (n->op == P_OROR && x) {
            *out = 1;
            return true;
        }
        if (!eval(pb, asg, n->b, &y))
            return false;
        switch (n->op) {
        case P_STAR: *out = x * y; break;
        case P_SLASH: if (!y) return false; *out = x / y; break;
        case P_PERCENT: if (!y) return false; *out = x % y; break;
        case P_PLUS: *out = x + y; break;
        case P_MINUS: *out = x - y; break;
        case P_SHL: *out = (y < 0 || y > 62) ? 0 : x << y; break;
        case P_SHR: *out = (y < 0 || y > 62) ? 0 : x >> y; break;
        case P_LT: *out = x < y; break;
        case P_GT: *out = x > y; break;
        case P_LE: *out = x <= y; break;
        case P_GE: *out = x >= y; break;
        case P_EQEQ: *out = x == y; break;
        case P_NE: *out = x != y; break;
        case P_AMP: *out = x & y; break;
        case P_CARET: *out = x ^ y; break;
        case P_PIPE: *out = x | y; break;
        case P_ANDAND: *out = y != 0; break;
        case P_OROR: *out = y != 0; break;
        default: return false;
        }
        return true;
    }
    return false;
}

static bool has_vars(Node *n)
{
    if (!n)
        return false;
    if (n->kind == N_DEFINED || n->kind == N_VAR || n->kind == N_CALL)
        return true;
    return has_vars(n->a) || has_vars(n->b) || has_vars(n->c);
}

/* Is the conjunction of pb->f (with pb->neg) satisfiable?
 * Returns 1 yes, 0 no, -1 unknown (too large). */
static int satisfiable(Problem *pb)
{
    int asg[16], radix[16], i;
    uint64_t total = 1, k;
    for (i = 0; i < pb->nvars; i++) {
        Var *v = &pb->vars[i];
        if (v->kind == 1) {
            dom_add(v, 0);
            dom_add(v, 1);
            radix[i] = v->ndom;
        } else {
            radix[i] = 2;
        }
        total *= (uint64_t)radix[i];
        if (total > (1u << 18))
            return -1;
    }
    /* link value vars to their defined() vars */
    for (i = 0; i < pb->nvars; i++) {
        int j;
        pb->vars[i].defined_var = -1;
        if (pb->vars[i].kind == 1)
            for (j = 0; j < pb->nvars; j++)
                if (pb->vars[j].kind == 0 && pb->vars[j].id == pb->vars[i].id)
                    pb->vars[i].defined_var = j;
    }
    for (k = 0; k < total; k++) {
        uint64_t r = k;
        bool okay = true;
        for (i = 0; i < pb->nvars; i++) {
            asg[i] = (int)(r % (uint64_t)radix[i]);
            r /= (uint64_t)radix[i];
        }
        /* undefined X evaluates to 0 */
        for (i = 0; i < pb->nvars && okay; i++) {
            Var *v = &pb->vars[i];
            if (v->kind == 1 && v->defined_var >= 0 && !asg[v->defined_var] &&
                v->dom[asg[i]] != 0)
                okay = false;
        }
        for (i = 0; i < pb->nf && okay; i++) {
            intmax_t val;
            if (!eval(pb, asg, pb->f[i], &val))
                okay = false;
            else if ((val != 0) == pb->neg[i])
                okay = false;
        }
        if (okay)
            return 1;
    }
    return 0;
}

/* ---- per-file walk -------------------------------------------------- */

typedef struct Frame {
    int opener;             /* skeleton index of the #if */
    Node *prior[32];        /* conditions of earlier branches */
    int nprior;
    Node *cur;              /* condition of the current branch (NULL: #else) */
    bool cur_is_else;
    bool dead;              /* current branch already known dead */
    bool unknown;           /* a condition could not be modeled */
} Frame;

struct CondState {
    VEC(SrcFile *) done;
    /* dynamic */
    VEC(Tok) undefined_refs;
};

static Node *branch_cond(Analysis *an, SkDirective *d)
{
    switch (d->kind) {
    case SK_IFDEF:
        return d->name ? make_defined(an->arena, d->name, false) : NULL;
    case SK_IFNDEF:
        return d->name ? make_defined(an->arena, d->name, true) : NULL;
    case SK_IF:
    case SK_ELIF:
        return parse_cond(an, d->toks);
    default:
        return NULL;
    }
}

/* Build the problem: enclosing path + earlier branches (negated) + extra. */
static bool build(Problem *pb, Frame *stack, int depth, Node *extra,
                  bool extra_neg)
{
    int i, j;
    memset(pb, 0, sizeof *pb);
    for (i = 0; i < depth; i++) {
        Frame *fr = &stack[i];
        if (fr->unknown)
            return false;
        for (j = 0; j < fr->nprior && pb->nf < 63; j++) {
            pb->f[pb->nf] = fr->prior[j];
            pb->neg[pb->nf++] = true;
        }
        if (i < depth - 1 && fr->cur && pb->nf < 63) {
            pb->f[pb->nf] = fr->cur;
            pb->neg[pb->nf++] = false;
        }
    }
    if (extra && pb->nf < 63) {
        pb->f[pb->nf] = extra;
        pb->neg[pb->nf++] = extra_neg;
    }
    for (i = 0; i < pb->nf; i++)
        bind(pb, pb->f[i]);
    for (i = 0; i < pb->nf; i++)
        collect_consts(pb, pb->f[i]);
    return !pb->too_big;
}

static bool path_dead(Frame *stack, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        if (stack[i].dead)
            return true;
    return false;
}

static const char *dir_name(SkKind k)
{
    switch (k) {
    case SK_IF: return "#if";
    case SK_IFDEF: return "#ifdef";
    case SK_IFNDEF: return "#ifndef";
    case SK_ELIF: return "#elif";
    case SK_ELSE: return "#else";
    default: return "#endif";
    }
}

static void check_branch(Analysis *a, Skeleton *sk, Frame *stack, int depth,
                         SkDirective *d)
{
    Frame *fr = &stack[depth - 1];
    Problem pb;
    Node *c = fr->cur;
    int reach, sat_c, sat_notc;

    if (fr->unknown || path_dead(stack, depth - 1)) {
        return;
    }
    if (c && !has_vars(c)) {
        /* constant condition: #if 0 / #if 1 */
        Problem k;
        memset(&k, 0, sizeof k);
        k.f[0] = c;
        k.nf = 1;
        if (d->kind != SK_IFNDEF && d->kind != SK_IFDEF)
            diag_report(a->diag, DL_REMARK, "cond-constant", d->kw_loc,
                        "%s condition is constant (%s)", dir_name(d->kind),
                        satisfiable(&k) == 1 ? "always true" : "always false");
        fr->dead = satisfiable(&k) == 0;
        return;
    }
    /* reachability of this branch position */
    if (!build(&pb, stack, depth, NULL, false))
        return;
    reach = satisfiable(&pb);
    if (reach == 0) {
        Diagnostic *dg = diag_report(a->diag, DL_WARNING, "cond-dead-branch",
            d->kw_loc, "%s branch can never be taken: the enclosing and "
            "earlier conditions already cover every configuration",
            dir_name(d->kind));
        if (d->opener >= 0)
            diag_note(a->diag, dg, sk->dirs.data[d->opener].kw_loc,
                      "conditional starts here");
        fr->dead = true;
        return;
    }
    if (!c || reach < 0)
        return;
    if (!build(&pb, stack, depth, c, false))
        return;
    sat_c = satisfiable(&pb);
    if (sat_c == 0) {
        Problem alone;
        int self;
        memset(&alone, 0, sizeof alone);
        alone.f[0] = c;
        alone.nf = 1;
        bind(&alone, c);
        collect_consts(&alone, c);
        self = satisfiable(&alone);
        diag_report(a->diag, DL_WARNING, "cond-dead-branch", d->kw_loc,
                    self == 0 ? "%s condition can never be true"
                              : "%s condition contradicts the enclosing or "
                                "earlier conditions; this branch is dead in "
                                "every configuration",
                    dir_name(d->kind));
        fr->dead = true;
        return;
    }
    if (!build(&pb, stack, depth, c, true))
        return;
    sat_notc = satisfiable(&pb);
    if (sat_notc == 0 && d->kind != SK_IFNDEF) {
        diag_report(a->diag, DL_WARNING, "cond-redundant", d->kw_loc,
                    "%s condition is always true here (implied by the "
                    "enclosing and earlier conditions)%s", dir_name(d->kind),
                    d->kind == SK_ELIF ? "; use #else" : "");
    }
}

static bool macro_like_word(const char *s, size_t n)
{
    static const char *const skip[] = {"if", "ifdef", "ifndef", "endif", "else",
                                       "elif", "defined", "not", "end", "of",
                                       "the", "and", "or", "NOT", "TODO",
                                       "XXX", "FIXME", NULL};
    size_t i;
    bool upper = false;
    int k;
    if (n < 2)
        return false;
    for (k = 0; skip[k]; k++)
        if (strlen(skip[k]) == n && !memcmp(skip[k], s, n))
            return false;
    for (i = 0; i < n; i++)
        if ((s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_')
            upper = true;
    return upper;
}

static bool chain_mentions(const Interner *in, Skeleton *sk, int opener,
                           int upto, const char *w,
                           size_t n)
{
    int i;
    for (i = opener; i <= upto && i < (int)sk->dirs.len; i++) {
        SkDirective *d = &sk->dirs.data[i];
        uint32_t k;
        if (i != opener && d->opener != opener)
            continue;
        if (d->kind == SK_ENDIF)
            continue;
        for (k = 0; k < d->toks.n; k++) {
            const Tok *t = &d->toks.t[k];
            if (t->kind == TK_IDENT && t->len == n &&
                !memcmp(tok_ident(in, t)->str, w, n))
                return true;
        }
    }
    return false;
}

static void check_label(Analysis *a, Skeleton *sk, int idx)
{
    SkDirective *d = &sk->dirs.data[idx];
    const char *s = d->comment, *first_word = NULL;
    size_t first_len = 0;
    bool any = false;
    if (!s || d->opener < 0)
        return;
    while (*s) {
        const char *b;
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) {
            s++;
            continue;
        }
        b = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_')
            s++;
        if (!macro_like_word(b, (size_t)(s - b)))
            continue;
        if (chain_mentions(a->in, sk, d->opener, idx, b, (size_t)(s - b)))
            return; /* matches */
        if (!any) {
            first_word = b;
            first_len = (size_t)(s - b);
        }
        any = true;
    }
    if (any) {
        Diagnostic *dg = diag_report(a->diag, DL_WARNING, "endif-label",
            d->kw_loc, "comment on %s mentions '%.*s', which the conditional "
            "does not test", dir_name(d->kind), (int)first_len, first_word);
        diag_note(a->diag, dg, sk->dirs.data[d->opener].kw_loc,
                  "the conditional being closed starts here");
    }
}

static void analyze_file(Analysis *a, SrcFile *f)
{
    Skeleton *sk = an_skeleton(a, f);
    Frame stack[64];
    int depth = 0;
    size_t i;
    for (i = 0; i < sk->dirs.len; i++) {
        SkDirective *d = &sk->dirs.data[i];
        switch (d->kind) {
        case SK_IF: case SK_IFDEF: case SK_IFNDEF:
            if (depth == (int)ARRAY_LEN(stack))
                return;
            memset(&stack[depth], 0, sizeof stack[depth]);
            stack[depth].opener = (int)i;
            stack[depth].cur = branch_cond(a, d);
            stack[depth].unknown = stack[depth].cur == NULL;
            depth++;
            check_branch(a, sk, stack, depth, d);
            break;
        case SK_ELIF: case SK_ELSE: {
            Frame *fr;
            if (depth == 0)
                break;
            fr = &stack[depth - 1];
            if (fr->cur && fr->nprior < (int)ARRAY_LEN(fr->prior))
                fr->prior[fr->nprior++] = fr->cur;
            else if (!fr->cur)
                fr->unknown = true;
            fr->dead = false;
            if (d->kind == SK_ELSE) {
                fr->cur = NULL;
                fr->cur_is_else = true;
                check_label(a, sk, (int)i);
            } else {
                fr->cur = branch_cond(a, d);
                if (!fr->cur)
                    fr->unknown = true;
            }
            check_branch(a, sk, stack, depth, d);
            break;
        }
        case SK_ENDIF:
            check_label(a, sk, (int)i);
            if (depth > 0)
                depth--;
            break;
        default:
            break;
        }
    }
}

/* FOO_A vs FOO_B, V1 vs V2: distinct names by design, not typos. */
static bool looks_like_sibling(const Ident *x, const Ident *y)
{
    uint32_t i, diffs = 0, digit_only = 1;
    const char *seg_x = x->str, *seg_y = y->str;
    if (x->len != y->len)
        return false;
    for (i = 0; i < x->len; i++) {
        if (x->str[i] != y->str[i]) {
            diffs++;
            if (!(x->str[i] >= '0' && x->str[i] <= '9') ||
                !(y->str[i] >= '0' && y->str[i] <= '9'))
                digit_only = 0;
        }
    }
    if (diffs == 0)
        return false;
    if (digit_only)
        return true;
    if (diffs == 1) {
        /* the differing char is in a short final segment */
        const char *px = strrchr(seg_x, '_'), *py = strrchr(seg_y, '_');
        size_t lx = px ? strlen(px + 1) : x->len;
        size_t ly = py ? strlen(py + 1) : y->len;
        for (i = 0; x->str[i] == y->str[i]; i++)
            ;
        if (px && py && (size_t)(px - seg_x) < i && lx <= 2 && ly <= 2)
            return true;
    }
    return false;
}

/* ---- listeners ------------------------------------------------------ */

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Analysis *a = ctx;
    size_t i;
    (void)via;
    if (!an_user_file(f))
        return;
    for (i = 0; i < a->cond->done.len; i++)
        if (a->cond->done.data[i] == f)
            return;
    vec_push(&a->cond->done, f);
    analyze_file(a, f);
}

static void on_macro_ref(void *ctx, Ident *id, Macro *m, const Tok *tok,
                         RefKind kind)
{
    Analysis *a = ctx;
    (void)id;
    if (m || (kind != REF_IFDEF && kind != REF_DEFINED && kind != REF_IF_VALUE))
        return;
    if (!an_user_loc(a, tok->loc))
        return;
    vec_push(&a->cond->undefined_refs, *tok);
}

void cond_attach(Analysis *a)
{
    PPListener l;
    memset(&l, 0, sizeof l);
    a->cond = NEW(a->arena, CondState);
    l.ctx = a;
    l.file_enter = on_file_enter;
    l.macro_ref = on_macro_ref;
    pp_add_listener(a->pp, l);
}

void cond_finish(Analysis *a)
{
    size_t i, j;
    VEC(Ident *) seen = {0};
    for (i = 0; i < a->cond->undefined_refs.len; i++) {
        Tok *t = &a->cond->undefined_refs.data[i];
        Ident *id = tok_ident(a->in, t), *best = NULL;
        unsigned best_d = 3, limit;
        bool dup = false;
        if (id->history)
            continue; /* defined at some point in the TU */
        {
            /* a misspelled include guard is reported by -Wheader-guard */
            SrcFile *f = srcmgr_file_of(a->sm, t->loc);
            Skeleton *sk = f ? an_skeleton(a, f) : NULL;
            if (sk && sk->guard_ifndef == id && sk->guard_define &&
                sk->guard_define != id)
                continue;
        }
        for (j = 0; j < seen.len; j++)
            if (seen.data[j] == id)
                dup = true;
        if (dup)
            continue;
        vec_push(&seen, id);
        limit = id->len >= 8 ? 2 : 1;
        if (id->len >= 4) {
            INTERNER_FOREACH(a->in, bucket, cand) {
                unsigned dist;
                if (!cand->history || cand == id || looks_like_sibling(id, cand))
                    continue;
                dist = edit_distance(id->str, id->len, cand->str, cand->len,
                                     limit);
                if (dist <= limit && dist < best_d) {
                    best_d = dist;
                    best = cand;
                }
            }
        }
        if (best) {
            Diagnostic *d = diag_report(a->diag, DL_WARNING, "cond-typo", t->loc,
                "'%s' is never defined in this translation unit; did you mean "
                "'%s'?", id->str, best->str);
            diag_set_range(d, t->loc, t->loc + t->len);
            if (d && best->history->file && best->history->file->kind != SF_VIRTUAL)
                diag_note(a->diag, d, best->history->name_loc,
                          "'%s' is defined here", best->str);
            if (d)
                d->fixit = best->str;
        } else {
            diag_report(a->diag, DL_REMARK, "cond-never-defined", t->loc,
                        "'%s' is never defined in this translation unit",
                        id->str);
        }
    }
    vec_free(&seen);
    vec_free(&a->cond->done);
    vec_free(&a->cond->undefined_refs);
}
