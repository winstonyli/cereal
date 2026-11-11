/* A walk down a chain of self-links (CallInfo.previous in Lua's luaV_execute, `ci = ci->previous`) with no bound but the NULL at its end:
 * the draw gives one NULL per call and half the calls none, so without a rule the chain makes a new blob per step and the call stops at the
 * blob budget.  A cell that continues a chain LCHAIN deep is NULL, so the call finishes unless a share draw closes the chain into a cycle. */
struct CN { struct CN *prev; long v; };

long lc_up(struct CN *c)
{
    long sum = 0;
    while (c) {
        sum += c->v;
        c = c->prev;
    }
    return sum;
}
