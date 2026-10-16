#define BASE 4
#define LEVEL (BASE * 2)
#define IS_NEW (BASE >= 3)
#define ALIAS BASE
#define K (1 << 4)
#if IS_NEW
#endif
#if ALIAS == 4
#endif
#if ALIAS == 5
#endif
#if LEVEL > 7
#endif
#if K == 16
#endif
#ifdef LATER
#define BASE 1
#endif
#if ALIAS == 4
#endif
