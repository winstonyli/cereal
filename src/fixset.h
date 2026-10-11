/* fixset.h - the edit builder (docs/C_DESIGN.md section 5).
 *
 * Edits arrive in groups (one diagnostic's fix-its).  fixset_resolve picks
 * the groups that go in together: it sorts, collapses exact duplicates,
 * keeps several insertions at one point in group order, and skips a whole
 * later group when one of its edits overlaps an accepted one.  Touching
 * edits do not overlap.  Strings and file keys are borrowed. */
#ifndef CEREAL_FIXSET_H
#define CEREAL_FIXSET_H

#include "common.h"

typedef struct FixEdit {
    uint32_t file;           /* index from fixset_file */
    uint32_t begin, end;     /* byte offsets, [begin, end) */
    const char *text;
    uint32_t group;
} FixEdit;

typedef struct FixSet {
    VEC(const char *) files;
    VEC(FixEdit) edits;      /* as added */
    uint32_t ngroups;
    VEC(FixEdit) acc;        /* resolved: accepted, in application order */
    VEC(uint32_t) skipped_by; /* per group: 0 = applied, else the group (1-based)
                                 whose edit it overlaps, or its own number */
} FixSet;

void fixset_init(FixSet *s);
void fixset_free(FixSet *s);
uint32_t fixset_file(FixSet *s, const char *key);
void fixset_group(FixSet *s);      /* starts the next group */
void fixset_add(FixSet *s, uint32_t file, uint32_t b, uint32_t e,
                const char *text);
/* Number of groups skipped.  Groups with no edits count as applied. */
size_t fixset_resolve(FixSet *s);
/* The file's text with the accepted edits applied (malloc'd). */
char *fixset_apply(const FixSet *s, uint32_t file, const char *src, size_t n,
                   size_t *outn);
/* A unified diff of the accepted edits in gcc's shape (edit-context.cc).
 * False, printing nothing, if an edit crosses a line or lies outside src. */
bool fixset_diff(const FixSet *s, uint32_t file, const char *name,
                 const char *src, size_t n, FILE *out);


#endif
