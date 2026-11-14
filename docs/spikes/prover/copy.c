char *copy(char *dst, const char *src, unsigned long n)
{
    unsigned long i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
    return dst;
}
