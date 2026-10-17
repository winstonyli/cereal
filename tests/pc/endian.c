/* the byte order (host pinned by endian.flags: gcc, linux, little-endian) */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
int a;                  /* holds */
#endif
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
int b;                  /* target: endian = big */
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
int c;                  /* target: endian = big */
#endif
#if defined(_WIN32) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
int d;                  /* big-endian needs a non-Windows target */
#endif
#if !defined(__BYTE_ORDER__)
int e;                  /* a compiler without it */
#endif
