/* integer-valued members of the family table (host pinned by versions.flags:
 * linux, gcc 13.2, LP64) */
#if __GNUC__ * 100 + __GNUC_MINOR__ >= 403
int a;                  /* two digits: clang is 4.2, gcc is open */
#endif
#if (__GNUC__ << 16) + __GNUC_MINOR__ >= (4 << 16) + 3
int b;                  /* the same with another radix */
#endif
#if __GNUC__ * 100 >= 302
int c;                  /* a scaled macro: __GNUC__ >= 4 */
#endif
#if (__INT_MAX__ >> 15) >> 15 >= 1
int d;                  /* a limit every compiler agrees on */
#endif
#if __LONG_MAX__ > 2147483647L
int e;                  /* decided by the data model */
#endif
#if defined(_WIN64) && __SIZEOF_POINTER__ == 4
int f;                  /* LLP64 has 8-byte pointers: never */
#endif
#if __clang_major__ * 100 + __clang_minor__ >= 304
int g;
#endif
