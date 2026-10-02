// flags: -std=c99 -pedantic -Wformat-security
#include <stdarg.h>
int mylog(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int myscan(const char *s, const char *fmt, ...) __attribute__((__format__(__scanf__, 2, 3)));
int vlog(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));
struct S { int x; };
static inline __attribute__((format(printf, 1, 2))) void lg(const char *f, ...) { (void)f; }
int mylog(int lvl, const char *fmt, ...);

void t(const char *u, va_list ap)
{
    long l = 1;
    char c[4];
    int i;
    mylog(1, "%d\n", l);
    mylog(1, "%s", 3);
    mylog(1, "%d %d", 1);
    mylog(1, "hi", 1);
    mylog(1, u);
    myscan("x", "%d", &l);
    myscan("x", "%s %d", c, &i);
    myscan("x", "%d", i);
    vlog("%d", ap);
    vlog("%q", ap);
    lg("%s", 1);
    lg(u);
}
