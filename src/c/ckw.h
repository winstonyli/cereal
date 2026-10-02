/* ckw.h - C keywords (C99, the C11 keywords glibc uses, GNU extensions),
 * recognized through Ident.ckw.  Alternate GNU spellings (__const__,
 * __inline, __asm__, ...) map to the same id. */
#ifndef CEREAL_CKW_H
#define CEREAL_CKW_H

#define CKW_LIST(X)                                                        \
    X(TYPEDEF) X(EXTERN) X(STATIC) X(AUTO) X(REGISTER) X(THREAD_LOCAL)     \
    X(CONST) X(VOLATILE) X(RESTRICT) X(ATOMIC)                             \
    X(INLINE) X(NORETURN)                                                  \
    X(VOID) X(CHAR) X(SHORT) X(INT) X(LONG) X(FLOAT) X(DOUBLE) X(SIGNED)   \
    X(UNSIGNED) X(BOOL) X(COMPLEX) X(IMAGINARY) X(INT128) X(FLOATN)        \
    X(DECIMAL) X(FIXED) X(SAT) X(AUTO_TYPE)                                                \
    X(STRUCT) X(UNION) X(ENUM)                                             \
    X(IF) X(ELSE) X(SWITCH) X(CASE) X(DEFAULT) X(WHILE) X(DO) X(FOR)       \
    X(GOTO) X(CONTINUE) X(BREAK) X(RETURN)                                 \
    X(SIZEOF) X(ALIGNOF) X(GENERIC) X(STATIC_ASSERT) X(ALIGNAS)            \
    X(ATTRIBUTE) X(ASM) X(TYPEOF) X(EXTENSION) X(LABEL) X(REAL) X(IMAG)    \
    X(VA_ARG) X(OFFSETOF) X(TYPES_COMPATIBLE) X(CONVERTVECTOR) X(GIMPLE) X(HAS_ATTRIBUTE)

typedef enum {
    CK_NONE,
#define X(n) CK_##n,
    CKW_LIST(X)
#undef X
    CK_COUNT
} CKeyword;

#define CKW_GNU_ONLY 0x100 /* a keyword only with -std=gnu* (typeof, asm) */

typedef struct CKwSpelling {
    const char *s;
    unsigned short kw;
} CKwSpelling;

/* NULL-terminated; interned (and Ident.ckw set) by interner_init. */
static const CKwSpelling ckw_spellings[] = {
    {"typedef", CK_TYPEDEF}, {"extern", CK_EXTERN}, {"static", CK_STATIC},
    {"auto", CK_AUTO}, {"register", CK_REGISTER},
    {"_Thread_local", CK_THREAD_LOCAL}, {"__thread", CK_THREAD_LOCAL},
    {"const", CK_CONST}, {"__const", CK_CONST}, {"__const__", CK_CONST},
    {"volatile", CK_VOLATILE}, {"__volatile", CK_VOLATILE},
    {"__volatile__", CK_VOLATILE}, {"restrict", CK_RESTRICT},
    {"__restrict", CK_RESTRICT}, {"__restrict__", CK_RESTRICT},
    {"_Atomic", CK_ATOMIC},
    {"inline", CK_INLINE}, {"__inline", CK_INLINE}, {"__inline__", CK_INLINE},
    {"_Noreturn", CK_NORETURN},
    {"void", CK_VOID}, {"char", CK_CHAR}, {"short", CK_SHORT},
    {"int", CK_INT}, {"long", CK_LONG}, {"float", CK_FLOAT},
    {"double", CK_DOUBLE}, {"signed", CK_SIGNED}, {"__signed", CK_SIGNED},
    {"__signed__", CK_SIGNED}, {"unsigned", CK_UNSIGNED}, {"_Bool", CK_BOOL},
    {"_Complex", CK_COMPLEX}, {"__complex", CK_COMPLEX},
    {"__complex__", CK_COMPLEX}, {"_Imaginary", CK_IMAGINARY},
    {"__int128", CK_INT128}, {"__int128__", CK_INT128},
    {"_Float16", CK_FLOATN}, {"_Float32", CK_FLOATN}, {"_Float64", CK_FLOATN},
    {"_Float128", CK_FLOATN}, {"_Float32x", CK_FLOATN},
    {"_Float64x", CK_FLOATN}, {"_Float128x", CK_FLOATN},
    {"__float128", CK_FLOATN}, {"__float80", CK_FLOATN},
    {"__ibm128", CK_FLOATN}, {"__bf16", CK_FLOATN},
    {"_Fract", CK_FIXED | CKW_GNU_ONLY}, {"_Accum", CK_FIXED | CKW_GNU_ONLY},
    {"_Sat", CK_SAT | CKW_GNU_ONLY},
    {"_Decimal32", CK_DECIMAL}, {"_Decimal64", CK_DECIMAL},
    {"_Decimal128", CK_DECIMAL}, {"__auto_type", CK_AUTO_TYPE},
    {"struct", CK_STRUCT}, {"union", CK_UNION}, {"enum", CK_ENUM},
    {"if", CK_IF}, {"else", CK_ELSE}, {"switch", CK_SWITCH},
    {"case", CK_CASE}, {"default", CK_DEFAULT}, {"while", CK_WHILE},
    {"do", CK_DO}, {"for", CK_FOR}, {"goto", CK_GOTO},
    {"continue", CK_CONTINUE}, {"break", CK_BREAK}, {"return", CK_RETURN},
    {"sizeof", CK_SIZEOF}, {"_Alignof", CK_ALIGNOF}, {"__alignof", CK_ALIGNOF},
    {"__alignof__", CK_ALIGNOF}, {"_Generic", CK_GENERIC},
    {"_Static_assert", CK_STATIC_ASSERT}, {"_Alignas", CK_ALIGNAS},
    {"__attribute", CK_ATTRIBUTE}, {"__attribute__", CK_ATTRIBUTE},
    {"__asm", CK_ASM}, {"__asm__", CK_ASM}, {"asm", CK_ASM | CKW_GNU_ONLY},
    {"__typeof", CK_TYPEOF}, {"__typeof__", CK_TYPEOF},
    {"__GIMPLE", CK_GIMPLE},
    {"typeof", CK_TYPEOF | CKW_GNU_ONLY}, {"__extension__", CK_EXTENSION},
    {"__label__", CK_LABEL}, {"__real", CK_REAL}, {"__real__", CK_REAL},
    {"__imag", CK_IMAG}, {"__imag__", CK_IMAG},
    {"__builtin_va_arg", CK_VA_ARG}, {"__builtin_offsetof", CK_OFFSETOF},
    {"__builtin_types_compatible_p", CK_TYPES_COMPATIBLE},
    {"__builtin_convertvector", CK_CONVERTVECTOR},
    {"__builtin_has_attribute", CK_HAS_ATTRIBUTE},
    {0, 0}};

#endif
