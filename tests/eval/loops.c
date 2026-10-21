/* loops, recursion, switch, comma, post/pre increment */
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int gcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }
static int sw(int x)
{
    switch (x) {
    case 0: return 10;
    case 1: case 2: return 20;
    case 5: x += 3;      /* fall through */
    case 6: return x * 2;
    default: return -1;
    }
}
int main(void)
{
    int i, s = 0, k = 0;
    for (i = 0; i < 100; i++)
        s += i & 3 ? i : -i;
    do { k++; } while (k < 17);
    s += fib(15) + gcd(1071, 462) + sw(0) + sw(2) + sw(5) + sw(6) + sw(9);
    s += (k++, k++, k);
    s += k-- + --k + ++k;
    i = 0;
    while (1) { if (++i > 50) break; if (i % 2) continue; s += i; }
    return s;
}
