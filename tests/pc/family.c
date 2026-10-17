/* the family table: OS and compiler leaves (host pinned by family.flags:
 * linux, gcc) */
#if defined(_MSC_VER) && defined(__linux__)
int a;                  /* satisfiable: some other compiler defines _MSC_VER */
#endif
#ifdef _WIN32
int b;                  /* a target change */
#endif
#if defined(__linux__) && defined(__GNUC__)
int c;
#endif
#if defined(_MSC_VER) && defined(USE_X)
int d;                  /* target + config */
#endif
#if defined(__clang__) && defined(__unix__)
int e;
#endif
#ifdef USE_Y
int f;                  /* config only */
#endif
