/* cpragma.c - the #pragmas the checker interprets: pack, GCC diagnostic,
 * redefine_extname, scalar_storage_order.  The parsing follows gcc 13's
 * c-pragma.cc (handle_pragma_*): the same tokens are accepted, the same
 * -Wpragmas warnings are reported at the same places.
 *
 * A #pragma reaches the checker as one token holding the line's text; it is
 * re-lexed here, with a source location for every token. */
#include "c/check_int.h"

#include <ctype.h>
#include <string.h>

#include "gcc_opts.h"

enum { PK_EOF, PK_NAME, PK_NUM, PK_STR, PK_PUNCT };

typedef struct PrTok {
    int kind;
    int ch;                     /* PK_PUNCT: the character */
    size_t off, len;
    SrcLoc loc;
} PrTok;

typedef struct Prag {
    Checker *c;
    const char *s;              /* the text from the pragma's name on */
    size_t n, pos;
    SrcLoc base;                /* location of s[0] */
    SrcLoc eof;                 /* gcc's EOF token: the end of the line */
    SrcLoc name;                /* input_location: the pragma's name token */
} Prag;

static bool name_start(unsigned char ch)
{
    return isalpha(ch) || ch == '_' || ch == '$' || ch >= 0x80;
}

static bool name_char(unsigned char ch)
{
    return isalnum(ch) || ch == '_' || ch == '$' || ch >= 0x80;
}

static PrTok lex(Prag *p)
{
    PrTok t;
    const char *s = p->s;
    memset(&t, 0, sizeof t);
    for (;;) {
        while (p->pos < p->n && (s[p->pos] == ' ' || s[p->pos] == '\t'))
            p->pos++;
        if (p->pos + 1 < p->n && s[p->pos] == '/' && s[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->n &&
                   !(s[p->pos] == '*' && s[p->pos + 1] == '/'))
                p->pos++;
            p->pos = p->pos + 2 < p->n ? p->pos + 2 : p->n;
            continue;
        }
        if (p->pos + 1 < p->n && s[p->pos] == '/' && s[p->pos + 1] == '/')
            p->pos = p->n;
        break;
    }
    t.off = p->pos;
    t.loc = p->base + (SrcLoc)p->pos;
    if (p->pos >= p->n) {
        t.kind = PK_EOF;
        t.loc = p->eof;
        return t;
    }
    if (name_start((unsigned char)s[p->pos]) ||
        (s[p->pos] == '\\' && p->pos + 1 < p->n &&
         (s[p->pos + 1] == 'u' || s[p->pos + 1] == 'U'))) {
        t.kind = PK_NAME;
        while (p->pos < p->n) {
            if (name_char((unsigned char)s[p->pos]))
                p->pos++;
            else if (s[p->pos] == '\\' && p->pos + 1 < p->n &&
                     (s[p->pos + 1] == 'u' || s[p->pos + 1] == 'U')) {
                size_t d = s[p->pos + 1] == 'u' ? 4 : 8, k = 0;
                p->pos += 2;
                while (k < d && p->pos < p->n && isxdigit((unsigned char)s[p->pos])) {
                    p->pos++;
                    k++;
                }
            } else
                break;
        }
    } else if (isdigit((unsigned char)s[p->pos]) ||
               (s[p->pos] == '.' && p->pos + 1 < p->n &&
                isdigit((unsigned char)s[p->pos + 1]))) {
        t.kind = PK_NUM;
        while (p->pos < p->n) {
            char ch = s[p->pos];
            if ((ch == '+' || ch == '-') && p->pos > t.off &&
                strchr("eEpP", s[p->pos - 1]))
                p->pos++;
            else if (isalnum((unsigned char)ch) || ch == '.' || ch == '_')
                p->pos++;
            else
                break;
        }
    } else if (s[p->pos] == '"') {
        t.kind = PK_STR;
        p->pos++;
        while (p->pos < p->n && s[p->pos] != '"') {
            if (s[p->pos] == '\\' && p->pos + 1 < p->n)
                p->pos++;
            p->pos++;
        }
        if (p->pos < p->n)
            p->pos++;
    } else {
        t.kind = PK_PUNCT;
        t.ch = (unsigned char)s[p->pos++];
    }
    t.len = p->pos - t.off;
    return t;
}

static bool is_punct(const PrTok *t, int ch)
{
    return t->kind == PK_PUNCT && t->ch == ch;
}

/* A name token's identifier as gcc prints it (extended characters as
 * \UXXXXXXXX), in a ring of static buffers. */
static const char *name_text(const Prag *p, const PrTok *t)
{
    static __thread char ring[4][160];
    static __thread unsigned next;
    char *b = ring[next++ & 3];
    size_t n = t->len < 150 ? t->len : 150;
    memcpy(b, p->s + t->off, n);
    b[n] = 0;
    return cident_ucn(b);
}

static bool name_is(const Prag *p, const PrTok *t, const char *s)
{
    return t->kind == PK_NAME && strlen(s) == t->len &&
           !strncmp(p->s + t->off, s, t->len);
}

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h | 1;
}

/* An integer constant (gcc's INTEGER_CST), else false. */
static bool int_const(const Prag *p, const PrTok *t, long *v)
{
    char buf[64], *e;
    size_t n = t->len < 63 ? t->len : 63;
    memcpy(buf, p->s + t->off, n);
    buf[n] = 0;
    *v = strtol(buf, &e, 0);
    while (*e == 'u' || *e == 'U' || *e == 'l' || *e == 'L')
        e++;
    return *e == 0 && n > 0;
}

/* ---- #pragma pack ----------------------------------------------------------- */

static void pack_bad(Prag *p, const char *msg)
{
    cwarn(p->c, p->name, "pragmas", "%s", msg);
}

static void pack_pop(Prag *p, bool have_id, uint32_t idh, const char *idtxt)
{
    Checker *c = p->c;
    size_t k;
    if (!c->pack_stack.len) {
        cwarn(c, p->name, "pragmas", "'#pragma pack (pop)' encountered "
              "without matching '#pragma pack (push)'");
        return;
    }
    if (!have_id) {
        c->pack = c->pack_stack.data[c->pack_stack.len - 2];
        c->pack_stack.len -= 2;
        return;
    }
    for (k = c->pack_stack.len; k >= 2; k -= 2)
        if (c->pack_stack.data[k - 1] == idh) {
            c->pack = c->pack_stack.data[k - 2];
            c->pack_stack.len = k - 2;
            return;
        }
    cwarn(c, p->name, "pragmas", "'#pragma pack(pop, %s)' encountered "
          "without matching '#pragma pack(push, %s)'", idtxt, idtxt);
    /* gcc still pops the top entry */
    c->pack = c->pack_stack.data[c->pack_stack.len - 2];
    c->pack_stack.len -= 2;
}

static void pragma_pack(Prag *p)
{
    Checker *c = p->c;
    PrTok t = lex(p);
    enum { SET, PUSH, POP } action = SET;
    bool have_id = false;
    uint32_t idh = 0;
    char idtxt[160] = "";
    long align = -1;
    if (!is_punct(&t, '(')) {
        pack_bad(p, "missing '(' after '#pragma pack' - ignored");
        return;
    }
    t = lex(p);
    if (is_punct(&t, ')')) {
        align = 0;
    } else if (t.kind == PK_NUM) {
        if (!int_const(p, &t, &align)) {
            cwarn(c, t.loc, "pragmas", "invalid constant in '#pragma pack' "
                  "- ignored");
            return;
        }
        t = lex(p);
        if (!is_punct(&t, ')')) {
            pack_bad(p, "malformed '#pragma pack' - ignored");
            return;
        }
    } else if (t.kind == PK_NAME) {
        if (name_is(p, &t, "push"))
            action = PUSH;
        else if (name_is(p, &t, "pop"))
            action = POP;
        else {
            cwarn(c, t.loc, "pragmas", "unknown action '%s' for "
                  "'#pragma pack' - ignored", name_text(p, &t));
            return;
        }
        t = lex(p);
        if (is_punct(&t, ',')) {
            t = lex(p);
            if (t.kind == PK_NAME) {
                have_id = true;
                snprintf(idtxt, sizeof idtxt, "%s", name_text(p, &t));
                idh = name_hash(idtxt);
                t = lex(p);
                if (action == PUSH && is_punct(&t, ',')) {
                    t = lex(p);
                    if (t.kind != PK_NUM) {
                        cwarn(c, t.loc, "pragmas", "malformed "
                              "'#pragma pack(push[, id], <n>)' - ignored");
                        return;
                    }
                    if (!int_const(p, &t, &align)) {
                        cwarn(c, t.loc, "pragmas", "invalid constant in "
                              "'#pragma pack' - ignored");
                        return;
                    }
                    t = lex(p);
                }
            } else if (action == PUSH && t.kind == PK_NUM) {
                if (!int_const(p, &t, &align)) {
                    cwarn(c, t.loc, "pragmas", "invalid constant in "
                          "'#pragma pack' - ignored");
                    return;
                }
                t = lex(p);
            } else {
                pack_bad(p, action == PUSH
                         ? "malformed '#pragma pack(push[, id], <n>)' - ignored"
                         : "malformed '#pragma pack(pop[, id])' - ignored");
                return;
            }
        }
        if (!is_punct(&t, ')')) {
            pack_bad(p, action == PUSH
                     ? "malformed '#pragma pack(push[, id][, <n>])' - ignored"
                     : "malformed '#pragma pack(pop[, id])' - ignored");
            return;
        }
    } else {
        pack_bad(p, "malformed '#pragma pack' - ignored");
        return;
    }
    t = lex(p);
    if (t.kind != PK_EOF)
        cwarn(c, t.loc, "pragmas", "junk at end of '#pragma pack'");
    if (action != POP) {
        if (align == -1 && action == PUSH)
            ;                   /* push without a value keeps the state */
        else if (!(align == 0 || align == 1 || align == 2 || align == 4 ||
                   align == 8 || align == 16)) {
            cwarn(c, p->name, "pragmas", "alignment must be a small power "
                  "of two, not %ld", align);
            return;
        }
    }
    if (action == SET) {
        c->pack = (unsigned)align;
    } else if (action == PUSH) {
        vec_push(&c->pack_stack, c->pack);
        vec_push(&c->pack_stack, have_id ? idh : 0u);
        if (align >= 0)
            c->pack = (unsigned)align;
    } else
        pack_pop(p, have_id, idh, idtxt);
}

/* ---- #pragma GCC diagnostic ------------------------------------------------- */

static void diag_apply(Checker *c, const char *kind, const char *opt)
{
    char flag[224];
    uint64_t h;
    const char *s;
    if (!c->diag_cur) {
        c->diag_cfg0 = c->diag->cfg;
        c->diag_cur = diag_config_clone(c->diag->cfg);
    }
    if (!strcmp(kind, "ignored")) {
        snprintf(flag, sizeof flag, "no-%s", opt);
        diag_config_apply(c->diag_cur, flag);
    } else if (!strcmp(kind, "warning")) {
        diag_config_apply(c->diag_cur, opt);
        snprintf(flag, sizeof flag, "no-error=%s", opt);
        diag_config_apply(c->diag_cur, flag);
    } else {
        snprintf(flag, sizeof flag, "error=%s", opt);
        diag_config_apply(c->diag_cur, flag);
    }
    c->diag->cfg = c->diag_cur;
    for (h = 14695981039346656037ull, s = kind; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    for (s = opt; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    c->diag_dig = (c->diag_dig + h) * 0x9E3779B97F4A7C15ull | 1;
}

static void pragma_diagnostic(Prag *p)
{
    Checker *c = p->c;
    PrTok t = lex(p), o;
    char kind[24], opt[160];
    size_t n = 0;
    if (t.kind != PK_NAME) {
        cwarn(c, t.loc, "pragmas", "missing 'error', 'warning', 'ignored', "
              "'push', 'pop', or 'ignored_attributes' after "
              "'#pragma GCC diagnostic'");
        return;
    }
    snprintf(kind, sizeof kind, "%.*s", (int)(t.len < 23 ? t.len : 23),
             p->s + t.off);
    if (!strcmp(kind, "push")) {
        DiagState st;
        st.cfg = c->diag_cur ? diag_config_clone(c->diag_cur) : NULL;
        st.dig = c->diag_dig;
        vec_push(&c->diag_stack, st);
        return;
    }
    if (!strcmp(kind, "pop")) {
        DiagState st;
        if (!c->diag_stack.len)
            return;
        st = vec_last(&c->diag_stack);
        c->diag_stack.len--;
        diag_config_free(c->diag_cur);
        c->diag_cur = st.cfg;
        c->diag_dig = st.dig;
        c->diag->cfg = c->diag_cur ? c->diag_cur : c->diag_cfg0;
        return;
    }
    if (!strcmp(kind, "ignored_attributes"))
        return;
    if (strcmp(kind, "ignored") && strcmp(kind, "warning") &&
        strcmp(kind, "error")) {
        cwarn(c, t.loc, "pragmas", "expected 'error', 'warning', 'ignored', "
              "'push', 'pop', 'ignored_attributes' after "
              "'#pragma GCC diagnostic'");
        return;
    }
    o = lex(p);
    if (o.kind != PK_STR) {
        cwarn(c, o.loc, "pragmas", "missing option after "
              "'#pragma GCC diagnostic' kind");
        return;
    }
    /* adjacent string literals concatenate (no escapes in an option) */
    for (;;) {
        size_t save = p->pos;
        PrTok more = lex(p);
        if (more.kind != PK_STR) {
            p->pos = save;
            break;
        }
        if (n + more.len < sizeof opt) {
            /* handled below with the first part */
        }
        o.len = more.off + more.len - o.off;    /* span; trimmed on copy */
    }
    {
        const char *q = p->s + o.off;
        size_t k = 0, m = o.len;
        bool in = false;
        for (n = 0; k < m && n < sizeof opt - 1; k++) {
            if (q[k] == '"')
                in = !in;
            else if (in)
                opt[n++] = q[k];
        }
        opt[n] = 0;
    }
    {
        const char *langs = NULL;
        switch (gcc_pragma_opt(opt, &langs)) {
        case PO_UNKNOWN: {
            Diagnostic *d = cwarn_d(c, DL_WARNING, o.loc, "pragmas", "unknown "
                                    "option after '#pragma GCC diagnostic' kind");
            const char *dym = opt[0] == '-' && opt[1] == 'W'
                              ? gcc_wopt_suggest(opt + 2) : NULL;
            if (d && dym)
                cnote(c, d, o.loc, "did you mean '-W%s'?", dym);
            return;
        }
        case PO_OTHER_LANG:
            cwarn(c, o.loc, "pragmas", "option '%s' is valid for %s but not "
                  "for C", opt, langs);
            return;
        case PO_NOT_WARNING:
            cwarn(c, o.loc, "pragmas", "'%s' is not an option that controls "
                  "warnings", opt);
            return;
        default:
            break;
        }
    }
    diag_apply(c, kind, opt + 2);
}

/* ---- the others ------------------------------------------------------------- */

static void pragma_redefine_extname(Prag *p)
{
    PrTok t = lex(p);
    if (t.kind != PK_NAME) {
        pack_bad(p, "malformed '#pragma redefine_extname', ignored");
        return;
    }
    t = lex(p);
    if (t.kind != PK_NAME && t.kind != PK_STR) {
        pack_bad(p, "malformed '#pragma redefine_extname', ignored");
        return;
    }
    t = lex(p);
    if (t.kind != PK_EOF)
        cwarn(p->c, p->name, "pragmas", "junk at end of "
              "'#pragma redefine_extname'");
}

static void pragma_sso(Prag *p)
{
    PrTok t = lex(p);
    static const char want[] = "'big-endian', 'little-endian', or 'default' "
                               "after '#pragma scalar_storage_order'";
    if (t.kind != PK_NAME) {
        cwarn(p->c, p->name, "pragmas", "missing %s", want);
        return;
    }
    if (!name_is(p, &t, "default")) {
        bool big = name_is(p, &t, "big"), little = name_is(p, &t, "little");
        PrTok m, e;
        if (!big && !little) {
            cwarn(p->c, p->name, "pragmas", "expected %s", want);
            return;
        }
        m = lex(p);
        e = lex(p);
        if (!is_punct(&m, '-') || !name_is(p, &e, "endian")) {
            cwarn(p->c, p->name, "pragmas", "expected %s", want);
            return;
        }
    }
}

/* The text of the #pragma token tok (from "pragma" on) is interpreted:
 * pack and GCC diagnostic change the checker's state. */
void cpragma_apply(Checker *c, uint32_t tok)
{
    const Tok *t = &c->u->toks[tok].t;
    const char *s = tok_text_raw(c->sm, c->in, t), *end = s + t->len;
    Prag p;
    SrcFile *f;
    uint32_t line, col, len;
    PrTok name;
    memset(&p, 0, sizeof p);
    p.c = c;
    while (s < end && (*s == ' ' || *s == '\t' || *s == '#'))
        s++;
    if (end - s >= 6 && !strncmp(s, "pragma", 6))
        s += 6;
    p.s = s;
    p.n = (size_t)(end - s);
    /* the token's text starts after the '#', its location at it */
    p.base = ctok_loc(c, tok) + (SrcLoc)(s - tok_text_raw(c->sm, c->in, t)) +
             (*tok_text_raw(c->sm, c->in, t) == '#' ? 0 : 1);
    p.eof = p.base + (SrcLoc)p.n;
    f = srcmgr_file_of(c->sm, p.base);
    if (f) {
        srcmgr_linecol(f, p.base, &line, &col);
        (void)srcmgr_line_text(f, line, &len);
        p.eof = srcmgr_loc_of(f, line, len + 1);
    }
    name = lex(&p);
    p.name = name.loc;
    if (name_is(&p, &name, "pack")) {
        pragma_pack(&p);
    } else if (name_is(&p, &name, "redefine_extname")) {
        p.name = name.loc;
        pragma_redefine_extname(&p);
    } else if (name_is(&p, &name, "scalar_storage_order")) {
        pragma_sso(&p);
    } else if (name_is(&p, &name, "GCC")) {
        PrTok w = lex(&p);
        p.name = w.loc;
        if (name_is(&p, &w, "diagnostic"))
            pragma_diagnostic(&p);
    }
}
