/* optrow.c - see optrow.h. */
#include "optrow.h"

#include <string.h>

static bool row_matches(const OptRow *r, const char *a)
{
    size_t n = strlen(r->name);
    switch (r->match) {
    case O_EXACT: case O_SEP: return !strcmp(a, r->name);
    case O_PFX1:              return !strncmp(a, r->name, n) && a[n];
    case O_WORD:              return !strncmp(a, r->name, n) && (!a[n] || a[n] == '=');
    default:                  return !strncmp(a, r->name, n);
    }
}

const OptRow *optrow_find(const OptRow *rows, size_t n, const char *a)
{
    size_t k;
    for (k = 0; k < n; k++)
        if (row_matches(&rows[k], a))
            return &rows[k];
    return NULL;
}

bool optrow_apply(void *const *bases, const OptRow *r, const char *v)
{
    unsigned char *p;
    if (r->act > OPT_STR)
        return false;
    p = (unsigned char *)bases[r->base] + r->off;
    if (r->act == OPT_SET)
        *p = (unsigned char)r->val;
    else
        memcpy(p, &v, sizeof v);
    return true;
}
