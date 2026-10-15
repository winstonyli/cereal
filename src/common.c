/* common.c - shared utilities. */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static __thread FatalTrap *fatal_trap;
__thread unsigned fatal_locks_held;

void fatal_trap_push(FatalTrap *t)
{
    t->msg[0] = 0;
    t->prev = fatal_trap;
    fatal_trap = t;
}

void fatal_trap_pop(FatalTrap *t)
{
    fatal_trap = t->prev;
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (fatal_trap && !fatal_locks_held) {
        FatalTrap *t = fatal_trap;
        fatal_trap = t->prev;
        vsnprintf(t->msg, sizeof t->msg, fmt, ap);
        va_end(ap);
        longjmp(t->jb, 1);
    }
    fputs("cereal: fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        fatal("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* ---- Arena ---------------------------------------------------------- */

struct ArenaBlock {
    ArenaBlock *next;
    size_t used, size;
    /* data follows, max-aligned */
};

#define ARENA_ALIGN 16
#define ARENA_BLOCK 65536
#define BLOCK_HDR ((sizeof(ArenaBlock) + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1))

void arena_init(Arena *a)
{
    a->head = NULL;
    a->total = 0;
}

void arena_free(Arena *a)
{
    ArenaBlock *b = a->head;
    while (b) {
        ArenaBlock *n = b->next;
        free(b);
        b = n;
    }
    a->head = NULL;
    a->total = 0;
}

void *arena_alloc(Arena *a, size_t n)
{
    ArenaBlock *b = a->head;
    n = (n + ARENA_ALIGN - 1) & ~(size_t)(ARENA_ALIGN - 1);
    if (!b || b->size - b->used < n) {
        size_t sz = n > ARENA_BLOCK / 4 ? n : ARENA_BLOCK;
        b = xmalloc(BLOCK_HDR + sz);
        b->used = 0;
        b->size = sz;
        if (n > ARENA_BLOCK / 4 && a->head) {
            /* big allocation: keep the current block on top */
            b->next = a->head->next;
            a->head->next = b;
        } else {
            b->next = a->head;
            a->head = b;
        }
        a->total += sz;
    }
    {
        char *p = (char *)b + BLOCK_HDR + b->used;
        b->used += n;
        memset(p, 0, n);
        return p;
    }
}

char *arena_strndup(Arena *a, const char *s, size_t n)
{
    char *p = arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *arena_strdup(Arena *a, const char *s)
{
    return arena_strndup(a, s, strlen(s));
}

char *arena_printf(Arena *a, const char *fmt, ...)
{
    StrBuf sb = {0};
    va_list ap;
    char *r;
    va_start(ap, fmt);
    sb_vprintf(&sb, fmt, ap);
    va_end(ap);
    r = arena_strndup(a, sb.data ? sb.data : "", sb.len);
    sb_free(&sb);
    return r;
}

/* ---- Vectors -------------------------------------------------------- */

void *vec_grow_(void *data, size_t *cap, size_t elem)
{
    size_t nc = *cap ? *cap * 2 : 8;
    data = xrealloc(data, nc * elem);
    *cap = nc;
    return data;
}

/* ---- StrBuf --------------------------------------------------------- */

static void sb_reserve(StrBuf *sb, size_t extra)
{
    if (sb->len + extra + 1 > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 64;
        while (nc < sb->len + extra + 1)
            nc *= 2;
        sb->data = xrealloc(sb->data, nc);
        sb->cap = nc;
    }
}

void sb_putc(StrBuf *sb, char c)
{
    sb_reserve(sb, 1);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = 0;
}

void sb_putn(StrBuf *sb, const char *s, size_t n)
{
    sb_reserve(sb, n);
    if (n)
        memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = 0;
}

void sb_puts(StrBuf *sb, const char *s)
{
    sb_putn(sb, s, strlen(s));
}

void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0)
        return;
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    sb->len += (size_t)n;
}

void sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    sb_vprintf(sb, fmt, ap);
    va_end(ap);
}

const char *sb_cstr(StrBuf *sb)
{
    if (!sb->data)
        sb_reserve(sb, 0), sb->data[0] = 0;
    return sb->data;
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

/* ---- Interner ------------------------------------------------------- */

/* Word-at-a-time hash with a murmur3 finalizer (open addressing needs
 * well-mixed low bits: generated names like get_1, get_2... cluster). */
uint32_t hash_bytes(const char *s, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, s, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h = (h << 31) | (h >> 33);
        s += 8;
        n -= 8;
    }
    if (n) {
        uint64_t w = 0;
        memcpy(&w, s, n);
        h ^= w;
    }
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return (uint32_t)h;
}

/* ---- misc ----------------------------------------------------------- */

bool str_eq_n(const char *a, size_t an, const char *b)
{
    return strlen(b) == an && memcmp(a, b, an) == 0;
}

unsigned edit_distance(const char *a, size_t an, const char *b, size_t bn,
                       unsigned limit)
{
    unsigned row[128], i, j;
    if (bn >= ARRAY_LEN(row) || an >= 128)
        return limit + 1;
    if ((an > bn ? an - bn : bn - an) > limit)
        return limit + 1;
    for (j = 0; j <= bn; j++)
        row[j] = j;
    for (i = 1; i <= an; i++) {
        unsigned prev = row[0], best;
        row[0] = i;
        best = row[0];
        for (j = 1; j <= bn; j++) {
            unsigned cur = row[j];
            unsigned sub = prev + (a[i - 1] != b[j - 1]);
            unsigned v = MIN(MIN(row[j] + 1, row[j - 1] + 1), sub);
            row[j] = v;
            prev = cur;
            if (v < best)
                best = v;
        }
        if (best > limit)
            return limit + 1;
    }
    return row[bn];
}

char *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    StrBuf sb = {0};
    char buf[65536];
    size_t n;
    if (!f)
        return NULL;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        sb_putn(&sb, buf, n);
    if (ferror(f)) {
        fclose(f);
        sb_free(&sb);
        return NULL;
    }
    fclose(f);
    sb_cstr(&sb);
    *len_out = sb.len;
    return sb.data;
}

bool file_loadable(const struct stat *st)
{
    return S_ISREG(st->st_mode) && (uint64_t)st->st_size <= 0xF0000000u;
}

bool file_openable(const char *path)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && file_loadable(&st);
    if (fd >= 0)
        close(fd);
    return ok;
}

size_t bom_len(const void *buf, size_t n)
{
    return n >= 3 && !memcmp(buf, "\357\273\277", 3) ? 3 : 0;
}

char *read_file_nobom(const char *path, size_t *len_out, struct stat *st)
{
    struct stat local;
    size_t n, got = 0, bom;
    char *text;
    int fd = open(path, O_RDONLY);
    if (!st)
        st = &local;
    if (fd < 0)
        return NULL;
    if (fstat(fd, st) != 0 || !file_loadable(st)) {     /* before the read */
        close(fd);
        return NULL;
    }
    n = (size_t)st->st_size;
    text = xmalloc(n + 1);
    while (got < n) {
        ssize_t r = read(fd, text + got, n - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    bom = bom_len(text, got);
    got -= bom;
    memmove(text, text + bom, got);
    text[got] = 0;
    *len_out = got;
    return text;
}

/* ---- atomic file replacement (docs/C_DESIGN.md 6.5) ---------------------- */

static void werr(char *err, size_t errn, const char *what, const char *path)
{
    snprintf(err, errn, "%s '%s': %s", what, path, strerror(errno));
}

bool file_write_atomic(const char *path, const char *buf, size_t n, bool sync,
                       char *err, size_t errn)
{
    struct stat st;
    const char *slash = strrchr(path, '/');
    size_t dl = slash ? (size_t)(slash - path) + 1 : 0;
    char *tmp = xmalloc(strlen(path) + 32);
    int fd;
    size_t put = 0;
    bool had = stat(path, &st) == 0;
    if (!had && errno != ENOENT) {
        werr(err, errn, "cannot stat", path);
        free(tmp);
        return false;
    }
    memcpy(tmp, path, dl);
    strcpy(tmp + dl, ".cereal-fix-XXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0) {
        werr(err, errn, "cannot create a temporary file next to", path);
        free(tmp);
        return false;
    }
    while (put < n) {
        ssize_t w = write(fd, buf + put, n - put);
        if (w <= 0) {
            werr(err, errn, "cannot write", tmp);
            goto fail;
        }
        put += (size_t)w;
    }
    if (sync && fsync(fd) != 0) {
        werr(err, errn, "cannot sync", tmp);
        goto fail;
    }
    if (had && fchmod(fd, st.st_mode & 07777) != 0) {
        werr(err, errn, "cannot set the mode of", tmp);
        goto fail;
    }
    if (had && fchown(fd, st.st_uid, st.st_gid) != 0) {
        /* best effort: only root may give a file away */
    }
    if (close(fd) != 0) {
        fd = -1;
        werr(err, errn, "cannot close", tmp);
        goto fail;
    }
    fd = -1;
    if (rename(tmp, path) != 0) {
        werr(err, errn, "cannot replace", path);
        goto fail;
    }
    free(tmp);
    return true;
fail:
    if (fd >= 0)
        close(fd);
    unlink(tmp);
    free(tmp);
    return false;
}

bool dir_create_all(const char *path, char *err, size_t errn)
{
    char *p = xstrdup(path), *s;
    bool ok = true;
    for (s = p + 1; ok; s++) {
        char c = *s;
        if (c == '/' || !c) {
            *s = 0;
            if (mkdir(p, 0700) != 0 && errno != EEXIST) {
                snprintf(err, errn, "cannot create '%s': %s", p, strerror(errno));
                ok = false;
            }
            *s = c;
        }
        if (!c)
            break;
    }
    free(p);
    return ok;
}

int u64_cmp(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa, b = *(const uint64_t *)pb;
    return a < b ? -1 : a > b;
}

size_t sort_unique_u64(uint64_t *v, size_t n)
{
    size_t i, k = 0;
    if (n > 1)
        qsort(v, n, sizeof *v, u64_cmp);
    for (i = 0; i < n; i++)
        if (!k || v[k - 1] != v[i])
            v[k++] = v[i];
    return k;
}
