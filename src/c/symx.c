/* symx.c - expression arena and the SMT-LIB2 pipe backend (see symx.h). */
#include "symx.h"

#include <stdio.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define SX_POSIX 1
#endif

/* ---- arena ---- */

void sx_free(Sx *sx) { vec_free(&sx->n); }
void sx_clear(Sx *sx) { sx->n.len = 0; }

static uint32_t sx_new(Sx *sx, int op, unsigned w, uint32_t a, uint32_t b, uint64_t k) {
    SxNode n = { (uint8_t)op, (uint8_t)w, a, b, k };
    vec_push(&sx->n, n);
    return (uint32_t)sx->n.len;
}

static const SxNode *nd(const Sx *sx, uint32_t id) { return &sx->n.data[id - 1]; }

uint32_t sx_sym(Sx *sx, uint32_t off) { return sx_new(sx, SX_SYM, 8, 0, 0, off); }
uint32_t sx_const(Sx *sx, uint8_t w, uint64_t v) { return sx_new(sx, SX_CONST, w, 0, 0, v); }
/* Byte j of node a, looking through the zero-extension and joins that loads built, so that a
 * byte stored and loaded again is the same node. */
uint32_t sx_byte(Sx *sx, uint32_t a, unsigned j)
{
    for (;;) {
        const SxNode *n = nd(sx, a);
        if (n->op == SX_EXT && !(n->k >> 9 & 1) && j < (n->k & 0xff) && j * 8 < nd(sx, n->a)->w) {
            a = n->a;
        } else if (n->op == SX_JOIN) {
            unsigned wl = nd(sx, n->b)->w;
            if (j * 8 < wl) {
                a = n->b;
            } else {
                a = n->a;
                j -= wl / 8;
            }
        } else if (n->w == 8 && j == 0 && (n->op == SX_SYM || n->op == SX_BYTE || n->op == SX_CONST)) {
            return a;
        } else if (n->op == SX_CONST && j * 8 < n->w) {
            return sx_const(sx, 8, n->k >> (8 * j) & 0xff);
        } else {
            return sx_new(sx, SX_BYTE, 8, a, 0, j);
        }
    }
}
uint32_t sx_join(Sx *sx, uint32_t hi, uint32_t lo) {
    return sx_new(sx, SX_JOIN, nd(sx, hi)->w + nd(sx, lo)->w, hi, lo, 0);
}
uint32_t sx_ext(Sx *sx, uint32_t a, unsigned bytes, bool sg, bool boolean) {
    return sx_new(sx, SX_EXT, 64, a, 0, bytes | (sg ? 1u << 8 : 0) | (boolean ? 1u << 9 : 0));
}
uint32_t sx_op(Sx *sx, int op, uint32_t a, uint32_t b) { return sx_new(sx, op, 64, a, b, 0); }

/* ---- SMT-LIB2 printing ---- */

struct SxSolver {
    int to, from;               /* pipe ends: commands out, answers in */
    long pid;
    const Sx *sx;
    VEC(uint8_t) def;           /* per node: 0 unseen, 1 reachable (being defined), 2 defined */
    VEC(uint8_t) declared;      /* per input offset: its constant is declared */
    VEC(uint32_t) syms;         /* the declared offsets, in order */
    unsigned long rlimit;
    bool broken;
    StrBuf in;                  /* answer text */
};

static void put_bits(StrBuf *sb, unsigned w, uint64_t v) {
    if (w < 64) v &= (1ull << w) - 1;
    sb_printf(sb, "#x%0*llx", (int)(w / 4), (unsigned long long)v);
}

/* A 64-bit view of node id. */
static void put_opnd(StrBuf *sb, const Sx *sx, uint32_t id) {
    unsigned w = nd(sx, id)->w;
    if (w < 64) sb_printf(sb, "((_ zero_extend %u) n%u)", 64 - w, id);
    else sb_printf(sb, "n%u", id);
}

static void put_bool(StrBuf *sb, const char *cond) {   /* a 64-bit 0 or 1 from a Bool term */
    sb_printf(sb, "(ite %s #x0000000000000001 #x0000000000000000)", cond);
}

static const char *const bin_name[] = {
    [SX_ADD] = "bvadd", [SX_SUB] = "bvsub", [SX_MUL] = "bvmul", [SX_UDIV] = "bvudiv",
    [SX_SDIV] = "bvsdiv", [SX_UREM] = "bvurem", [SX_SREM] = "bvsrem", [SX_AND] = "bvand",
    [SX_OR] = "bvor", [SX_XOR] = "bvxor", [SX_SHL] = "bvshl", [SX_LSHR] = "bvlshr",
    [SX_ASHR] = "bvashr",
    [SX_EQ] = "=", [SX_NE] = "distinct", [SX_ULT] = "bvult", [SX_ULE] = "bvule",
    [SX_SLT] = "bvslt", [SX_SLE] = "bvsle",
};

static void put_def(const Sx *sx, StrBuf *sb, uint32_t id) {
    const SxNode *n = nd(sx, id);
    sb_printf(sb, "(define-fun n%u () (_ BitVec %u) ", id, n->w);
    switch (n->op) {
    case SX_SYM:
        sb_printf(sb, "s%u", (unsigned)n->k);
        break;
    case SX_CONST:
        put_bits(sb, n->w, n->k);
        break;
    case SX_BYTE:
        sb_printf(sb, "((_ extract %u %u) n%u)", (unsigned)n->k * 8 + 7, (unsigned)n->k * 8, n->a);
        break;
    case SX_JOIN:
        sb_printf(sb, "(concat n%u n%u)", n->a, n->b);
        break;
    case SX_EXT: {
        unsigned wa = nd(sx, n->a)->w, nb = (unsigned)(n->k & 0xff) * 8;
        bool sg = n->k >> 8 & 1, boolean = n->k >> 9 & 1;
        StrBuf t = {0};
        if (nb > wa) nb = wa;
        if (nb < wa) sb_printf(&t, "((_ extract %u 0) n%u)", nb - 1, n->a);
        else sb_printf(&t, "n%u", n->a);
        if (boolean) {
            /* x != 0 on the whole value */
            sb_printf(sb, "(ite (= n%u (_ bv0 %u)) #x0000000000000000 #x0000000000000001)", n->a, wa);
        } else if (nb == 64) {
            sb_puts(sb, sb_cstr(&t));
        } else {
            sb_printf(sb, "((_ %s_extend %u) %s)", sg ? "sign" : "zero", 64 - nb, sb_cstr(&t));
        }
        sb_free(&t);
        break;
    }
    case SX_NEG: case SX_NOT:
        sb_printf(sb, "(%s ", n->op == SX_NEG ? "bvneg" : "bvnot");
        put_opnd(sb, sx, n->a);
        sb_putc(sb, ')');
        break;
    case SX_LNOT:
        sb_puts(sb, "(ite (= ");
        put_opnd(sb, sx, n->a);
        sb_puts(sb, " #x0000000000000000) #x0000000000000001 #x0000000000000000)");
        break;
    default: {
        bool cmp = n->op >= SX_EQ;
        StrBuf c = {0};
        sb_printf(&c, "(%s ", bin_name[n->op]);
        {
            StrBuf o = {0};
            put_opnd(&o, sx, n->a);
            sb_puts(&c, sb_cstr(&o));
            sb_putc(&c, ' ');
            sb_free(&o);
            put_opnd(&o, sx, n->b);
            sb_puts(&c, sb_cstr(&o));
            sb_free(&o);
        }
        sb_putc(&c, ')');
        if (cmp) put_bool(sb, sb_cstr(&c));
        else sb_puts(sb, sb_cstr(&c));
        sb_free(&c);
    }
    }
    sb_puts(sb, ")\n");
}

#define growto(v, n) do { while ((v)->len < (n)) { uint8_t z_ = 0; vec_push((v), z_); } } while (0)

/* Appends the declarations and definitions node a needs and the session lacks. */
static void put_defs(SxSolver *s, StrBuf *sb, uint32_t a) {
    const Sx *sx = s->sx;
    VEC(uint32_t) stk = {0};
    uint32_t lo = a;
    growto(&s->def, sx->n.len);
    if (s->def.data[a - 1]) return;
    s->def.data[a - 1] = 1;
    vec_push(&stk, a);
    while (stk.len) {
        const SxNode *n = nd(sx, vec_pop(&stk));
        uint32_t ch[2] = { 0, 0 };
        switch (n->op) {
        case SX_BYTE: case SX_EXT: case SX_NEG: case SX_NOT: case SX_LNOT: ch[0] = n->a; break;
        case SX_SYM: case SX_CONST: break;
        default: ch[0] = n->a; ch[1] = n->b; break;
        }
        for (int i = 0; i < 2; i++) {
            if (ch[i] && !s->def.data[ch[i] - 1]) {
                s->def.data[ch[i] - 1] = 1;
                if (ch[i] < lo) lo = ch[i];
                vec_push(&stk, ch[i]);
            }
        }
    }
    vec_free(&stk);
    /* children have lower ids than parents, so ascending order defines before use */
    for (uint32_t id = lo; id <= a; id++) {
        if (s->def.data[id - 1] != 1) continue;
        const SxNode *n = nd(sx, id);
        if (n->op == SX_SYM) {
            uint32_t off = (uint32_t)n->k;
            growto(&s->declared, (size_t)off + 1);
            if (!s->declared.data[off]) {
                s->declared.data[off] = 1;
                vec_push(&s->syms, off);
                sb_printf(sb, "(declare-const s%u (_ BitVec 8))\n", off);
            }
        }
        put_def(sx, sb, id);
        s->def.data[id - 1] = 2;
    }
}

static void put_cond(SxSolver *s, StrBuf *sb, uint32_t a, bool want) {
    unsigned w = nd(s->sx, a)->w;
    if (want) sb_printf(sb, "(assert (not (= n%u (_ bv0 %u))))\n", a, w);
    else sb_printf(sb, "(assert (= n%u (_ bv0 %u)))\n", a, w);
}

/* ---- the pipe ---- */

#ifdef SX_POSIX

static bool rt(SxSolver *s, const char *text, bool *err) {
    static const char eoq[] = "<<EOQ>>\n";
    size_t len, off = 0;
    StrBuf cmd = {0};
    sb_puts(&cmd, text);
    sb_puts(&cmd, "(echo \"<<EOQ>>\")\n");
    len = cmd.len;
    while (off < len) {
        ssize_t w = write(s->to, cmd.data + off, len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { sb_free(&cmd); s->broken = true; return false; }
        off += (size_t)w;
    }
    sb_free(&cmd);
    s->in.len = 0;
    for (;;) {
        char buf[4096];
        struct pollfd p = { s->from, POLLIN, 0 };
        int pr = poll(&p, 1, 120000);
        ssize_t r;
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) { s->broken = true; return false; }
        r = read(s->from, buf, sizeof buf);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { s->broken = true; return false; }
        sb_putn(&s->in, buf, (size_t)r);
        if (s->in.len >= 8 && memcmp(s->in.data + s->in.len - 8, eoq, 8) == 0) break;
    }
    s->in.len -= 8;
    if (err) *err = strstr(sb_cstr(&s->in), "(error") != NULL;
    return true;
}

SxSolver *sx_open(const char *cmd, char *err, size_t errn) {
    int a[2], b[2];
    SxSolver *s;
    pid_t pid;
    bool e = false;
    signal(SIGPIPE, SIG_IGN);
    if (pipe(a) < 0 || pipe(b) < 0) { snprintf(err, errn, "pipe: %s", strerror(errno)); return NULL; }
    pid = fork();
    if (pid < 0) { snprintf(err, errn, "fork: %s", strerror(errno)); return NULL; }
    if (pid == 0) {
        dup2(a[0], 0);
        dup2(b[1], 1);
        close(a[0]); close(a[1]); close(b[0]); close(b[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(a[0]);
    close(b[1]);
    s = xcalloc(1, sizeof *s);
    s->to = a[1];
    s->from = b[0];
    s->pid = pid;
    s->rlimit = 0;
    if (!rt(s, "(set-option :produce-models true)\n(set-logic QF_BV)\n"
               "(declare-const p (_ BitVec 8))\n(assert (= (bvmul p #x03) #x63))\n(check-sat)\n",
            &e) ||
        e || strncmp(s->in.data, "sat", 3) != 0) {
        snprintf(err, errn, "solver '%s' does not answer an SMT-LIB2 query%s%.80s", cmd,
                 s->broken ? "" : ": ", s->broken ? "" : s->in.data);
        sx_close(s);
        return NULL;
    }
    return s;
}

void sx_close(SxSolver *s) {
    int st;
    if (!s) return;
    close(s->to);
    close(s->from);
    waitpid((pid_t)s->pid, &st, 0);
    vec_free(&s->def);
    vec_free(&s->declared);
    vec_free(&s->syms);
    sb_free(&s->in);
    free(s);
}

#else

SxSolver *sx_open(const char *cmd, char *err, size_t errn) {
    (void)cmd;
    snprintf(err, errn, "the solver pipe needs POSIX fork/pipe");
    return NULL;
}
void sx_close(SxSolver *s) { (void)s; }
static bool rt(SxSolver *s, const char *text, bool *err) { (void)s; (void)text; (void)err; return false; }

#endif

void sx_limit(SxSolver *s, unsigned long rlimit) { s->rlimit = rlimit; }

void sx_begin(SxSolver *s, const Sx *sx) {
    StrBuf sb = {0};
    bool e = false;
    s->sx = sx;
    s->def.len = 0;
    s->declared.len = 0;
    s->syms.len = 0;
    sb_puts(&sb, "(reset)\n(set-option :produce-models true)\n");
    if (s->rlimit) sb_printf(&sb, "(set-option :rlimit %lu)\n", s->rlimit);
    sb_puts(&sb, "(set-logic QF_BV)\n");
    if (!rt(s, sb.data, &e) || e) s->broken = true;
    sb_free(&sb);
}

bool sx_assert(SxSolver *s, uint32_t a, bool want) {
    StrBuf sb = {0};
    bool e = false, ok;
    if (s->broken) return false;
    put_defs(s, &sb, a);
    put_cond(s, &sb, a, want);
    ok = rt(s, sb.data, &e) && !e;
    sb_free(&sb);
    if (!ok) s->broken = true;
    return ok;
}

void sx_model_free(SxModel *m) { vec_free(&m->off); vec_free(&m->val); }

int sx_try(SxSolver *s, const SxCond *c, size_t n, SxModel *m) {
    StrBuf sb = {0};
    bool e = false;
    int res;
    if (s->broken) return SX_ERROR;
    for (size_t i = 0; i < n; i++) put_defs(s, &sb, c[i].a);    /* before the push, so the pop keeps them */
    sb_puts(&sb, "(push 1)\n");
    for (size_t i = 0; i < n; i++) put_cond(s, &sb, c[i].a, c[i].want);
    sb_puts(&sb, "(check-sat)\n");
    if (!rt(s, sb.data, &e) || e) { sb_free(&sb); s->broken = true; return SX_ERROR; }
    if (!strncmp(s->in.data, "sat", 3)) res = SX_SAT;
    else if (!strncmp(s->in.data, "unsat", 5)) res = SX_UNSAT;
    else res = SX_UNKNOWN;
    sb.len = 0;
    m->off.len = m->val.len = 0;
    if (res == SX_SAT && s->syms.len) {
        sb_puts(&sb, "(get-value (");
        for (size_t i = 0; i < s->syms.len; i++) sb_printf(&sb, "%ss%u", i ? " " : "", s->syms.data[i]);
        sb_puts(&sb, "))\n");
        if (!rt(s, sb.data, &e) || e) { sb_free(&sb); s->broken = true; return SX_ERROR; }
        for (const char *p = s->in.data; (p = strstr(p, "(s")) != NULL;) {
            char *q;
            unsigned long off = strtoul(p + 2, &q, 10);
            uint32_t o;
            uint8_t v;
            while (*q == ' ') q++;
            if (q[0] == '#' && q[1] == 'x') v = (uint8_t)strtoul(q + 2, &q, 16);
            else if (q[0] == '#' && q[1] == 'b') v = (uint8_t)strtoul(q + 2, &q, 2);
            else { p += 2; continue; }
            o = (uint32_t)off;
            vec_push(&m->off, o);
            vec_push(&m->val, v);
            p = q;
        }
        sb.len = 0;
    }
    sb_puts(&sb, "(pop 1)\n");
    if (!rt(s, sb.data, &e) || e) s->broken = true;
    sb_free(&sb);
    return s->broken ? SX_ERROR : res;
}
