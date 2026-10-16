/* function-like macros: the body is instantiated at each call */
#ifdef __has_builtin
# define HAS_BUILTIN(x) __has_builtin(x)
#else
# define HAS_BUILTIN(x) 0
#endif
#define GE(v, n) ((v) >= (n))
#define TWICE(x) x * 2
#define NONE() 0
#if HAS_BUILTIN(__builtin_clz)
int a;
#endif
#if GE(LEVEL, 3) && defined(B)
int b;
#endif
#if TWICE(1 + 2) == 5
int c;           /* textual: 1 + 2 * 2 */
#endif
#if NONE()
int d;
#endif
#if GE(1)
int e;           /* wrong argument count: opaque */
#endif
#ifndef GUARDED
# define GUARDED(x) (x)
#endif
#if GUARDED(1)
int f;
#endif
