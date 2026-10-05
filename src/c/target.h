/* target.h - the target ABI facts the type checker needs: sizes and
 * alignments of the builtin types, which integer types the standard
 * typedefs name, how records are laid out.  See docs/TYPES.md.
 *
 * Presets exist for the targets gcc is commonly run for; the default is
 * the host's (x86-64 System V, which the predefined macros also come
 * from). */
#ifndef CEREAL_TARGET_H
#define CEREAL_TARGET_H

#include "common.h"

/* Builtin type kinds: their TypeId index equals the kind (type.h). */
#define BUILTIN_TYPES(X)                                                    \
    X(ERROR, "<type-error>") X(VOID, "void") X(BOOL, "_Bool") X(CHAR, "char")    \
    X(SCHAR, "signed char") X(UCHAR, "unsigned char")                      \
    X(SHORT, "short int") X(USHORT, "short unsigned int")                  \
    X(INT, "int") X(UINT, "unsigned int") X(LONG, "long int")              \
    X(ULONG, "long unsigned int") X(LLONG, "long long int")                \
    X(ULLONG, "long long unsigned int") X(INT128, "__int128")              \
    X(UINT128, "__int128 unsigned")                                        \
    X(FLOAT16, "_Float16") X(BF16, "__bf16") X(FLOAT, "float")              \
    X(DOUBLE, "double") X(LDOUBLE, "long double") X(FLOAT32, "_Float32")    \
    X(FLOAT64, "_Float64") X(FLOAT128, "_Float128")                        \
    X(FLOAT32X, "_Float32x") X(FLOAT64X, "_Float64x")                      \
    X(IBM128, "__ibm128")                                                  \
    X(DEC32, "_Decimal32") X(DEC64, "_Decimal64") X(DEC128, "_Decimal128")

typedef enum {
#define X(n, s) TY_##n,
    BUILTIN_TYPES(X)
#undef X
    TY_NBUILTIN,
    /* derived */
    TY_PTR = TY_NBUILTIN,
    TY_ARRAY,        /* base: element; n: count (TF_INCOMPLETE: []) */
    TY_VLA,          /* base: element */
    TY_FUNC,         /* base: return; extra: params in the pool; n: count */
    TY_STRUCT,       /* extra: record */
    TY_UNION,        /* extra: record */
    TY_ENUM,         /* extra: enum */
    TY_TYPEDEF,      /* base: the type named; extra: the name (ident) */
    TY_VECTOR,       /* base: element; n: size in bytes */
    TY_COMPLEX,      /* base: the real type */
    TY_NKINDS
} TypeKind;

typedef enum {
    LD_X87,          /* 80-bit extended */
    LD_IEEE64,       /* long double is double */
    LD_IEEE128,
    LD_IBM128        /* double-double */
} LongDoubleFormat;

typedef enum {
    VA_CHAR_PTR,     /* char * */
    VA_X86_64,       /* struct __va_list_tag [1] */
    VA_AARCH64       /* struct __va_list */
} VaListKind;

typedef struct Target {
    const char *name;
    uint8_t size[TY_NBUILTIN];      /* bytes; 0: unsupported */
    uint8_t align[TY_NBUILTIN];     /* alignof */
    uint8_t member_align[TY_NBUILTIN]; /* in a record (i386: 4 for 8) */
    uint8_t ptr_size, ptr_align;
    uint8_t max_align;              /* __BIGGEST_ALIGNMENT__ */
    uint8_t default_aligned;        /* __attribute__((aligned)) */
    bool char_signed;
    LongDoubleFormat long_double;
    TypeKind size_type, ptrdiff_type, wchar_type, wint_type;
    TypeKind intmax_type, uintmax_type, char16_type, char32_type;
    VaListKind va_list;
    bool ms_bitfields;              /* MinGW: -mms-bitfields by default */
    /* bit-field quirks (clang's TargetInfo names) */
    bool ignore_nonzero_bitfield_align;
    bool ignore_zero_bitfield_align;
    uint8_t min_zero_bitfield_align; /* bytes; 0: none */
    bool unnamed_field_affects_align;
} Target;

extern const Target target_x86_64;
extern const Target target_i386;
extern const Target target_aarch64;
extern const Target target_win64;

/* A preset by name (x86_64-linux-gnu, i386, aarch64, x86_64-w64-mingw32,
 * and short forms); NULL if unknown. */
const Target *target_find(const char *name);
/* The names target_find knows, for help text. */
extern const char *const target_names;

#endif
