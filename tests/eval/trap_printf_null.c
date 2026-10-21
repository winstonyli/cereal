// trap: through a null pointer
#include <stdio.h>
int main(void) { printf("%s\n", (char *)0); return 0; }
