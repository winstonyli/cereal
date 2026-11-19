/* A signed char index with 0 <= i: the sign extension makes the bound provable. */
char get(char *a, signed char i)
{
#pragma cereal requires 128 <= $len(a) && 0 <= i
    return a[i];
}
