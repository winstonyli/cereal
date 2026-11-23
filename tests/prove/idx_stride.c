/* a strided write: i * s stays below n * s when s >= 1 */
void stride(int *a, int n, int s)
{
#pragma cereal requires n >= 0 && s >= 1 && n * s <= $len(a) && n * s <= 2147483647
    int i;
    for (i = 0; i < n; i++) {
#pragma cereal invariant 0 <= i && i <= n
        a[i * s] = 0;
    }
}
