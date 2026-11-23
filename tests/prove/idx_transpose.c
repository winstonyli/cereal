/* a transposing copy over int: both indices are products of loop counters and sizes, nonlinear over narrow arithmetic */
void transpose(int *dst, int *src, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c <= $len(dst) && r * c <= $len(src) && r * c <= 2147483647
    int i, j;
    for (i = 0; i < r; i++) {
#pragma cereal invariant 0 <= i && i <= r
        for (j = 0; j < c; j++) {
#pragma cereal invariant 0 <= i && i < r && 0 <= j && j <= c
            dst[j * r + i] = src[i * c + j];
        }
    }
}
