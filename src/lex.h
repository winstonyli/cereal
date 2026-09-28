/* lex.h - translation phases 1-3: streaming pp-token lexer. */
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
    const char *p;       /* cursor */
    const char *lim;     /* end of content (NUL + zero padding follow) */
    const char *region;  /* location 0 */
    SrcMgr *sm;
    Interner *in;
    DiagEngine *diag;    /* may be NULL (silent) */
    LexOptions opt;
    bool bol, space;
    bool warned_nul;
    StrBuf clean;        /* slow-path spelling buffer */
} Lexer;

void lex_global_init(void);

void lexer_init(Lexer *L, SrcMgr *sm, Interner *in, DiagEngine *d,
                LexOptions opt, SrcFile *f);
/* Lex an arbitrary byte range of the location space (scratch). */
void lexer_init_range(Lexer *L, SrcMgr *sm, Interner *in, LexOptions opt,
                      SrcLoc begin, uint32_t len);
void lexer_free(Lexer *L);

void lex_next(Lexer *L, Tok *t);

static inline SrcLoc lexer_loc(const Lexer *L)
{
    return (SrcLoc)(L->p - L->region);
}
void lexer_seek(Lexer *L, SrcLoc loc, bool bol);

/* Skip scanner (inactive groups).  At a line start: skip blanks and
 * comments; true if the line is a directive (cursor left at the '#'). */
bool lex_line_is_directive(Lexer *L);
/* Advance to the start of the next line; false at end of input. */
bool lex_next_line(Lexer *L);

/* Raw extent of a token in the source (differs from len if spliced). */
uint32_t tok_raw_len(const SrcMgr *sm, const Interner *in, const Tok *t);

#endif
