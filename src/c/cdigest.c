/* cdigest.c - the AST digest (docs/C_DESIGN.md 6.3): a hash of a unit's
 * tree that ignores parentheses, for gating the fixes that only add them.
 * It mixes each node's tag, flags and child count, the spelling of the
 * tokens that carry meaning, and the checked type of every expression. */
#include "c/cexpr_int.h"

#include <string.h>

static inline uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0x100000001B3ull;
    return h;
}

static uint64_t hash_str(const char *s, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--)
        h = (h ^ (unsigned char)*s++) * 0x100000001B3ull;
    return h;
}

/* tags whose token spelling is part of the meaning */
static bool spelled(unsigned tag)
{
    switch (tag) {
    case N_IDENT: case N_NUMBER: case N_CHAR: case N_STRING:
    case N_MEMBER_EXPR: case N_POSTFIX: case N_UNARY: case N_BINARY:
    case N_ASSIGN: case N_STORAGE: case N_QUAL: case N_FUNCSPEC:
    case N_TYPESPEC: case N_TYPEDEF_NAME: case N_TAG: case N_NAME:
    case N_KR_IDENT: case N_DESIG_FIELD: case N_GOTO: case N_LABEL:
    case N_ENUMERATOR: case N_ATTR_ITEM: case N_ADDR_LABEL:
        return true;
    }
    return false;
}

typedef struct TyMemo {
    TypeId t;
    uint64_t h;
    bool set;
} TyMemo;

static uint64_t type_hash(Checker *c, TyMemo *memo, TypeId t)
{
    TyMemo *m = &memo[(t * 2654435761u) & 255];
    if (!m->set || m->t != t) {
        StrBuf sb = {0};
        type_print(&c->tt, &sb, t);
        m->h = hash_str(sb.data ? sb.data : "", sb.len);
        m->t = t;
        m->set = true;
        sb_free(&sb);
    }
    return m->h;
}

/* Bottom-up over the post-order array (a child's hash is ready before its
 * parent's), so a deep expression costs no recursion. */
void checker_digest_unit(Checker *c)
{
    TyMemo memo[256];
    uint32_t i, k;
    uint64_t *hs;
    uint32_t stackbuf[64], *kids;
    if (!c->nn)
        return;
    memset(memo, 0, sizeof memo);
    hs = xmalloc(c->nn * sizeof *hs);
    for (i = 0; i < c->nn; i++) {
        const Node *n = &c->nodes[i];
        uint32_t nk;
        uint64_t h;
        kids = stackbuf;
        nk = node_children(c->nodes, i, stackbuf, 64);
        if (nk > 64) {
            kids = xmalloc(nk * sizeof *kids);
            node_children(c->nodes, i, kids, nk);
        }
        if (n->tag == N_PAREN && nk == 1) {
            hs[i] = hs[kids[0]];
            continue;
        }
        h = mix(mix(n->tag, n->flags), nk);
        if (spelled(n->tag)) {
            size_t len;
            const char *s = ttext(c, n->tok, &len);
            h = mix(h, hash_str(s, len));
        }
        if (cexpr_is_expr(n->tag)) {
            TypeId t = c->ty[i];
            h = mix(h, t == TYPE_B(ERROR) ? 0 : type_hash(c, memo, t));
        }
        for (k = 0; k < nk; k++)
            h = mix(h, hs[kids[k]]);
        if (kids != stackbuf)
            free(kids);
        hs[i] = h;
    }
    *c->opt.digest = mix(*c->opt.digest, hs[c->nn - 1]);
    free(hs);
}
