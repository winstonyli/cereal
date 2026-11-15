/* parse_int.h - what parse.c shares with the parser's other translation
 * units (parse_spec.c): the node and error helpers, and the spec reader's
 * entry points. */
#ifndef CEREAL_C_PARSE_INT_H
#define CEREAL_C_PARSE_INT_H

#include "c/parse.h"

uint32_t p_ci(Parser *p);                   /* index of the current token */
bool p_at_eof(Parser *p);
uint32_t p_nmark(Parser *p);                /* where the next node's subtree starts */
void p_emit(Parser *p, NodeTag tag, uint32_t tok, uint32_t start, unsigned flags);
void p_leaf(Parser *p, NodeTag tag, uint32_t tok);
void p_set_aux(Parser *p, uint32_t n);
const char *p_tok_desc(Parser *p, uint32_t i, char *buf, size_t n);
Diagnostic *p_err(Parser *p, uint32_t i, const char *fmt, ...);
Diagnostic *p_err_at(Parser *p, uint32_t i, SrcLoc loc, const char *fmt, ...);
void parse_expr(Parser *p);

/* parse_spec.c: what a body's leading specs may be (Parser.spec_ctx). */
enum { SCTX_NONE, SCTX_FUNC, SCTX_LOOP };
int spec_kind(Parser *p, const Tok *t, const char **kw);
void spec_pragma(Parser *p, uint32_t pt, int k, const char *kw, int ctx);
void spec_loop_prelude(Parser *p, uint32_t j, bool braced);

#endif
