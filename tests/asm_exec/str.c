/* String literals for tests/asm_exec.py: read-only data and %rip-relative addresses. */
int slen(int k)
{
    const char *s = k & 1 ? "hello" : "abc";
    int n = 0;
    while (s[n])
        n++;
    return n;
}

int pick(int k)
{
    return "abcdef"[k & 3] + "xyz"[k & 1];
}

int mid(int k)
{
    const char *s = "a\0b";
    return s[0] + s[2] * k;
}

int cmp(int a, int b)
{
    const char *p = "abcxyz" + (a & 3);
    const char *q = "abcxyz" + (b & 3);
    int d = 0;
    while (*p && *p == *q) {
        p++;
        q++;
        d++;
    }
    return d * 100 + (*p - *q);
}
