/* A spec product of two ints is exact in 64 bits: accepted, and an `ensures` over it is a goal. */
int area(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c <= $len(a)
#pragma cereal ensures r * c <= $len(a)
    return r;
}
