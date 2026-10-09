// flags: -std=gnu11
#define A
#if 0
#elifdef A  /* taken: warns at the end of the line */
#elifndef A
#endif
#if 1
#elifdef A
#endif
#elifdef A
#if 0
#else
#elifndef A
#endif
