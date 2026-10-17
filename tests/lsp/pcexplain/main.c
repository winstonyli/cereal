#include "cfg.h"
#if defined(USE_FAST) && VERSION >= 3
int fast;
#elif defined(__clang__)
int clangy;
#else
int plain;
#endif
#ifdef __linux__
int lin;
#endif
#ifdef _WIN32
int win;
#endif
#if __STDC_VERSION__ >= 202311L
int c23;
#endif
#if __has_attribute(noreturn)
int attr;
#endif
