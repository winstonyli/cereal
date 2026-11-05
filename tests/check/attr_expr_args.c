// flags: -std=gnu99 -Wall
/* Attributes whose arguments are expressions look their arguments up like any
 * expression; cleanup's is a function name (a use for the symbol index). */
void cl(int *p);
void h(void)
{
    int x __attribute__((cleanup(undeclared)));
    int y __attribute__((__cleanup__(cl)));
}
void f1(const char *) __attribute__((__nonnull__(undeclared)));
const char *f2(const char *) __attribute__((format_arg(undeclared)));
int z __attribute__((aligned(undeclared)));
int w __attribute__((__vector_size__(undeclared)));
int v __attribute__((section(undeclared)));
