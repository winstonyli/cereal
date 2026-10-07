// flags: -Wall -O2
typedef __SIZE_TYPE__ size_t;
size_t strlen(const char *);
size_t strspn(const char *, const char *);
size_t strcspn(const char *, const char *);
const char arr[7] = "abc\0def";
const char unt[3] = "abc";
#define unterm (arr + __builtin_strlen(arr) + 1)
size_t b1(void) { return strlen(unterm); }
size_t b2(const char *s) { return strspn(unterm, s); }
size_t b3(const char *s) { return strspn(s, unt); }
size_t b4(const char *s) { return strcspn(unt, s); }
size_t b5(const char *s) { return strcspn(s, unt); }
size_t b6(void) { return strlen(arr + 4); }
