/* cintrin.h - the built-ins the back end implements itself (TO_INTRIN nodes).
 *
 * One table: a name (after "__builtin_"), the way the back end generates it (a class) and
 * one argument for the class.  The front end looks a name up once (bi_find) and keeps the
 * row in the node (pun); the back end switches on the class.  A name with no row is left to
 * the back end to refuse.  The atomics (__atomic_*, __sync_*) are rows too, named in full; va_*
 * has nodes of its own.
 *
 * The same row tells the front end what gcc's own resolution of the call does: the argument
 * count (argc), which checks the arguments get (chk), the type of the result (res), and
 * whether the call has no prototype (BIF_GEN: the arguments are used as written) or is
 * lowered from the row although it has one (BIF_LOWER: the operands convert to the
 * prototype's parameters).  A row of class BIC_NONE with such data is checked and typed
 * but left to the back end to refuse. */
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
    BIC_CLRSB,      /* arg: the width in bytes (the front end folds it; the back end refuses it) */
    BIC_FFS,        /* arg: the width in bytes */
    BIC_POPCNT, BIC_PARITY,   /* a call of a libgcc helper (bi_helper); arg: the width in bytes */
    BIC_SQRT, BIC_RCP, BIC_RSQRT,   /* arg: the float's width */
    BIC_SFENCE, BIC_LFENCE, BIC_MFENCE, BIC_PAUSE, BIC_UD2,
    BIC_CLFLUSH, BIC_STMXCSR, BIC_LDMXCSR,
    BIC_PREFETCH,   /* a hint: only the address is evaluated */
    BIC_LIBC,       /* a call of the library function of the same name */
    BIC_ATOMIC,     /* an operation on an object of 1, 2, 4 or 8 bytes; arg: AT_ op | AF_ flags << 4 */
    BIC_OVF,        /* arithmetic with an overflow flag; arg: OV_ op */
    BIC_STDC        /* a C23 <stdbit.h> function of an unsigned operand; arg: ST_ op | ST_INV */
};

/* Row flags (fl). */
enum {
    BIF_GEN = 1,    /* no prototype: the arguments are used as written, the type comes from res */
    BIF_LOWER = 2   /* lowered from the row; the operands convert to the prototype's parameters */
};

/* Which checks gcc makes of the arguments (chk). */
enum {
    BK_NONE,
    BK_LOCKFREE,    /* __atomic_is_lock_free, __atomic_always_lock_free */
    BK_STDC,        /* one operand, an unsigned integer that is not a _Bool */
    BK_COUNT,       /* only the argument count */
    BK_ALIGN,       /* alloca_with_align: the alignment is a constant power of 2 of 8 to 2^31 bits */
    BK_ASSUME,      /* assume_aligned: 2 or 3 arguments, an integer third one */
    BK_CLEARPAD,    /* clear_padding: a pointer to a complete, non-const type */
    BK_FPCLASS,     /* fpclassify: five constants, then a floating operand */
    BK_FP1,         /* one floating operand (isnan, signbit ...) */
    BK_FPCMP,       /* two operands, one of them floating (isgreater ...) */
    BK_OVF,         /* the generic overflow built-ins: two integers and a pointer to an integer */
    BK_OVFP,        /* the _p forms: three integers */
    BK_SPEC,        /* speculation_safe_value: 1 or 2 arguments */
    BK_VASTART,     /* va_start: at least 1 argument */
    BK_GENERIC,     /* the generic atomics: void * operands of one nonzero size */
    BK_SIZED,       /* the atomics sized by a pointer to an integer or pointer (1 to 16 bytes) */
    BK_FETCH        /* the same, and the object is not a _Bool */
};

/* The type of the result (res): a rule, and for the argument rules the index of the argument
 * in bits 4.. (BR_ARG: its type, BR_PTEE: its pointee). */
enum { BR_NONE, BR_VOID, BR_BOOL, BR_UINT, BR_ARG, BR_PTEE };
#define BR(rule, ix) ((rule) | (ix) << 4)

/* The overflow built-ins: the generic ones (no prototype) take their operand types from the
 * arguments, the typed ones (__builtin_sadd_overflow ...) have prototypes. */
enum { OV_ADD, OV_SUB, OV_MUL };
#define BI_OVF(Y, n, op)                                                    \
    Y(n "_overflow", BIC_OVF, op, BIF_GEN, 3, BK_OVF, BR(BR_BOOL, 0))      \
    BI_OVT(Y, "s" n "_overflow", op) BI_OVT(Y, "u" n "_overflow", op)       \
    BI_OVT(Y, "s" n "l_overflow", op) BI_OVT(Y, "u" n "l_overflow", op)     \
    BI_OVT(Y, "s" n "ll_overflow", op) BI_OVT(Y, "u" n "ll_overflow", op)
#define BI_OVT(Y, nm, op) Y(nm, BIC_OVF, op, BIF_LOWER, 3, BK_NONE, 0)

/* The <stdbit.h> functions: the bit scans and counts of the operand, or of its complement. */
enum { ST_LZ, ST_TZ, ST_FLO, ST_FTO, ST_POP, ST_HAS1, ST_BW, ST_FLOOR, ST_CEIL, ST_OP = 15, ST_INV = 16 };
#define BI_ST(Y, n, op, res)                                                    Y("stdc_" n, BIC_STDC, op, BIF_GEN, 1, BK_STDC, res)
#define BI_STU(Y, n, op) BI_ST(Y, n, op, BR(BR_UINT, 0))

/* A row for the front end alone: argument count and checks, nothing generated. */
#define BI_CK(Y, n, argc, chk) Y(n, BIC_NONE, 0, 0, argc, chk, 0)
#define BI_FP1(Y, n) BI_CK(Y, n, 1, BK_FP1)
#define BI_CMP(Y, n) BI_CK(Y, n, 2, BK_FPCMP)

/* The atomic operations, and what the flags say about the arguments and the result. */
enum { AT_LOAD, AT_STORE, AT_ST0, AT_XCHG, AT_ADD, AT_SUB, AT_AND, AT_OR, AT_XOR, AT_NAND, AT_CAS };
enum {
    AF_NEW = 1,     /* a read-modify-write gives the new value, not the old */
    AF_PTR = 2,     /* the generic forms: values and results go by pointer */
    AF_EXPV = 4,    /* compare-and-swap: the expected value is an argument, not a pointer to it */
    AF_OLD = 8      /* compare-and-swap: the result is the old value, not a flag */
};
#define BI_AT(op, fl) ((op) | (fl) << 4)
/* An atomic row: argc is that of the __atomic_ form (the __sync_ ones take any count). */
#define BI_A(Y, n, op, af, argc, chk, res)                                  \
    Y(n, BIC_ATOMIC, BI_AT(op, af), BIF_GEN, argc, chk, res)
#define BI_RMW(Y, n, op)                                                    \
    BI_A(Y, "__atomic_fetch_" n, op, 0, 3, BK_FETCH, BR(BR_PTEE, 0))        \
    BI_A(Y, "__atomic_" n "_fetch", op, AF_NEW, 3, BK_FETCH, BR(BR_PTEE, 0)) \
    BI_A(Y, "__sync_fetch_and_" n, op, 0, 0, BK_FETCH, BR(BR_PTEE, 0))      \
    BI_A(Y, "__sync_" n "_and_fetch", op, AF_NEW, 0, BK_FETCH, BR(BR_PTEE, 0))

/* X: a row with only a class and an argument; Y: one with the front end's data too. */
#define BI_ROWS(X, Y)                                                           \
    X("expect", BIC_EXPECT, 0)                                                  \
    X("huge_val", BIC_HUGE, 8) X("inf", BIC_HUGE, 8)                            \
    X("huge_valf", BIC_HUGE, 4) X("inff", BIC_HUGE, 4)                          \
    X("bswap16", BIC_BSWAP, 2) X("bswap32", BIC_BSWAP, 4) X("bswap64", BIC_BSWAP, 8) \
    X("clz", BIC_CLZ, 4) X("clzl", BIC_CLZ, 8) X("clzll", BIC_CLZ, 8)           \
    X("ctz", BIC_CTZ, 4) X("ctzl", BIC_CTZ, 8) X("ctzll", BIC_CTZ, 8)           \
    X("ffs", BIC_FFS, 4) X("ffsl", BIC_FFS, 8) X("ffsll", BIC_FFS, 8)           \
    X("ffsimax", BIC_FFS, 8)                                                    \
    X("clrsb", BIC_CLRSB, 4) X("clrsbl", BIC_CLRSB, 8) X("clrsbll", BIC_CLRSB, 8) \
    X("clrsbimax", BIC_CLRSB, 8)                                                \
    X("popcount", BIC_POPCNT, 4) X("popcountl", BIC_POPCNT, 8)                  \
    X("popcountll", BIC_POPCNT, 8) X("popcountimax", BIC_POPCNT, 8)             \
    X("parity", BIC_PARITY, 4) X("parityl", BIC_PARITY, 8)                      \
    X("parityll", BIC_PARITY, 8) X("parityimax", BIC_PARITY, 8)                 \
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
    X("labs", BIC_LIBC, 0) X("llabs", BIC_LIBC, 0)                              \
    Y("speculation_safe_value", BIC_NONE, 0, 0, 0, BK_SPEC, BR(BR_ARG, 0))      \
    BI_CK(Y, "constant_p", 1, BK_COUNT) BI_CK(Y, "alloca_with_align", 2, BK_ALIGN) \
    BI_CK(Y, "alloca_with_align_and_max", 3, BK_COUNT)                          \
    BI_CK(Y, "assume_aligned", 0, BK_ASSUME) BI_CK(Y, "fpclassify", 6, BK_FPCLASS) \
    BI_CK(Y, "clear_padding", 1, BK_CLEARPAD) BI_CK(Y, "va_start", 0, BK_VASTART) \
    BI_CK(Y, "c23_va_start", 0, BK_VASTART)                                     \
    BI_FP1(Y, "isfinite") BI_FP1(Y, "isinf_sign") BI_FP1(Y, "isinf")            \
    BI_FP1(Y, "isnan") BI_FP1(Y, "isnormal") BI_FP1(Y, "signbit")               \
    BI_CMP(Y, "isgreater") BI_CMP(Y, "isgreaterequal") BI_CMP(Y, "isless")      \
    BI_CMP(Y, "islessequal") BI_CMP(Y, "islessgreater") BI_CMP(Y, "isunordered") \
    BI_CK(Y, "add_overflow_p", 3, BK_OVFP) BI_CK(Y, "sub_overflow_p", 3, BK_OVFP) \
    BI_CK(Y, "mul_overflow_p", 3, BK_OVFP)                                      \
    BI_A(Y, "__atomic_load", AT_LOAD, AF_PTR, 3, BK_GENERIC, BR(BR_VOID, 0))    \
    BI_A(Y, "__atomic_load_n", AT_LOAD, 0, 2, BK_SIZED, BR(BR_PTEE, 0))         \
    BI_A(Y, "__atomic_store", AT_STORE, AF_PTR, 3, BK_GENERIC, BR(BR_VOID, 0))  \
    BI_A(Y, "__atomic_store_n", AT_STORE, 0, 3, BK_SIZED, BR(BR_VOID, 0))       \
    BI_A(Y, "__atomic_exchange", AT_XCHG, AF_PTR, 4, BK_GENERIC, BR(BR_VOID, 0)) \
    BI_A(Y, "__atomic_exchange_n", AT_XCHG, 0, 3, BK_SIZED, BR(BR_PTEE, 0))     \
    BI_A(Y, "__atomic_compare_exchange", AT_CAS, AF_PTR, 6, BK_GENERIC, BR(BR_BOOL, 0)) \
    BI_A(Y, "__atomic_compare_exchange_n", AT_CAS, 0, 6, BK_SIZED, BR(BR_BOOL, 0)) \
    BI_A(Y, "__sync_bool_compare_and_swap", AT_CAS, AF_EXPV, 0, BK_SIZED, BR(BR_BOOL, 0)) \
    BI_A(Y, "__sync_val_compare_and_swap", AT_CAS, AF_EXPV | AF_OLD, 0, BK_SIZED, BR(BR_PTEE, 0)) \
    BI_A(Y, "__sync_lock_test_and_set", AT_XCHG, 0, 0, BK_SIZED, BR(BR_PTEE, 0)) \
    BI_A(Y, "__sync_lock_release", AT_ST0, 0, 0, BK_SIZED, BR(BR_VOID, 0))      \
    BI_RMW(Y, "add", AT_ADD) BI_RMW(Y, "sub", AT_SUB) BI_RMW(Y, "and", AT_AND)  \
    BI_RMW(Y, "or", AT_OR) BI_RMW(Y, "xor", AT_XOR) BI_RMW(Y, "nand", AT_NAND)  \
    BI_STU(Y, "leading_zeros", ST_LZ) BI_STU(Y, "leading_ones", ST_LZ | ST_INV)     BI_STU(Y, "trailing_zeros", ST_TZ) BI_STU(Y, "trailing_ones", ST_TZ | ST_INV)     BI_STU(Y, "first_leading_zero", ST_FLO | ST_INV) BI_STU(Y, "first_leading_one", ST_FLO)     BI_STU(Y, "first_trailing_zero", ST_FTO | ST_INV) BI_STU(Y, "first_trailing_one", ST_FTO)     BI_STU(Y, "count_zeros", ST_POP | ST_INV) BI_STU(Y, "count_ones", ST_POP)       BI_STU(Y, "bit_width", ST_BW)                                                   BI_ST(Y, "has_single_bit", ST_HAS1, BR(BR_BOOL, 0))                             BI_ST(Y, "bit_floor", ST_FLOOR, BR(BR_ARG, 0)) BI_ST(Y, "bit_ceil", ST_CEIL, BR(BR_ARG, 0))     Y("__atomic_test_and_set", BIC_NONE, 0, 0, 2, BK_NONE, BR(BR_BOOL, 0))      \
    Y("__atomic_clear", BIC_NONE, 0, 0, 2, BK_NONE, BR(BR_VOID, 0))             \
    Y("__atomic_thread_fence", BIC_NONE, 0, 0, 1, BK_NONE, BR(BR_VOID, 0))      \
    Y("__atomic_signal_fence", BIC_NONE, 0, 0, 1, BK_NONE, BR(BR_VOID, 0))      \
    Y("__atomic_is_lock_free", BIC_NONE, 0, 0, 2, BK_LOCKFREE, BR(BR_BOOL, 0))  \
    Y("__atomic_always_lock_free", BIC_NONE, 0, 0, 2, BK_LOCKFREE, BR(BR_BOOL, 0)) \
    BI_OVF(Y, "add", OV_ADD) BI_OVF(Y, "sub", OV_SUB) BI_OVF(Y, "mul", OV_MUL)

/* What gcc calls for the bit counts the CPU baseline has no instruction for. */
static const char *const bi_helper[] = {"__popcountdi2", "__paritydi2"};

typedef struct BiRow {
    const char *name;
    uint8_t cls;
    uint8_t arg;
    uint8_t fl;     /* BIF_ */
    uint8_t argc;   /* the argument count gcc requires; 0: not checked */
    uint8_t chk;    /* BK_ */
    uint8_t res;    /* BR(rule, index); BR_NONE: the prototype gives the type */
} BiRow;

/* Rows are numbered from 1; 0 is no row. */
static const BiRow bi_rows[] = {
    {"", BIC_NONE, 0, 0, 0, 0, 0},
#define X(n, c, a) {n, c, a, 0, 0, 0, 0},
#define Y(n, c, a, f, k, ck, r) {n, c, a, f, k, ck, r},
    BI_ROWS(X, Y)
#undef X
#undef Y
};

static inline unsigned bi_find(const char *name)
{
    size_t i;
    for (i = 1; i < sizeof bi_rows / sizeof *bi_rows; i++)
        if (!strcmp(bi_rows[i].name, name))
            return (unsigned)i;
    return 0;
}

/* The row of a callee named as written in the source: "__builtin_x" is row x, an atomic is
 * named in full (and "__builtin___atomic_x" is nothing). */
static inline unsigned bi_find_call(const char *name)
{
    if (strncmp(name, "__builtin_", 10))
        return bi_find(name);
    name += 10;
    return name[0] == '_' && name[1] == '_' ? 0 : bi_find(name);
}

/* The argument count of an __atomic_ built-in, the sized variants (_1 to _16) included; 0 when
 * there is none to check. */
static inline unsigned bi_argc(const char *name)
{
    char b[48];
    size_t len = strlen(name);
    if (strncmp(name, "__atomic_", 9) || len >= sizeof b)
        return 0;
    memcpy(b, name, len + 1);
    while (len && b[len - 1] >= '0' && b[len - 1] <= '9')
        b[--len] = 0;
    if (len < strlen(name) && len && b[len - 1] == '_')
        b[--len] = 0;
    return bi_rows[bi_find(b)].argc;
}

/* The integer built-ins as pure functions of one operand, shared by the front end's constant
 * folder and the evaluator.  The width is the row's arg, or w (bytes) for BIC_STDC.  BI_NO: the
 * row is not such an operation; BI_UNDEF: the result is undefined (clz/ctz of 0, a bit_ceil that
 * does not fit), and *out is the value the folder and the code generator settle on. */
enum { BI_OK, BI_NO, BI_UNDEF };

static inline int bi_int(const BiRow *r, unsigned w, uint64_t x, uint64_t *out)
{
    unsigned bits, op;
    uint64_t m, s;
    if (r->cls == BIC_STDC) {
        op = r->arg & ST_OP;
    } else {
        w = r->arg;
        op = 0;
    }
    if (w < 1 || w > 8)
        return BI_NO;
    bits = w * 8;
    m = bits == 64 ? ~(uint64_t)0 : ((uint64_t)1 << bits) - 1;
    x &= m;
    if (r->cls == BIC_STDC && (r->arg & ST_INV))
        x = ~x & m;
#define LZ(v) ((unsigned)__builtin_clzll(v) - (64 - bits))
    switch (r->cls) {
    case BIC_CLZ: case BIC_CTZ:
        *out = bits;
        if (!x)
            return BI_UNDEF;
        *out = r->cls == BIC_CLZ ? LZ(x) : (unsigned)__builtin_ctzll(x);
        return BI_OK;
    case BIC_FFS:
        *out = x ? (unsigned)__builtin_ctzll(x) + 1 : 0;
        return BI_OK;
    case BIC_CLRSB:
        s = x >> (bits - 1) ? ~x & m : x;
        *out = s ? LZ(s) - 1 : bits - 1;
        return BI_OK;
    case BIC_POPCNT:
        *out = (unsigned)__builtin_popcountll(x);
        return BI_OK;
    case BIC_PARITY:
        *out = (unsigned)__builtin_popcountll(x) & 1;
        return BI_OK;
    case BIC_BSWAP:
        *out = __builtin_bswap64(x) >> (64 - bits);
        return BI_OK;
    case BIC_STDC:
        break;
    default:
        return BI_NO;
    }
    switch (op) {
    case ST_LZ:    *out = x ? LZ(x) : bits; break;
    case ST_TZ:    *out = x ? (unsigned)__builtin_ctzll(x) : bits; break;
    case ST_FLO:   *out = x ? LZ(x) + 1 : 0; break;
    case ST_FTO:   *out = x ? (unsigned)__builtin_ctzll(x) + 1 : 0; break;
    case ST_POP:   *out = (unsigned)__builtin_popcountll(x); break;
    case ST_HAS1:  *out = x && !(x & (x - 1)); break;
    case ST_BW:    *out = x ? bits - LZ(x) : 0; break;
    case ST_FLOOR: *out = x ? (uint64_t)1 << (bits - 1 - LZ(x)) : 0; break;
    case ST_CEIL:
        if (x <= 1) {
            *out = 1;
            break;
        }
        s = bits - LZ(x - 1);
        *out = 0;
        if (s >= bits)
            return BI_UNDEF;
        *out = (uint64_t)1 << s;
        break;
    default:
        return BI_NO;
    }
    return BI_OK;
#undef LZ
}

#endif
