/* target.c - target presets.  Numbers are gcc's (probed with -m64, -m32,
 * aarch64-linux-gnu and x86_64-w64-mingw32 cross compilers).  align[] is
 * __alignof__ (the preferred alignment); member_align[] what a record
 * member of the type gets, which differs on i386. */
#include "target.h"

#include <string.h>

/* Common to all presets. */
#define SMALL_TYPES                                                        \
    [TY_VOID] = 1, [TY_BOOL] = 1, [TY_CHAR] = 1, [TY_SCHAR] = 1,           \
    [TY_UCHAR] = 1, [TY_SHORT] = 2, [TY_USHORT] = 2, [TY_INT] = 4,         \
    [TY_UINT] = 4, [TY_FLOAT16] = 2, [TY_BF16] = 2, [TY_FLOAT] = 4,        \
    [TY_FLOAT32] = 4, [TY_DEC32] = 4

const Target target_x86_64 = {
    .name = "x86_64-linux-gnu",
    .size = { SMALL_TYPES, [TY_LONG] = 8, [TY_ULONG] = 8, [TY_LLONG] = 8,
              [TY_ULLONG] = 8, [TY_INT128] = 16, [TY_UINT128] = 16,
              [TY_DOUBLE] = 8, [TY_LDOUBLE] = 16, [TY_FLOAT64] = 8,
              [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8, [TY_FLOAT64X] = 16,
              [TY_DEC64] = 8, [TY_DEC128] = 16 },
    .align = { SMALL_TYPES, [TY_LONG] = 8, [TY_ULONG] = 8, [TY_LLONG] = 8,
               [TY_ULLONG] = 8, [TY_INT128] = 16, [TY_UINT128] = 16,
               [TY_DOUBLE] = 8, [TY_LDOUBLE] = 16, [TY_FLOAT64] = 8,
               [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8, [TY_FLOAT64X] = 16,
               [TY_DEC64] = 8, [TY_DEC128] = 16 },
    .member_align = { SMALL_TYPES, [TY_LONG] = 8, [TY_ULONG] = 8,
               [TY_LLONG] = 8, [TY_ULLONG] = 8, [TY_INT128] = 16,
               [TY_UINT128] = 16, [TY_DOUBLE] = 8, [TY_LDOUBLE] = 16,
               [TY_FLOAT64] = 8, [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8,
               [TY_FLOAT64X] = 16, [TY_DEC64] = 8, [TY_DEC128] = 16 },
    .ptr_size = 8, .ptr_align = 8, .max_align = 16, .default_aligned = 16,
    .char_signed = true, .long_double = LD_X87,
    .size_type = TY_ULONG, .ptrdiff_type = TY_LONG, .wchar_type = TY_INT,
    .wint_type = TY_UINT, .intmax_type = TY_LONG, .uintmax_type = TY_ULONG,
    .char16_type = TY_USHORT, .char32_type = TY_UINT,
    .va_list = VA_X86_64,
};

/* i386: no __int128; long long and double are 8-aligned alone but
 * 4-aligned in records; long double is 12 bytes, 4-aligned. */
const Target target_i386 = {
    .name = "i386-linux-gnu",
    .size = { SMALL_TYPES, [TY_LONG] = 4, [TY_ULONG] = 4, [TY_LLONG] = 8,
              [TY_ULLONG] = 8, [TY_DOUBLE] = 8, [TY_LDOUBLE] = 12,
              [TY_FLOAT64] = 8, [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8,
              [TY_FLOAT64X] = 12, [TY_DEC64] = 8, [TY_DEC128] = 16 },
    .align = { SMALL_TYPES, [TY_LONG] = 4, [TY_ULONG] = 4, [TY_LLONG] = 8,
               [TY_ULLONG] = 8, [TY_DOUBLE] = 8, [TY_LDOUBLE] = 4,
               [TY_FLOAT64] = 8, [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8,
               [TY_FLOAT64X] = 4, [TY_DEC64] = 8, [TY_DEC128] = 16 },
    .member_align = { SMALL_TYPES, [TY_LONG] = 4, [TY_ULONG] = 4,
               [TY_LLONG] = 4, [TY_ULLONG] = 4, [TY_DOUBLE] = 4,
               [TY_LDOUBLE] = 4, [TY_FLOAT64] = 4, [TY_FLOAT128] = 16,
               [TY_FLOAT32X] = 4, [TY_FLOAT64X] = 4, [TY_DEC64] = 8,
               [TY_DEC128] = 16 },
    .ptr_size = 4, .ptr_align = 4, .max_align = 16, .default_aligned = 16,
    .char_signed = true, .long_double = LD_X87,
    .size_type = TY_UINT, .ptrdiff_type = TY_INT, .wchar_type = TY_LONG,
    .wint_type = TY_UINT, .intmax_type = TY_LLONG,
    .uintmax_type = TY_ULLONG, .char16_type = TY_USHORT,
    .char32_type = TY_UINT, .va_list = VA_CHAR_PTR,
};

#define AARCH64_TYPES                                                      \
    SMALL_TYPES, [TY_LONG] = 8, [TY_ULONG] = 8, [TY_LLONG] = 8,            \
    [TY_ULLONG] = 8, [TY_INT128] = 16, [TY_UINT128] = 16,                  \
    [TY_DOUBLE] = 8, [TY_LDOUBLE] = 16, [TY_FLOAT64] = 8,                  \
    [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8, [TY_FLOAT64X] = 16,             \
    [TY_DEC64] = 8, [TY_DEC128] = 16

const Target target_aarch64 = {
    .name = "aarch64-linux-gnu",
    .size = { AARCH64_TYPES },
    .align = { AARCH64_TYPES },
    .member_align = { AARCH64_TYPES },
    .ptr_size = 8, .ptr_align = 8, .max_align = 16, .default_aligned = 16,
    .char_signed = false, .long_double = LD_IEEE128,
    .size_type = TY_ULONG, .ptrdiff_type = TY_LONG, .wchar_type = TY_UINT,
    .wint_type = TY_UINT, .intmax_type = TY_LONG, .uintmax_type = TY_ULONG,
    .char16_type = TY_USHORT, .char32_type = TY_UINT,
    .va_list = VA_AARCH64, .unnamed_field_affects_align = true,
};

#define WIN64_TYPES                                                        \
    SMALL_TYPES, [TY_LONG] = 4, [TY_ULONG] = 4, [TY_LLONG] = 8,            \
    [TY_ULLONG] = 8, [TY_INT128] = 16, [TY_UINT128] = 16,                  \
    [TY_DOUBLE] = 8, [TY_LDOUBLE] = 16, [TY_FLOAT64] = 8,                  \
    [TY_FLOAT128] = 16, [TY_FLOAT32X] = 8, [TY_FLOAT64X] = 16,             \
    [TY_DEC64] = 8, [TY_DEC128] = 16

const Target target_win64 = {
    .name = "x86_64-w64-mingw32",
    .size = { WIN64_TYPES },
    .align = { WIN64_TYPES },
    .member_align = { WIN64_TYPES },
    .ptr_size = 8, .ptr_align = 8, .max_align = 16, .default_aligned = 16,
    .char_signed = true, .long_double = LD_X87,
    .size_type = TY_ULLONG, .ptrdiff_type = TY_LLONG,
    .wchar_type = TY_USHORT, .wint_type = TY_USHORT,
    .intmax_type = TY_LLONG, .uintmax_type = TY_ULLONG,
    .char16_type = TY_USHORT, .char32_type = TY_UINT,
    .va_list = VA_CHAR_PTR, .ms_bitfields = true,
};

const char *const target_names =
    "x86_64 (default), i386, aarch64, win64 (or their triples)";

static const struct { const char *name; const Target *t; } aliases[] = {
    {"x86_64", &target_x86_64}, {"x86-64", &target_x86_64},
    {"amd64", &target_x86_64}, {"x86_64-linux-gnu", &target_x86_64},
    {"x86_64-pc-linux-gnu", &target_x86_64},
    {"i386", &target_i386}, {"i686", &target_i386}, {"x86", &target_i386},
    {"i386-linux-gnu", &target_i386}, {"i686-linux-gnu", &target_i386},
    {"i686-pc-linux-gnu", &target_i386},
    {"aarch64", &target_aarch64}, {"arm64", &target_aarch64},
    {"aarch64-linux-gnu", &target_aarch64},
    {"win64", &target_win64}, {"mingw", &target_win64},
    {"mingw64", &target_win64}, {"x86_64-w64-mingw32", &target_win64},
};

const Target *target_find(const char *name)
{
    for (size_t i = 0; i < ARRAY_LEN(aliases); i++)
        if (!strcmp(aliases[i].name, name))
            return aliases[i].t;
    return NULL;
}
