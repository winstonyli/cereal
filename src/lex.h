/* lex.h - translation phases 1-3: streaming pp-token lexer. */
#ifndef CEREAL_LEX_H
#define CEREAL_LEX_H

#include "common.h"
#include "diag.h"
#include "token.h"

typedef struct LexOptions {
    bool trigraphs;      /* C99 requires them; GCC disables by default */
    bool dollar_idents;  /* accept '$' in identifiers (GNU) */
    bool uliterals;      /* u'' U'' u"" U"" u8"" (gnu99, C11) */
    bool scope;          /* '::' is one token (GNU modes, C2X) */
    bool ucn_c99;        /* -pedantic: only C99 Annex D UCNs in identifiers */
    uint8_t bidi;        /* -Wbidi-chars=: BIDI_* */
    uint8_t norm;        /* -Wnormalized=: 0 nfkc, 1 nfc, 2 id, 3 none */
} LexOptions;

/* -Wbidi-chars= values: unpaired and any are exclusive; ucn extends either to
 * bidirectional controls written as UCNs. */
enum { BIDI_UNPAIRED = 1, BIDI_ANY = 2, BIDI_UCN = 4 };

typedef struct Lexer Lexer;
/* -Wnormalized for the identifier or pp-number [start, end) at loc. */
void lex_norm_check(const Lexer *L, DiagEngine *dg, SrcLoc loc, const char *start,
                    const char *end, bool raw);

typedef struct LineNote {
    SrcLoc loc;          /* where gcc reports it (cleaned-line column) */
    SrcLoc line_start;   /* its line */
    SrcLoc pos;          /* raw offset: reported once the lexer reaches it */
    char kind;           /* 'c' converted, 'i' ignored, 's' backslash + blank */
    char x;              /* the trigraph's third character */
} LineNote;

struct Lexer {
    const char *p;       /* cursor */
    const char *lim;     /* end of content (NUL + zero padding follow) */
    const char *region;  /* location 0 */
    SrcMgr *sm;
    Interner *in;
    DiagEngine *diag;    /* may be NULL (silent) */
    ScratchCursor *scratch; /* NULL: spliced spellings are not recorded */
    LexOptions opt;
    bool bol, space;
    const char *line_begin; /* start of the last line the cursor entered */
    bool unterminated;      /* met a block comment that runs to the end */
    const char *nul_line;   /* line of the last null-character warning */
    bool hi8;               /* the file has a byte >= 0x80 */
    bool bidi_live;         /* -Wbidi-chars can fire in this file */
    const char *bidi_hi;    /* the contexts before this were scanned */
    uint32_t bd_n;          /* open bidirectional contexts (libcpp bidi::vec) */
    uint8_t bd[32];         /* bit 0: closed by PDF (else PDI), bit 1: a UCN */
    StrBuf clean;        /* slow-path spelling buffer */
    struct LineNote *notes; /* trigraph / backslash-blank warnings, in file order */
    uint32_t nnotes, note_i;    /* ... and how many are already reported */
};

void lex_global_init(void);

void lexer_init(Lexer *L, SrcMgr *sm, Interner *in, DiagEngine *d,
                ScratchCursor *sc, LexOptions opt, SrcFile *f);
/* Lex an arbitrary byte range of the location space (scratch). */
void lexer_init_range(Lexer *L, SrcMgr *sm, Interner *in, ScratchCursor *sc,
                      LexOptions opt, SrcLoc begin, uint32_t len);
void lexer_set_diag(Lexer *L, DiagEngine *d);
void lexer_free(Lexer *L);

void lex_next(Lexer *L, Tok *t);
/* Reports the line notes of the lines up to the cursor (libcpp does it as
 * it cleans each line). */
void lexer_flush_notes(Lexer *L);

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
