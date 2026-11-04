// flags: -std=gnu99 -Wall
/* nonnull of library built-ins, read from the builtin table: a gnu-only name
 * declared by the user, a standard one, and the __builtin_ spelling */
char *index(const char *, int);
char *strdup(const char *);
unsigned long strlen(const char *);
char *sink;
unsigned long n;

void f(void)
{
    sink = index(0, 'a');
    sink = strdup(0);
    n = strlen(0);
    n = __builtin_strlen(0);
}
