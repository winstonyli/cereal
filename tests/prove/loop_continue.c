/* A continue gives the head two back edges; each must preserve the invariant. */
unsigned long skip(unsigned long n, const char *s)
{
#pragma cereal requires n <= $len(s)
#pragma cereal ensures return <= n
    unsigned long i = 0, c = 0;
    for (;;) {
#pragma cereal invariant i <= n && c <= i
        if (i >= n)
            break;
        i++;
        if (s[i - 1] == 0)
            continue;
        c++;
    }
    return c;
}
