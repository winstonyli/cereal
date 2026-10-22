// trap: __builtin_unreachable reached
int main(void)
{
    volatile int x = 1;
    if (x)
        __builtin_unreachable();
    return 0;
}
