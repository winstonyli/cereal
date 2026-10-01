// flags: -std=gnu99
void t(void)
{
    char *p = strdup("a");
    p = index(p, 1);
    stpcpy(p, p);
    strnlen(p, 1);
    ffs(1);
    __builtin_strndup(p, 1);
}
char *strndup(int);
int rindex(void);
