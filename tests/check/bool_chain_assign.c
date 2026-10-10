// flags: -Wparentheses
#include <stdbool.h>
struct S { bool a : 1; bool b; int x : 3; };
void f(bool *r, bool *m, bool b, bool c, int i, int j, char *p, char *q, struct S *s, int k)
{
    *r = *m = false;            /* both _Bool: no conversion, no warning */
    b = c = true;
    c = b = i;
    c = b = c = i;
    s->b = s->a = s->b;
    b = s->a = k;
    s->a = b = k;
    b = i = j;                  /* int converted to _Bool: warns */
    b = p = q;
    b = s->x = k;
    b = (i = j);                /* parenthesised: no warning */
    i = b = j;
}
