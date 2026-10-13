#include "api.h"
int shared_counter = VERSION;
static int local_state;
int helper(int v) { return v + local_state; }
int run(pt_t *p)
{
    int total = SQ(p->x) + CALL_TWICE(p->y) + declared_only(RED) + twice(GREEN);
    local_state++;
    return total + helper(shared_counter);
}
