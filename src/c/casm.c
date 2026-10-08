/* casm.c - asm statements: the template string, operands, constraints and
 * clobbers (split from cexpr.c).  The shared readers are cexpr_int.h's. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

/* The characters of the string literal node n (adjacent literals joined;
 * simple escapes decoded). */
size_t asm_string(Checker *c, uint32_t n, char *out, size_t cap)
{
    uint32_t np = node_pieces(c, n), t;
    size_t o = 0;
    for (t = 0; t < np; t++) {
        size_t len, j;
        const char *s = ttext(c, c->nodes[n].tok + t, &len);
        for (j = 0; j < len && s[j] != '"'; j++)
            ;
        for (j++; j + 1 < len && o + 1 < cap; j++) {
            char ch = s[j];
            if (ch == '\\' && j + 2 < len) {
                ch = s[++j];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'u': case 'U': {   /* a UCN: its UTF-8 */
                    unsigned digits = ch == 'u' ? 4 : 8, k = 0;
                    uint32_t v = 0;
                    while (k < digits && j + 1 + k < len &&
                           isxdigit((unsigned char)s[j + 1 + k])) {
                        char h = s[j + 1 + k++];
                        v = v << 4 | (uint32_t)(h <= '9' ? h - '0'
                                                : (h | 32) - 'a' + 10);
                    }
                    if (k == digits && v >= 0x80 && v < 0x110000 &&
                        o + 4 < cap) {
                        unsigned nb = v < 0x800 ? 2 : v < 0x10000 ? 3 : 4, b;
                        out[o++] = (char)(nb == 2 ? 0xC0 | v >> 6
                                          : nb == 3 ? 0xE0 | v >> 12
                                                    : 0xF0 | v >> 18);
                        for (b = nb - 1; b-- > 0;)
                            out[o++] = (char)(0x80 | ((v >> (6 * b)) & 0x3F));
                        j += digits;
                        continue;
                    }
                    ch = '\1';         /* not a valid one: left undecoded */
                    break;
                }
                case 'x': {
                    int v = 0;
                    while (j + 2 < len && isxdigit((unsigned char)s[j + 1])) {
                        char h = s[++j];
                        v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                    }
                    ch = (char)v;
                    break;
                }
                default:
                    if (ch >= '0' && ch <= '7') {
                        int v = ch - '0', k = 0;
                        while (k++ < 2 && j + 2 < len && s[j + 1] >= '0' &&
                               s[j + 1] <= '7')
                            v = v * 8 + (s[++j] - '0');
                        ch = (char)v;
                    }
                    break;
                }
            }
            out[o++] = ch;
        }
    }
    out[o] = 0;
    return o;
}

static const char *asm_chr(char ch, char *buf)
{
    if (ch >= 32 && ch < 127)
        snprintf(buf, 8, "%c", ch);
    else
        snprintf(buf, 8, "\\x%02x", (unsigned char)ch);
    return buf;
}

/* parse_output_constraint / parse_input_constraint, as far as they report
 * errors; false after one. */
static bool asm_constraint(Checker *c, SrcLoc loc, const char *k, bool out,
                           bool last, uint32_t nouts, char opn[][64],
                           uint32_t nopn, bool *reg, bool *mem)
{
    const char *p;
    char b[8];
    *reg = *mem = false;
    if (out && k[0] != '=' && k[0] != '+') {
        cerror(c, loc, "output operand constraint lacks '='");
        return false;
    }
    for (p = k; *p; p++) {
        switch (*p) {
        case '=': case '+':
            if (!out) {
                cerror(c, loc, "input operand constraint contains '%c'", *p);
                return false;
            }
            if (p != k) {
                cerror(c, loc, "operand constraint contains incorrectly "
                       "positioned '+' or '='");
                return false;
            }
            break;
        case '&':
            if (!out) {
                cerror(c, loc, "input operand constraint contains '&'");
                return false;
            }
            break;
        case '%':
            if (last) {
                cerror(c, loc, "'%%' constraint used with last operand");
                return false;
            }
            break;
        case '?': case '!': case '*': case '#': case '$': case '^': case ',':
        case '<': case '>':
            break;
        case ' ': case '\t':      /* only an output constraint takes them */
            if (!out) {
                cerror(c, loc, "invalid punctuation '%s' in constraint",
                       asm_chr(*p, b));
                return false;
            }
            break;
        case 'V': case 'm': case 'o':
            *mem = true;
            break;
        case 'g': case 'X':
            *reg = *mem = true;
            break;
        case 'r': case 'p': case 'a': case 'b': case 'c': case 'd': case 'S':
        case 'D': case 'q': case 'Q': case 'R': case 'l': case 'A': case 'f':
        case 't': case 'u': case 'y': case 'x': case 'Y': case 'k': case 'v':
            *reg = true;
            break;
        case 'E': case 'F': case 'G': case 'H': case 's': case 'i': case 'n':
        case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'e': case 'Z': case 'B': case 'C': case 'T': case 'W':
            break;
        case '[': {
            /* resolve_asm_operand_names: [name] is an operand number */
            const char *e = strchr(p, ']');
            char nm[64];
            uint32_t q, idx = nopn;
            if (out) {
                cerror(c, loc, "matching constraint not valid in output "
                       "operand");
                return false;
            }
            if (!e) {
                cerror(c, loc, "missing close brace for named operand");
                cerror(c, loc, "invalid punctuation '[' in constraint");
                return false;
            }
            snprintf(nm, sizeof nm, "%.*s", (int)(e - p - 1), p + 1);
            for (q = 0; q < nopn; q++)
                if (!strcmp(opn[q], nm)) {
                    idx = q;
                    break;
                }
            if (idx == nopn) {
                cerror(c, loc, "undefined named operand '%s'", nm);
                return false;
            }
            if (idx >= nouts) {
                cerror(c, loc, "matching constraint references invalid "
                       "operand number");
                return false;
            }
            *reg = true;
            p = e;
            break;
        }
        default:
            if (*p >= '0' && *p <= '9') {
                if (out) {
                    cerror(c, loc, "matching constraint not valid in output "
                           "operand");
                    return false;
                }
                if (strtoul(p, NULL, 10) >= nouts) {
                    cerror(c, loc, "matching constraint references invalid "
                           "operand number");
                    return false;
                }
                while (p[1] >= '0' && p[1] <= '9')
                    p++;
                *reg = true;
            } else if (!out) {
                if (!isalpha((unsigned char)*p)) {
                    cerror(c, loc, "invalid punctuation '%s' in constraint",
                           asm_chr(*p, b));
                    return false;
                }
                *reg = *mem = true;     /* unknown: treat like "g" */
            } else {
                *reg = *mem = true;     /* an output: any other character */
            }
            break;
        }
    }
    return true;
}

/* An asm operand's expression e (build_asm_expr). */
static void asm_operand(Checker *c, SrcLoc loc, uint32_t e, bool out, bool reg,
                        bool mem)
{
    uint32_t s;
    TypeId t;
    if (node_err(c, e))
        return;
    t = c->ty[e];
    if (out) {
        if (!(c->ef[e] & EF_LVALUE)) {
            cerror(c, loc, "lvalue required in 'asm' statement");
            return;
        }
        if (tquals(c, t) & TQ_CONST) {
            uint32_t v = strip_paren(c, e), ref = SYM_NONE;
            if (ntag(c, v) == N_IDENT)
                ref = lookup_ord(c, cnode_ident(c, v));
            if (ntag(c, v) == N_MEMBER_EXPR && cnode_ident(c, v))
                cerror(c, loc, "read-only member '%s' used as 'asm' output",
                       cident(c, cnode_ident(c, v)));
            else if (ref != SYM_NONE && csym(c, ref)->kind != CS_FUNC)
                cerror(c, loc, csym(c, ref)->flags & CSF_PARAM
                           ? "read-only parameter '%s' use as 'asm' output"
                           : "read-only variable '%s' used as 'asm' output",
                       cident(c, cnode_ident(c, v)));
            else
                cerror(c, loc, "read-only location '%s' used as 'asm' output",
                       estr(c, e));
            return;
        }
    }
    if (reg && (is_void(c, t) || (!out && !is_func(c, t) && tkind(c, t) != TY_ARRAY &&
                                   !type_is_complete(TT, rvt(c, e))))) {
        incomplete_error(c, loc, NO_NODE, rvt(c, e));
        return;
    }
    if (!reg && mem) {          /* c_mark_addressable */
        s = strip_paren(c, e);
        while (ntag(c, s) == N_CAST && s > 0)
            s = strip_paren(c, s - 1);
        if (c->ef[e] & EF_BITFIELD)
            cerror(c, loc, "cannot take address of bit-field '%s'",
                   ntag(c, s) == N_MEMBER_EXPR ? cident(c, cnode_ident(c, s))
                                               : "");
        else if (ntag(c, s) == N_IDENT && (c->ef[s] & EF_REGISTER)) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, s));
            cerror(c, loc, ref != SYM_NONE && !(ref & SYM_LOCAL)
                       ? "address of global register variable '%s' requested"
                       : "address of register variable '%s' requested",
                   cident(c, cnode_ident(c, s)));
        }
    }
}

uint32_t cdep_msg(Checker *c, uint32_t str_node)
{
    char buf[512];
    asm_string(c, str_node, buf, sizeof buf);
    vec_push(&c->dep_msgs, xstrdup(buf));
    return c->dep_msgs.len;
}

void cdep_use(Checker *c, SrcLoc loc, const CSym *s, const SrcLoc *note)
{
    cdep_report(c, loc, s->name, s->flags, s->dep_msg, note);
}

void cdep_report(Checker *c, SrcLoc loc, uint32_t nameid, uint32_t flags,
                 uint32_t dep_msg, const SrcLoc *note)
{
    if (nameid)
        cdep_named(c, loc, cident(c, nameid), flags, dep_msg, note);
}

/* name NULL: an anonymous type ("type is deprecated") */
void cdep_named(Checker *c, SrcLoc loc, const char *name, uint32_t flags,
                uint32_t dep_msg, const SrcLoc *note)
{
    const char *msg = NULL;
    char what[160];
    Diagnostic *d;
    if (!(flags & (CSF_DEPRECATED | CSF_UNAVAILABLE)))
        return;
    if (dep_msg && dep_msg <= c->dep_msgs.len)
        msg = c->dep_msgs.data[dep_msg - 1];
    if (name)
        snprintf(what, sizeof what, "'%s'", name);
    else
        snprintf(what, sizeof what, "type");
    if (flags & CSF_UNAVAILABLE)
        d = msg ? cerror_d(c, loc, "%s is unavailable: %s", what, msg)
                : cerror_d(c, loc, "%s is unavailable", what);
    else
        d = msg ? cwarn_d(c, DL_WARNING, loc, "deprecated-declarations",
                          "%s is deprecated: %s", what, msg)
                : cwarn_d(c, DL_WARNING, loc, "deprecated-declarations",
                          "%s is deprecated", what);
    if (d && note)
        cnote(c, d, *note, "declared here");
}

typedef struct AsmOp {
    uint32_t e;
    bool out, reg, mem;
} AsmOp;

void cexpr_asm(Checker *c, uint32_t i)
{
    uint32_t kids[64], nk = node_children(c->nodes, i, kids, 64), k, j;
    SrcLoc loc = cnode_loc(c, i);
    char tmpl[1024], names[64][64];
    uint32_t nname = 0, nops = 0, tn = NO_NODE, nouts = 0, nins = 0;
    char opn[64][64];                   /* operand names by ordinal */
    bool extended = false, ok = true;
    AsmOp ops[64];
    if (nk > 64)
        return;
    /* count the operands first (the last one may not use '%') */
    for (k = 0; k < nk; k++) {
        if (ntag(c, kids[k]) == N_STRING && tn == NO_NODE)
            tn = kids[k];
        if (ntag(c, kids[k]) == N_ASM_SECTION) {
            uint32_t sec = c->nodes[kids[k]].aux, oc[64];
            uint32_t n2 = node_children(c->nodes, kids[k], oc, 64);
            extended = true;
            for (j = 0; j < n2 && j < 64; j++)
                if (ntag(c, oc[j]) == N_ASM_OPERAND) {
                    uint32_t ch[8], pc, q, ord = nouts + nins;
                    if (ord < 64) {
                        opn[ord][0] = 0;
                        pc = node_children(c->nodes, oc[j], ch, 8);
                        for (q = 0; q < pc && q < 8; q++)
                            if (ntag(c, ch[q]) == N_NAME) {
                                size_t len;
                                const char *s = ttext(c, c->nodes[ch[q]].tok,
                                                      &len);
                                const Tok *nt = cnode_tok(c, ch[q]);
                                if (nt->kind == TK_IDENT)
                                    snprintf(opn[ord], 64, "%s",
                                             ident_by_id(c->in,
                                                         nt->aux)->str);
                                else
                                    snprintf(opn[ord], 64, "%.*s", (int)len, s);
                            }
                    }
                    if (sec == 1)
                        nouts++;
                    else if (sec == 2)
                        nins++;
                }
        }
    }
    for (k = 0; k < nk; k++) {
        uint32_t sec, n2, o, all[64];
        if (ntag(c, kids[k]) != N_ASM_SECTION)
            continue;
        sec = c->nodes[kids[k]].aux;
        n2 = node_children(c->nodes, kids[k], all, 64);
        if (sec > 2) {
            if (sec == 4)
                for (j = 0; j < n2 && j < 64; j++)
                    if (ntag(c, all[j]) == N_NAME && nname < 64) {
                        size_t len;
                        const char *s = ttext(c, c->nodes[all[j]].tok, &len);
                        const Tok *nt = cnode_tok(c, all[j]);
                        if (nt->kind == TK_IDENT)   /* UTF-8, UCNs decoded */
                            snprintf(names[nname++], 64, "%s",
                                     ident_by_id(c->in, nt->aux)->str);
                        else
                            snprintf(names[nname++], 64, "%.*s", (int)len, s);
                    }
            continue;
        }
        for (o = 0; o < n2 && o < 64; o++) {
            uint32_t oper = all[o], pc, st = NO_NODE, ex = NO_NODE;
            uint32_t nm = NO_NODE, ch[8];
            char con[256];
            bool reg, mem;
            if (ntag(c, oper) != N_ASM_OPERAND)
                continue;
            pc = node_children(c->nodes, oper, ch, 8);
            for (j = 0; j < pc && j < 8; j++) {
                if (ntag(c, ch[j]) == N_NAME)
                    nm = ch[j];
                else if (ntag(c, ch[j]) == N_STRING)
                    st = ch[j];
                else
                    ex = ch[j];
            }
            if (nm != NO_NODE && nname < 64) {
                size_t len, q;
                const char *s = ttext(c, c->nodes[nm].tok, &len);
                char nb[64];
                snprintf(nb, sizeof nb, "%.*s", (int)len, s);
                for (q = 0; q < nname; q++)
                    if (!strcmp(names[q], nb)) {
                        cerror(c, loc, "duplicate 'asm' operand name '%s'", nb);
                        ok = false;
                        break;
                    }
                strcpy(names[nname++], nb);
            }
            if (st == NO_NODE || ex == NO_NODE)
                continue;
            asm_string(c, st, con, sizeof con);
            if (asm_constraint(c, loc, con, sec == 1,
                               nops + 1 == nouts + nins, nouts, opn,
                               nouts + nins < 64 ? nouts + nins : 64,
                               &reg, &mem) &&
                nops < 64) {
                ops[nops].e = ex;
                ops[nops].out = sec == 1;
                ops[nops].reg = reg;
                ops[nops].mem = mem;
                nops++;
            }
        }
    }
    for (j = 0; j < nops; j++)
        asm_operand(c, loc, ops[j].e, ops[j].out, ops[j].reg, ops[j].mem);
    if (!extended || tn == NO_NODE || !ok)
        return;
    /* %[name] in the template must name an operand */
    asm_string(c, tn, tmpl, sizeof tmpl);
    {
        const char *p;
        for (p = tmpl; *p; p++) {
            const char *q;
            size_t len, m;
            bool found = false;
            char nb[64];
            if (*p != '%')
                continue;
            q = p + 1;
            if (*q == '%') {
                p++;
                continue;
            }
            while (isalpha((unsigned char)*q) || *q == '=' || *q == '+' ||
                   *q == '-' || *q == '#' || *q == '*' || *q == '&')
                q++;
            if (*q != '[')
                continue;
            q++;
            len = strcspn(q, "]");
            if (!q[len])
                continue;
            snprintf(nb, sizeof nb, "%.*s", (int)len, q);
            for (m = 0; m < nname; m++)
                if (!strcmp(names[m], nb) || strchr(names[m], 92) ||
                    strchr(nb, 1))
                    found = true;
            if (!found) {
                cerror(c, cinput_loc(c, last_tok(c, i) + 1),
                       "undefined named operand '%s'", cident_ucn(nb));
                return;
            }
            p = q + len;
        }
    }
}
