/* Nested loops over int indices: the write a[i * c + j] needs a nonlinear bound over narrow arithmetic (r * c within $len(a) and INT_MAX). */
void fill2(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c <= $len(a) && r * c <= 2147483647
    int i, j;
    for (i = 0; i < r; i++) {
#pragma cereal invariant 0 <= i && i <= r
        for (j = 0; j < c; j++) {
#pragma cereal invariant 0 <= i && i < r && 0 <= j && j <= c
            a[i * c + j] = 0;
        }
    }
}
