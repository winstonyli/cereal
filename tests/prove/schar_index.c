/* A signed char index: negative means out of bounds, so the bound is not proved without 0 <= i. */
char get(char *a, signed char i)
{
#pragma cereal requires 128 <= $len(a)
    return a[i];
}
