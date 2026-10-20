/* cintrin.h - the built-ins the back end implements itself (TO_INTRIN nodes).
 *
 * One table: a name (after "__builtin_"), the way the back end generates it (a class) and
 * one argument for the class.  The front end looks a name up once (bi_find) and keeps the
 * row in the node (pun); the back end switches on the class.  A name with no row is left to
 * the back end to refuse.  Atomics and va_* are not here: they have nodes of their own. */
#ifndef CEREAL_CINTRIN_H
#define CEREAL_CINTRIN_H

#include <stdint.h>
#include <string.h>

enum {
    BIC_NONE,
    BIC_EXPECT,     /* the first argument (when the second is a constant) */
    BIC_HUGE,       /* an infinity; arg: the float's width */
    BIC_BSWAP,      /* arg: the width in bytes */
    BIC_CLZ, BIC_CTZ,   /* arg: the width in bytes */
    BIC_FFS,        /* arg: the width in bytes */
    BIC_RT,         /* a call of a libgcc helper; arg: its index in bi_helper */
    BIC_SQRT, BIC_RCP, BIC_RSQRT,   /* arg: the float's width */
    BIC_SFENCE, BIC_LFENCE, BIC_MFENCE, BIC_PAUSE, BIC_UD2,
    BIC_CLFLUSH, BIC_STMXCSR, BIC_LDMXCSR,
    BIC_PREFETCH,   /* a hint: only the address is evaluated */
    BIC_LIBC        /* a call of the library function of the same name */
};

#define BI_ROWS(X)                                                              \
    X("expect", BIC_EXPECT, 0)                                                  \
    X("huge_val", BIC_HUGE, 8) X("inf", BIC_HUGE, 8)                            \
    X("huge_valf", BIC_HUGE, 4) X("inff", BIC_HUGE, 4)                          \
    X("bswap16", BIC_BSWAP, 2) X("bswap32", BIC_BSWAP, 4) X("bswap64", BIC_BSWAP, 8) \
    X("clz", BIC_CLZ, 4) X("clzl", BIC_CLZ, 8) X("clzll", BIC_CLZ, 8)           \
    X("ctz", BIC_CTZ, 4) X("ctzl", BIC_CTZ, 8) X("ctzll", BIC_CTZ, 8)           \
    X("ffs", BIC_FFS, 4) X("ffsl", BIC_FFS, 8) X("ffsll", BIC_FFS, 8)           \
    X("ffsimax", BIC_FFS, 8)                                                    \
    X("popcount", BIC_RT, 0) X("popcountl", BIC_RT, 0) X("popcountll", BIC_RT, 0) \
    X("popcountimax", BIC_RT, 0) X("parity", BIC_RT, 1) X("parityl", BIC_RT, 1) \
    X("parityll", BIC_RT, 1) X("parityimax", BIC_RT, 1)                         \
    X("sqrt", BIC_SQRT, 8) X("sqrtf", BIC_SQRT, 4)                              \
    X("ia32_rcpss", BIC_RCP, 4) X("ia32_rsqrtss", BIC_RSQRT, 4)                 \
    X("ia32_sfence", BIC_SFENCE, 0) X("ia32_lfence", BIC_LFENCE, 0)             \
    X("ia32_mfence", BIC_MFENCE, 0) X("ia32_pause", BIC_PAUSE, 0)               \
    X("unreachable", BIC_UD2, 0) X("ia32_clflush", BIC_CLFLUSH, 0)              \
    X("ia32_stmxcsr", BIC_STMXCSR, 0) X("ia32_ldmxcsr", BIC_LDMXCSR, 0)         \
    X("prefetch", BIC_PREFETCH, 0)                                              \
    X("memset", BIC_LIBC, 0) X("memcpy", BIC_LIBC, 0) X("memmove", BIC_LIBC, 0) \
    X("memcmp", BIC_LIBC, 0) X("strlen", BIC_LIBC, 0) X("strcmp", BIC_LIBC, 0)  \
    X("strncmp", BIC_LIBC, 0) X("strcpy", BIC_LIBC, 0) X("strncpy", BIC_LIBC, 0) \
    X("strcat", BIC_LIBC, 0) X("strncat", BIC_LIBC, 0) X("strchr", BIC_LIBC, 0) \
    X("strrchr", BIC_LIBC, 0) X("strstr", BIC_LIBC, 0) X("abs", BIC_LIBC, 0)    \
    X("labs", BIC_LIBC, 0) X("llabs", BIC_LIBC, 0)

/* What gcc calls for the bit counts the CPU baseline has no instruction for. */
static const char *const bi_helper[] = {"__popcountdi2", "__paritydi2"};

typedef struct BiRow {
    const char *name;
    uint8_t cls;
    uint8_t arg;
} BiRow;

/* Rows are numbered from 1; 0 is no row. */
static const BiRow bi_rows[] = {
    {"", BIC_NONE, 0},
#define X(n, c, a) {n, c, a},
    BI_ROWS(X)
#undef X
};

static inline unsigned bi_find(const char *name)
{
    size_t i;
    for (i = 1; i < sizeof bi_rows / sizeof *bi_rows; i++)
        if (!strcmp(bi_rows[i].name, name))
            return (unsigned)i;
    return 0;
}

#endif
