int helper(int v) { return v + 1; }
int work(int n)
{
    int total = 0;
    int extra = helper(n);
    total += extra;
    return total;
}
int after(void) { return helper(2) + work(3); }
