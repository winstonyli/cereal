// trap: out of range
int main(void)
{
    volatile double big = 1e30;
    return (int)big;
}
