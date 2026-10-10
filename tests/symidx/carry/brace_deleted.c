int a1(int x)
{
    if (x) {
        return 1;
    return 0;
}
int a2(int y)
{
    int z = y;
    return z + a1(y);
}
