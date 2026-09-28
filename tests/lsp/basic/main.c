#include "defs.h"
#define SQUARE(x) ((x) * (x))
#define TWICE(x) (2 * SQUARE(x))
int a = SQUARE(3);
int b = TWICE(MODE);
#if MODE > 1
int c = LIMIT;
#else
int c = 0;
#endif
#define BAD(x) x * x
int d = BAD(1);
