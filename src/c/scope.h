/* scope.h - ordinary identifiers in scope, for the parser's typedef
 * feedback: is this identifier a typedef name here?
 *
 * One entry per declaration, chained per identifier (innermost first);
 * a scope is a mark in the entry log, and leaving it unwinds the log to
 * the mark.  A scope's entries can be saved when it is left and declared
 * again later (a function definition's parameters, reopened for its
 * body). */
#ifndef CEREAL_SCOPE_H
#define CEREAL_SCOPE_H

#include "common.h"

typedef enum {
    SYM_NONE, SYM_ORDINARY, SYM_TYPEDEF, SYM_ENUMERATOR,
    SYM_TAG_STRUCT, SYM_TAG_UNION, SYM_TAG_ENUM  /* in the parser's tag scope */
} SymKind;

typedef struct SymEnt {
    uint32_t ident;
    uint32_t prev;              /* previous innermost entry + 1 (0: none) */
    uint8_t kind;
} SymEnt;

typedef struct SymSave {
    uint32_t ident;
    uint8_t kind;
} SymSave;

typedef VEC(SymSave) SymSaveVec;

typedef struct Scope {
    VEC(SymEnt) log;
    uint32_t *top;              /* by ident id: innermost entry + 1 */
    size_t ntop;
    VEC(uint32_t) marks;        /* log length at each open scope */
} Scope;

void scope_init(Scope *s);
void scope_free(Scope *s);
void scope_push(Scope *s);
/* Leave the innermost scope; its entries are appended to save if given. */
void scope_pop(Scope *s, SymSaveVec *save);
void scope_declare(Scope *s, uint32_t ident, SymKind kind);
size_t scope_depth(const Scope *s);

static inline SymKind scope_lookup(const Scope *s, uint32_t ident)
{
    return ident < s->ntop && s->top[ident]
               ? (SymKind)s->log.data[s->top[ident] - 1].kind : SYM_NONE;
}

#endif
