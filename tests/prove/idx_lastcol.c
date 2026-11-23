/* the last element of each row, (i + 1) * c - 1 */
void lastcol(int *a, int r, int c)
{
#pragma cereal requires r >= 1 && c >= 1 && r * c <= $len(a) && r * c <= 2147483647
    int i;
    for (i = 0; i < r; i++) {
#pragma cereal invariant 0 <= i && i <= r
        a[(i + 1) * c - 1] = 0;
    }
}
