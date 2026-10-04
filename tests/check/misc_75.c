// flags: -Wnonnull
int f(int a, int b)
{
    int x = 0;
    x += __builtin_add_overflow(a, b, (int *)0);
    x += __builtin_sub_overflow(0, 0, (char *)0);
    x += __builtin_mul_overflow(a, b, &x);
    x += __builtin_smul_overflow(a, b, (int *)0);
    return x;
}
