/* common.c - shared utilities. */
#include "common.h"

#include <stdlib.h>
#include <string.h>

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
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

void interner_init(Interner *in, Arena *a)
{
    memset(in, 0, sizeof *in);
    in->arena = a;
    in->cap = 8192;
    in->slots = xcalloc(in->cap, sizeof(Ident *));
    vec_push(&in->byid, NULL); /* id 0 is reserved: "no identifier" */
}

void interner_free(Interner *in)
{
    free(in->slots);
    vec_free(&in->byid);
    in->slots = NULL;
}

static void interner_grow(Interner *in)
{
    size_t nc = in->cap * 2, i;
    Ident **ns = xcalloc(nc, sizeof(Ident *));
    for (i = 0; i < in->cap; i++) {
        Ident *id = in->slots[i];
        size_t k;
        if (!id)
            continue;
        for (k = id->hash & (nc - 1); ns[k]; k = (k + 1) & (nc - 1))
            ;
        ns[k] = id;
    }
    free(in->slots);
    in->slots = ns;
    in->cap = nc;
}

Ident *intern(Interner *in, const char *s, size_t n)
{
    uint32_t h = hash_bytes(s, n);
    size_t k = h & (in->cap - 1);
    Ident *id;
    for (; (id = in->slots[k]) != NULL; k = (k + 1) & (in->cap - 1))
        if (id->hash == h && id->len == n && memcmp(id->str, s, n) == 0)
            return id;
    id = NEW(in->arena, Ident);
    id->str = arena_strndup(in->arena, s, n);
    id->len = (uint32_t)n;
    id->hash = h;
    id->id = (uint32_t)in->byid.len;
    vec_push(&in->byid, id);
    in->slots[k] = id;
    if (++in->count * 2 > in->cap)
        interner_grow(in);
    return id;
}

Ident *intern_cstr(Interner *in, const char *s)
{
    return intern(in, s, strlen(s));
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
