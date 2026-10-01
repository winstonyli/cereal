/* fuzzy.h - spelling suggestions (gcc's spellcheck.cc): Damerau-Levenshtein
 * with gcc's costs, best_match's cutoff, and a work budget (gcc has none and
 * is quadratic in the number of names and errors). */
#ifndef CEREAL_FUZZY_H
#define CEREAL_FUZZY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SC_MAXLEN 256
#define SC_NONE 0xFFFFFFFFu

/* Suggestions stop being computed for a translation unit once this much
 * work has been spent on them. */
#define SC_BUDGET UINT64_C(150000000)

typedef struct Best {
    uint64_t *work;
    const char *goal;
    size_t goal_len;
    const char *str;
    size_t len;
    unsigned dist;
} Best;

unsigned sc_dist(const char *s, size_t n, const char *t, size_t m,
                 unsigned limit);
unsigned sc_cutoff(size_t goal, size_t cand);
void best_init(Best *b, const char *goal, uint64_t *work);
void best_consider_n(Best *b, const char *s, size_t n);
void best_consider(Best *b, const char *s);
const char *best_get(Best *b);
/* Names reserved to the implementation: not suggested unless the goal is
 * one too. */
bool reserved_name(const char *s);

#endif
