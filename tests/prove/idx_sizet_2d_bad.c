/* the size_t 2D fill with an off-by-one index: the contract does not cover it, so the write is not proved */
void fill(int *a, unsigned long r, unsigned long c)
{
#pragma cereal requires r * c <= $len(a)
    unsigned long i, j;
    for (i = 0; i < r; i++) {
#pragma cereal invariant i <= r
        for (j = 0; j < c; j++) {
#pragma cereal invariant i < r && j <= c
            a[i * c + j + 1] = 0;
        }
    }
}
