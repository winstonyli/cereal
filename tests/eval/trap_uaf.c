// trap: after its lifetime
#include <stdlib.h>
int main(void) { int *p = malloc(sizeof *p); *p = 1; free(p); return *p; }
