// trap: result undefined
int main(void)
{
    volatile unsigned x = 0;
    return __builtin_clz(x);
}
