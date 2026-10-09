#include "csym.h"
#define width 3
#undef width
#define LIMIT 10
#define DECLARE(n) int n = LIMIT
#define TMP(v) ({ int _t = (v); _t * 2; })
static int width = 4;
int total;
int total = 1;
struct s { int x; };
int get(struct s *p)
{
    DECLARE(local);
    int a = TMP(p->x) + TMP(local);
    return a + width + total + LIMIT;
}
