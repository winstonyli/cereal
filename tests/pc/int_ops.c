/* integer arithmetic and shifts in #if; diff_pp compares with gcc -E */
#if 8 << -1 == 4
a1
#endif
#if (8 >> -2) == 32
a2
#endif
#if -1 >> 100 == -1
a3
#endif
#if (1 << 63) < 0
a4
#endif
#if (1u << 63) > 0
a5
#endif
#if (-8 << 1) == -16
a6
#endif
#if (1 << 64) == 0
a7
#endif
#if (1 << 70) == 0
a8
#endif
#if ((-9223372036854775807 - 1) % -1) == 0
a9
#endif
#if (0u - 1) == 18446744073709551615u
a10
#endif
#if (-7 / 2) == -3 && (-7 % 2) == -1 && (7u / 2) == 3
a11
#endif
#if (6 & 3) == 2 && (6 | 3) == 7 && (6 ^ 3) == 5
a12
#endif
#if (-1 >> 1) == -1 && (-2 >> 1) == -1
a13
#endif
#if (3000000000 * 3) / 3 == 3000000000
a14
#endif
#if ~0 == -1
a15
#endif
#if ~0u == 18446744073709551615u
a16
#endif
#if -1u == 18446744073709551615u
a17
#endif
#if -(-5) == 5 && -(5u) == 18446744073709551611u
a18
#endif
#if ~(-1) == 0 && ~1 == -2
a19
#endif
