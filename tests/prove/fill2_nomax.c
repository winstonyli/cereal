/* fill2 without r * c <= INT_MAX: the int index arithmetic may wrap, so the write is not provable. */
void fill2(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c <= $len(a)
    int i, j;
    for (i = 0; i < r; i++) {
#pragma cereal invariant 0 <= i && i <= r
        for (j = 0; j < c; j++) {
#pragma cereal invariant 0 <= i && i < r && 0 <= j && j <= c
            a[i * c + j] = 0;
        }
    }
}
