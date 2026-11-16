/* Taking a local's address is unsupported. */
int h(int x)
{
#pragma cereal ensures return == 1
    int *p = &x;
    *p = 1;
    return x;
}
