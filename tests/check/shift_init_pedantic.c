// flags: -std=c99 -pedantic
int b = 2147483647 << 2;
int e = 1 << 40;
int e2 = 1 << 32;
int e3 = 1 << -1;
int e4 = 3 << 40;
