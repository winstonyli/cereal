/* the cheapest ways to turn a group on: one change per macro */
#ifdef USE_A
int a;
#endif
#if defined(USE_A) && defined(USE_B)
int ab;
#endif
#if LEVEL >= 3 && LEVEL < 6
int level;
#endif
#if defined(USE_A) || defined(USE_B) || defined(USE_C)
int any;
#endif
#if FOO(1) && defined(USE_B)
int opaque;
#endif
#if 0
int never;
#endif
#define HOSTV 4
#if HOSTV > 4 || (defined(USE_C) && HOSTV == 4)
int host;
#endif
