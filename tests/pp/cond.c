#define A 1
#if A && (defined B || !defined(C)) && 0x10 == 16 && 'a' == 97 && -1 < 0
yes1
#endif
#if -1 > 0u
unsigned_wrap
#endif
#if (2 || 1/0) && (0 && 1/0) == 0
short_circuit
#endif
#if 1 ? 2 : (1/0)
ternary
#endif
#ifdef A
# ifndef A
no
# elif A == 1
yes2
# else
no
# endif
#elif 1/0
#endif
#if 0
#if garbage (((
#error not reached
#endif
'unterminated in skipped block
#else
else_taken
#endif
#define ZERO 0
#if ZERO
#elif defined ZERO
elif_taken
#endif
#if (-9223372036854775807 - 1) / 2 < 0 && 18446744073709551615u == -1
bigints
#endif
#if 10 >> 1 == 5 && -16 >> 2 == -4 && (1 << 62) > 0 && ~0u == 0xffffffffffffffff
shifts
#endif
#if '\377' < 0 && L'\377' > 0 && '\n' == 10 && 'ab' == 24930
chars
#endif
