/* integer arithmetic, wrap-around and conversions; compared with gcc -O0 -fwrapv */
static unsigned u32(unsigned a, unsigned b) { return a * 2654435761u + b / 3u - (b % 7u); }
static int s32(int a, int b) { return (a * 31 + b) ^ (a >> 3) ^ (b << 2); }
static long long s64(long long a, long long b) { return a * 1000003LL - (b >> 5) + (a % 97); }
static signed char narrow(int x) { return (signed char)x; }
static unsigned short wide(short x) { return (unsigned short)(x * 3); }
int main(void)
{
    int r = 0;
    r += (int)(u32(123456u, 98765u) & 0xff);
    r += s32(-77, 12345) & 0xff;
    r += (int)(s64(-9000000000LL, 12345678901LL) & 0xff);
    r += narrow(300) + narrow(-129);
    r += wide(-20000) & 0xff;
    r += (2147483647 + 1) & 0xff;       /* wraps under -fwrapv */
    r += (int)(~0u >> 28) + (-17 / 5) + (-17 % 5) + (17u / 5u);
    r += (1 << 30) + !5 + !0 + -(-3);
    return r;
}
