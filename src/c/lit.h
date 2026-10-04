/* lit.h - C literals: the type and value of integer, floating and
 * character constants, and the element type and length of string
 * literals (C99 6.4.4, 6.4.5, gcc's extensions).
 *
 * Malformed constants are classified as libcpp does (cpp_classify_number)
 * and described in Lit.msg.  Values wider than 64 bits (__int128
 * constants) are not represented: LIT_WIDE is set and the value is 0. */
#ifndef CEREAL_LIT_H
#define CEREAL_LIT_H

#include "common.h"
#include "c/target.h"

enum {
    LIT_WIDE = 1,            /* the value does not fit 64 bits */
    LIT_UNSIGNED_WARN = 2,   /* "integer constant is so large that it is
                                unsigned" */
    LIT_TOO_LARGE = 4,       /* "integer constant is too large for its
                                type" */
    LIT_FLOAT = 8,           /* a floating constant */
    LIT_IMAGINARY = 16,      /* suffix i / j: _Complex */
    LIT_MULTICHAR = 32,      /* 'ab' */
    LIT_BAD = 64             /* not understood (already diagnosed) */
};

typedef struct Lit {
    TypeKind ty;             /* the (real) type */
    unsigned flags;
    uint64_t v;              /* integers, characters */
    long double f;           /* floating constants */
    /* a diagnostic (gcc's wording), if msg[0]: level 2 error, 1 warning
     * (under option id, "" if none), 0 pedantic only */
    int level;
    const char *id;
    char msg[96];
} Lit;

/* A pp-number (integer or floating constant). */
void lit_number(const Target *tgt, const char *s, size_t n, Lit *out);
/* A character constant, prefix included. */
uint32_t lit_named_ucn(const char *s, const char *e);
void lit_char(const Target *tgt, const char *s, size_t n, Lit *out);

/* String literal prefixes, from the spelling: 0 none, 'L', 'u', 'U', '8'
 * (u8). */
int lit_str_prefix(const char *s, size_t n);
/* Adds one piece's code units (without the terminator) to *units, as
 * the literal's element width (1, 2 or 4 bytes) counts them. */
void lit_str_units(const char *s, size_t n, unsigned width, uint64_t *units);

#endif
