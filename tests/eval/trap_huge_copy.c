// trap: out of bounds
/* A length near 2^64 must not wrap the bounds check around to zero. */
int main(void)
{
    char a[16], b[16];
    __builtin_memcpy(a + 8, b, (unsigned long)-8);
    return a[0];
}
