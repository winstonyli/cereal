extern char *d;
extern const char **e;
void f(void)
{
    __builtin_execv("a", &d);
    __builtin_execve("a", &d, &d);
    __builtin_execv("a", e);
}
