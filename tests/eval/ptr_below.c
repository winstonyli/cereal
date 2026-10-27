/* a pointer below its array's start still orders before it (the lazy sweep's blobs have a head to look into) */
int main(void)
{
    int a[4] = {1, 2, 3, 4};
    int *p = a + 1, *q = p - 2;
    return (q < p) + 2 * (p > q) + 4 * (q <= p) + 8 * (p >= q) + 16 * (q != p) + 32 * !(p < q);
}
