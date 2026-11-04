// flags: -Wall
/* a va_list printf: the format is checked, its arguments are not */
int vprintf(const char *, __builtin_va_list);
int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);

void f(char *buf, __builtin_va_list ap)
{
    vprintf("%Q\n", ap);
    vsnprintf(buf, 8, "%d %Q", ap);
}
