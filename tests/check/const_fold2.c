// Static initializers gcc folds to constants.
char *p;
char c = "abc"[1];
long d = (p + 4) - (p + 1);
int e = 1 ? 2 : 3;
#ifndef __TIMESTAMP__
#error no timestamp
#endif
