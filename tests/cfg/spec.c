/* Specs (#pragma cereal) reach the graph as `spec` statements: requires and
 * ensures lead the entry block, an invariant leads its loop's body block, and
 * the ==> and && inside one split no block. */

int sum(int *a, int n)
{
#pragma cereal requires n >= 0
#pragma cereal ensures n >= 0 ==> n < 100 && n >= 0
    int s = 0, i;
    for (i = 0; i < n; i++) {
#pragma cereal invariant 0 <= i && i <= n
        s += a[i];
    }
    return s;
}

int count(int n)
{
#pragma cereal requires n > 0
    int i = 0;
    while (i < n) {
#pragma cereal invariant i >= 0 ==> i <= n
        i++;
    }
    return i;
}

int spin(int n)
{
    int i = n;
    do {
#pragma cereal invariant i <= n
        i--;
    } while (i > 0);
    return i;
}

int forever(int n)
{
    int i = 0;
    for (;;) {
#pragma cereal invariant i >= 0
        if (i >= n)
            break;
        i++;
    }
    return i;
}

/* return is lowered to a `result` operation (spec_result.c has $len and slices). */
int result(int n)
{
#pragma cereal ensures return == n
    return n;
}
