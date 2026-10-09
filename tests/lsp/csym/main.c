#include "api.h"
int counter;
int counter = 1;
static int shadow = 1;
struct node;
struct node { struct node *next; int v; };
#define width 3
#undef width
static int width = 4;
#define twice(v) ((v) * 2)
int area(point_t p)
{
    return p.x * p.y + width;
}
int walk(struct node *n)
{
    return n ? n->v : shadow;
}
int main(void)
{
    struct point pt = { .x = 1, .y = 2 };
    enum color c = GREEN;
    int shadow = 2;
    int total = area(pt) + counter + shadow + c;
    if (total > LIMIT)
        goto done;
    total = twice(total);
done:
    return total + walk(0);
}
