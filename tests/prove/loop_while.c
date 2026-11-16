/* A while loop with an invariant: the sum counter stays within n. */
unsigned long upto(unsigned long n)
{
#pragma cereal ensures return == n
    unsigned long i = 0;
    while (i < n) {
#pragma cereal invariant i <= n
        i++;
    }
    return i;
}
