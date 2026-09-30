/* Valid C99: no diagnostics.  A `// flags: ...` line adds flags. */
typedef unsigned long size_t;
struct point { int x, y; };
union num { int i; double d; };
enum color { RED, GREEN = 4, BLUE };
static const int table[3] = { 1, 2, 3 };
extern struct point origin;
int area(const struct point *a, const struct point *b);

int area(const struct point *a, const struct point *b)
{
    int w = b->x - a->x, h = b->y - a->y;
    return w * h + table[BLUE - GREEN];
}
