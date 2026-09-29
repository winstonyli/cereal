/* toks.c - regenerating a cell build's tokens (toks.h). */
#include "toks.h"

#include <string.h>

void tokregen_init(TokRegen *s)
{
    memset(s, 0, sizeof *s);
    mutex_init(&s->m);
}

void tokregen_free(TokRegen *s)
{
    plan_free(&s->plan);
    free(s->cells);
    vec_free(&s->spare);
    mutex_destroy(&s->m);
    memset(s, 0, sizeof *s);
}

uint64_t tokregen_hash(const TokRegen *s, uint64_t *ntoks)
{
    uint64_t h = 0, n = 0;
    size_t i;
    for (i = 0; i < s->ncells; i++) {
        h = m61_concat(h, s->cells[i].hash, s->cells[i].ntoks);
        n += s->cells[i].ntoks;
    }
    if (ntoks)
        *ntoks = n;
    return h;
}

void tokcur_open(TokCursor *c, TokRegen *src, size_t s, size_t e)
{
    TU *tu = src->tu;
    const Plan *plan = &src->plan;
    memset(c, 0, sizeof *c);
    c->src = src;
    c->end = e;
    arena_init(&c->arena);
    diag_init(&c->diag, &c->arena, &tu->sm);
    c->diag.cfg = tu->diag.cfg;
    pp_init_worker(&c->pp, &tu->pp, &c->arena, &c->diag);
    c->pp.track = TRACK_NONE; /* no expansion records, no counts */
    c->pp.cancel = NULL;
    c->pp.plan_stop = e < plan->items.len ? e : 0;
    mutex_lock(&src->m);
    if (src->spare.len)
        c->pp.scratch = vec_pop(&src->spare);
    mutex_unlock(&src->m);
    c->mark = srcmgr_scratch_mark(&c->pp.scratch);
    pp_plan_start(&c->pp, &src->plan, s);
}

bool tokcur_next(TokCursor *c, Tok *t, SrcLoc *exp_loc)
{
    while (pp_next(&c->pp, t)) {
        if (!tok_in_stream(t))
            continue;
        if (exp_loc)
            *exp_loc = t->kind == TK_PRAGMA ? t->loc : c->pp.out_exp_loc;
        return true;
    }
    if (c->pp.plan_stop && !c->pp.plan_stop_clean)
        c->unclean = true;
    return false;
}

void tokcur_close(TokCursor *c)
{
    ScratchCursor sc = c->pp.scratch;
    srcmgr_scratch_rewind(&sc, c->mark);
    c->pp.scratch.chunk = NULL;
    pp_free(&c->pp);
    diag_free(&c->diag);
    arena_free(&c->arena);
    if (sc.chunk) {
        mutex_lock(&c->src->m);
        vec_push(&c->src->spare, sc);
        mutex_unlock(&c->src->m);
    }
}
