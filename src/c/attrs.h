/* attrs.h - the attribute descriptor table (docs/ATTR_TABLE_DESIGN.md).  Neutral: the parser and
 * the checker both read it. */
#ifndef ATTRS_H
#define ATTRS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Declaration contexts for attribute checks (typedef, global, local, static local, parameter,
 * field), plus AC_E for an enumerator (AC_ALL does not include it: no object context sees one). */
enum { AC_T = 1, AC_G = 2, AC_L = 4, AC_S = 8, AC_P = 16, AC_F = 32, AC_ALL = 63,
       AC_TLS = 64, AC_PUB = 128,    /* the object has thread storage duration / is externally visible */
       AC_E = 256, AC_ALLE = AC_ALL | AC_E };

enum { AF_KNOWN = 1, AF_STD = 2, AF_FN_ONLY = 4, AF_EXPR_ARGS = 8, AF_COPY_EXCL = 16 };

#define AT_ARGC_ANY UINT32_MAX

/* AT_* names must stay clear of the atomic-operation enum in cintrin.h (a clash fails to compile). */
#define ATTR(n, id, fl, cx, lo, hi) id,
enum AttrId {
#include "c/attrs.def"
    AT_COUNT_
};
#undef ATTR

typedef struct AttrDesc {
    const char *name;
    uint16_t id, flags, ctx;
    uint32_t argc_lo, argc_hi;
} AttrDesc;

/* The row for a normalised name (no __ affixes), or NULL. */
const AttrDesc *attr_lookup(const char *name);
/* The row for a spelling s[0..n) that may be unterminated and wrapped in __ __ (attr_norm's rule). */
const AttrDesc *attr_lookup_n(const char *s, size_t n);

/* Whether gcc 13 registers the normalised name (__has_attribute, __builtin_has_attribute). */
static inline bool attr_is_known(const char *name)
{
    const AttrDesc *d = attr_lookup(name);
    return d && (d->flags & AF_KNOWN);
}

#endif
