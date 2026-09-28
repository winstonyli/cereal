/* srcmgr.c - source files and the global location space. */
#include "srcmgr.h"

#include <string.h>

void srcmgr_init(SrcMgr *sm, Arena *a)
{
    memset(sm, 0, sizeof *sm);
    sm->arena = a;
    sm->next_base = 1;
}

void srcmgr_free(SrcMgr *sm)
{
    size_t i;
    for (i = 0; i < sm->files.len; i++)
        free(sm->files.data[i]->lines);
    vec_free(&sm->files);
}

static void compute_lines(SrcFile *f)
{
    VEC(uint32_t) v = {0};
    uint32_t i;
    vec_push(&v, 0);
    for (i = 0; i < f->size; i++) {
        if (f->buf[i] == '\n')
            vec_push(&v, i + 1);
        else if (f->buf[i] == '\r') {
            if (i + 1 < f->size && f->buf[i + 1] == '\n')
                i++;
            vec_push(&v, i + 1);
        }
    }
    f->lines = v.data;
    f->nlines = (uint32_t)v.len;
}

static SrcFile *add_buffer(SrcMgr *sm, const char *path, char *buf,
                           size_t len, SrcFileKind kind)
{
    SrcFile *f = NEW(sm->arena, SrcFile);
    if ((uint64_t)sm->next_base + len + 2 > UINT32_MAX)
        fatal("source location space exhausted");
    f->id = (int)sm->files.len;
    f->path = path;
    f->name = path;
    f->buf = buf;
    f->size = (uint32_t)len;
    f->base = sm->next_base;
    f->kind = kind;
    f->system_header = kind == SF_SYSTEM;
    /* +1 so the EOF position is addressable, +1 gap between files */
    sm->next_base += (SrcLoc)len + 2;
    compute_lines(f);
    vec_push(&sm->files, f);
    return f;
}

SrcFile *srcmgr_load(SrcMgr *sm, const char *path, SrcFileKind kind)
{
    size_t i, len;
    char *buf, *norm = path_normalize(sm->arena, path);
    for (i = 0; i < sm->files.len; i++)
        if (sm->files.data[i]->kind != SF_VIRTUAL &&
            strcmp(sm->files.data[i]->path, norm) == 0)
            return sm->files.data[i];
    buf = read_file(norm, &len);
    if (!buf)
        return NULL;
    {
        /* keep contents in the arena so the whole TU frees in bulk */
        char *copy = arena_strndup(sm->arena, buf, len);
        free(buf);
        return add_buffer(sm, norm, copy, len, kind);
    }
}

SrcFile *srcmgr_add_virtual(SrcMgr *sm, const char *name, const char *buf,
                            size_t len)
{
    return add_buffer(sm, arena_strdup(sm->arena, name),
                      arena_strndup(sm->arena, buf, len), len, SF_VIRTUAL);
}

SrcFile *srcmgr_file_of(const SrcMgr *sm, SrcLoc loc)
{
    size_t lo = 0, hi = sm->files.len;
    if (!loc)
        return NULL;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        SrcFile *f = sm->files.data[mid];
        if (loc < f->base)
            hi = mid;
        else if (loc > f->base + f->size)
            lo = mid + 1;
        else
            return f;
    }
    return NULL;
}

uint32_t srcmgr_offset(const SrcFile *f, SrcLoc loc)
{
    return loc - f->base;
}

void srcmgr_linecol(const SrcFile *f, SrcLoc loc, uint32_t *line, uint32_t *col)
{
    uint32_t off = loc - f->base, lo = 0, hi = f->nlines;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (f->lines[mid] <= off)
            lo = mid;
        else
            hi = mid;
    }
    *line = lo + 1;
    *col = off - f->lines[lo] + 1;
}

SrcLoc srcmgr_loc_of(const SrcFile *f, uint32_t line, uint32_t col)
{
    uint32_t off;
    if (line == 0 || line > f->nlines)
        return 0;
    off = f->lines[line - 1] + (col ? col - 1 : 0);
    if (off > f->size)
        off = f->size;
    return f->base + off;
}

const char *srcmgr_line_text(const SrcFile *f, uint32_t line, uint32_t *len)
{
    uint32_t b, e;
    if (line == 0 || line > f->nlines) {
        *len = 0;
        return "";
    }
    b = f->lines[line - 1];
    e = b;
    while (e < f->size && f->buf[e] != '\n' && f->buf[e] != '\r')
        e++;
    *len = e - b;
    return f->buf + b;
}

char *path_dirname(Arena *a, const char *path)
{
    const char *s = strrchr(path, '/');
    if (!s)
        return arena_strdup(a, ".");
    if (s == path)
        return arena_strdup(a, "/");
    return arena_strndup(a, path, (size_t)(s - path));
}

char *path_join(Arena *a, const char *dir, const char *file)
{
    if (file[0] == '/' || !dir || !*dir || strcmp(dir, ".") == 0)
        return arena_strdup(a, file);
    return arena_printf(a, "%s/%s", dir, file);
}

/* Lexically normalize: collapse "//", "/./", and "dir/.." pairs. */
char *path_normalize(Arena *a, const char *path)
{
    size_t n = strlen(path), nseg = 0, i;
    const char **seg = xmalloc(sizeof(char *) * (n / 2 + 2));
    size_t *seglen = xmalloc(sizeof(size_t) * (n / 2 + 2));
    bool abs = path[0] == '/';
    const char *p = path;
    StrBuf sb = {0};
    char *r;
    while (*p) {
        const char *s;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        s = p;
        while (*p && *p != '/')
            p++;
        if (p - s == 1 && s[0] == '.')
            continue;
        if (p - s == 2 && s[0] == '.' && s[1] == '.' && nseg > 0 &&
            !(seglen[nseg - 1] == 2 && seg[nseg - 1][0] == '.' &&
              seg[nseg - 1][1] == '.')) {
            nseg--;
            continue;
        }
        if (p - s == 2 && s[0] == '.' && s[1] == '.' && abs && nseg == 0)
            continue;
        seg[nseg] = s;
        seglen[nseg++] = (size_t)(p - s);
    }
    if (abs)
        sb_putc(&sb, '/');
    for (i = 0; i < nseg; i++) {
        if (i)
            sb_putc(&sb, '/');
        sb_putn(&sb, seg[i], seglen[i]);
    }
    if (sb.len == 0)
        sb_putc(&sb, '.');
    r = arena_strndup(a, sb.data, sb.len);
    sb_free(&sb);
    free(seg);
    free(seglen);
    return r;
}
