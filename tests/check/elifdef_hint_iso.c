// flags: -std=c11
#if 0
#elifdef X
#elifndef Y
#endif
#elifdef Z
#elifdefx
#elifnde
#else
#endif
int x;
