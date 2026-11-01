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
    X(COMMA, ",") X(HASH, "#") X(HASHHASH, "##")                            X(COLONCOLON, "::")

typedef enum {
    P_NONE,
#define X(name, s) P_##name,
    PUNCT_LIST(X)
#undef X
    P_COUNT
} Punct;

extern const char *const punct_spelling[P_COUNT];

/* C's expression precedence, lowest first.  A binary operator's level is
 * punct_info[op].prec; PR_COMMA (0) means "not a binary operator" to the
 * Pratt parsers. */
enum {
    PR_COMMA, PR_ASSIGN, PR_COND, PR_LOR, PR_LAND, PR_OR, PR_XOR, PR_AND,
    PR_EQ, PR_REL, PR_SHIFT, PR_ADD, PR_MUL, PR_UNARY, PR_POSTFIX,
    PR_PRIMARY
};

enum { PF_CMP = 1, PF_ASSIGN = 2, PF_LOGICAL = 4 };

typedef struct {
    uint8_t prec;    /* binary precedence (PR_*); 0: not a binary operator */
    uint8_t flags;   /* PF_* */
    uint8_t swap;    /* a OP b == b swap(OP) a; 0: the operator itself */
    uint8_t negate;  /* !(a OP b) == a negate(OP) b; 0: none (ordered operands only) */
    uint8_t base;    /* the binary operator of a compound assignment; 0: the operator itself */
} PunctInfo;

extern const PunctInfo punct_info[P_COUNT];

static inline const PunctInfo *punct_ent(int op)
{
    return &punct_info[(unsigned)op < P_COUNT ? op : P_NONE];
}
static inline int punct_prec(int op) { return punct_ent(op)->prec; }
static inline bool punct_is_cmp(int op) { return punct_ent(op)->flags & PF_CMP; }
static inline bool punct_is_assign(int op) { return punct_ent(op)->flags & PF_ASSIGN; }
static inline bool punct_is_logical(int op) { return punct_ent(op)->flags & PF_LOGICAL; }
/* the value is 0 or 1: a comparison, && or || */
static inline bool punct_is_bool(int op)
{
    return punct_ent(op)->flags & (PF_CMP | PF_LOGICAL);
}
static inline int punct_swap(int op)
{
    return punct_ent(op)->swap ? punct_ent(op)->swap : op;
}
static inline int punct_negate(int op) { return punct_ent(op)->negate; }
static inline int punct_assign_base(int op)
{
    return punct_ent(op)->base ? punct_ent(op)->base : op;
}

/* `a OP b` for a comparison OP, from cmp = rel_cmp_*(a, b): -1, 0, 1, or
 * REL_UNORDERED (a NaN operand: only != holds). */
enum { REL_UNORDERED = 2 };
static inline bool rel_holds(int op, int cmp)
{
    switch (op) {
    case P_EQEQ: return cmp == 0;
    case P_NE: return cmp != 0;
    case P_LT: return cmp == -1;
    case P_GT: return cmp == 1;
    case P_LE: return cmp == -1 || cmp == 0;
    default: return cmp == 0 || cmp == 1;        /* P_GE */
    }
}
static inline int rel_cmp_s(int64_t a, int64_t b) { return (a > b) - (a < b); }
static inline int rel_cmp_u(uint64_t a, uint64_t b) { return (a > b) - (a < b); }
static inline int rel_cmp_f(long double a, long double b)
{
    return a < b ? -1 : a > b ? 1 : a == b ? 0 : REL_UNORDERED;
}

int punct_selftest(void);   /* `cereal selftest punct`: the table against first principles */

enum {
    TF_BOL          = 1 << 0,  /* first token on a line */
    TF_SPACE        = 1 << 1,  /* preceded by white space */
    TF_NOEXPAND     = 1 << 2,  /* painted: never macro-expand */
    TF_UNTERMINATED = 1 << 3,  /* unterminated ' or " (diagnosed if active) */
    TF_DIGRAPH      = 1 << 4,
    TF_SPELL        = 1 << 5,  /* spelling at region + aux */
    TF_SPLICED      = 1 << 6,  /* raw text contains splices/trigraphs */
    TF_UCN          = 1 << 7,
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

/* the binary precedence of a token, 0 when it is not a binary operator */
static inline int tok_prec(const Tok *t)
{
    return t->kind == TK_PUNCT ? punct_prec(t->punct) : 0;
}

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
