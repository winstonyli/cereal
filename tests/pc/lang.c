/* the language field: the standard and its mode (host pinned by lang.flags:
 * gcc, linux, gnu17) */
#if __STDC_VERSION__ >= 201112L
int a;                  /* holds */
#endif
#if __STDC_VERSION__ >= 202311L
int b;                  /* language: std = c23 */
#endif
#ifdef __STRICT_ANSI__
int c;                  /* language: mode = iso */
#endif
#if !defined(__STDC_VERSION__)
int d;                  /* language: std = c89 */
#endif
#if defined(__STRICT_ANSI__) && __STDC_VERSION__ < 201112L
int e;                  /* two language changes */
#endif
#if defined(_MSC_VER) && __STDC_VERSION__ >= 202311L
int f;                  /* target and language */
#endif
#if __STDC_VERSION__ >= 199901L && defined(USE_X)
int g;                  /* config only */
#endif
