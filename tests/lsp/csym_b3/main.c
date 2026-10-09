#include "shapes.h"
#define LIMIT 8
#define SQUARE(x) ((x) * (x))
typedef struct point { int x, y; } point_t;
typedef point_t *point_p;
enum color { RED, GREEN };
static const int origin[2] = {0, 0};
struct handler { int (*on_event)(int code, const char *msg); };
int log_msg(const char *fmt, ...);
static int count_shapes(const struct shape *s, int n)
{
    point_t p = {1, 2};
    point_p pp = &p;
    enum color c = GREEN;
    struct handler h = {0};
    int total = SQUARE(n) + pp->x + c + origin[0];
    total += h.on_event(total, "x");
    total += area(s, LIMIT);
    log_msg("%d", total);
    return total;
}
int main(void) { return count_shapes(0, 0); }
enum { OFF, ON };
typedef struct { int lo, hi; } span_t;
