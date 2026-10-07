// flags: -Wall -O2
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *);
int printf(const char *, ...);
long strfmon(char *, size_t, const char *, ...);
const char a[] = { 97, 98, 99, 100 };
const char b[] = { 97, 0, 99, 0, 101 };
const char c[4] = { 97, 98 };
const char d[7] = "abc\0def";
char buf[8];
size_t f(void) { return strlen(a) + strlen(b) + strlen(c); }
void g(void) { printf(a); printf(b); printf(c); printf(d); __builtin_strfmon(buf, 8, a); }
