// flags: -Wall
#include <string.h>
struct Ax { char n, a[]; };
const struct Ax ax = { 3, { 3, 2, 1, 0 } };
unsigned long l1;
void f(void) { l1 = strlen(ax.a); }
