/* lex.h - translation phases 1-3: pp-token lexer. */
#ifndef CEREAL_LEX_H
#define CEREAL_LEX_H

#include "common.h"
#include "diag.h"
#include "token.h"

typedef struct LexOptions {
    bool trigraphs;      /* C99 requires them; GCC disables by default */
    bool dollar_idents;  /* accept '$' in identifiers (GNU) */
} LexOptions;

typedef struct Lexer {
    Arena *arena;
    Interner *in;
    DiagEngine *diag;    /* may be NULL (silent) */
    LexOptions opt;
    const char *buf;
    uint32_t size, pos;
    SrcLoc base;         /* 0 = locations are not meaningful */
    StrBuf scratch;
} Lexer;

/* Tokenize a whole source file into a TK_EOF-terminated list. */
Token *lex_file(Arena *a, Interner *in, DiagEngine *d, LexOptions opt,
                SrcFile *f);
/* Tokenize an arbitrary buffer (e.g. a ## result or _Pragma string).
 * Token locations are all set to `loc`. */
Token *lex_buffer(Arena *a, Interner *in, LexOptions opt, const char *buf,
                  size_t len, SrcLoc loc);

/* Spelling of a token as a NUL-terminated string in the arena. */
char *tok_str(Arena *a, const Token *t);

#endif
