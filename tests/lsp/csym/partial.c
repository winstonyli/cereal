#include "missing.h"
static int later = 1;
int use(void)
{
    return later + undeclared_here;
}
