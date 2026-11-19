/* A slice with an int bound: a negative bound extends to a huge value, so even a reflexive slice equality fails. */
int refl(char *a, int lo, unsigned long n)
{
#pragma cereal requires n <= $len(a) && lo <= 0
#pragma cereal ensures a[lo:n] == a[lo:n]
    return 0;
}
