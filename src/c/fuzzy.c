/* fuzzy.c - spelling suggestions (fuzzy.h). */
#include "c/fuzzy.h"

#include <string.h>

/* Damerau-Levenshtein distance with gcc's costs: 2 per edit, 1 for a
 * change of case only. */
unsigned sc_dist(const char *s, size_t n, const char *t, size_t m,
                 unsigned limit)
{
    enum { INF = 0x3fffffff };
    unsigned rows[3][SC_MAXLEN + 2];
    unsigned *pp = rows[0], *pr = rows[1], *cur = rows[2];
    unsigned prevmin = 0;
    /* a cell (i, j) costs at least 2 |i - j|: outside the band of half
     * width limit / 2 nothing can be within the limit */
    size_t w = limit == SC_NONE ? m + n : limit / 2, i, j;
    if (n > SC_MAXLEN || m > SC_MAXLEN)
        return SC_NONE;
    if (n > m ? n - m > w : m - n > w)
        return SC_NONE;
    for (j = 0; j <= m; j++)
        pr[j] = (unsigned)j * 2;
    pr[m + 1] = INF;
    for (i = 1; i <= n; i++) {
        unsigned *tmp, rowmin = INF;
        size_t lo = i > w ? i - w : 1, hi = i + w < m ? i + w : m;
        if (lo > 1)
            cur[lo - 1] = INF;
        else {
            cur[0] = (unsigned)i * 2;
            rowmin = cur[0];
        }
        for (j = lo; j <= hi; j++) {
            unsigned sub, del = pr[j] + 2, ins = cur[j - 1] + 2, best;
            char a = s[i - 1], b = t[j - 1];
            if (a == b)
                sub = pr[j - 1];
            else if (a >= 0 && b >= 0 && (a | 0x20) == (b | 0x20) &&
                     ((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z')))
                sub = pr[j - 1] + 1;
            else
                sub = pr[j - 1] + 2;
            best = sub < del ? sub : del;
            if (ins < best)
                best = ins;
            if (i > 1 && j > 1 && s[i - 1] == t[j - 2] && s[i - 2] == t[j - 1] &&
                pp[j - 2] + 2 < best)
                best = pp[j - 2] + 2;
            cur[j] = best;
            if (best < rowmin)
                rowmin = best;
        }
        if (hi < m)
            cur[hi + 1] = INF;
        /* every later cell derives from this and the previous row */
        if (rowmin > limit && prevmin > limit)
            return SC_NONE;
        prevmin = rowmin;
        tmp = pp;
        pp = pr;
        pr = cur;
        cur = tmp;
    }
    return pr[m] >= INF ? SC_NONE : pr[m];
}

unsigned sc_cutoff(size_t goal, size_t cand)
{
    size_t mx = goal > cand ? goal : cand, mn = goal > cand ? cand : goal;
    if (mx <= 1)
        return 0;
    if (mx - mn <= 1)
        return 2 * (unsigned)(mx / 3 > 1 ? mx / 3 : 1);
    return 2 * (unsigned)((mx + 2) / 3);
}

/* Suggestions stop being computed for a translation unit once this much
 * work has been spent on them (gcc has no such limit and is quadratic in
 * the number of names and errors). */


void best_init(Best *b, const char *goal, uint64_t *work)
{
    b->work = work;
    b->goal = goal;
    b->goal_len = strlen(goal);
    b->str = NULL;
    b->len = 0;
    b->dist = SC_NONE;
}

void best_consider_n(Best *b, const char *s, size_t n)
{
    unsigned mind = (unsigned)(n > b->goal_len ? n - b->goal_len
                                               : b->goal_len - n) * 2, d;
    *b->work += 1;
    if (*b->work > SC_BUDGET)
        return;
    if (mind >= b->dist || mind > sc_cutoff(b->goal_len, n))
        return;
    *b->work += n + b->goal_len;
    d = sc_dist(b->goal, b->goal_len, s, n, b->dist == SC_NONE ? SC_NONE
                                                               : b->dist - 1);
    if (d < b->dist) {
        b->dist = d;
        b->str = s;
        b->len = n;
    }
}

void best_consider(Best *b, const char *s)
{
    best_consider_n(b, s, strlen(s));
}

const char *best_get(Best *b)
{
    if (!b->str || b->dist == 0 || b->dist > sc_cutoff(b->goal_len, b->len))
        return NULL;
    return b->str;
}

/* Names reserved to the implementation: not suggested unless the goal is
 * one too. */
bool reserved_name(const char *s)
{
    return s[0] == '_' && (s[1] == '_' || (s[1] >= 'A' && s[1] <= 'Z'));
}
