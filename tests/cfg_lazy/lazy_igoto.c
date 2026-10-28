/* A computed-goto interpreter over bytes the sweep draws, the shape of Lua's luaV_execute: the opcode comes from a drawn
 * byte, every handler ends in its own dispatch through a table of label addresses, and a run ends at the halt handler or
 * on the step budget.  The sweep must follow the dispatch (every call finishes) and gcc must agree on the results. */
long ig_run(const unsigned char *pc)
{
    static const void *const jt[] = { &&op_halt, &&op_inc, &&op_dbl, &&op_jnz };
    long acc = 1;
    int steps = 0;
#define NEXT() do { if (++steps > 40) goto op_halt; goto *jt[*pc++ & 3]; } while (0)
    NEXT();
op_inc:  acc += 3; NEXT();
op_dbl:  acc *= 2; NEXT();
op_jnz:  if (acc & 1) pc -= 2; NEXT();
op_halt: return acc * 64 + steps;
#undef NEXT
}
