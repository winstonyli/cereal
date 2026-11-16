/* A do loop: the body runs before the test, so the invariant holds at the first arrival too. */
unsigned long upto1(unsigned long n)
{
#pragma cereal requires n >= 1
#pragma cereal ensures return == n
    unsigned long i = 0;
    do {
#pragma cereal invariant i < n
        i++;
    } while (i < n);
    return i;
}
