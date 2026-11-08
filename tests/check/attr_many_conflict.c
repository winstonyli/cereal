// flags: -Wall
/* gcc keeps no cap on the attributes of a declaration: the conflict shows after 25 others */
int f(void) __attribute__((unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, pure, const));
int g(void) __attribute__((pure, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, unused, const));
