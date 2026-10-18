/* variadic and unprototyped calls, builtins (T3a) */
typedef unsigned long size_t;
int printf(const char *, ...);
int unproto();
int (*vfp)(const char *, ...);
struct S { int a; };
struct B { unsigned x : 3; unsigned y : 32; };
enum E { E0, E1 };

int fixed_only(const char *s) { return printf(s); }
int small(char c, short h, _Bool b, unsigned char uc) { return printf("%d", c, h, b, uc); }
int ptrs(const char *s, int *p) { return printf("%s %p", s, p); }
int strct(struct S s) { return printf("%d", s); }
int bits(struct B *b) { return printf("%u %u", b->x, b->y); }
int en(enum E e) { return printf("%d", e); }
int up(int i, char c) { return unproto(i, c, &i); }
int ind(int i) { return vfp("x", i, (char)i); }
int exp1(long l) { return __builtin_expect(l != 0, 0) ? 1 : 2; }
void *ms(void *p, int n) { return __builtin_memset(p, 0, n); }
