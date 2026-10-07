// flags: -Wformat
void a(const char *f, ...) __attribute__((format(__bogus__, 1, 2)));
void b(const char *f, ...) __attribute__((__format__(__strftime__, 1, 0)));
void c(const char *f, ...) __attribute__((format(gnu_printf, 1, 2)));
void d(const char *f, ...) __attribute__((format(gnu_scanf, 1, 2)));
void e(const char *f, ...) __attribute__((format(gnu_strfmon, 1, 2)));
void g(const char *f, ...) __attribute__((format(gnu_gcc_diag, 1, 2)));
void h(const char *f, ...) __attribute__((format(gnu_asm_fprintf, 1, 2)));
void i(const char *f, ...) __attribute__((format(NSString, 1, 2)));
void j(const char *f, ...) __attribute__((format(__NSString__, 1, 2)));
void k(const char *f, ...) __attribute__((format(CFString, 1, 2)));
void l(const char *f, ...) __attribute__((format(x, 1, 2), format(printf, 1, 2)));
void
m1(const char *f, ...)
  __attribute__((format(bogus1, 1, 2)));
int a1, b1,
  m2(const char *f, ...) __attribute__((format(bogus2, 1, 2)));
void p(void) { int q; void m4(const char *f, ...) __attribute__((format(bogus4, 1, 2))); }
