// trap: already freed
#include <stdlib.h>
int main(void) { int *p = malloc(sizeof *p); free(p); free(p); return 0; }
