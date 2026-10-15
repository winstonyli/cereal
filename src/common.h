/* common.h - shared utilities: arena, vectors, strings, interner. */
#ifndef CEREAL_COMMON_H
#define CEREAL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void fatal(const char *fmt, ...);

/* A fatal() trap: on a thread that has pushed one, fatal() jumps back to
 * it with the message instead of exiting (the language server's builder
 * drops the one build).  It is popped before the jump; the code after a
 * normal return pops it itself.  Not taken while the thread holds a lock
 * (thread.c counts them: a jump would leave it held); fatal() exits then.
 *     FatalTrap tr;
 *     fatal_trap_push(&tr);
 *     if (setjmp(tr.jb) == 0) { work(); fatal_trap_pop(&tr); }
 *     else { failed: tr.msg }                                          */
typedef struct FatalTrap {
    jmp_buf jb;
    char msg[256];
    struct FatalTrap *prev;
} FatalTrap;
void fatal_trap_push(FatalTrap *t);
void fatal_trap_pop(FatalTrap *t);
extern __thread unsigned fatal_locks_held; /* thread.c's mutex_lock */

/* Fault injection for tests: true at the Nth call naming `site` when
 * CEREAL_FAULT is "site:N" (N defaults to 1), process-wide. */
bool fault_hit(const char *site);

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

/* Identifier interning lives in intern.h. */
uint32_t hash_bytes(const char *s, size_t n);

/* ---- misc ------------------------------------------------------------ */

bool str_eq_n(const char *a, size_t an, const char *b);
unsigned edit_distance(const char *a, size_t an, const char *b, size_t bn,
                       unsigned limit);
char *read_file(const char *path, size_t *len_out);
/* What the source manager can load (srcmgr_load): a regular file of at most
 * 0xF0000000 bytes.  file_loadable takes its stat data; file_openable also
 * needs the file to open. */
struct stat;
bool file_loadable(const struct stat *st);
bool file_openable(const char *path);
/* The length of the UTF-8 byte order mark at the start of buf (libcpp drops
 * it), else 0. */
size_t bom_len(const void *buf, size_t n);
/* qsort order of u64s, and sorting with the duplicates removed (returns the new length). */
int u64_cmp(const void *a, const void *b);
size_t sort_unique_u64(uint64_t *v, size_t n);
/* A loadable file whole, as the index sees it (no byte order mark, NUL
 * terminated); NULL if it is not.  *st gets the fstat from before the read (7.2:
 * stat data must not be newer than the bytes it describes). */
char *read_file_nobom(const char *path, size_t *len_out, struct stat *st);
/* Replaces the file at path (or creates it, mode 0600) with buf: written to a
 * temporary file in the same directory, fsync'd if sync (the user's files; the
 * index's are checksummed and heal on a bad read), given the original's mode
 * (and owner, if it can; a new file keeps mkstemp's), then renamed over it.  Nothing is changed on
 * failure; err gets the reason. */
bool file_write_atomic(const char *path, const char *buf, size_t n, bool sync,
                       char *err, size_t errn);
/* mkdir -p, directories mode 0700 (the index cache holds source-derived names). */
bool dir_create_all(const char *path, char *err, size_t errn);

#endif
