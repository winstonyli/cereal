/* ast.c - syntax tree utilities (ast.h) and the tree dump (parse.h). */
#include "c/ast.h"

#include <stdio.h>

#include "c/parse.h"

const char *const node_names[N_COUNT] = {
#define X(n) #n,
    NODE_LIST(X)
#undef X
};

uint32_t node_children(const Node *nodes, uint32_t i, uint32_t *out,
                       uint32_t max)
{
    /* Walk the children last to first; the first `max` in source order are
     * stored (out[0] is the first child), the return value is the total. */
    uint32_t n = 0, j = i, lo = i + 1 - nodes[i].size, pos;
    while (j > lo) {
        j--;
        n++;
        j = j + 1 - nodes[j].size;
    }
    pos = n;
    j = i;
    while (j > lo) {
        j--;
        pos--;
        if (pos < max)
            out[pos] = j;
        j = j + 1 - nodes[j].size;
    }
    return n;
}

static void dump_node(FILE *out, const ParseUnit *u, SrcMgr *sm,
                      const Interner *in, uint32_t i, int depth)
{
    const Node *n = &u->nodes[i];
    uint32_t kids[64], nk, k;
    uint32_t *all = kids;
    fprintf(out, "%*s%s", depth * 2, "", node_names[n->tag]);
    if (n->tok < u->ntoks) {
        const PTok *t = &u->toks[n->tok];
        SrcLoc loc = t->exp ? t->exp : t->t.loc;
        SrcFile *f = srcmgr_file_of(sm, loc);
        uint32_t line = 0, col = 0;
        fprintf(out, " '%.*s'", t->t.len > 32 ? 32 : (int)t->t.len,
                tok_text_raw(sm, in, &t->t));
        if (f) {
            srcmgr_linecol(f, loc, &line, &col);
            fprintf(out, " %u:%u", line, col);
        }
    }
    {
        uint32_t aux = n->aux;
        if (n->tag == N_STRING) {       /* the pieces: the string-token run */
            aux = 1;
            while (n->tok + aux < u->ntoks &&
                   u->toks[n->tok + aux].t.kind == TK_STRING)
                aux++;
        }
        if (aux)
            fprintf(out, " #%u", aux);
    }
    if (n->flags)
        fprintf(out, " [%x]", n->flags);
    fputc('\n', out);
    nk = node_children(u->nodes, i, kids, 64);
    if (nk > 64) {
        all = xmalloc(sizeof(uint32_t) * nk);
        node_children(u->nodes, i, all, nk);
    }
    for (k = 0; k < nk; k++)
        dump_node(out, u, sm, in, all[k], depth + 1);
    if (all != kids)
        free(all);
}

void ast_dump(FILE *out, const ParseUnit *u, SrcMgr *sm, const Interner *in)
{
    if (u->nnodes)
        dump_node(out, u, sm, in, u->nnodes - 1, 0);
}
