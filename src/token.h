/* token.h - preprocessing tokens (C99 6.4).
 *
 * A Tok is a 16-byte value with no pointers: it can be copied freely,
 * stored in pooled buffers, and retained by listeners.  Its spelling is
 * found without storing a pointer:
 *   - identifiers:          the interned Ident (aux = ident id)
 *   - TF_SPELL tokens:      region + aux (a scratch copy: splices removed,
 *                           ## / # / builtin results, pragmas)
 *   - everything else:      region + loc, len bytes */
#ifndef CEREAL_TOKEN_H
#define CEREAL_TOKEN_H

#include <string.h>

#include "common.h"
#include "intern.h"
#include "srcmgr.h"

typedef enum {
    TK_EOF,
    TK_IDENT,
    TK_PPNUM,
    TK_CHAR,     /* character-constant, incl. L'x' */
    TK_STRING,   /* string-literal, incl. L"x" */
    TK_PUNCT,
    TK_OTHER,    /* any other non-white-space character */
    TK_PRAGMA,   /* #pragma / _Pragma result; spelling "pragma ..." */
    TK_DIRMARK   /* phase B: a directive was here (aux = version after) */
} TokKind;

#define PUNCT_LIST(X)                                                      \
    X(LBRACKET, "[") X(RBRACKET, "]") X(LPAREN, "(") X(RPAREN, ")")         \
    X(LBRACE, "{") X(RBRACE, "}") X(DOT, ".") X(ARROW, "->")                \
    X(INC, "++") X(DEC, "--") X(AMP, "&") X(STAR, "*") X(PLUS, "+")          \
    X(MINUS, "-") X(TILDE, "~") X(BANG, "!") X(SLASH, "/") X(PERCENT, "%")   \
    X(SHL, "<<") X(SHR, ">>") X(LT, "<") X(GT, ">") X(LE, "<=") X(GE, ">=")  \
    X(EQEQ, "==") X(NE, "!=") X(CARET, "^") X(PIPE, "|") X(ANDAND, "&&")     \
    X(OROR, "||") X(QUESTION, "?") X(COLON, ":") X(SEMI, ";")               \
    X(ELLIPSIS, "...") X(ASSIGN, "=") X(MUL_ASSIGN, "*=")                   \
    X(DIV_ASSIGN, "/=") X(MOD_ASSIGN, "%=") X(ADD_ASSIGN, "+=")             \
    X(SUB_ASSIGN, "-=") X(SHL_ASSIGN, "<<=") X(SHR_ASSIGN, ">>=")           \
    X(AND_ASSIGN, "&=") X(XOR_ASSIGN, "^=") X(OR_ASSIGN, "|=")              \
    X(COMMA, ",") X(HASH, "#") X(HASHHASH, "##")

typedef enum {
    P_NONE,
#define X(name, s) P_##name,
    PUNCT_LIST(X)
#undef X
    P_COUNT
} Punct;

extern const char *const punct_spelling[P_COUNT];

enum {
    TF_BOL          = 1 << 0,  /* first token on a line */
    TF_SPACE        = 1 << 1,  /* preceded by white space */
    TF_NOEXPAND     = 1 << 2,  /* painted: never macro-expand */
    TF_UNTERMINATED = 1 << 3,  /* unterminated ' or " (diagnosed if active) */
    TF_DIGRAPH      = 1 << 4,
    TF_SPELL        = 1 << 5,  /* spelling at region + aux */
    TF_SPLICED      = 1 << 6,  /* raw text contains splices/trigraphs */
    TF_UCN          = 1 << 7,
    TF_DOLLAR       = 1 << 8,
    TF_FROM_DEFINED = 1 << 9,  /* 1/0 produced by `defined` in #if */
    TF_PARAM        = 1 << 10, /* macro body: parameter, index in punct */
    TF_ORIGIN_BODY  = 1 << 11, /* copied from a replacement list */
    TF_ORIGIN_ARG   = 1 << 12, /* substituted from an argument */
    TF_PASTED       = 1 << 13, /* result of ## */
    TF_SYNTH        = 1 << 14  /* result of #, builtin, _Pragma */
};

typedef struct Tok {
    uint8_t kind;
    uint8_t punct;       /* Punct; parameter index for TF_PARAM */
    uint16_t flags;
    SrcLoc loc;          /* spelling location */
    uint32_t len;        /* spelling length */
    uint32_t aux;        /* ident id | scratch loc (TF_SPELL) | 0 */
} Tok;

typedef struct TokSpan {
    const Tok *t;
    uint32_t n;
} TokSpan;

static inline bool tok_is_punct(const Tok *t, Punct p)
{
    return t->kind == TK_PUNCT && t->punct == p;
}

static inline const char *tok_text_raw(const SrcMgr *sm, const Interner *in,
                                       const Tok *t)
{
    if (t->kind == TK_IDENT)
        return ident_by_id(in, t->aux)->str;
    if (t->flags & TF_SPELL)
        return sm->region + t->aux;
    return sm->region + t->loc;
}

static inline Ident *tok_ident(const Interner *in, const Tok *t)
{
    return t->kind == TK_IDENT ? ident_by_id(in, t->aux) : NULL;
}

static inline bool tok_is_word(const Interner *in, const Tok *t, const char *s)
{
    const Ident *id;
    if (t->kind != TK_IDENT)
        return false;
    id = ident_by_id(in, t->aux);
    return strlen(s) == id->len && memcmp(id->str, s, id->len) == 0;
}

#endif
