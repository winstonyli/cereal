/* The same reflexive slice equality with 0 <= lo: provable. */
int refl(char *a, int lo, unsigned long n)
{
#pragma cereal requires n <= $len(a) && 0 <= lo && lo <= n
#pragma cereal ensures a[lo:n] == a[lo:n]
    return 0;
}
