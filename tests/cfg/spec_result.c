/* return, $len and slices in a spec are lowered to result, len and sliceeq
 * (docs/PROVE_OBLIGATIONS_DESIGN.md section 4); the verifier checks they stand
 * only under a spec. */

char *copy(char *dst, const char *src, unsigned long n)
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

int twice(int n)
{
#pragma cereal ensures return == n + n
    return n + n;
}

int neq(int *a, int *b, int n)
{
#pragma cereal ensures a[0:n] != b[1:n + 1]
    return 0;
}
