/* The same copy over int elements: element width 4 bytes. */
int *copy(int *dst, const int *src, unsigned long n)
{
#pragma cereal requires n <= $len(dst) && n <= $len(src)
#pragma cereal ensures dst[0:n] == src[0:n]
    unsigned long i;
    for (i = 0; i < n; i++) {
#pragma cereal invariant i <= n && dst[0:i] == src[0:i]
        dst[i] = src[i];
    }
    return dst;
}
