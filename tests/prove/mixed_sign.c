/* A signed index against an unsigned length: the bound is proved only if i is non-negative. */
int get(int *a, int i, unsigned long n)
{
#pragma cereal requires n <= $len(a)
    return a[i];
}
