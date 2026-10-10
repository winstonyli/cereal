/* fixset.c - the edit builder (docs/C_DESIGN.md section 5). */
#include "fixset.h"

#include <string.h>

void fixset_init(FixSet *s)
{
    memset(s, 0, sizeof *s);
}

void fixset_free(FixSet *s)
{
    vec_free(&s->files);
    vec_free(&s->edits);
    vec_free(&s->acc);
    vec_free(&s->skipped_by);
}

uint32_t fixset_file(FixSet *s, const char *key)
{
    size_t k;
    for (k = 0; k < s->files.len; k++)
        if (!strcmp(s->files.data[k], key))
            return (uint32_t)k;
    vec_push(&s->files, key);
    return (uint32_t)(s->files.len - 1);
}

void fixset_group(FixSet *s)
{
    s->ngroups++;
}

void fixset_add(FixSet *s, uint32_t file, uint32_t b, uint32_t e,
                const char *text)
{
    FixEdit x;
    if (!s->ngroups)
        s->ngroups = 1;
    x.file = file;
    x.begin = b;
    x.end = e;
    x.text = text;
    x.group = s->ngroups;
    vec_push(&s->edits, x);
}

/* sort order of the accepted edits: file, begin, insertions first, end */
static int cmp_edit(const FixEdit *a, const FixEdit *b)
{
    int ai = a->begin == a->end, bi = b->begin == b->end;
    if (a->file != b->file)
        return a->file < b->file ? -1 : 1;
    if (a->begin != b->begin)
        return a->begin < b->begin ? -1 : 1;
    if (ai != bi)
        return ai ? -1 : 1;
    if (a->end != b->end)
        return a->end < b->end ? -1 : 1;
    return 0;
}

static bool overlap(const FixEdit *a, const FixEdit *b)
{
    bool ai = a->begin == a->end, bi = b->begin == b->end;
    if (a->file != b->file || (ai && bi))
        return false;
    if (ai)
        return b->begin < a->begin && a->begin < b->end;
    if (bi)
        return a->begin < b->begin && b->begin < a->end;
    return a->begin < b->end && b->begin < a->end;
}

static bool same_edit(const FixEdit *a, const FixEdit *b)
{
    return a->file == b->file && a->begin == b->begin && a->end == b->end &&
           !strcmp(a->text, b->text);
}

/* First index of acc whose key is greater than x's. */
static size_t upper(const FixSet *s, const FixEdit *x)
{
    size_t lo = 0, hi = s->acc.len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cmp_edit(&s->acc.data[mid], x) <= 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* The accepted edit x collides with, if any; *dup: x is already there. */
static const FixEdit *collide(const FixSet *s, const FixEdit *x, bool *dup)
{
    size_t pos = upper(s, x), i;
    *dup = false;
    for (i = pos; i-- > 0;) {           /* the equal keys just before pos */
        const FixEdit *a = &s->acc.data[i];
        if (cmp_edit(a, x) != 0)
            break;
        if (same_edit(a, x)) {
            *dup = true;
            return NULL;
        }
    }
    for (i = pos; i-- > 0;) {           /* the nearest replacement before */
        const FixEdit *a = &s->acc.data[i];
        if (a->file != x->file)
            break;
        if (a->begin == a->end)
            continue;
        if (overlap(a, x))
            return a;
        break;
    }
    for (i = pos; i < s->acc.len; i++) {
        const FixEdit *a = &s->acc.data[i];
        if (a->file != x->file || a->begin >= x->end)
            break;
        if (overlap(a, x))
            return a;
    }
    return NULL;
}

size_t fixset_resolve(FixSet *s)
{
    size_t i = 0, nskip = 0;
    uint32_t g;
    s->acc.len = 0;
    s->skipped_by.len = 0;
    for (g = 0; g <= s->ngroups; g++)
        vec_push(&s->skipped_by, 0);
    while (i < s->edits.len) {
        size_t j = i, k, m, nacc = 0;
        uint32_t by = 0;
        VEC(FixEdit) fresh = {0};
        g = s->edits.data[i].group;
        while (j < s->edits.len && s->edits.data[j].group == g)
            j++;
        for (k = i; k < j && !by; k++) {
            const FixEdit *x = &s->edits.data[k];
            const FixEdit *a;
            bool dup, self = false;
            for (m = 0; m < fresh.len; m++) {
                if (same_edit(&fresh.data[m], x)) {
                    self = true;                /* twice in one group */
                    break;
                }
                if (overlap(&fresh.data[m], x)) {
                    by = g;
                    break;
                }
            }
            if (by || self)
                continue;
            a = collide(s, x, &dup);
            if (a)
                by = a->group;
            else if (!dup)
                vec_push(&fresh, *x);
        }
        if (by) {
            s->skipped_by.data[g] = by;
            nskip++;
        } else {
            for (m = 0; m < fresh.len; m++) {   /* insert in order */
                size_t pos = upper(s, &fresh.data[m]);
                vec_push(&s->acc, fresh.data[m]);
                memmove(s->acc.data + pos + 1, s->acc.data + pos,
                        (s->acc.len - 1 - pos) * sizeof *s->acc.data);
                s->acc.data[pos] = fresh.data[m];
                nacc++;
            }
        }
        vec_free(&fresh);
        (void)nacc;
        i = j;
    }
    return nskip;
}

char *fixset_apply(const FixSet *s, uint32_t file, const char *src, size_t n,
                   size_t *outn)
{
    StrBuf sb = {0};
    size_t pos = 0, i;
    for (i = 0; i < s->acc.len; i++) {
        const FixEdit *e = &s->acc.data[i];
        if (e->file != file || e->begin < pos || e->end > n)
            continue;
        sb_putn(&sb, src + pos, e->begin - pos);
        sb_puts(&sb, e->text);
        pos = e->end;
    }
    sb_putn(&sb, src + pos, n - pos);
    *outn = sb.len;
    sb_putc(&sb, 0);
    sb.len--;
    return sb.data;
}

/* ---- unified diff, as gcc's edit_context prints it ---------------------- */

typedef struct EditedLine {
    uint32_t line;                  /* 1-based */
    bool edited;                    /* some edit changed the line's text */
    VEC(const char *) pred;         /* lines inserted before it (no newline) */
    VEC(uint32_t) predlen;
    StrBuf content;                 /* the edited text */
    uint32_t cur;                   /* offset in the original line copied so far */
} EditedLine;

static void put_line(FILE *o, char prefix, const char *s, size_t n)
{
    fputc(prefix, o);
    fwrite(s, 1, n, o);
    fputc('\n', o);
}

typedef struct DiffCtx {
    const char *src;
    const uint32_t *lines;
    uint32_t nl;
    size_t n;
    EditedLine *rec;
    size_t nrec;
} DiffCtx;

static void line_text(const DiffCtx *c, uint32_t line, const char **s,
                      size_t *len)
{
    size_t b = c->lines[line - 1];
    size_t e = line < c->nl ? c->lines[line] - 1 : c->n;
    if (line == c->nl && c->n && c->src[c->n - 1] == '\n')
        e = c->n - 1;
    *s = c->src + b;
    *len = e - b;
}

static EditedLine *rec_of(const DiffCtx *c, uint32_t line)
{
    size_t lo = 0, hi = c->nrec;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c->rec[mid].line < line)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < c->nrec && c->rec[lo].line == line ? &c->rec[lo] : NULL;
}

static int effective_count(const EditedLine *r)
{
    return r ? (int)r->pred.len + 1 : 1;
}

static int print_hunk(const DiffCtx *c, FILE *o, int os, int oe, int ns)
{
    int old_n = oe - os + 1, new_n = 0, line;
    for (line = os; line <= oe; line++)
        new_n += effective_count(rec_of(c, (uint32_t)line));
    fprintf(o, "@@ -%d,%d +%d,%d @@\n", os, old_n, ns, new_n);
    line = os;
    while (line <= oe) {
        if (rec_of(c, (uint32_t)line)) {
            int first = line, last, ln;
            while (rec_of(c, (uint32_t)line))
                line++;
            last = line - 1;
            for (ln = first; ln <= last; ln++) {
                const EditedLine *r = rec_of(c, (uint32_t)ln);
                const char *t;
                size_t tl;
                if (r->edited) {
                    line_text(c, (uint32_t)ln, &t, &tl);
                    put_line(o, '-', t, tl);
                }
            }
            for (ln = first; ln <= last; ln++) {
                const EditedLine *r = rec_of(c, (uint32_t)ln);
                size_t k;
                for (k = 0; k < r->pred.len; k++)
                    put_line(o, '+', r->pred.data[k], r->predlen.data[k]);
                if (r->edited) {
                    put_line(o, '+', r->content.data, r->content.len);
                } else {
                    const char *t;
                    size_t tl;
                    line_text(c, (uint32_t)ln, &t, &tl);
                    put_line(o, ' ', t, tl);
                }
            }
        } else {
            const char *t;
            size_t tl;
            line_text(c, (uint32_t)line, &t, &tl);
            put_line(o, ' ', t, tl);
            line++;
        }
    }
    return new_n - old_n;
}

bool fixset_diff(const FixSet *s, uint32_t file, const char *name,
                 const char *src, size_t n, FILE *out)
{
    DiffCtx c = {0};
    VEC(uint32_t) starts = {0};
    VEC(EditedLine) recs = {0};
    size_t i, k;
    bool ok = true;
    int delta = 0;
    c.src = src;
    c.n = n;
    vec_push(&starts, 0);
    for (i = 0; i < n; i++)
        if (src[i] == '\n' && i + 1 < n)
            vec_push(&starts, (uint32_t)(i + 1));
    c.nl = n ? (uint32_t)starts.len : 0;
    c.lines = starts.data;
    for (i = 0; i < s->acc.len && ok; i++) {
        const FixEdit *e = &s->acc.data[i];
        size_t lo = 0, hi = c.nl, len;
        uint32_t line, off;
        EditedLine *r;
        const char *t;
        size_t tl;
        if (e->file != file)
            continue;
        if (e->begin >= n || e->end > n || e->end < e->begin) {
            ok = false;
            break;
        }
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (c.lines[mid] <= e->begin)
                lo = mid;
            else
                hi = mid;
        }
        line = (uint32_t)lo + 1;
        line_text(&c, line, &t, &tl);
        off = e->begin - c.lines[lo];
        if (off > tl || e->end - c.lines[lo] > tl) {    /* crosses the line */
            ok = false;
            break;
        }
        if (!recs.len || vec_last(&recs).line != line) {
            EditedLine z = {0};
            z.line = line;
            vec_push(&recs, z);
        }
        r = &vec_last(&recs);
        len = strlen(e->text);
        if (len > 1 && e->text[len - 1] == '\n') {
            vec_push(&r->pred, e->text);
            vec_push(&r->predlen, (uint32_t)(len - 1));
            continue;
        }
        r->edited = true;
        sb_putn(&r->content, t + r->cur, off - r->cur);
        sb_puts(&r->content, e->text);
        r->cur = e->end - c.lines[lo];
    }
    for (k = 0; k < recs.len; k++) {
        EditedLine *r = &recs.data[k];
        const char *t;
        size_t tl;
        if (ok && r->edited) {
            line_text(&c, r->line, &t, &tl);
            sb_putn(&r->content, t + r->cur, tl - r->cur);
        }
    }
    c.rec = recs.data;
    c.nrec = recs.len;
    if (ok && recs.len) {
        i = 0;
        fprintf(out, "--- %s\n+++ %s\n", name, name);
        while (i < recs.len) {
            int start = (int)recs.data[i].line - 3, end;
            if (start < 1)
                start = 1;
            while (i + 1 < recs.len) {
                int endp = (int)recs.data[i].line + 3;
                if (!recs.data[i].edited)
                    endp--;
                if (endp >= (int)recs.data[i + 1].line - 3)
                    i++;
                else
                    break;
            }
            end = (int)recs.data[i].line + 3;
            if (!recs.data[i].edited)
                end--;
            if (end > (int)c.nl)
                end = (int)c.nl;
            delta += print_hunk(&c, out, start, end, start + delta);
            i++;
        }
    }
    for (k = 0; k < recs.len; k++) {
        vec_free(&recs.data[k].pred);
        vec_free(&recs.data[k].predlen);
        sb_free(&recs.data[k].content);
    }
    vec_free(&recs);
    vec_free(&starts);
    return ok;
}
