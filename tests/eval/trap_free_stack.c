// trap: malloc did not return
#include <stdlib.h>
int main(void) { int x = 0; free(&x); return x; }
