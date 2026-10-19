/* Computed goto for tests/asm_exec.py: label addresses in expressions, in a
 * block-scope static table (a relocation against the function's text), and
 * goto *expr as an interpreter dispatch. */
int interp(int n, int x)
{
    static void *tab[] = {&&op_inc, &&op_dbl, &&op_neg, &&op_end};
    int pc = 0, acc = x;
    int prog[5];
    int i;
    for (i = 0; i < 4; i++)
        prog[i] = (n >> (2 * i)) & 3;
    prog[4] = 3;
    goto *tab[prog[pc]];
op_inc:
    acc += 3;
    pc++;
    goto *tab[prog[pc]];
op_dbl:
    acc *= 2;
    pc++;
    goto *tab[prog[pc]];
op_neg:
    acc = -acc;
    pc++;
    goto *tab[prog[pc]];
op_end:
    return acc + pc;
}

int local(int x)
{
    void *p = x > 0 ? &&pos : &&neg;
    goto *p;
pos:
    return x * 2;
neg:
    return x - 100;
}
