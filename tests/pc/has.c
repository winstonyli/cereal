/* feature tests are HAS atoms: exact, never flipped by a query, nonzero when
 * the dated ones are present (host pinned by has.flags: linux, gcc) */
#if defined(__has_attribute) && __has_attribute(noreturn)
int a;
#endif
#if defined(__has_cpp_attribute) && __has_cpp_attribute(fallthrough)
int b;                  /* a year when present, never zero */
#endif
#if defined(_MSC_VER) && defined(__has_feature)
int c;                  /* MSVC has no __has_feature: needs another compiler */
#endif
#if defined(__clang__) && __has_feature(address_sanitizer) && defined(USE_X)
int d;                  /* target + environment + config */
#endif
#if __has_include(<stdio.h>) && !__has_include(<stdio.h>)
int e;
#endif
