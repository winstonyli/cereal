// trap: overlapping
#include <string.h>
int main(void) { char s[8] = "abcdefg"; memcpy(s + 1, s, 4); return s[0]; }
