// trap: out of bounds
#include <stdlib.h>
int main(void) { int *p = malloc(4 * sizeof *p); p[4] = 1; return 0; }
