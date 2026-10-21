// trap: uninitialized
#include <stdlib.h>
int main(void) { int *p = malloc(sizeof *p); return *p; }
