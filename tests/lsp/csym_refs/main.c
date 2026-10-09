#include "refs.h"
#define SQUARE(x) ((x) * (x))
#define BUMP() (hits++)
int hits;
extern int hits;
int hits = 0;
struct pair { int key; int val; };
struct entry { int key; struct entry *next; };
typedef struct pair pair_t;
enum mode { OFF, ON };
static int level = 1;
int scale(int factor, pair_t p)
{
    int level = factor * p.key;
    return level + SQUARE(factor) + p.val;
}
int lookup(struct entry *e, int key)
{
    struct pair q = { .key = key, .val = ON };
    int pair = q.val;
    while (e) {
        if (e->key == key)
            goto found;
        e = e->next;
    }
    return BUMP() + hits + level + pair;
found:
    return scale(key, q);
}
