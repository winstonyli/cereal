/* token.h - preprocessing tokens (C99 6.4) and provenance. */
#ifndef CEREAL_TOKEN_H
#define CEREAL_TOKEN_H

#include <string.h>

#include "common.h"
#include "srcmgr.h"

typedef enum {
    TK_EOF,
    TK_IDENT,
    TK_PPNUM,
    TK_CHAR,     /* character-constant, incl. L'x' */
    TK_STRING,   /* string-literal, incl. L"x" */
    TK_PUNCT,
    TK_OTHER,    /* any other non-white-space character */
    TK_PLACEMARKER,
    TK_PRAGMA    /* a #pragma / _Pragma, text = "pragma ..." */
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
    TF_BOL         = 1 << 0,  /* first token on a line */
    TF_SPACE       = 1 << 1,  /* preceded by white space */
    TF_NOEXPAND    = 1 << 2,  /* "painted blue": never expand again */
    TF_UNTERMINATED= 1 << 3,  /* unterminated ' or " (diagnosed if active) */
    TF_DIGRAPH     = 1 << 4,
    TF_SPLICED     = 1 << 5,  /* spelling contained line splices/trigraphs */
    TF_STRAY_DOLLAR= 1 << 6,
    TF_FROM_DEFINED= 1 << 7,  /* result of `defined` in #if */
    TF_UCN         = 1 << 8
};

struct Macro;
struct Expansion;

/* Hide set: a small sorted linked list of macros (Prosser). */
typedef struct Hideset {
    struct Hideset *next;
    struct Macro *macro;
} Hideset;

typedef enum {
    PROV_BODY,      /* copied from a replacement list */
    PROV_ARG,       /* substituted for a parameter */
    PROV_PASTE,     /* result of ## */
    PROV_STRINGIZE, /* result of # */
    PROV_BUILTIN    /* __LINE__, __FILE__, defined-result, ... */
} ProvKind;

typedef struct Prov {
    ProvKind kind;
    int param;               /* PROV_ARG: parameter index */
    SrcLoc loc;              /* body token / parameter occurrence loc */
    struct Expansion *exp;
    struct Prov *inner;      /* PROV_ARG: provenance at the call site */
} Prov;

typedef struct Token {
    uint8_t kind;
    uint8_t punct;
    uint16_t flags;
    uint32_t len;            /* spelling length */
    uint32_t rawlen;         /* length in the source buffer */
    const char *text;        /* spelling (NOT NUL terminated) */
    SrcLoc loc;              /* spelling location */
    struct Ident *ident;     /* TK_IDENT */
    Hideset *hs;
    Prov *prov;              /* NULL = straight from a source file */
    struct Token *next;
} Token;

static inline bool tok_is_punct(const Token *t, Punct p)
{
    return t->kind == TK_PUNCT && t->punct == p;
}

static inline bool tok_is_ident(const Token *t, const char *s)
{
    return t->kind == TK_IDENT && strlen(s) == t->len &&
           memcmp(t->text, s, t->len) == 0;
}

static inline SrcRange tok_range(const Token *t)
{
    SrcRange r;
    r.begin = t->loc;
    r.end = t->loc + t->rawlen;
    return r;
}

#endif
