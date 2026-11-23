/* the 2D fill over unsigned long: the spec product has a 64-bit operand, so spec arithmetic is exact Int (PS_B2I) and the Int tier proves it */
void fill(int *a, unsigned long r, unsigned long c)
{
#pragma cereal requires r * c <= $len(a)
    unsigned long i, j;
    for (i = 0; i < r; i++) {
#pragma cereal invariant i <= r
        for (j = 0; j < c; j++) {
#pragma cereal invariant i < r && j <= c
            a[i * c + j] = 0;
        }
    }
}
