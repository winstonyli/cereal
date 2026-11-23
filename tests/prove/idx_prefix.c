/* a loop from 1 reading a[i - 1]: the index is a difference */
void prefix(int *a, int n)
{
#pragma cereal requires n >= 1 && n <= $len(a)
    int i;
    for (i = 1; i < n; i++) {
#pragma cereal invariant 1 <= i && i <= n
        a[i] = a[i - 1];
    }
}
