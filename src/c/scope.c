/* scope.c - ordinary identifiers in scope (scope.h). */
#include "c/scope.h"

#include <string.h>

void scope_init(Scope *s)
{
    memset(s, 0, sizeof *s);
    scope_push(s); /* file scope */
}

void scope_free(Scope *s)
{
    vec_free(&s->log);
    vec_free(&s->marks);
    free(s->top);
    memset(s, 0, sizeof *s);
}

void scope_push(Scope *s)
{
    vec_push(&s->marks, (uint32_t)s->log.len);
}

size_t scope_depth(const Scope *s)
{
    return s->marks.len;
}

void scope_pop(Scope *s, SymSaveVec *save)
{
    uint32_t mark = vec_pop(&s->marks);
    size_t i;
    if (save)
        for (i = mark; i < s->log.len; i++) {
            SymSave v;
            v.ident = s->log.data[i].ident;
            v.kind = s->log.data[i].kind;
            vec_push(save, v);
        }
    while (s->log.len > mark) {
        SymEnt *e = &s->log.data[--s->log.len];
        s->top[e->ident] = e->prev;
    }
}

void scope_declare(Scope *s, uint32_t ident, SymKind kind)
{
    SymEnt e;
    uint32_t cur;
    if (ident >= s->ntop) {
        size_t n = s->ntop ? s->ntop : 1024;
        while (n <= ident)
            n *= 2;
        s->top = xrealloc(s->top, n * sizeof *s->top);
        memset(s->top + s->ntop, 0, (n - s->ntop) * sizeof *s->top);
        s->ntop = n;
    }
    cur = s->top[ident];
    if (cur && cur - 1 >= vec_last(&s->marks)) {
        s->log.data[cur - 1].kind = (uint8_t)kind; /* same scope */
        return;
    }
    e.ident = ident;
    e.prev = cur;
    e.kind = (uint8_t)kind;
    vec_push(&s->log, e);
    s->top[ident] = (uint32_t)s->log.len;
}
