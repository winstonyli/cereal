/* A byte copy: bounds, the loop invariant and the postcondition all follow from the contract. */
char *copy(char *dst, const char *src, unsigned long n)
{
#pragma cereal requires n <= $len(dst) && n <= $len(src)
#pragma cereal ensures dst[0:n] == src[0:n]
    unsigned long i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
    return dst;
}
