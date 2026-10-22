/* sweep_rt.h - the gcc side of `cereal check --eval-sweep` (docs/EVAL_DESIGN.md).
 *
 * The sweep prints, per function, a thunk `unsigned t_NAME(in, out)`: it unpacks the call's
 * argument bytes, calls NAME, and packs the result bytes (the return value, a returned
 * pointer as a tag, then the final contents of every pointed-to object).  sw_main runs the
 * calls of a file of `NAME INHEX` lines through the table of thunks and prints each result as
 * hex, one line per call ("-" is empty). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* r points into argument i's buffer b (n bytes): tag it (index + 1) << 32 | offset */
#define SW_IN(t, r, i, b, n)                                                                    \
    if ((unsigned char *)(r) >= (b) && (unsigned char *)(r) <= (b) + (n))                       \
        t = ((unsigned long long)((i) + 1) << 32) | (unsigned)((unsigned char *)(r) - (b));

typedef struct SwTh {
    const char *name;
    unsigned (*fn)(const unsigned char *, unsigned char *);
} SwTh;

static int sw_nib(int c)
{
    return c <= '9' ? c - '0' : c - 'a' + 10;
}

static int sw_main(const SwTh *tab, int ntab, const char *calls)
{
    static unsigned char in[1 << 15], out[1 << 15];
    char line[1 << 16 | 256];
    FILE *f = fopen(calls, "r");
    if (!f)
        return 2;
    while (fgets(line, sizeof line, f)) {
        char *sp = strchr(line, ' '), *h;
        unsigned n = 0, i, k;
        int t;
        if (!sp)
            return 2;
        *sp = 0;
        for (t = 0; t < ntab && strcmp(tab[t].name, line); t++)
            ;
        if (t == ntab)
            return 3;
        h = sp + 1;
        if (*h != '-')
            for (; h[0] > ' ' && h[1] > ' '; h += 2)
                in[n++] = (unsigned char)(sw_nib(h[0]) << 4 | sw_nib(h[1]));
        memset(out, 0, sizeof out);
        k = tab[t].fn(in, out);
        if (!k)
            putchar('-');
        for (i = 0; i < k; i++)
            printf("%02x", out[i]);
        putchar('\n');
        fflush(stdout);
    }
    return 0;
}
