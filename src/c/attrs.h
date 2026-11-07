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
       AC_E = 256, AC_ALLE = AC_ALL | AC_E,
       AC_FN = 512,                  /* a function declaration or definition (in no AC_ALL*) */
       AC_FPV = 1024,                /* row bit only: also where the type is a function or pointer to one */
       AC_FNL = 2048,                /* with AC_FN: a block-scope function declaration */
       AC_BIT = 4096 };              /* with AC_F: a bit-field */

enum { AF_KNOWN = 1, AF_STD = 2, AF_FN_ONLY = 4, AF_EXPR_ARGS = 8, AF_COPY_EXCL = 16,
       AF_FPKEPT = 32 };             /* stays on a pointer-to-function variable (the others are ignored) */

#define AT_ARGC_ANY UINT32_MAX

/* AT_* names must stay clear of the atomic-operation enum in cintrin.h (a clash fails to compile). */
#define ATTR(n, id, fl, cx, lo, hi) id,
typedef enum AttrId {
    AT_NONE = 0,                /* no attribute: the tombstone of a dropped entry (not a row) */
#include "c/attrs.def"
    AT_UNKNOWN                  /* a name with no row (not a row) */
} AttrId;
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
/* The row of an id (NULL for AT_NONE and AT_UNKNOWN), a row's name ("" for AT_NONE, "?" for AT_UNKNOWN: never
 * printed), and the id of a normalised name (AT_UNKNOWN when it has no row). */
const AttrDesc *attr_by_id(AttrId id);
const char *attr_name(AttrId id);
AttrId attr_id_of(const char *name);

/* Whether gcc 13 registers the normalised name (__has_attribute, __builtin_has_attribute). */
static inline bool attr_is_known(const char *name)
{
    const AttrDesc *d = attr_lookup(name);
    return d && (d->flags & AF_KNOWN);
}

#endif
