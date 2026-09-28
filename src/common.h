/* common.h - shared utilities: arena, vectors, strings, interner. */
#ifndef CEREAL_COMMON_H
#define CEREAL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void fatal(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);

/* ---- Arena ---------------------------------------------------------- */

typedef struct ArenaBlock ArenaBlock;
typedef struct Arena {
    ArenaBlock *head;
    size_t total;
} Arena;

void arena_init(Arena *a);
void arena_free(Arena *a);
void *arena_alloc(Arena *a, size_t n);          /* zeroed */
char *arena_strndup(Arena *a, const char *s, size_t n);
char *arena_strdup(Arena *a, const char *s);
char *arena_printf(Arena *a, const char *fmt, ...);

#define NEW(A, T) ((T *)arena_alloc((A), sizeof(T)))
#define NEW_ARRAY(A, T, n) ((T *)arena_alloc((A), sizeof(T) * (size_t)(n)))

/* ---- Vectors (heap backed; free with vec_free) ---------------------- */

#define VEC(T) struct { T *data; size_t len, cap; }

void *vec_grow_(void *data, size_t *cap, size_t elem);

#define vec_push(v, x)                                                   \
    ((v)->len == (v)->cap                                                \
         ? (void)((v)->data = vec_grow_((v)->data, &(v)->cap,            \
                                        sizeof *(v)->data))              \
         : (void)0,                                                      \
     (v)->data[(v)->len++] = (x))
#define vec_pop(v) ((v)->data[--(v)->len])
#define vec_last(v) ((v)->data[(v)->len - 1])
#define vec_free(v) (free((v)->data), (v)->data = NULL, (v)->len = (v)->cap = 0)

/* ---- String buffer --------------------------------------------------- */

typedef struct StrBuf {
    char *data;
    size_t len, cap;
} StrBuf;

void sb_putc(StrBuf *sb, char c);
void sb_putn(StrBuf *sb, const char *s, size_t n);
void sb_puts(StrBuf *sb, const char *s);
void sb_printf(StrBuf *sb, const char *fmt, ...);
void sb_vprintf(StrBuf *sb, const char *fmt, va_list ap);
const char *sb_cstr(StrBuf *sb);   /* NUL-terminated view */
void sb_free(StrBuf *sb);

/* ---- Identifier interner --------------------------------------------- */

struct Macro;

typedef struct Ident {
    const char *str;
    uint32_t len;
    uint32_t hash;
    uint32_t id;           /* dense, >= 1 */
    uint16_t kw;           /* keyword/special id, 0 if none */
    uint16_t flags;
    struct Macro *macro;   /* currently active definition, or NULL */
    struct Macro *history; /* most recent definition ever made (linked via prev) */
    void *user;            /* scratch slot for analyzers */
} Ident;

enum {
    IDF_POISONED   = 1 << 0,
    IDF_EVER_REFD  = 1 << 1  /* referenced in #ifdef/defined/expansion */
};

typedef struct Interner {
    Arena *arena;
    Ident **slots;         /* open addressing */
    size_t cap, count;
    VEC(Ident *) byid;
} Interner;

void interner_init(Interner *in, Arena *a);
void interner_free(Interner *in);
Ident *intern(Interner *in, const char *s, size_t n);
Ident *intern_cstr(Interner *in, const char *s);
uint32_t hash_bytes(const char *s, size_t n);

static inline Ident *ident_by_id(const Interner *in, uint32_t id)
{
    return in->byid.data[id];
}

/* Iterate all identifiers. */
#define INTERNER_FOREACH(in, b, id)                                       \
    for (size_t b = 1; b < (in)->byid.len; b++)                           \
        for (Ident *id = (in)->byid.data[b]; id; id = NULL)

/* ---- misc ------------------------------------------------------------ */

bool str_eq_n(const char *a, size_t an, const char *b);
unsigned edit_distance(const char *a, size_t an, const char *b, size_t bn,
                       unsigned limit);
char *read_file(const char *path, size_t *len_out);

#endif
