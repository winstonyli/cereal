// flags: -Wexpansion-to-defined
#define Z
#define bad0 defined Z
#define bad1 defined
#define bad2 defined (Z
#define ok defined(Z)
#if !bad0
#endif
#if !bad1 Z
#endif
#if !bad1 (Z)
#endif
#if !bad2)
#endif
#if defined Z && defined(Z)
#endif
#undef defined
#define defined 1
#assert a(b)
#include_next <stddef.h>
#endif x
