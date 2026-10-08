/* cprint.c - printing expressions the way gcc's %E does, for diagnostics
 * (split from cexpr.c).  The shared readers are cexpr_int.h's. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- printing expressions (gcc's %E) ---------------------------------------- */

static void print_ival(Checker *c, StrBuf *sb, TypeId t, uint64_t v)
{
    if (is_int(c, t) && is_signed(c, t))
        sb_printf(sb, "%lld", (long long)(int64_t)v);
    else
        sb_printf(sb, "%llu", (unsigned long long)v);
}

static int bin_prec(int op)
{
    switch (op) {
    case P_COMMA: return PR_COMMA;
    case P_OROR: return PR_LOR;
    case P_ANDAND: return PR_LAND;
    case P_PIPE: return PR_OR;
    case P_CARET: return PR_XOR;
    case P_AMP: return PR_AND;
    case P_EQEQ: case P_NE: return PR_EQ;
    case P_LT: case P_GT: case P_LE: case P_GE: return PR_REL;
    case P_SHL: case P_SHR: return PR_SHIFT;
    case P_PLUS: case P_MINUS: return PR_ADD;
    default: return PR_MUL;
    }
}

/* Does node i print as its value (an INTEGER_CST)? */
bool prints_value(Checker *c, uint32_t i)
{
    return has_ival(c, i) &&
           (c->ck[i] == K_ICE || (c->ef[i] & (EF_CST | EF_NOPCST)));
}

static bool pfloat(Checker *c, StrBuf *sb, const char *s, size_t len);
static void pcond_arm(Checker *c, StrBuf *sb, uint32_t arm, TypeId rt, int prec);
static void pcond_test(Checker *c, StrBuf *sb, uint32_t cond);

/* Operand i converted to type t (an implicit conversion prints as a
 * cast). */
static void pconv(Checker *c, StrBuf *sb, uint32_t i, TypeId t, int prec)
{
    i = strip_paren(c, i);
    if (i == NO_NODE)
        return;
    if (is_arith(c, t) && !is_err(c, t) && mainv(c, rvt(c, i)) != mainv(c, t)) {
        if (prints_value(c, i) && is_int(c, t)) {
            print_ival(c, sb, t, cexpr_trunc(c, t, c->cv[i]));
            return;
        }
        if (prec > PR_UNARY)
            sb_putc(sb, '(');
        sb_putc(sb, '(');
        type_print(TT, sb, mainv(c, t));
        sb_putc(sb, ')');
        pexpr(c, sb, i, PR_UNARY);
        if (prec > PR_UNARY)
            sb_putc(sb, ')');
        return;
    }
    pexpr(c, sb, i, prec);
}

/* gcc builds p[i] on a pointer as *(p + i * size) and prints that tree:
 * the byte offset of a constant index, (sizetype)(...) of a variable one.
 * Returns the base and index nodes when node i is such a subscript and its
 * spelling is one cereal reproduces (no folded sums of a signed index). */
static bool lowered_sub(Checker *c, uint32_t i, uint32_t *base, uint32_t *idx,
                        uint64_t *size)
{
    uint32_t k[3], b, x, sx;
    TypeId bt, pt;
    bool ov;
    if (ntag(c, i) != N_INDEX || nkids(c, i, k, 3) < 2)
        return false;
    b = strip_paren(c, k[0]);
    x = strip_paren(c, k[1]);
    if (b == NO_NODE || x == NO_NODE)
        return false;
    bt = type_canon(TT, c->ty[b]);
    if (type_ckind(TT, bt) != TY_PTR) {
        uint32_t t = b;
        b = x;
        x = t;
        bt = type_canon(TT, c->ty[b]);
        if (type_ckind(TT, bt) != TY_PTR)
            return false;
    }
    if (!is_int(c, rvt(c, x)))
        return false;
    pt = type_canon(TT, type_base(TT, bt));
    *size = type_size(TT, pt, &ov);
    if (!ov || !*size || tkind(c, pt) == TY_VLA)
        return false;
    if (c->ck[x] != K_ICE) {
        sx = x;
        /* sums and differences of a signed index are folded by gcc */
        if (ntag(c, sx) == N_BINARY && (npunct(c, sx) == P_PLUS ||
                                        npunct(c, sx) == P_MINUS) &&
            is_signed(c, rvt(c, sx))) {
            uint32_t sk[3];
            if (nkids(c, sx, sk, 3) >= 2 &&
                c->ck[strip_paren(c, sk[1])] == K_ICE &&
                (npunct(c, sx) != P_PLUS ||
                 cexpr_sval(c, strip_paren(c, sk[1])) <= 0 ||
                 c->ck[strip_paren(c, sk[0])] == K_ICE ||
                 type_size(TT, rvt(c, sx), &ov) != 4))
                return false;
        }
        if (ntag(c, sx) == N_COND) {
            /* gcc folds a conditional with a constant arm */
            uint32_t ck[3];
            if (nkids(c, sx, ck, 3) < 3 ||
                c->ck[strip_paren(c, ck[1])] == K_ICE ||
                c->ck[strip_paren(c, ck[2])] == K_ICE)
                return false;
        }
        if (has_ival(c, sx))
            return false;
    }
    *base = b;
    *idx = x;
    return true;
}

static void plowered(Checker *c, StrBuf *sb, uint32_t b, uint32_t x,
                     uint64_t size)
{
    char num[32];
    sb_putc(sb, '*');
    if (c->ck[x] == K_ICE) {
        int64_t off = cexpr_sval(c, x) * (int64_t)size;
        if (off == 0) {
            pexpr(c, sb, b, PR_UNARY);
            return;
        }
        sb_putc(sb, '(');
        pexpr(c, sb, b, PR_ADD);
        snprintf(num, sizeof num, " + %lld)", (long long)off);
        sb_puts(sb, num);
        return;
    }
    sb_putc(sb, '(');
    pexpr(c, sb, b, PR_ADD);
    sb_puts(sb, " + ");
    {
        uint32_t sk[3], sx = strip_paren(c, x);
        if (ntag(c, sx) == N_BINARY && npunct(c, sx) == P_PLUS &&
            is_signed(c, rvt(c, sx)) && nkids(c, sx, sk, 3) >= 2 &&
            c->ck[strip_paren(c, sk[1])] == K_ICE) {
            /* gcc distributes: ((sizetype)i + 1) * size */
            snprintf(num, sizeof num, " + %lld)", (long long)
                     cexpr_sval(c, strip_paren(c, sk[1])));
            sb_puts(sb, "((sizetype)");
            pexpr(c, sb, sk[0], PR_UNARY);
            sb_puts(sb, num);
            if (size != 1) {
                snprintf(num, sizeof num, " * %llu", (unsigned long long)size);
                sb_puts(sb, num);
            }
            sb_putc(sb, ')');
            return;
        }
    }
    sb_puts(sb, "(sizetype)");
    {
        TypeId xt = type_canon(TT, rvt(c, x));
        bool wide_u = type_size(TT, xt, &(bool){0}) == 8 && !is_signed(c, xt);
        if (size == 1) {
            pexpr(c, sb, x, PR_UNARY);
        } else if (wide_u) {
            sb_putc(sb, '(');
            pexpr(c, sb, x, PR_MUL);
            snprintf(num, sizeof num, " * %llu)", (unsigned long long)size);
            sb_puts(sb, num);
        } else {
            sb_puts(sb, "((long unsigned int)");
            pexpr(c, sb, x, PR_UNARY);
            snprintf(num, sizeof num, " * %llu)", (unsigned long long)size);
            sb_puts(sb, num);
        }
    }
    sb_putc(sb, ')');
}

/* Whether the lvalue at node i is const through its own type or through a
 * const-qualified enclosing object (gcc's tree gives such members const
 * types). */
static bool const_through(Checker *c, uint32_t i)
{
    uint32_t b;
    while (i != NO_NODE && ntag(c, i) == N_PAREN)
        i = first_child(c, i);
    if (i == NO_NODE)
        return false;
    if (tquals(c, c->ty[i]) & TQ_CONST)
        return true;
    if (ntag(c, i) == N_MEMBER_EXPR) {
        b = first_child(c, i);
        if (b == NO_NODE)
            return false;
        if ((c->nodes[i].flags & NF_ARROW) && is_ptr(c, rvt(c, b)))
            return tquals(c, pointee(c, rvt(c, b))) & TQ_CONST;
        return const_through(c, b);
    }
    if (ntag(c, i) == N_INDEX) {
        b = first_child(c, i);
        return b != NO_NODE && is_array(c, c->ty[b]) && const_through(c, b);
    }
    return false;
}

/* The condition of `?:` as gcc prints it after truth-value conversion:
 * `(c) != 0`, `(d) != (0.0)`, `(a) < (b)`, `(c) == 0` for `!c`. */
static bool pcond_plain;   /* print a ?: test as written, not folded */

static void pcond_test(Checker *c, StrBuf *sb, uint32_t cond)
{
    uint32_t e = strip_paren(c, cond), k[3];
    TypeId t;
    int op;
    if (e == NO_NODE || has_ival(c, e) || prints_value(c, e) ||
        c->ck[e] == K_ERR || is_err(c, c->ty[e])) {
        pexpr(c, sb, cond, PR_LOR);
        return;
    }
    t = rvt(c, e);
    if (pcond_plain) {
        bool lg = (ntag(c, e) == N_BINARY && (op = npunct(c, e), op == P_ANDAND ||
                   op == P_OROR || bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL)) ||
                  (ntag(c, e) == N_UNARY && npunct(c, e) == P_BANG);
        pexpr(c, sb, e, lg ? PR_LOR : PR_EQ);
        if (!lg)
            sb_puts(sb, " != 0");
        return;
    }
    if (ntag(c, e) == N_BINARY && nkids(c, e, k, 3) == 2) {
        op = npunct(c, e);
        if (op == P_ANDAND || op == P_OROR) {
            pcond_test(c, sb, k[0]);
            sb_puts(sb, op == P_ANDAND ? " && " : " || ");
            pcond_test(c, sb, k[1]);
            return;
        }
        if (bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL) {
            bool ar = is_arith(c, rvt(c, k[0])) && is_arith(c, rvt(c, k[1]));
            if (ar) {
                sb_putc(sb, '(');
                pexpr(c, sb, k[0], PR_COMMA);
                sb_putc(sb, ')');
            } else {
                pexpr(c, sb, k[0], bin_prec(op));
            }
            sb_printf(sb, " %s ", punct_spelling[op]);
            if (ar) {
                sb_putc(sb, '(');
                pexpr(c, sb, k[1], PR_COMMA);
                sb_putc(sb, ')');
            } else {
                pexpr(c, sb, k[1], bin_prec(op) + 1);
            }
            return;
        }
    }
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_BANG &&
        nkids(c, e, k, 3) == 1 && is_arith(c, rvt(c, k[0]))) {
        sb_putc(sb, '(');
        pexpr(c, sb, k[0], PR_COMMA);
        sb_puts(sb, ") == 0");
        return;
    }
    if (is_ptr(c, t)) {
        pexpr(c, sb, e, PR_EQ);
        sb_puts(sb, " != 0");
    } else if (is_arith(c, t)) {
        sb_putc(sb, '(');
        if (tkind(c, t) == TY_CHAR || tkind(c, t) == TY_SCHAR) {
            sb_puts(sb, "(signed char)");
            pexpr(c, sb, e, PR_UNARY);
        } else if (tkind(c, t) == TY_UCHAR) {
            sb_puts(sb, "(unsigned char)");
            pexpr(c, sb, e, PR_UNARY);
        } else {
            pexpr(c, sb, e, PR_COMMA);
        }
        sb_puts(sb, is_flt(c, t) ? ") != (0.0)" : ") != 0");
    } else {
        pexpr(c, sb, cond, PR_LOR);
    }
}

/* An arm of `?:` with a pointer result: gcc prints it converted to the
 * result type (an array arm as the address of the whole array). */
static void pcond_arm(Checker *c, StrBuf *sb, uint32_t arm, TypeId rt, int prec)
{
    TypeId at = c->ty[arm];
    bool arr = is_array(c, at);
    if (is_ptr(c, rt) && (arr || is_ptr(c, at)) && (arr || at != rt)) {
        sb_putc(sb, '(');
        type_print(TT, sb, rt);
        sb_putc(sb, ')');
        if (arr)
            sb_putc(sb, '&');
        pexpr(c, sb, arm, PR_UNARY);
        return;
    }
    pexpr(c, sb, arm, prec);
}

void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec)
{
    uint32_t k[3], n, lb, lx;
    uint64_t lsz;
    int my = PR_PRIMARY, op;
    size_t len;
    const char *s;
    i = strip_paren(c, i);
    if (i == NO_NODE)
        return;
    if (prints_value(c, i)) {
        print_ival(c, sb, rvt(c, i), c->cv[i]);
        return;
    }
    n = nkids(c, i, k, 3);
    switch (ntag(c, i)) {
    case N_BINARY: my = bin_prec(npunct(c, i)); break;
    case N_ASSIGN: my = PR_ASSIGN; break;
    case N_COND: my = PR_COND; break;
    case N_CAST: case N_UNARY: case N_SIZEOF_EXPR: case N_SIZEOF_TYPE:
    case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE:
        my = PR_UNARY;
        break;
    case N_INDEX: case N_CALL: case N_MEMBER_EXPR: case N_POSTFIX:
        my = ntag(c, i) == N_INDEX && lowered_sub(c, i, &lb, &lx, &lsz)
                 ? PR_UNARY : PR_POSTFIX;
        break;
    default: break;
    }
    if (my < prec)
        sb_putc(sb, '(');
    switch (ntag(c, i)) {
    case N_IDENT: case N_NUMBER: case N_CHAR:
        s = ttext(c, c->nodes[i].tok, &len);
        if (ntag(c, i) == N_NUMBER && pfloat(c, sb, s, len))
            break;
        sb_putn(sb, s, len);
        break;
    case N_STRING: {
        uint32_t t;
        for (t = 0; t < node_pieces(c, i); t++) {
            if (t)
                sb_putc(sb, ' ');
            s = ttext(c, c->nodes[i].tok + t, &len);
            sb_putn(sb, s, len);
        }
        break;
    }
    case N_BINARY:
        if (n < 2)
            break;
        op = npunct(c, i);
        if (op == P_COMMA || op == P_ANDAND || op == P_OROR) {
            pexpr(c, sb, k[0], my);
            sb_puts(sb, op == P_COMMA ? ", " : op == P_ANDAND ? " && "
                                                              : " || ");
            pexpr(c, sb, k[1], my + 1);
        } else {
            TypeId t0 = rvt(c, i), t1 = rvt(c, i);
            if (op == P_SHL || op == P_SHR) {
                t0 = promoted(c, k[0]);
                t1 = promoted(c, k[1]);
            } else if (bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL) {
                t0 = t1 = TYPE_B(ERROR);
                if (is_arith(c, rvt(c, k[0])) && is_arith(c, rvt(c, k[1])))
                    t0 = t1 = common_type(c, promoted(c, k[0]),
                                          promoted(c, k[1]));
            } else if (!is_arith(c, t0)) {
                t0 = t1 = TYPE_B(ERROR);
            }
            pconv(c, sb, k[0], t0, my);
            sb_printf(sb, " %s ", punct_spelling[op]);
            pconv(c, sb, k[1], t1, my + 1);
        }
        break;
    case N_ASSIGN:
        if (n < 2)
            break;
        pexpr(c, sb, k[0], PR_UNARY);
        sb_printf(sb, " %s ", punct_spelling[npunct(c, i)]);
        pexpr(c, sb, k[1], PR_ASSIGN);
        break;
    case N_COND:
        if (n < 2)
            break;
        pcond_test(c, sb, k[0]);
        sb_puts(sb, " ? ");
        if (n == 3)
            pcond_arm(c, sb, k[1], c->ty[i], PR_COMMA);
        sb_puts(sb, " : ");
        pcond_arm(c, sb, k[n - 1], c->ty[i], PR_COND);
        break;
    case N_CAST:
        if (n < 2)
            break;
        if (mainv(c, c->ty[i]) == mainv(c, rvt(c, k[1]))) {
            pexpr(c, sb, k[1], PR_UNARY);
            break;
        }
        sb_putc(sb, '(');
        type_print(TT, sb, c->ty[i]);
        sb_putc(sb, ')');
        if (c->ty[k[1]] != ERRT && tkind(c, c->ty[k[1]]) == TY_FUNC) {
            /* a function designator has decayed: gcc prints its address */
            uint32_t x = strip_paren(c, k[1]), q[3];
            while (x != NO_NODE && ntag(c, x) == N_UNARY &&
                   npunct(c, x) == P_STAR && nkids(c, x, q, 3) == 1 &&
                   tkind(c, c->ty[q[0]]) == TY_FUNC)
                x = strip_paren(c, q[0]);
            sb_putc(sb, '&');
            pexpr(c, sb, x, PR_UNARY);
            break;
        }
        pexpr(c, sb, k[1], PR_UNARY);
        break;
    case N_UNARY:
        if (n < 1)
            break;
        op = npunct(c, i);
        if (op == P_STAR && c->ty[k[0]] != ERRT && is_array(c, c->ty[k[0]])) {
            /* gcc prints a deref of an array as *(T *)&array */
            uint32_t x = k[0];
            TypeId et = elem_of(c, c->ty[x]);
            while (ntag(c, x) == N_UNARY && npunct(c, x) == P_STAR &&
                   nkids(c, x, k, 3) >= 1 && is_array(c, c->ty[k[0]]))
                x = k[0];
            while (is_array(c, et))
                et = elem_of(c, et);
            if (const_through(c, x))
                et = type_qual(et, TQ_CONST);
            sb_puts(sb, "*(");
            type_print(TT, sb, et);
            sb_puts(sb, " *)&");
            pexpr(c, sb, x, PR_UNARY);
            break;
        }
        if (op != P_NONE) {
            sb_puts(sb, punct_spelling[op]);
        } else {
            s = ttext(c, c->nodes[i].tok, &len);
            sb_putn(sb, s, len);
            sb_putc(sb, ' ');
        }
        if ((op == P_MINUS || op == P_PLUS || op == P_TILDE) &&
            is_arith(c, rvt(c, i)))
            pconv(c, sb, k[0], rvt(c, i), PR_UNARY);
        else
            pexpr(c, sb, k[0], PR_UNARY);
        break;
    case N_POSTFIX:
        if (n < 1)
            break;
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_puts(sb, punct_spelling[npunct(c, i)]);
        break;
    case N_SIZEOF_EXPR: case N_ALIGNOF_EXPR:
        s = ttext(c, c->nodes[i].tok, &len);
        sb_putn(sb, s, len);
        sb_putc(sb, ' ');
        if (n)
            pexpr(c, sb, k[0], PR_UNARY);
        break;
    case N_INDEX:
        if (n < 2)
            break;
        if (lowered_sub(c, i, &lb, &lx, &lsz)) {
            plowered(c, sb, lb, lx, lsz);
            break;
        }
        if (c->ty[k[0]] != ERRT && tkind(c, c->ty[k[0]]) == TY_VECTOR) {
            /* gcc subscripts a vector through an array view of it */
            TypeId vt = c->ty[k[0]];
            uint64_t esz = type_size(TT, type_base(TT, vt), &(bool){0});
            sb_puts(sb, "((");
            type_print(TT, sb, type_qual(type_base(TT, vt), TYPE_QUALS(vt)));
            sb_printf(sb, "[%u])", (unsigned)(type_size(TT, vt, &(bool){0}) / (esz ? esz : 1)));
            pexpr(c, sb, k[0], PR_UNARY);
            sb_putc(sb, ')');
        } else {
            pexpr(c, sb, k[0], PR_POSTFIX);
        }
        sb_putc(sb, '[');
        pexpr(c, sb, k[1], PR_COMMA);
        sb_putc(sb, ']');
        break;
    case N_MEMBER_EXPR:
        if (n < 1)
            break;
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_puts(sb, c->nodes[i].flags & NF_ARROW ? "->" : ".");
        if (!(c->nodes[i].flags & NF_ERROR)) {
            s = ttext(c, c->nodes[i].tok, &len);
            sb_putn(sb, s, len);
        }
        break;
    case N_CALL: {
        uint32_t a[64], m = nkids(c, i, a, 64), j;
        if (m == 0)
            break;
        pexpr(c, sb, a[0], PR_POSTFIX);
        sb_puts(sb, " (");
        for (j = 1; j < m && j < 64; j++) {
            if (j > 1)
                sb_puts(sb, ", ");
            pexpr(c, sb, a[j], PR_ASSIGN);
        }
        sb_putc(sb, ')');
        break;
    }
    case N_CONVERTVECTOR: {
        uint32_t a[2];
        if (nkids(c, i, a, 2) < 1)
            break;
        sb_puts(sb, "VEC_CONVERT(");
        pexpr(c, sb, a[0], PR_ASSIGN);
        sb_putc(sb, ')');
        break;
    }
    default:
        sb_puts(sb, "...");
        break;
    }
    if (my < prec)
        sb_putc(sb, ')');
}

/* A floating literal as gcc prints it (real_to_decimal at the type's full
 * digit count, trailing zeros dropped: 'd.ddde+X', and the f/l suffix);
 * false for anything else. */
static bool pfloat(Checker *c, StrBuf *sb, const char *s, size_t len)
{
    char t[96], o[64], *e, *ep, *z;
    long double v;
    int ex;
    char sfx = 0;
    bool hex = len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    (void)c;
    if (len >= sizeof t || len == 0)
        return false;
    memcpy(t, s, len);
    t[len] = 0;
    if (t[len - 1] == 'f' || t[len - 1] == 'F' || t[len - 1] == 'l' ||
        t[len - 1] == 'L') {
        if (!hex || strpbrk(t, "pP")) {
            sfx = (char)tolower((unsigned char)t[len - 1]);
            t[--len] = 0;
        }
    }
    if (!strpbrk(t, hex ? ".pP" : ".eE"))
        return false;
    v = strtold(t, &e);
    if (*e)
        return false;
    if (sfx == 'f')
        snprintf(o, sizeof o, "%.8e", (double)(float)v);
    else if (sfx == 'l')
        snprintf(o, sizeof o, "%.20Le", v);
    else
        snprintf(o, sizeof o, "%.16e", (double)v);
    ep = strchr(o, 'e');
    ex = atoi(ep + 1);
    *ep = 0;
    for (z = ep - 1; z > o && *z == '0' && z[-1] != '.'; z--)
        *z = 0;
    sb_puts(sb, o);
    sb_printf(sb, "e%c%d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
    if (sfx)
        sb_putc(sb, sfx);
    return true;
}

/* %E of node i, in a buffer valid until the next call (two rotate). */
const char *estr(Checker *c, uint32_t i)
{
    StrBuf *sb = &c->esb[c->enext++ & 1];
    sb->len = 0;
    pexpr(c, sb, i, PR_COMMA);
    return sb_cstr(sb);
}

const char *cexpr_str(Checker *c, uint32_t i)
{
    return estr(c, i);
}

/* cexpr_str as written (gcc prints the unfolded expression in some messages) */
const char *cexpr_str_plain(Checker *c, uint32_t i)
{
    const char *r;
    pcond_plain = true;
    r = estr(c, i);
    pcond_plain = false;
    return r;
}

const char *vstr(Checker *c, TypeId t, uint64_t v)
{
    char *b = c->vbuf[c->vnext++ & 1];
    if (is_int(c, t) && is_signed(c, t) && int_bits(c, t) <= 64)
        snprintf(b, sizeof c->vbuf[0], "%lld", (long long)(int64_t)v);
    else
        snprintf(b, sizeof c->vbuf[0], "%llu", (unsigned long long)v);
    return b;
}

