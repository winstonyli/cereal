#include "shapes.h"
#define SCALE 2
#define area_of(s) ((s).w * (s).h * SCALE)
#define limit 5
#undef limit
static const int limit = 10;
extern volatile unsigned long ticks;
typedef int (*cmp_fn)(const void *, const void *);
static int count;
int measure(const struct rect *r, int pad, ...)
{
    const char *const label_text = "x";
    int count = r->w + pad + r->kind;
    union cell u = { .i = 1 };
    struct big b = { 0 };
    enum mode m = MODE_FAST;
    if (count > limit)
        goto out;
    count += area_of(*r) + u.i + b.f17 + m + (int)ticks;
out:
    return count + (label_text != 0);
}
int total(void) { return count; }
