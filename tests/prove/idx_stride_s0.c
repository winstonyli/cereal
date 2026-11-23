/* the same with s >= 0: s = 0 and an empty array make the write out of bounds, and the prover says so (sat) */
void stride(int *a, int n, int s)
{
#pragma cereal requires n >= 0 && s >= 0 && n * s <= $len(a) && n * s <= 2147483647
    int i;
    for (i = 0; i < n; i++) {
#pragma cereal invariant 0 <= i && i <= n
        a[i * s] = 0;
    }
}
