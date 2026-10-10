/* srcmgr.c - source files and the global location space. */
#define _DEFAULT_SOURCE 1
#include "srcmgr.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define REGION_SIZE (((size_t)1 << 32) + ((size_t)1 << 20))
#define MMAP_THRESHOLD (64u * 1024u)
#define SCRATCH_CHUNK (1u << 20)

static size_t ps;
static pthread_once_t ps_once = PTHREAD_ONCE_INIT;

static void init_page_size(void)
{
    long v = sysconf(_SC_PAGESIZE);
    ps = v > 0 ? (size_t)v : 4096;
}

static size_t page_size(void)
{
    pthread_once(&ps_once, init_page_size);
    return ps;
}

static size_t round_up(size_t x, size_t a)
{
    return (x + a - 1) & ~(a - 1);
}

void srcmgr_init(SrcMgr *sm, Arena *a)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    memset(sm, 0, sizeof *sm);
    sm->arena = a;
    sm->region = mmap(NULL, REGION_SIZE, PROT_NONE, flags, -1, 0);
    if (sm->region == MAP_FAILED)
        fatal("cannot reserve source location space");
    sm->region_size = REGION_SIZE;
    sm->next_base = (SrcLoc)page_size(); /* location 0 stays invalid */
    mutex_init(&sm->m);
    sm->path_cap = 256;
    sm->path_slots = xcalloc(sm->path_cap, sizeof(SrcFile *));
}

void srcmgr_free(SrcMgr *sm)
{
    uint32_t i;
    for (i = 0; i < sm->nfiles; i++)
        free(srcmgr_file(sm, i)->lines), free(srcmgr_file(sm, i)->sysmarks),
        free(srcmgr_file(sm, i)->linemap);
    for (i = 0; i < FILE_CHUNKS; i++)
        free(sm->fchunks[i]);
    free(sm->path_slots);
    mutex_destroy(&sm->m);
    if (sm->region && sm->region != MAP_FAILED)
        munmap(sm->region, sm->region_size);
    sm->region = NULL;
}

#define COMMIT_CHUNK ((size_t)4 << 20)

/* Reserve [base, base+span) in the location space as readable memory.
 * Memory is committed in large chunks to keep mprotect calls rare. */
static SrcLoc reserve(SrcMgr *sm, size_t span)
{
    size_t base = sm->next_base;
    span = round_up(span, page_size());
    if (base + span + page_size() > ((size_t)1 << 32))
        fatal("source location space exhausted (4 GiB)");
    if (base + span > sm->committed) {
        size_t upto = round_up(base + span, COMMIT_CHUNK);
        if (upto > ((size_t)1 << 32))
            upto = (size_t)1 << 32;
        if (mprotect(sm->region + sm->committed, upto - sm->committed,
                     PROT_READ | PROT_WRITE) != 0)
            fatal("cannot commit source memory");
        sm->committed = upto;
    }
    sm->next_base = (SrcLoc)(base + span);
    return (SrcLoc)base;
}

/* Caller holds sm->m. */
static SrcFile *new_file(SrcMgr *sm, const char *path, const char *name,
                         SrcLoc base, uint32_t size, uint32_t span,
                         SrcFileKind kind)
{
    SrcFile *f = NEW(sm->arena, SrcFile);
    uint32_t n = sm->nfiles;
    if (n / FILE_CHUNK >= FILE_CHUNKS)
        fatal("too many source files");
    if (!sm->fchunks[n / FILE_CHUNK])
        sm->fchunks[n / FILE_CHUNK] = xcalloc(FILE_CHUNK, sizeof(SrcFile *));
    f->id = (int)n;
    f->path = path;
    f->name = name;
    f->base = base;
    f->buf = sm->region + base;
    f->size = size;
    f->span = span;
    f->kind = kind;
    f->system_header = kind == SF_SYSTEM;
    sm->fchunks[n / FILE_CHUNK][n % FILE_CHUNK] = f;
    atomic_store_u32(&sm->nfiles, n + 1); /* publish */
    return f;
}

static size_t path_hash(const char *p)
{
    return hash_bytes(p, strlen(p));
}

static void path_insert(SrcMgr *sm, SrcFile *f)
{
    size_t k;
    if ((sm->path_count + 1) * 2 > sm->path_cap) {
        size_t nc = sm->path_cap * 2, i;
        SrcFile **ns = xcalloc(nc, sizeof(SrcFile *));
        for (i = 0; i < sm->path_cap; i++) {
            SrcFile *g = sm->path_slots[i];
            if (!g)
                continue;
            for (k = path_hash(g->path) & (nc - 1); ns[k]; k = (k + 1) & (nc - 1))
                ;
            ns[k] = g;
        }
        free(sm->path_slots);
        sm->path_slots = ns;
        sm->path_cap = nc;
    }
    for (k = path_hash(f->path) & (sm->path_cap - 1); sm->path_slots[k];
         k = (k + 1) & (sm->path_cap - 1))
        ;
    sm->path_slots[k] = f;
    sm->path_count++;
}

static SrcFile *path_find(SrcMgr *sm, const char *path)
{
    size_t k;
    SrcFile *f;
    for (k = path_hash(path) & (sm->path_cap - 1); (f = sm->path_slots[k]) != NULL;
         k = (k + 1) & (sm->path_cap - 1))
        if (!strcmp(f->path, path))
            return f;
    return NULL;
}

/* Paths known not to exist share this sentinel in the path map. */
static SrcFile *missing_file(SrcMgr *sm, const char *norm)
{
    SrcFile *f = NEW(sm->arena, SrcFile);
    f->path = norm;
    f->id = -1;
    path_insert(sm, f);
    return NULL;
}

static SrcFile *load_locked(SrcMgr *sm, const char *path, SrcFileKind kind);

SrcFile *srcmgr_load(SrcMgr *sm, const char *path, SrcFileKind kind)
{
    SrcFile *f;
    mutex_lock(&sm->m);
    f = load_locked(sm, path, kind);
    mutex_unlock(&sm->m);
    return f;
}

static SrcFile *load_locked(SrcMgr *sm, const char *path, SrcFileKind kind)
{
    char *name = path_normalize(sm->arena, path);
    /* the key: absolute when the TU has a working directory of its own */
    char *norm = sm->cwd && name[0] != '/'
                     ? path_normalize(sm->arena, arena_printf(sm->arena, "%s/%s",
                                                              sm->cwd, name))
                     : name;
    SrcFile *f = path_find(sm, norm);
    struct stat st;
    int fd;
    size_t size;
    SrcLoc base;
    if (f)
        return f->id < 0 ? NULL : f;
    if (sm->overlay) {
        const char *ob;
        size_t ol;
        if (sm->overlay(sm->overlay_ctx, norm, &ob, &ol) && ol <= 0xF0000000u) {
            base = reserve(sm, ol + SRC_PAD);
            memcpy(sm->region + base, ob, ol);
            f = new_file(sm, norm, name, base, (uint32_t)ol,
                         (uint32_t)round_up(ol + SRC_PAD, page_size()), kind);
            path_insert(sm, f);
            return f;
        }
    }
    fd = open(norm, O_RDONLY);
    if (fd < 0)
        return missing_file(sm, norm);
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint64_t)st.st_size > 0xF0000000u) {
        close(fd);
        return missing_file(sm, norm);
    }
    size = (size_t)st.st_size;
    base = reserve(sm, size + SRC_PAD);
    if (size >= MMAP_THRESHOLD) {
        void *p = mmap(sm->region + base, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_FIXED, fd, 0);
        if (p == MAP_FAILED) {
            close(fd);
            return NULL;
        }
    } else {
        size_t got = 0;
        while (got < size) {
            ssize_t r = read(fd, sm->region + base + got, size - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        size = got;
        /* libcpp drops a UTF-8 byte order mark */
        if (size >= 3 && !memcmp(sm->region + base, "\357\273\277", 3)) {
            memmove(sm->region + base, sm->region + base + 3, size - 3);
            memset(sm->region + base + size - 3, 0, 3);
            size -= 3;
        }
    }
    close(fd);
    f = new_file(sm, norm, name, base, (uint32_t)size,
                 (uint32_t)round_up(size + SRC_PAD, page_size()), kind);
    path_insert(sm, f);
    return f;
}

SrcFile *srcmgr_add_virtual(SrcMgr *sm, const char *name, const char *buf,
                            size_t len)
{
    SrcLoc base;
    SrcFile *f;
    const char *copy;
    mutex_lock(&sm->m);
    base = reserve(sm, len + SRC_PAD);
    memcpy(sm->region + base, buf, len);
    copy = arena_strdup(sm->arena, name);
    f = new_file(sm, copy, copy, base, (uint32_t)len,
                 (uint32_t)round_up(len + SRC_PAD, page_size()), SF_VIRTUAL);
    mutex_unlock(&sm->m);
    return f;
}

SrcLoc srcmgr_scratch(SrcMgr *sm, ScratchCursor *c, const char *s, size_t n)
{
    SrcFile *f = c->chunk;
    SrcLoc loc;
    if (!f || (size_t)f->size + n + 1 + SRC_PAD > f->span) {
        size_t span = MAX((size_t)SCRATCH_CHUNK, round_up(n + 1 + SRC_PAD,
                                                          page_size()));
        SrcLoc base;
        mutex_lock(&sm->m);
        base = reserve(sm, span);
        f = new_file(sm, "<scratch>", "<scratch>", base, 0, (uint32_t)span, SF_SCRATCH);
        mutex_unlock(&sm->m);
        c->chunk = f;
    }
    loc = f->base + f->size;
    memcpy(sm->region + loc, s, n);
    sm->region[loc + n] = 0;
    f->size += (uint32_t)n + 1;
    return loc;
}

ScratchMark srcmgr_scratch_mark(const ScratchCursor *c)
{
    ScratchMark m;
    m.chunk = c->chunk;
    m.size = c->chunk ? c->chunk->size : 0;
    return m;
}

void srcmgr_scratch_rewind(ScratchCursor *c, ScratchMark m)
{
    if (!c->chunk)
        return;
    /* a chunk started since the mark is reused from its beginning; the
     * marked one keeps what it had */
    c->chunk->size = c->chunk == m.chunk ? m.size : 0;
}

SrcFile *srcmgr_file_of(const SrcMgr *sm, SrcLoc loc)
{
    uint32_t lo = 0, hi = srcmgr_nfiles(sm);
    if (!loc)
        return NULL;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        SrcFile *f = srcmgr_file(sm, mid);
        if (loc < f->base)
            hi = mid;
        else if (loc >= f->base + f->span)
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

static Mutex lines_lock = PTHREAD_MUTEX_INITIALIZER;

static void compute_lines(SrcFile *f)
{
    VEC(uint32_t) v = {0};
    uint32_t i;
    if (atomic_load_ptr((void *const *)&f->lines))
        return;
    mutex_lock(&lines_lock);
    if (f->lines) {
        mutex_unlock(&lines_lock);
        return;
    }
    vec_push(&v, 0);
    i = 0;
    if (!memchr(f->buf, 0x0d, f->size)) { /* the common case: memchr's speed */
        const char *p = f->buf, *end = f->buf + f->size;
        while ((p = memchr(p, '\n', (size_t)(end - p)))) {
            p++;
            vec_push(&v, (uint32_t)(p - f->buf));
        }
        i = f->size;
    }
    for (; i < f->size; i++) {
        char c = f->buf[i];
        if (c == '\n') {
            vec_push(&v, i + 1);
        } else if (c == '\r') {
            if (i + 1 < f->size && f->buf[i + 1] == '\n')
                i++;
            vec_push(&v, i + 1);
        }
    }
    f->nlines = (uint32_t)v.len;
    atomic_store_ptr((void **)&f->lines, v.data);
    mutex_unlock(&lines_lock);
}

void srcmgr_mark_system(SrcFile *f, uint32_t line, bool sys)
{
    uint32_t *v = realloc(f->sysmarks, (f->nsysmarks + 2) * sizeof *v);
    if (!v)
        return;
    f->sysmarks = v;
    v[f->nsysmarks++] = line;
    v[f->nsysmarks++] = sys;
}

void srcmgr_add_linemap(SrcFile *f, uint32_t from, int32_t delta,
                        const char *name)
{
    LineMapEnt *v = realloc(f->linemap, (f->nlinemap + 1) * sizeof *v);
    if (!v)
        return;
    f->linemap = v;
    v[f->nlinemap].from = from;
    v[f->nlinemap].delta = delta;
    v[f->nlinemap].name = name;
    f->nlinemap++;
}

uint32_t srcmgr_presumed(const SrcFile *f, uint32_t phys, const char **name)
{
    uint32_t k;
    *name = f->name;
    for (k = f->nlinemap; k--;)
        if (phys >= f->linemap[k].from) {
            *name = f->linemap[k].name;
            return (uint32_t)((int32_t)phys + f->linemap[k].delta);
        }
    return phys;
}

bool srcmgr_is_system(SrcFile *f, SrcLoc loc)
{
    uint32_t line, col, k;
    bool sys;
    if (!f)
        return false;
    sys = f->system_header;
    if (!f->nsysmarks)
        return sys;
    srcmgr_linecol(f, loc, &line, &col);
    for (k = 0; k < f->nsysmarks; k += 2)
        if (f->sysmarks[k] <= line)
            sys = f->sysmarks[k + 1] != 0;
    return sys;
}

void srcmgr_linecol(SrcFile *f, SrcLoc loc, uint32_t *line, uint32_t *col)
{
    uint32_t off = loc - f->base, lo = 0, hi;
    compute_lines(f);
    hi = f->nlines;
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

SrcLoc srcmgr_loc_of(SrcFile *f, uint32_t line, uint32_t col)
{
    uint32_t off;
    compute_lines(f);
    if (line == 0 || line > f->nlines)
        return 0;
    off = f->lines[line - 1] + (col ? col - 1 : 0);
    if (off > f->size)
        off = f->size;
    return f->base + off;
}

const char *srcmgr_line_text(SrcFile *f, uint32_t line, uint32_t *len)
{
    uint32_t b, e;
    compute_lines(f);
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

static bool file_has_cr(SrcFile *f)
{
    uint32_t st = atomic_load_u32(&f->cr_state);
    if (!st) {
        st = memchr(f->buf, '\r', f->size) ? 2 : 1;
        atomic_store_u32(&f->cr_state, st);
    }
    return st == 2;
}

uint32_t linecursor_slow(LineCursor *c, SrcFile *f, SrcLoc loc)
{
    uint32_t off = loc - f->base;
    const char *nl;
    unsigned k;
    if (file_has_cr(f)) {
        uint32_t col;
        srcmgr_linecol(f, loc, &c->line, &col);
        c->file = f;
        c->off = off;
        c->next_nl = off; /* no caching for CR files */
        return c->line;
    }
    if (c->file != f) {
        /* save the current file's position, resume f's if we have it */
        if (c->file) {
            for (k = 0; k < LINECURSOR_WAYS && c->saved[k].file != c->file; k++)
                ;
            if (k == LINECURSOR_WAYS)
                k = c->next_way++ % LINECURSOR_WAYS;
            c->saved[k].file = c->file;
            c->saved[k].off = c->off;
            c->saved[k].line = c->line;
        }
        c->file = f;
        c->off = 0;
        c->line = 1;
        for (k = 0; k < LINECURSOR_WAYS; k++)
            if (c->saved[k].file == f) {
                c->off = c->saved[k].off;
                c->line = c->saved[k].line;
            }
    }
    if (off >= c->off) {
        const char *p = f->buf + c->off, *e = f->buf + off;
        uint32_t n = 0;
        while (p < e && (p = memchr(p, '\n', (size_t)(e - p))) != NULL) {
            n++;
            p++;
        }
        c->line += n;
    } else {
        uint32_t col;
        srcmgr_linecol(f, loc, &c->line, &col);
    }
    c->off = off;
    nl = memchr(f->buf + off, '\n', f->size - off);
    c->next_nl = nl ? (uint32_t)(nl - f->buf) : f->size;
    return c->line;
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
