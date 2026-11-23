/* A spec product with a 64-bit operand is exact Int, not a wrapping 64-bit value: n * n >= 1 holds for every n >= 1, even where n * n would wrap. */
int area(int *a, unsigned long n)
{
#pragma cereal requires 1 <= n && n * n <= $len(a)
    a[0] = 0;
    return 0;
}
