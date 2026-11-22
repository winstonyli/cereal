/* fill2 without c >= 0: the loop invariant does not hold on entry, and the bound fails. */
void fill2(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && r * c <= $len(a) && r * c <= 2147483647
    int i, j;
    for (i = 0; i < r; i++) {
#pragma cereal invariant 0 <= i && i <= r
        for (j = 0; j < c; j++) {
#pragma cereal invariant 0 <= i && i < r && 0 <= j && j <= c
            a[i * c + j] = 0;
        }
    }
}
