/* parse.h - C99 + GNU parser: tokens in, one syntax tree per external
 * declaration out (ast.h).  See docs/PARSER.md.
 *
 * Recursive descent with typedef feedback (scope.h), following the scope
 * rules of Jourdan & Pottier, "A simple, possibly correct LR parser for
 * C11": a declarator's name is in scope right after the declarator (before
 * its initializer); an enumeration constant after its enumerator; a
 * function definition's parameters again in its body; selection and
 * iteration statements and their bodies are blocks; in a parameter
 * declaration, `(T)` with T a typedef name is a function declarator.
 *
 * Errors are reported and recovered from locally (at ';' and '}').  An
 * unclosed '{' is caught at the next function definition that starts in
 * column 0: C has no function definitions in blocks (GNU nested functions
 * are indented in practice), so it ends the open function there instead
 * of swallowing the rest of the file. */
#ifndef CEREAL_PARSE_H
#define CEREAL_PARSE_H

#include "c/ast.h"
#include "c/scope.h"
#include "diag.h"
#include "intern.h"
#include "srcmgr.h"

/* The next token and its presentation location; false at the end. */
typedef bool (*ParseSource)(void *ctx, Tok *t, SrcLoc *exp_loc);

typedef struct Parser {
    ParseSource src;
    void *src_ctx;
    bool src_done;
    SrcMgr *sm;
    Interner *in;
    DiagEngine *diag;
    bool gnu;                   /* typeof and asm are keywords */
    /* toks[0] is the current unit's first token; lookahead follows */
    VEC(PTok) toks;
    uint32_t pos;
    uint64_t base;              /* stream index of toks[0] */
    uint32_t unit_end;          /* tokens of the unit last returned */
    VEC(Node) nodes;
    Scope scope;
    SymSaveVec saved;           /* parameters of function declarators */
    VEC(uint32_t) open_braces;  /* '{' of the compound statements open */
    int fn_depth;
    bool unwind;                /* ending an unclosed function body */
    uint32_t unwind_to;
    bool have_err;
    uint32_t last_err;          /* token of the last error (no cascades) */
    uint64_t units, errors;
} Parser;

void parser_init(Parser *p, SrcMgr *sm, Interner *in, DiagEngine *diag,
                 bool gnu, ParseSource src, void *ctx);
/* The next external declaration; false at the end.  The unit is valid
 * until the next call. */
bool parser_next(Parser *p, ParseUnit *u);
void parser_free(Parser *p);

/* Print a unit's tree, indented, one node a line. */
void ast_dump(FILE *out, const ParseUnit *u, SrcMgr *sm, const Interner *in);

#endif
