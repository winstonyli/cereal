/* A struct copy that carries uninitialised bytes into the call's state (the pattern behind Lua's boolF and nilK: TValue.value_).  The evaluator holds
 * zero in uninitialised memory; the gcc driver is built with -ftrivial-auto-var-init=zero (tests/eval_sweep.sh) so its stack is zero too.
 * lu_set calls lu_dirty first, so lu_fill's frame lies over bytes that are not zero. */
struct LU { long a; long b; };

static void lu_dirty(void)
{
    volatile long x[32];
    int i;
    for (i = 0; i < 32; i++)
        x[i] = 0x5a5a5a5a5a5a5a5aL;
}

static void lu_fill(struct LU *s)
{
    struct LU t;
    t.a = 1;
    *s = t;
}

void lu_set(struct LU *s)
{
    lu_dirty();
    lu_fill(s);
}
