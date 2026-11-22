/* r * c + 1 is exact (range 2^62 + 1): accepted. */
int area(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c + 1 <= $len(a)
#pragma cereal ensures r * c + 1 <= $len(a)
    return r;
}
