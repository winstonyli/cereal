// Static initializers gcc folds to constants.
char *p;
char c = "abc"[1];
long d = (p + 4) - (p + 1);
int e = 1 ? 2 : 3;
#ifndef __TIMESTAMP__
#error no timestamp
#endif
char c1 = 1["bar"];
char c2 = "str"[4];
char c3 = "str"[-1];
