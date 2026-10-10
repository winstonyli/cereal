/* fixmode.c - `cereal fix`, so far only the edit builder's testing input:
 *     cereal fix --edits=FILE --diff
 * FILE lists edit groups, one edit per line, groups separated by blank
 * lines (docs/C_DESIGN.md 5.3):
 *     PATH BEGIN END "TEXT"
 * BEGIN and END are byte offsets into PATH; TEXT takes \\ \" \n \t \r and
 * \ooo.  Lines starting with # are comments.  The result is the unified
 * diff of the accepted edits on stdout; a skipped group is named on stderr. */
#include "fixset.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct Src {
    const char *name;        /* as first written */
    char *buf;
    size_t len;
} Src;

/* TEXT between the quotes of s; false if malformed. */
static bool unquote(const char *s, StrBuf *out)
{
    if (*s++ != '"')
        return false;
    for (; *s && *s != '"'; s++) {
        if (*s != '\\') {
            sb_putc(out, *s);
            continue;
        }
        s++;
        if (*s == 'n')
            sb_putc(out, '\n');
        else if (*s == 't')
            sb_putc(out, '\t');
        else if (*s == 'r')
            sb_putc(out, '\r');
        else if (*s >= '0' && *s <= '7') {
            int v = 0, k;
            for (k = 0; k < 3 && s[k] >= '0' && s[k] <= '7'; k++)
                v = v * 8 + (s[k] - '0');
            sb_putc(out, (char)v);
            s += k - 1;
        } else if (*s == '\\' || *s == '"')
            sb_putc(out, *s);
        else
            return false;
    }
    return *s == '"' && !s[1];
}

static int cmp_src(const void *a, const void *b)
{
    return strcmp(((const Src *)a)->name, ((const Src *)b)->name);
}

int fix_main(int argc, char **argv)
{
    const char *edits = NULL;
    bool diff = false;
    FixSet fs;
    VEC(Src) srcs = {0};
    VEC(uint32_t) order = {0};
    Arena arena;
    char line[4096];
    size_t ln = 0, k;
    bool open = false;
    int i, rc = 0;
    FILE *ef;
    for (i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "--edits=", 8))
            edits = argv[i] + 8;
        else if (!strcmp(argv[i], "--diff"))
            diff = true;
        else {
            edits = NULL;
            diff = false;
            break;
        }
    }
    if (!edits || !diff) {
        fputs("usage: cereal fix --edits=FILE --diff\n"
              "(the rest of fix is not implemented yet)\n", stderr);
        return 2;
    }
    ef = fopen(edits, "r");
    if (!ef) {
        fprintf(stderr, "cereal: error: cannot open '%s'\n", edits);
        return 2;
    }
    arena_init(&arena);
    fixset_init(&fs);
    while (fgets(line, sizeof line, ef)) {
        char *p = line, *path, *end;
        unsigned long b, e;
        StrBuf t = {0};
        char resolved[PATH_MAX];
        const char *key;
        uint32_t fi;
        ln++;
        p[strcspn(p, "\r\n")] = 0;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#')
            continue;
        if (!*p) {
            open = false;
            continue;
        }
        path = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (!*p)
            goto bad;
        *p++ = 0;
        b = strtoul(p, &end, 10);
        if (end == p)
            goto bad;
        p = end;
        e = strtoul(p, &end, 10);
        if (end == p)
            goto bad;
        p = end;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!unquote(p, &t))
            goto bad;
        if (b > e) {
            sb_free(&t);
            goto bad;
        }
        key = realpath(path, resolved) ? arena_strdup(&arena, resolved)
                                       : arena_strdup(&arena, path);
        fi = fixset_file(&fs, key);
        if (fi == srcs.len) {
            Src s;
            s.name = arena_strdup(&arena, path);
            s.buf = read_file(path, &s.len);
            if (!s.buf) {
                fprintf(stderr, "cereal: error: cannot read '%s'\n", path);
                sb_free(&t);
                rc = 2;
                goto done;
            }
            vec_push(&srcs, s);
        }
        if (!open) {
            fixset_group(&fs);
            open = true;
        }
        fixset_add(&fs, fi, (uint32_t)b, (uint32_t)e,
                   arena_strndup(&arena, t.data ? t.data : "", t.len));
        sb_free(&t);
        continue;
    bad:
        fprintf(stderr, "cereal: error: %s:%zu: expected PATH BEGIN END \"TEXT\"\n",
                edits, ln);
        rc = 2;
        goto done;
    }
    {
        size_t nskip = fixset_resolve(&fs);
        uint32_t g;
        if (nskip)
            for (g = 1; g <= fs.ngroups; g++)
                if (fs.skipped_by.data[g])
                    fprintf(stderr, "cereal: fix: group %u skipped: overlaps "
                            "group %u\n", g, fs.skipped_by.data[g]);
    }
    /* files by name, as the patch of gcc's edit context */
    for (k = 0; k < srcs.len; k++)
        vec_push(&order, (uint32_t)k);
    for (k = 0; k < order.len; k++) {
        size_t m;
        for (m = k + 1; m < order.len; m++)
            if (cmp_src(&srcs.data[order.data[m]], &srcs.data[order.data[k]]) < 0) {
                uint32_t t2 = order.data[k];
                order.data[k] = order.data[m];
                order.data[m] = t2;
            }
    }
    for (k = 0; k < order.len; k++) {
        Src *s = &srcs.data[order.data[k]];
        if (!fixset_diff(&fs, order.data[k], s->name, s->buf, s->len, stdout)) {
            fprintf(stderr, "cereal: error: %s: an edit crosses a line or lies "
                    "outside the file\n", s->name);
            rc = 1;
        }
    }
done:
    fclose(ef);
    for (k = 0; k < srcs.len; k++)
        free(srcs.data[k].buf);
    vec_free(&srcs);
    vec_free(&order);
    fixset_free(&fs);
    arena_free(&arena);
    return rc;
}
