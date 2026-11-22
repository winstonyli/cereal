/* r * c * c can reach 2^93: reported, never guessed. */
int area(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c * c <= $len(a)
    return r;
}
