/* optrow.h - one row per command-line option.
 *
 * A table of rows is tried in order and the first that matches wins.  A row
 * stores a constant into a one-byte field (OPT_SET) or the option's value
 * into a string field (OPT_STR) itself; any other `act` is the owning
 * file's to run.  `base` picks which of the owner's structs `off` is in.
 * `match` says how `name` is read against the argument:
 *   O_EXACT  the argument is `name`
 *   O_SEP    the argument is `name`; the value is the next argument
 *   O_ARG    `name` and then the value, joined (-Ifoo) or the next argument
 *   O_PFX    `name` and then the value, which may be empty (-O2, --eval-only=f)
 *   O_PFX1   the same, with a value that is not empty
 *   O_WORD   `name` alone or `name=...` (-Wbidi-chars=any) */
#ifndef CEREAL_OPTROW_H
#define CEREAL_OPTROW_H

#include "common.h"

enum { O_EXACT, O_SEP, O_ARG, O_PFX, O_PFX1, O_WORD };
enum { OPT_SET, OPT_STR, OPT_ACT0 };    /* the owner's actions start at OPT_ACT0 */

typedef struct OptRow {
    const char *name;
    unsigned char match, act, base;
    unsigned short off;
    int val;    /* OPT_SET: the byte; else the owner's */
} OptRow;

/* A row that sets the one-byte field `f` of `T` (bool, char, uint8_t) to `v`;
 * a field of any other size does not compile. */
#define OPT_SET1(n, b, T, f, v) \
    {n, O_EXACT, OPT_SET, b, \
     (unsigned short)(offsetof(T, f) + 0 * sizeof(char[sizeof(((T *)0)->f) == 1 ? 1 : -1])), v}
/* A row that stores the value in the string field `f` of `T`. */
#define OPT_STR1(n, m, b, T, f) \
    {n, m, OPT_STR, b, \
     (unsigned short)(offsetof(T, f) + 0 * sizeof(char[sizeof(((T *)0)->f) == sizeof(const char *) ? 1 : -1])), 0}

/* The first row of rows[0..n) that matches `a`, or NULL. */
const OptRow *optrow_find(const OptRow *rows, size_t n, const char *a);

/* Run OPT_SET and OPT_STR rows against bases[r->base] (`v` is the value of
 * the latter); false for any other row. */
bool optrow_apply(void *const *bases, const OptRow *r, const char *v);

#endif
