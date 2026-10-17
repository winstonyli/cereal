/* values are guarded cases: one mechanism for macros with several
 * definitions, comparisons of two macros, arithmetic and ?: */
#define T_INT 1
#define T_LONG 2
#ifndef TYPE
# define TYPE T_LONG
#endif
#if TYPE == T_INT
int a;
#elif TYPE == T_LONG
int b;
#else
int c;
#endif
#ifdef WIDE
# define W 64
#else
# define W 32
#endif
#if W * 2 > 100
int d;
#endif
#if (W == 64 ? 8 : 4) == 8
int e;
#endif
#if EXT + 1 >= 5 && EXT - 1 < 5
int f;
#endif
#if EXT == OTHER
int g;
#endif
#if EXT * OTHER > 3
int h;
#endif
