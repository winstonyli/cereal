/* the 2D fill over unsigned long: the spec product has a 64-bit operand, so it is refused (PARKED.md: spec products over 64-bit operands) */
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
