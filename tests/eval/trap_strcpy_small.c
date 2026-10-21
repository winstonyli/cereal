// trap: out of bounds
#include <string.h>
int main(void) { char d[3]; strcpy(d, "abc"); return d[0]; }
