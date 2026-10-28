/* computed goto: a jump table, an address in a variable, a conditional address, and the shape of Lua's luaV_execute
 * (opcodes in a byte array, every handler ends in its own dispatch) */
static int disp(int n)
{
    static const void *const tab[] = { &&a, &&b, &&c };
    int r = 0;
    goto *tab[n];
a:  r += 1;
b:  r += 2;
c:  return r;
}
static int loop(int n)
{
    void *p = &&top;
    int s = 0;
top:
    if (n-- > 0) { s++; goto *p; }
    return s;
}
static int pick(int i)
{
    goto *(i ? &&x : &&y);
x:  return 10;
y:  return 20;
}
static int vm(const unsigned char *pc)
{
    static const void *const jt[] = { &&op_halt, &&op_inc, &&op_dbl, &&op_jnz };
    int acc = 1, n = 0;
#define NEXT() goto *jt[*pc++ & 3]
    NEXT();
op_inc:  acc += 3; NEXT();
op_dbl:  acc *= 2; NEXT();
op_jnz:  if (++n < 4) pc -= 3; NEXT();
op_halt: return acc * 100 + n;
#undef NEXT
}
int main(void)
{
    static const unsigned char prog[] = { 1, 2, 1, 3, 0 };
    return disp(0) * 1000 + disp(1) * 100 + disp(2) * 10 + loop(5) + pick(1) + pick(0) + vm(prog);
}
