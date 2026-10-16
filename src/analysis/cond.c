/* cond.c - conditional-compilation analysis.
 *
 * Configuration space: every #if/#elif/#ifdef/#ifndef/#else in a user file
 * (active or not) is turned into a presence condition, a BDD over
 *   - defined(X)
 *   - threshold atoms "the value of X is >= c" for every constant c that X
 *     is compared with, with the C99 rule that an undefined X is 0
 *   - opaque atoms for calls such as __GNUC_PREREQ(4, 1) and for arithmetic
 *     or two-macro comparisons, keyed by canonical text
 * Each branch is checked by satisfiability: dead if it can never be
 * selected, redundant if it is always selected once reached.
 *
 * Dynamic checks use preprocessor events: tested-but-never-defined macros
 * with a similar defined name (typos). */
#include "analysis.h"

#include "pcexpr.h"

#include <string.h>

#define COND_BDD_CAP ((size_t)1 << 20)

/* ---- per-file walk -------------------------------------------------- */

typedef struct Frame {
    int opener;             /* skeleton index of the #if */
    Bdd outer;              /* the enclosing branches are taken */
    Bdd negprior;           /* no earlier branch of this chain was taken */
    Bdd reach;              /* outer and negprior: this position is reached */
    Bdd cur;                /* the current branch's condition holds */
    bool has_cur;           /* false for #else and for an unmodeled condition */
    bool cur_novars;        /* the condition mentions no macro (#if 0) */
    bool dead;              /* current branch already known dead */
    bool unknown;           /* a condition could not be modeled */
} Frame;

struct CondState {
    VEC(SrcFile *) done;
    /* dynamic */
    VEC(Tok) undefined_refs;
};

void cond_discard(Analysis *a)
{
    vec_free(&a->cond->done);
    vec_free(&a->cond->undefined_refs);
}

/* Evaluate the branch condition c into fr (c is NULL when it did not parse). */
static void set_cond(PcCtx *cx, Frame *fr, PcNode *c)
{
    fr->has_cur = c != NULL;
    fr->unknown |= c == NULL;
    fr->cur_novars = c && !pc_has_vars(c);
    if (c)
        fr->cur = pc_truth(cx, c);
}

static void set_reach(PcCtx *cx, Frame *fr)
{
    fr->reach = bdd_and(&cx->m, fr->outer, fr->negprior);
}

static bool path_dead(Frame *stack, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        if (stack[i].dead)
            return true;
    return false;
}

static bool path_unknown(Frame *stack, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        if (stack[i].unknown)
            return true;
    return false;
}

static const char *dir_name(SkKind k)
{
    switch (k) {
    case SK_IF: return "#if";
    case SK_IFDEF: return "#ifdef";
    case SK_IFNDEF: return "#ifndef";
    case SK_ELIF: return "#elif";
    case SK_ELIFDEF: return "#elifdef";
    case SK_ELIFNDEF: return "#elifndef";
    case SK_ELSE: return "#else";
    default: return "#endif";
    }
}

static void check_branch(Analysis *a, Skeleton *sk, PcCtx *cx, Frame *stack,
                         int depth, SkDirective *d)
{
    BddMgr *m = &cx->m;
    Frame *fr = &stack[depth - 1];
    int reach, sat_c, sat_notc;

    if (m->over || path_unknown(stack, depth) || path_dead(stack, depth - 1))
        return;
    if (fr->has_cur && fr->cur_novars) {
        /* constant condition: #if 0 / #if 1 */
        if (fr->cur != BDD_TRUE && fr->cur != BDD_FALSE)
            return;             /* 1 / 0 by zero, say: nothing to claim */
        if (d->kind != SK_IFNDEF && d->kind != SK_IFDEF &&
            d->kind != SK_ELIFNDEF && d->kind != SK_ELIFDEF)
            diag_report(a->diag, DL_REMARK, "cond-constant", d->kw_loc,
                        "%s condition is constant (%s)", dir_name(d->kind),
                        fr->cur == BDD_TRUE ? "always true" : "always false");
        fr->dead = fr->cur == BDD_FALSE;
        return;
    }
    /* reachability of this branch position */
    reach = pc_sat(cx, fr->reach);
    if (reach == 0) {
        Diagnostic *dg = diag_report(a->diag, DL_WARNING, "cond-dead-branch",
            d->kw_loc, "%s branch can never be taken: the enclosing and "
            "earlier conditions already cover every configuration",
            dir_name(d->kind));
        if (d->opener >= 0)
            diag_note(a->diag, dg, sk->dirs.data[d->opener].kw_loc,
                      "conditional starts here");
        fr->dead = true;
        return;
    }
    if (!fr->has_cur || reach < 0)
        return;
    sat_c = pc_sat(cx, bdd_and(m, fr->reach, fr->cur));
    if (sat_c == 0) {
        int self = pc_sat(cx, fr->cur);
        diag_report(a->diag, DL_WARNING, "cond-dead-branch", d->kw_loc,
                    self == 0 ? "%s condition can never be true"
                              : "%s condition contradicts the enclosing or "
                                "earlier conditions; this branch is dead in "
                                "every configuration",
                    dir_name(d->kind));
        fr->dead = true;
        return;
    }
    sat_notc = pc_sat(cx, bdd_and(m, fr->reach, bdd_not(m, fr->cur)));
    if (sat_notc == 0 && d->kind != SK_IFNDEF && d->kind != SK_ELIFNDEF) {
        diag_report(a->diag, DL_WARNING, "cond-redundant", d->kw_loc,
                    "%s condition is always true here (implied by the "
                    "enclosing and earlier conditions)%s", dir_name(d->kind),
                    d->kind == SK_ELIF ? "; use #else" : "");
    }
}

static bool macro_like_word(const char *s, size_t n)
{
    static const char *const skip[] = {"if", "ifdef", "ifndef", "endif", "else",
                                       "elif", "defined", "not", "end", "of",
                                       "the", "and", "or", "NOT", "TODO",
                                       "XXX", "FIXME", NULL};
    size_t i;
    bool upper = false;
    int k;
    if (n < 2)
        return false;
    for (k = 0; skip[k]; k++)
        if (strlen(skip[k]) == n && !memcmp(skip[k], s, n))
            return false;
    for (i = 0; i < n; i++)
        if ((s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_')
            upper = true;
    return upper;
}

static bool chain_mentions(const Interner *in, Skeleton *sk, int opener,
                           int upto, const char *w,
                           size_t n)
{
    int i;
    for (i = opener; i <= upto && i < (int)sk->dirs.len; i++) {
        SkDirective *d = &sk->dirs.data[i];
        uint32_t k;
        if (i != opener && d->opener != opener)
            continue;
        if (d->kind == SK_ENDIF)
            continue;
        for (k = 0; k < d->toks.n; k++) {
            const Tok *t = &d->toks.t[k];
            if (t->kind == TK_IDENT && t->len == n &&
                !memcmp(tok_ident(in, t)->str, w, n))
                return true;
        }
    }
    return false;
}

static void check_label(Analysis *a, Skeleton *sk, int idx)
{
    SkDirective *d = &sk->dirs.data[idx];
    const char *s = d->comment, *first_word = NULL;
    size_t first_len = 0;
    bool any = false;
    if (!s || d->opener < 0)
        return;
    while (*s) {
        const char *b;
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) {
            s++;
            continue;
        }
        b = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_')
            s++;
        if (!macro_like_word(b, (size_t)(s - b)))
            continue;
        if (chain_mentions(a->in, sk, d->opener, idx, b, (size_t)(s - b)))
            return; /* matches */
        if (!any) {
            first_word = b;
            first_len = (size_t)(s - b);
        }
        any = true;
    }
    if (any) {
        Diagnostic *dg = diag_report(a->diag, DL_WARNING, "endif-label",
            d->kw_loc, "comment on %s mentions '%.*s', which the conditional "
            "does not test", dir_name(d->kind), (int)first_len, first_word);
        diag_note(a->diag, dg, sk->dirs.data[d->opener].kw_loc,
                  "the conditional being closed starts here");
    }
}

static void analyze_file(Analysis *a, SrcFile *f)
{
    Skeleton *sk = an_skeleton(a, f);
    PcCtx cx = {0};
    VEC(Frame) st = {0};
    size_t i;
    if (!sk->dirs.len)
        return;
    pc_init(&cx, a, COND_BDD_CAP);
    /* past the BDD cap (cx.m.over) the rest of the file is left unchecked */
    for (i = 0; i < sk->dirs.len && !cx.m.over; i++) {
        SkDirective *d = &sk->dirs.data[i];
        switch (d->kind) {
        case SK_IF: case SK_IFDEF: case SK_IFNDEF: {
            Frame fr;
            memset(&fr, 0, sizeof fr);
            fr.opener = (int)i;
            fr.outer = BDD_TRUE;
            if (st.len) {
                const Frame *up = &vec_last(&st);
                fr.outer = up->has_cur ? bdd_and(&cx.m, up->reach, up->cur)
                                       : up->reach;
            }
            fr.negprior = BDD_TRUE;
            set_reach(&cx, &fr);
            set_cond(&cx, &fr, pc_branch_cond(&cx, d));
            vec_push(&st, fr);
            check_branch(a, sk, &cx, st.data, (int)st.len, d);
            break;
        }
        case SK_ELIF: case SK_ELIFDEF: case SK_ELIFNDEF: case SK_ELSE: {
            Frame *fr;
            if (!st.len)
                break;
            fr = &vec_last(&st);
            if (fr->has_cur)
                fr->negprior = bdd_and(&cx.m, fr->negprior,
                                       bdd_not(&cx.m, fr->cur));
            else
                fr->unknown = true;
            fr->dead = false;
            set_reach(&cx, fr);
            if (d->kind == SK_ELSE) {
                fr->has_cur = false;
                fr->cur_novars = false;
                check_label(a, sk, (int)i);
            } else {
                set_cond(&cx, fr, pc_branch_cond(&cx, d));
            }
            check_branch(a, sk, &cx, st.data, (int)st.len, d);
            break;
        }
        case SK_ENDIF:
            check_label(a, sk, (int)i);
            if (st.len)
                st.len--;
            break;
        default:
            break;
        }
    }
    vec_free(&st);
    pc_free(&cx);
}

/* FOO_A vs FOO_B, V1 vs V2: distinct names by design, not typos. */
static bool looks_like_sibling(const Ident *x, const Ident *y)
{
    uint32_t i, diffs = 0, digit_only = 1;
    const char *seg_x = x->str, *seg_y = y->str;
    if (x->len != y->len)
        return false;
    for (i = 0; i < x->len; i++) {
        if (x->str[i] != y->str[i]) {
            diffs++;
            if (!(x->str[i] >= '0' && x->str[i] <= '9') ||
                !(y->str[i] >= '0' && y->str[i] <= '9'))
                digit_only = 0;
        }
    }
    if (diffs == 0)
        return false;
    if (digit_only)
        return true;
    if (diffs == 1) {
        /* the differing char is in a short final segment */
        const char *px = strrchr(seg_x, '_'), *py = strrchr(seg_y, '_');
        size_t lx = px ? strlen(px + 1) : x->len;
        size_t ly = py ? strlen(py + 1) : y->len;
        for (i = 0; x->str[i] == y->str[i]; i++)
            ;
        if (px && py && (size_t)(px - seg_x) < i && lx <= 2 && ly <= 2)
            return true;
    }
    return false;
}

/* ---- listeners ------------------------------------------------------ */

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Analysis *a = ctx;
    size_t i;
    (void)via;
    if (!an_user_file(f))
        return;
    for (i = 0; i < a->cond->done.len; i++)
        if (a->cond->done.data[i] == f)
            return;
    vec_push(&a->cond->done, f);
    analyze_file(a, f);
}

static void on_macro_ref(void *ctx, Ident *id, Macro *m, const Tok *tok,
                         RefKind kind)
{
    Analysis *a = ctx;
    (void)id;
    if (m || (kind != REF_IFDEF && kind != REF_DEFINED && kind != REF_IF_VALUE))
        return;
    if (!an_user_loc(a, tok->loc))
        return;
    vec_push(&a->cond->undefined_refs, *tok);
}

void cond_attach(Analysis *a)
{
    PPListener l;
    memset(&l, 0, sizeof l);
    a->cond = NEW(a->arena, CondState);
    l.ctx = a;
    l.file_enter = on_file_enter;
    l.macro_ref = on_macro_ref;
    pp_add_listener(a->pp, l);
}

void cond_finish(Analysis *a)
{
    size_t i, j;
    VEC(Ident *) seen = {0};
    for (i = 0; i < a->cond->undefined_refs.len; i++) {
        Tok *t = &a->cond->undefined_refs.data[i];
        Ident *id = tok_ident(a->in, t), *best = NULL;
        unsigned best_d = 3, limit;
        bool dup = false;
        if (mt_hist(a->pp->mt, id))
            continue; /* defined at some point in the TU */
        {
            /* a misspelled include guard is reported by -Wheader-guard */
            SrcFile *f = srcmgr_file_of(a->sm, t->loc);
            Skeleton *sk = f ? an_skeleton(a, f) : NULL;
            if (sk && sk->guard_ifndef == id && sk->guard_define &&
                sk->guard_define != id)
                continue;
        }
        for (j = 0; j < seen.len; j++)
            if (seen.data[j] == id)
                dup = true;
        if (dup)
            continue;
        vec_push(&seen, id);
        limit = id->len >= 8 ? 2 : 1;
        if (id->len >= 4) {
            /* candidates: every name defined in the TU; ties go to the
             * smallest spelling, so the pick does not depend on interning
             * order (the interner can outlive a build) */
            size_t k;
            for (k = 0; k < a->pp->macros.len; k++) {
                Ident *cand = a->pp->macros.data[k]->name;
                unsigned dist;
                if (cand == id || cand == best || looks_like_sibling(id, cand))
                    continue;
                dist = edit_distance(id->str, id->len, cand->str, cand->len,
                                     limit);
                if (dist <= limit && (dist < best_d ||
                                      (dist == best_d &&
                                       strcmp(cand->str, best->str) < 0))) {
                    best_d = dist;
                    best = cand;
                }
            }
        }
        if (best) {
            Diagnostic *d = diag_report(a->diag, DL_WARNING, "cond-typo", t->loc,
                "'%s' is never defined in this translation unit; did you mean "
                "'%s'?", id->str, best->str);
            diag_set_range(d, t->loc, t->loc + t->len);
            if (d && mt_hist(a->pp->mt, best)->file && mt_hist(a->pp->mt, best)->file->kind != SF_VIRTUAL)
                diag_note(a->diag, d, mt_hist(a->pp->mt, best)->name_loc,
                          "'%s' is defined here", best->str);
            if (d)
                fix_tok_replace(a->diag, d, 0, a->sm, a->in, t, best->str,
                                FIX_INFO);
        } else {
            diag_report(a->diag, DL_REMARK, "cond-never-defined", t->loc,
                        "'%s' is never defined in this translation unit",
                        id->str);
        }
    }
    vec_free(&seen);
    vec_free(&a->cond->done);
    vec_free(&a->cond->undefined_refs);
}
