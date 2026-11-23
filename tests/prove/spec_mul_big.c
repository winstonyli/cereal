/* r * c * c can reach 2^93: an exact Int, which the comparison with the unsigned $len would have to convert: reported, never guessed. */
int area(int *a, int r, int c)
{
#pragma cereal requires r >= 0 && c >= 0 && r * c * c <= $len(a)
    return r;
}
