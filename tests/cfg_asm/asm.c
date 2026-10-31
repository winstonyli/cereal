/* A function that runs asm in a sweep call: its edges count, but the call is not printed (gcc would differ). */
static unsigned cpu(unsigned leaf)
{
    unsigned a = leaf, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return d;
}
int feat(int x)
{
    if (x == 5)
        return cpu(1) ? 1 : 2;
    return 0;
}
