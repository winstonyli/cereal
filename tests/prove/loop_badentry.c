/* The invariant is false on entry (i starts at 1 with n == 0 allowed): the entry goal fails, the rest follow. */
unsigned long upto(unsigned long n)
{
#pragma cereal ensures return == n
    unsigned long i = 1;
    while (i < n) {
#pragma cereal invariant i <= n
        i++;
    }
    return i;
}
